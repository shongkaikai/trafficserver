/** @file

  This file implements the LogAccess class.

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

 */

#include "proxy/logging/LogAccess.h"

#include "tscore/Version.h"
#include "proxy/http/HttpSM.h"
#include "proxy/hdrs/MIME.h"
#include "iocore/utils/Machine.h"
#include "proxy/logging/LogFormat.h"
#include "proxy/logging/LogBuffer.h"
#include "tscore/Encoding.h"
#include "../private/SSLProxySession.h"
#include "tscore/ink_inet.h"

char INVALID_STR[] = "!INVALID_STR!";

#define HIDDEN_CONTENT_TYPE     "@Content-Type"
#define HIDDEN_CONTENT_TYPE_LEN 13

// should be at least 22 bytes to always accommodate a converted
// MgmtInt, MgmtIntCounter or  MgmtFloat. 22 bytes is enough for 64 bit
// ints + sign + eos, and enough for %e floating point representation
// + eos
//
#define MARSHAL_RECORD_LENGTH 32

namespace
{
DbgCtl dbg_ctl_log_escape{"log-escape"};
DbgCtl dbg_ctl_log_resolve{"log-resolve"};
DbgCtl dbg_ctl_log_unmarshal_orun{"log-unmarshal-orun"}; // Overrrun of unmarshaling destination buffer.
DbgCtl dbg_ctl_log_unmarshal_data{"log-unmarshal-data"}; // Error in txn data when unmarshalling.

} // end anonymous namespace

#define DBG_UNMARSHAL_DEST_OVERRUN Dbg(dbg_ctl_log_unmarshal_orun, "Unmarshal destination buffer overrun.");

/*-------------------------------------------------------------------------
  LogAccess

  Initialize the private data members and assert that we got a valid state
  machine pointer.
  -------------------------------------------------------------------------*/

LogAccess::LogAccess(HttpSM *sm) : m_http_sm(sm)
{
  ink_assert(m_http_sm != nullptr);
}

/*-------------------------------------------------------------------------
  LogAccess::init
  -------------------------------------------------------------------------*/

void
LogAccess::init()
{
  HttpTransact::HeaderInfo *hdr = &(m_http_sm->t_state.hdr_info);

  if (hdr->client_request.valid()) {
    m_client_request = &(hdr->client_request);

    // make a copy of the incoming url into the arena
    const char *url_string_ref = m_client_request->url_string_get_ref(&m_client_req_url_len);
    m_client_req_url_str       = m_arena.str_alloc(m_client_req_url_len + 1);
    memcpy(m_client_req_url_str, url_string_ref, m_client_req_url_len);
    m_client_req_url_str[m_client_req_url_len] = '\0';

    m_client_req_url_canon_str =
      Encoding::escapify_url(&m_arena, m_client_req_url_str, m_client_req_url_len, &m_client_req_url_canon_len);
    auto path{m_client_request->path_get()};
    m_client_req_url_path_str = path.data();
    m_client_req_url_path_len = static_cast<int>(path.length());
  }

  if (hdr->client_response.valid()) {
    m_proxy_response = &(hdr->client_response);
    MIMEField *field = m_proxy_response->field_find(static_cast<std::string_view>(MIME_FIELD_CONTENT_TYPE));
    if (field) {
      auto proxy_resp_content_type{field->value_get()};
      m_proxy_resp_content_type_str = const_cast<char *>(proxy_resp_content_type.data());
      m_proxy_resp_content_type_len = proxy_resp_content_type.length();
      LogUtils::remove_content_type_attributes(m_proxy_resp_content_type_str, &m_proxy_resp_content_type_len);
    } else {
      // If Content-Type field is missing, check for @Content-Type
      field = m_proxy_response->field_find(
        std::string_view{HIDDEN_CONTENT_TYPE, static_cast<std::string_view::size_type>(HIDDEN_CONTENT_TYPE_LEN)});
      if (field) {
        auto proxy_resp_content_type{field->value_get()};
        m_proxy_resp_content_type_str = const_cast<char *>(proxy_resp_content_type.data());
        m_proxy_resp_content_type_len = proxy_resp_content_type.length();
        LogUtils::remove_content_type_attributes(m_proxy_resp_content_type_str, &m_proxy_resp_content_type_len);
      }
    }
    auto reason{m_proxy_response->reason_get()};
    m_proxy_resp_reason_phrase_str = const_cast<char *>(reason.data());
    m_proxy_resp_reason_phrase_len = static_cast<int>(reason.length());
  }
  if (hdr->server_request.valid()) {
    m_proxy_request = &(hdr->server_request);
  }
  if (hdr->server_response.valid()) {
    m_server_response = &(hdr->server_response);
  }
  if (hdr->cache_response.valid()) {
    m_cache_response = &(hdr->cache_response);
  }
}

int
LogAccess::marshal_proxy_host_name(char *buf)
{
  int         len = 0;
  char const *str = nullptr;

  if (Machine *machine = Machine::instance(); machine) {
    str = machine->host_name.c_str();
    len = machine->host_name.length();
  }

  len = INK_ALIGN_DEFAULT(len + 1);
  if (buf) {
    marshal_str(buf, str, len);
  }

  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_host_ip(char *buf)
{
  return marshal_ip(buf, &Machine::instance()->ip.sa);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_process_uuid(char *buf)
{
  int len = round_strlen(TS_UUID_STRING_LEN + 1);

  if (buf) {
    const char *str = const_cast<char *>(Machine::instance()->process_uuid.getString());
    marshal_str(buf, str, len);
  }
  return len;
}

int
LogAccess::marshal_process_sfid(char *buf)
{
  char const *str = nullptr;
  int         len = 0;

  if (Machine *machine = Machine::instance(); machine) {
    std::string_view snowflake_id = machine->process_snowflake_id->get_string();
    str                           = snowflake_id.data();
    len                           = snowflake_id.length();
  }

  len = INK_ALIGN_DEFAULT(len + 1);
  if (buf) {
    marshal_str(buf, str, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_config_int_var(char *config_var, char *buf)
{
  if (buf) {
    int64_t val;
    val = RecGetRecordInt(config_var).value_or(0);
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_config_str_var(char *config_var, char *buf)
{
  auto str{RecGetRecordStringAlloc(config_var)};
  auto c_str{ats_as_c_str(str)};
  int  len = LogAccess::strlen(c_str);
  if (buf) {
    marshal_str(buf, c_str, len);
  }
  return len;
}

// To allow for a generic marshal_record function, rather than
// multiple functions (one per data type) we always marshal a record
// as a string of a fixed length.  We use a fixed length because the
// marshal_record function can be called with a null *buf to request
// the length of the record, and later with a non-null *buf to
// actually request the record to be inserted in the buffer, and both
// calls should return the same number of characters. If we did not
// enforce a fixed size, this would not necessarily be the case
// because records --statistics in particular-- can potentially change
// between one call and the other.
//
int
LogAccess::marshal_record(char *record, char *buf)
{
  const unsigned int max_chars = MARSHAL_RECORD_LENGTH;

  if (nullptr == buf) {
    return max_chars;
  }

  const char        *record_not_found_msg   = "RECORD_NOT_FOUND";
  const unsigned int record_not_found_chars = ::strlen(record_not_found_msg) + 1;

  char         ascii_buf[max_chars];
  const char  *out_buf;
  unsigned int num_chars;

#define LOG_INTEGER RECD_INT
#define LOG_COUNTER RECD_COUNTER
#define LOG_FLOAT   RECD_FLOAT
#define LOG_STRING  RECD_STRING

  RecDataT stype = RECD_NULL;
  bool     found = false;

  // Since, for now at least, String metrics are still in librecords, do that lookup
  // first, and only do the new metrics lookup on a miss.
  if (RecGetRecordDataType(record, &stype) != REC_ERR_OKAY) {
    ts::Metrics        &metrics = ts::Metrics::instance();
    ts::Metrics::IdType mid     = metrics[record];

    if (mid != ts::Metrics::NOT_FOUND) {
      int64_t val = metrics[mid].load();

      out_buf = int64_to_str(ascii_buf, max_chars, val, &num_chars);
      ink_assert(out_buf);
    } else {
      out_buf   = "INVALID_RECORD";
      num_chars = ::strlen(out_buf) + 1;
    }
  } else {
    if (LOG_INTEGER == stype || LOG_COUNTER == stype) {
      // we assume MgmtInt and MgmtIntCounter are int64_t for the
      // conversion below, if this ever changes we should modify
      // accordingly
      //
      ink_assert(sizeof(int64_t) >= sizeof(RecInt) && sizeof(int64_t) >= sizeof(RecCounter));

      // so that a 64 bit integer will fit (including sign and eos)
      //
      ink_assert(max_chars > 21);

      auto tmp{LOG_INTEGER == stype ? RecGetRecordInt(record) : RecGetRecordCounter(record)};
      auto found{tmp.has_value()};

      if (found) {
        out_buf = int64_to_str(ascii_buf, max_chars, tmp.value(), &num_chars);
        ink_assert(out_buf);
      } else {
        out_buf   = const_cast<char *>(record_not_found_msg);
        num_chars = record_not_found_chars;
      }
    } else if (LOG_FLOAT == stype) {
      // we assume MgmtFloat is at least a float for the conversion below
      // (the conversion itself assumes a double because of the %e)
      // if this ever changes we should modify accordingly
      //
      ink_assert(sizeof(double) >= sizeof(RecFloat));

      auto val{RecGetRecordFloat(record)};
      found = val.has_value();

      if (found) {
        // snprintf does not support "%e" in the format
        // and we want to use "%e" because it is the most concise
        // notation

        num_chars = snprintf(ascii_buf, sizeof(ascii_buf), "%e", val.value()) + 1; // include eos

        // the "%e" field above should take 13 characters at most
        //
        ink_assert(num_chars <= max_chars);

        // the following should never be true
        //
        if (num_chars > max_chars) {
          // data does not fit, output asterisks
          out_buf   = "***";
          num_chars = ::strlen(out_buf) + 1;
        } else {
          out_buf = ascii_buf;
        }
      } else {
        out_buf   = const_cast<char *>(record_not_found_msg);
        num_chars = record_not_found_chars;
      }
    } else if (LOG_STRING == stype) {
      if (auto sv{RecGetRecordString(record, ascii_buf, sizeof(ascii_buf))}; sv) {
        if (sv.value().length() > 0) {
          num_chars = sv.value().length() + 1;
          if (num_chars == max_chars) {
            // truncate string and write ellipsis at the end
            ascii_buf[max_chars - 1] = 0;
            ascii_buf[max_chars - 2] = '.';
            ascii_buf[max_chars - 3] = '.';
            ascii_buf[max_chars - 4] = '.';
          }
          out_buf = ascii_buf;
        } else {
          out_buf   = "NULL";
          num_chars = ::strlen(out_buf) + 1;
        }
      } else {
        out_buf   = const_cast<char *>(record_not_found_msg);
        num_chars = record_not_found_chars;
      }
    } else {
      out_buf   = "INVALID_MgmtType";
      num_chars = ::strlen(out_buf) + 1;
      ink_assert(!"invalid MgmtType for requested record");
    }
  }

  ink_assert(num_chars <= max_chars);
  memcpy(buf, out_buf, num_chars);

  return max_chars;
}

/*-------------------------------------------------------------------------
  LogAccess::marshal_str

  Copy the given string to the destination buffer, including the trailing
  NULL.  For binary formatting, we need the NULL to distinguish the end of
  the string, and we'll remove it for ascii formatting.
  ASSUMES dest IS NOT NULL.
  The array pointed to by dest must be at least padded_len in length.
  -------------------------------------------------------------------------*/

void
LogAccess::marshal_str(char *dest, const char *source, int padded_len)
{
  if (source == nullptr || source[0] == 0 || padded_len == 0) {
    source = DEFAULT_STR;
  }
  ink_strlcpy(dest, source, padded_len);

#ifdef DEBUG
  //
  // what padded_len should be, if there is no padding, is strlen()+1.
  // if not, then we needed to pad and should touch the intermediate
  // bytes to avoid UMR errors when the buffer is written.
  //
  size_t real_len = (::strlen(source) + 1);
  while (static_cast<int>(real_len) < padded_len) {
    dest[real_len] = '$';
    real_len++;
  }
#endif
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_all_header_fields(char *buf)
{
  return LogUtils::marshalMimeHdr(m_client_request, buf);
}

/*-------------------------------------------------------------------------
  LogAccess::marshal_mem

  This is a version of marshal_str that works with unterminated strings.
  In this case, we'll copy the buffer and then add a trailing null that
  the rest of the system assumes.
  -------------------------------------------------------------------------*/

void
LogAccess::marshal_mem(char *dest, const char *source, int actual_len, int padded_len)
{
  if (source == nullptr || source[0] == 0 || actual_len == 0) {
    source     = DEFAULT_STR;
    actual_len = DEFAULT_STR_LEN;
    ink_assert(actual_len < padded_len);
  }
  memcpy(dest, source, actual_len);
  dest[actual_len] = 0; // add terminating null

#ifdef DEBUG
  //
  // what len should be, if there is no padding, is strlen()+1.
  // if not, then we needed to pad and should touch the intermediate
  // bytes to avoid UMR errors when the buffer is written.
  //
  int real_len = actual_len + 1;
  while (real_len < padded_len) {
    dest[real_len] = '$';
    real_len++;
  }
#endif
}

/*-------------------------------------------------------------------------
  LogAccess::marshal_ip

  Marshal an IP address in a reasonably compact way. If the address isn't
  valid (NULL or not IP) then marshal an invalid address record.
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_ip(char *dest, sockaddr const *ip)
{
  LogFieldIpStorage data;
  int               len = sizeof(data._ip);
  if (nullptr == ip) {
    data._ip._family = AF_UNSPEC;
  } else if (ats_is_ip4(ip)) {
    if (dest) {
      data._ip4._family = AF_INET;
      data._ip4._addr   = ats_ip4_addr_cast(ip);
    }
    len = sizeof(data._ip4);
  } else if (ats_is_ip6(ip)) {
    if (dest) {
      data._ip6._family = AF_INET6;
      data._ip6._addr   = ats_ip6_addr_cast(ip);
    }
    len = sizeof(data._ip6);
  } else if (ats_is_unix(ip)) {
    if (dest) {
      data._un._family = AF_UNIX;
      strncpy(data._un._path, ats_unix_cast(ip)->sun_path, TS_UNIX_SIZE);
    }
    len = sizeof(data._un);
  } else {
    data._ip._family = AF_UNSPEC;
  }

  if (dest) {
    memcpy(dest, &data, len);
  }
  return INK_ALIGN_DEFAULT(len);
}

inline int
LogAccess::unmarshal_with_map(int64_t code, char *dest, int len, const Ptr<LogFieldAliasMap> &map, const char *msg)
{
  long int codeStrLen = 0;

  switch (map->asString(code, dest, len, reinterpret_cast<size_t *>(&codeStrLen))) {
  case LogFieldAliasMap::INVALID_INT:
    if (msg) {
      const int bufSize = 64;
      char      invalidCodeMsg[bufSize];
      codeStrLen = snprintf(invalidCodeMsg, 64, "%s(%" PRId64 ")", msg, code);
      if (codeStrLen < bufSize && codeStrLen < len) {
        ink_strlcpy(dest, invalidCodeMsg, len);
      } else {
        DBG_UNMARSHAL_DEST_OVERRUN
        codeStrLen = -1;
      }
    } else {
      codeStrLen = -1;
    }
    break;
  case LogFieldAliasMap::BUFFER_TOO_SMALL:
    DBG_UNMARSHAL_DEST_OVERRUN
    codeStrLen = -1;
    break;
  }

  return codeStrLen;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_int

  Return the integer pointed at by the buffer and advance the buffer
  pointer past the int.  The int will be converted back to host byte order.
  -------------------------------------------------------------------------*/

int64_t
LogAccess::unmarshal_int(char **buf)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  int64_t val;

  // TODO: this used to do nthol, do we need to worry? TS-1156.
  val   = *(reinterpret_cast<int64_t *>(*buf));
  *buf += INK_MIN_ALIGN;
  return val;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_all_header_fields(char *buf)
{
  return LogUtils::marshalMimeHdr(m_proxy_response, buf);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

/*-------------------------------------------------------------------------
  unmarshal_itoa

  This routine provides a fast conversion from a binary int to a string.
  It returns the number of characters formatted.  "dest" must point to the
  LAST character of an array large enough to store the complete formatted
  number.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_itoa(int64_t val, char *dest, int field_width, char leading_char)
{
  ink_assert(dest != nullptr);

  char *p        = dest;
  bool  negative = false;

  if (val < 0) {
    negative = true;
    val      = -val;
  }

  do {
    *p--  = '0' + (val % 10);
    val  /= 10;
  } while (val);

  while (dest - p < field_width) {
    *p-- = leading_char;
  }

  if (negative) {
    *p-- = '-';
  }

  return static_cast<int>(dest - p);
}

/*-------------------------------------------------------------------------
  unmarshal_itox

  This routine provides a fast conversion from a binary int to a hex string.
  It returns the number of characters formatted.  "dest" must point to the
  LAST character of an array large enough to store the complete formatted
  number.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_itox(int64_t val, char *dest, int field_width, char leading_char)
{
  ink_assert(dest != nullptr);

  char       *p       = dest;
  static char table[] = "0123456789abcdef?";

  for (int i = 0; i < static_cast<int>(sizeof(int64_t) * 2); i++) {
    *p--   = table[val & 0xf];
    val  >>= 4;
  }
  while (dest - p < field_width) {
    *p-- = leading_char;
  }

  return static_cast<int64_t>(dest - p);
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_int_to_str

  Return the string representation of the integer pointed at by buf.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_int_to_str(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  char    val_buf[128];
  int64_t val     = unmarshal_int(buf);
  int     val_len = unmarshal_itoa(val, val_buf + 127);

  if (val_len < len) {
    memcpy(dest, val_buf + 128 - val_len, val_len);
    return val_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_int_to_str_hex

  Return the string representation (hexadecimal) of the integer pointed at by buf.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_int_to_str_hex(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  char    val_buf[128];
  int64_t val     = unmarshal_int(buf);
  int     val_len = unmarshal_itox(val, val_buf + 127);

  if (val_len < len) {
    memcpy(dest, val_buf + 128 - val_len, val_len);
    return val_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_req_all_header_fields(char *buf)
{
  return LogUtils::marshalMimeHdr(m_proxy_request, buf);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

namespace
{
class EscLookup
{
public:
  static const char NO_ESCAPE{'\0'};
  static const char LONG_ESCAPE{'\x01'};

  static char
  result(char c)
  {
    return _lu.table[static_cast<unsigned char>(c)];
  }

private:
  struct _LUT {
    _LUT();

    char table[1 << 8];
  };

  inline static _LUT const _lu;
};

EscLookup::_LUT::_LUT()
{
  for (unsigned i = 0; i < ' '; ++i) {
    table[i] = LONG_ESCAPE;
  }
  for (unsigned i = '\x7f'; i < sizeof(table); ++i) {
    table[i] = LONG_ESCAPE;
  }

  // Short escapes.
  //
  table[static_cast<int>('\b')] = 'b';
  table[static_cast<int>('\t')] = 't';
  table[static_cast<int>('\n')] = 'n';
  table[static_cast<int>('\f')] = 'f';
  table[static_cast<int>('\r')] = 'r';
  table[static_cast<int>('\\')] = '\\';
  table[static_cast<int>('\"')] = '"';
  table[static_cast<int>('/')]  = '/';
}

char
nibble(int nib)
{
  return nib >= 0xa ? 'a' + (nib - 0xa) : '0' + nib;
}

int
escape_json(char *dest, const char *buf, int len)
{
  int escaped_len = 0;

  for (int i = 0; i < len; i++) {
    char c  = buf[i];
    char ec = EscLookup::result(c);
    if (__builtin_expect(EscLookup::NO_ESCAPE == ec, 1)) {
      if (dest) {
        if (escaped_len + 1 > len) {
          break;
        }
        *dest++ = c;
      }
      escaped_len++;

    } else if (EscLookup::LONG_ESCAPE == ec) {
      if (dest) {
        if (escaped_len + 6 > len) {
          break;
        }
        *dest++ = '\\';
        *dest++ = 'u';
        *dest++ = '0';
        *dest++ = '0';
        *dest++ = nibble(static_cast<unsigned char>(c) >> 4);
        *dest++ = nibble(c & 0x0f);
      }
      escaped_len += 6;

    } else { // Short escape.
      if (dest) {
        if (escaped_len + 2 > len) {
          break;
        }
        *dest++ = '\\';
        *dest++ = ec;
      }
      escaped_len += 2;
    }
  } // end for
  return escaped_len;
}

int
unmarshal_str_json(char **buf, char *dest, int len, LogSlice *slice)
{
  Dbg(dbg_ctl_log_escape, "unmarshal_str_json start, len=%d, slice=%p", len, slice);

  char *val_buf     = *buf;
  int   val_len     = static_cast<int>(::strlen(val_buf));
  int   escaped_len = escape_json(nullptr, val_buf, val_len);

  *buf += LogAccess::strlen(val_buf); // this is how it was stored

  if (slice && slice->m_enable) {
    int offset, n;

    n = slice->toStrOffset(escaped_len, &offset);
    Dbg(dbg_ctl_log_escape, "unmarshal_str_json start, n=%d, offset=%d", n, offset);
    if (n <= 0) {
      return 0;
    }

    if (n >= len) {
      DBG_UNMARSHAL_DEST_OVERRUN
      return -1;
    }

    return escape_json(dest, (val_buf + offset), n);
  }

  if (escaped_len < len) {
    escape_json(dest, val_buf, escaped_len);
    return escaped_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

} // end anonymous namespace

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_str

  Retrieve the string from the location pointed at by the buffer and
  advance the pointer past the string.  The local strlen function is used
  to advance the pointer, thus matching the corresponding strlen that was
  used to lay the string into the buffer.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_str(char **buf, char *dest, int len, LogSlice *slice, LogEscapeType escape_type)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  if (LOG_ESCAPE_JSON == escape_type) {
    return unmarshal_str_json(buf, dest, len, slice);
  }

  char *val_buf = *buf;
  int   val_len = static_cast<int>(::strlen(val_buf));

  *buf += LogAccess::strlen(val_buf); // this is how it was stored

  if (slice && slice->m_enable) {
    int offset, n;

    n = slice->toStrOffset(val_len, &offset);
    if (n <= 0) {
      return 0;
    }

    if (n >= len) {
      DBG_UNMARSHAL_DEST_OVERRUN
      return -1;
    }

    memcpy(dest, (val_buf + offset), n);
    return n;
  }

  if (val_len < len) {
    memcpy(dest, val_buf, val_len);
    return val_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

int
LogAccess::unmarshal_ttmsf(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  int64_t val     = unmarshal_int(buf);
  int     val_len = snprintf(dest, len, "%" PRId64 ".%03d", val / 1000, int((val < 0 ? -val : val) % 1000));
  if (val_len >= len) {
    DBG_UNMARSHAL_DEST_OVERRUN
    return -1;
  }
  return val_len;
}

int
LogAccess::unmarshal_int_to_date_str(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  int64_t value  = unmarshal_int(buf);
  char   *strval = LogUtils::timestamp_to_date_str(value);
  int     strlen = static_cast<int>(::strlen(strval));

  if (strlen > len) {
    DBG_UNMARSHAL_DEST_OVERRUN
    return -1;
  }

  memcpy(dest, strval, strlen);
  return strlen;
}

int
LogAccess::unmarshal_int_to_time_str(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  int64_t value  = unmarshal_int(buf);
  char   *strval = LogUtils::timestamp_to_time_str(value);
  int     strlen = static_cast<int>(::strlen(strval));

  if (strlen > len) {
    DBG_UNMARSHAL_DEST_OVERRUN
    return -1;
  }

  memcpy(dest, strval, strlen);
  return strlen;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_resp_all_header_fields(char *buf)
{
  return LogUtils::marshalMimeHdr(m_server_response, buf);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_int_to_netscape_str(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  int64_t value  = unmarshal_int(buf);
  char   *strval = LogUtils::timestamp_to_netscape_str(value);
  int     strlen = static_cast<int>(::strlen(strval));

  if (strlen > len) {
    DBG_UNMARSHAL_DEST_OVERRUN
    return -1;
  }

  memcpy(dest, strval, strlen);
  return strlen;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_http_method

  Retrieve the int pointed at by the buffer and treat as an HttpMethod
  enumerated type.  Then lookup the string representation for that enum and
  return the string.  Advance the buffer pointer past the enum.
  -------------------------------------------------------------------------*/
/*
int
LogAccess::unmarshal_http_method (char **buf, char *dest, int len)
{
    return unmarshal_str (buf, dest, len);
}
*/

int
LogAccess::marshal_cache_resp_all_header_fields(char *buf)
{
  return LogUtils::marshalMimeHdr(m_cache_response, buf);
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_http_version

  The http version is marshalled as two consecutive integers, the first for
  the major number and the second for the minor number.  Retrieve both
  numbers and return the result as "HTTP/major.minor".
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_http_version(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  static const char http[]   = "HTTP/";
  static int        http_len = static_cast<int>(sizeof(http) - 1);

  char  val_buf[128];
  char *p = val_buf;

  auto vb_left = [&]() -> int { return sizeof(val_buf) - (p - val_buf); };

  memcpy(p, http, http_len);
  p += http_len;

  int res1 = unmarshal_int_to_str(buf, p, vb_left());
  if (res1 < 0) {
    return -1;
  }
  p        += res1;
  *p++      = '.';
  int res2  = unmarshal_int_to_str(buf, p, vb_left());
  if (res2 < 0) {
    DBG_UNMARSHAL_DEST_OVERRUN
    return -1;
  }

  int val_len = p - val_buf;
  if (val_len < len) {
    memcpy(dest, val_buf, val_len);
    return val_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_http_text

  The http text is a reproduced HTTP/1.x request line. It's HTTP method (cqhm) + URL (pqu) + HTTP version.
  This doesn't support HTTP/2 and HTTP/3 since those don't have a request line.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_http_text(char **buf, char *dest, int len, LogSlice *slice, LogEscapeType escape_type)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  char *p = dest;

  //    int res1 = unmarshal_http_method (buf, p, len);
  int res1 = unmarshal_str(buf, p, len, nullptr, escape_type);
  if (res1 < 0) {
    return -1;
  }
  p        += res1;
  *p++      = ' ';
  int res2  = unmarshal_str(buf, p, len - res1 - 1, slice, escape_type);
  if (res2 < 0) {
    return -1;
  }
  p        += res2;
  *p++      = ' ';
  int res3  = unmarshal_http_version(buf, p, len - res1 - res2 - 2);
  if (res3 < 0) {
    return -1;
  }
  return res1 + res2 + res3 + 2;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_http_status

  An http response status code (pssc,sssc) is just an INT, but it's always
  formatted with three digits and leading zeros.  So, we need a special
  version of unmarshal_int_to_str that does this leading zero formatting.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_http_status(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  char    val_buf[128];
  int64_t val     = unmarshal_int(buf);
  int     val_len = unmarshal_itoa(val, val_buf + 127, 3, '0');
  if (val_len < len) {
    memcpy(dest, val_buf + 128 - val_len, val_len);
    return val_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_ip

  Retrieve an IP address directly.
  -------------------------------------------------------------------------*/
int
LogAccess::unmarshal_ip(char **buf, IpEndpoint *dest)
{
  int len = sizeof(LogFieldIp); // of object processed.

  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  LogFieldIp *raw = reinterpret_cast<LogFieldIp *>(*buf);
  if (AF_INET == raw->_family) {
    LogFieldIp4 *ip4 = static_cast<LogFieldIp4 *>(raw);
    ats_ip4_set(dest, ip4->_addr);
    len = sizeof(*ip4);
  } else if (AF_INET6 == raw->_family) {
    LogFieldIp6 *ip6 = static_cast<LogFieldIp6 *>(raw);
    ats_ip6_set(dest, ip6->_addr);
    len = sizeof(*ip6);
  } else if (AF_UNIX == raw->_family) {
    LogFieldUn *un = static_cast<LogFieldUn *>(raw);
    ats_unix_set(dest, un->_path, TS_UNIX_SIZE);
    len = sizeof(*un);
  } else {
    ats_ip_invalidate(dest);
  }
  len   = INK_ALIGN_DEFAULT(len);
  *buf += len;
  return len;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_ip_to_str

  Retrieve the IP addresspointed at by the buffer and convert to a
  string in standard format. The string is written to @a dest and its
  length (not including nul) is returned. @a *buf is advanced.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_ip_to_str(char **buf, char *dest, int len)
{
  IpEndpoint ip;

  if (len > 0) {
    unmarshal_ip(buf, &ip);
    if (!ats_is_ip(&ip) && !ats_is_unix(ip)) {
      *dest = '0';
      Dbg(dbg_ctl_log_unmarshal_data, "Invalid IP address");
      return 1;
    } else if (ats_ip_ntop(&ip, dest, len)) {
      return static_cast<int>(::strlen(dest));
    }
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_ip_to_hex

  Retrieve the int pointed at by the buffer and treat as an IP
  address.  Convert to a string in byte oriented hexadeciaml and
  return the string.  Advance the buffer pointer.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_ip_to_hex(char **buf, char *dest, int len)
{
  IpEndpoint ip;

  if (len > 0) {
    unmarshal_ip(buf, &ip);
    if (!ats_is_ip(&ip) && !ats_is_unix(ip)) {
      *dest = '0';
      Dbg(dbg_ctl_log_unmarshal_data, "Invalid IP address");
      return 1;
    } else {
      return ats_ip_to_hex(&ip.sa, dest, len);
    }
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_hierarchy

  Retrieve the int pointed at by the buffer and treat as a
  SquidHierarchyCode.  Use this as an index into the local string
  conversion tables and return the string equivalent to the enum.
  Advance the buffer pointer.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_hierarchy(char **buf, char *dest, int len, const Ptr<LogFieldAliasMap> &map)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  return (LogAccess::unmarshal_with_map(unmarshal_int(buf), dest, len, map, "INVALID_CODE"));
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_finish_status

  Retrieve the int pointed at by the buffer and treat as a finish code.
  Use the enum as an index into a string table and return the string equiv
  of the enum.  Advance the pointer.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_finish_status(char **buf, char *dest, int len, const Ptr<LogFieldAliasMap> &map)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  return (LogAccess::unmarshal_with_map(unmarshal_int(buf), dest, len, map, "UNKNOWN_FINISH_CODE"));
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_cache_code

  Retrieve the int pointed at by the buffer and treat as a SquidLogCode.
  Use this to index into the local string tables and return the string
  equiv of the enum.  Advance the pointer.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_cache_code(char **buf, char *dest, int len, const Ptr<LogFieldAliasMap> &map)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  return (LogAccess::unmarshal_with_map(unmarshal_int(buf), dest, len, map, "ERROR_UNKNOWN"));
}

/*-------------------------------------------------------------------------
  LogAccess::unmarshal_cache_hit_miss

  Retrieve the int pointed at by the buffer and treat as a SquidHitMissCode.
  Use this to index into the local string tables and return the string
  equiv of the enum.  Advance the pointer.
  -------------------------------------------------------------------------*/

int
LogAccess::unmarshal_cache_hit_miss(char **buf, char *dest, int len, const Ptr<LogFieldAliasMap> &map)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  return (LogAccess::unmarshal_with_map(unmarshal_int(buf), dest, len, map, "HIT_MISS_UNKNOWN"));
}

int
LogAccess::unmarshal_cache_write_code(char **buf, char *dest, int len, const Ptr<LogFieldAliasMap> &map)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  return (LogAccess::unmarshal_with_map(unmarshal_int(buf), dest, len, map, "UNKNOWN_CACHE_WRITE_CODE"));
}

int
LogAccess::unmarshal_record(char **buf, char *dest, int len)
{
  ink_assert(buf != nullptr);
  ink_assert(*buf != nullptr);
  ink_assert(dest != nullptr);

  char *val_buf  = *buf;
  int   val_len  = static_cast<int>(::strlen(val_buf));
  *buf          += MARSHAL_RECORD_LENGTH; // this is how it was stored
  if (val_len < len) {
    memcpy(dest, val_buf, val_len);
    return val_len;
  }
  DBG_UNMARSHAL_DEST_OVERRUN
  return -1;
}

/*-------------------------------------------------------------------------
  resolve_logfield_string

  This function resolves the given custom log format string using the given
  LogAccess context and returns the resulting string, which is ats_malloc'd.
  The caller is responsible for ats_free'ing the return result.  If there are
  any problems, NULL is returned.
  -------------------------------------------------------------------------*/
char *
resolve_logfield_string(LogAccess *context, const char *format_str)
{
  if (!context) {
    Dbg(dbg_ctl_log_resolve, "No context to resolve?");
    return nullptr;
  }

  if (!format_str) {
    Dbg(dbg_ctl_log_resolve, "No format to resolve?");
    return nullptr;
  }

  Dbg(dbg_ctl_log_resolve, "Resolving: %s", format_str);

  //
  // Divide the format string into two parts: one for the printf-style
  // string and one for the symbols.
  //
  char *printf_str = nullptr;
  char *fields_str = nullptr;
  int   n_fields   = LogFormat::parse_format_string(format_str, &printf_str, &fields_str);

  //
  // Perhaps there were no fields to resolve?  Then just return the
  // format_str. Nothing to free here either.
  //
  if (!n_fields) {
    Dbg(dbg_ctl_log_resolve, "No fields found; returning copy of format_str");
    ats_free(printf_str);
    ats_free(fields_str);
    return ats_strdup(format_str);
  }

  Dbg(dbg_ctl_log_resolve, "%d fields: %s", n_fields, fields_str);
  Dbg(dbg_ctl_log_resolve, "printf string: %s", printf_str);

  LogFieldList fields;
  bool         contains_aggregates;
  int          field_count = LogFormat::parse_symbol_string(fields_str, &fields, &contains_aggregates);

  if (field_count != n_fields) {
    Error("format_str contains %d invalid field symbols", n_fields - field_count);
    ats_free(printf_str);
    ats_free(fields_str);
    return nullptr;
  }
  //
  // Ok, now marshal the data out of the LogAccess object and into a
  // temporary storage buffer.  Make sure the LogAccess context is
  // initialized first.
  //
  Dbg(dbg_ctl_log_resolve, "Marshaling data from LogAccess into buffer ...");
  context->init();
  unsigned bytes_needed = fields.marshal_len(context);
  char    *buf          = static_cast<char *>(ats_malloc(bytes_needed));
  unsigned bytes_used   = fields.marshal(context, buf);

  ink_assert(bytes_needed == bytes_used);
  Dbg(dbg_ctl_log_resolve, "    %u bytes marshalled", bytes_used);

  //
  // Now we can "unmarshal" the data from the buffer into a string,
  // combining it with the data from the printf string.  The problem is,
  // we're not sure how much space it will take when it's unmarshalled.
  // So, we'll just guess.
  //
  char    *result = static_cast<char *>(ats_malloc(8192));
  unsigned bytes_resolved =
    LogBuffer::resolve_custom_entry(&fields, printf_str, buf, result, 8191, LogUtils::timestamp(), 0, LOG_SEGMENT_VERSION);
  ink_assert(bytes_resolved < 8192);

  if (!bytes_resolved) {
    ats_free(result);
    result = nullptr;
  } else {
    result[bytes_resolved] = 0; // NULL terminate
  }

  ats_free(printf_str);
  ats_free(fields_str);
  ats_free(buf);

  return result;
}
void
LogAccess::set_client_req_url(char *buf, int len)
{
  if (buf) {
    m_client_req_url_len = std::min(len, m_client_req_url_len);
    ink_strlcpy(m_client_req_url_str, buf, m_client_req_url_len + 1);
  }
}

void
LogAccess::set_client_req_url_canon(char *buf, int len)
{
  if (buf) {
    m_client_req_url_canon_len = std::min(len, m_client_req_url_canon_len);
    ink_strlcpy(m_client_req_url_canon_str, buf, m_client_req_url_canon_len + 1);
  }
}

void
LogAccess::set_client_req_unmapped_url_canon(char *buf, int len)
{
  if (buf && m_client_req_unmapped_url_canon_str) {
    // m_client_req_unmapped_url_canon_str is not necessarily null terminated.
    m_client_req_unmapped_url_canon_len = std::min(len, m_client_req_unmapped_url_canon_len);
    memcpy(m_client_req_unmapped_url_canon_str, buf, m_client_req_unmapped_url_canon_len);
  }
}

void
LogAccess::set_client_req_unmapped_url_path(char *buf, int len)
{
  if (buf && m_client_req_unmapped_url_path_str) {
    m_client_req_unmapped_url_path_len = std::min(len, m_client_req_unmapped_url_path_len);
    ink_strlcpy(m_client_req_unmapped_url_path_str, buf, m_client_req_unmapped_url_path_len + 1);
  }
}

void
LogAccess::set_client_req_unmapped_url_host(char *buf, int len)
{
  if (buf && m_client_req_unmapped_url_host_str) {
    m_client_req_unmapped_url_host_len = std::min(len, m_client_req_unmapped_url_host_len);
    ink_strlcpy(m_client_req_unmapped_url_host_str, buf, m_client_req_unmapped_url_host_len + 1);
  }
}

void
LogAccess::set_client_req_url_path(char *buf, int len)
{
  //?? use m_client_req_unmapped_url_path_str for now..may need to enhance later..
  this->set_client_req_unmapped_url_path(buf, len);
}

/*-------------------------------------------------------------------------
  The marshalling routines ...

  We know that m_http_sm is a valid pointer (we assert so in the ctor), but
  we still need to check the other header pointers before using them in the
  routines.
  -------------------------------------------------------------------------*/

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_plugin_identity_id(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->plugin_id);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_plugin_identity_tag(char *buf)
{
  int         len = INK_MIN_ALIGN;
  const char *tag = m_http_sm->plugin_tag;

  if (!tag) {
    tag = "*";
  } else {
    len = LogAccess::strlen(tag);
  }

  if (buf) {
    marshal_str(buf, tag, len);
  }

  return len;
}

int
LogAccess::marshal_client_host_ip(char *buf)
{
  return marshal_ip(buf, &m_http_sm->t_state.client_info.src_addr.sa);
}

int
LogAccess::marshal_host_interface_ip(char *buf)
{
  return marshal_ip(buf, &m_http_sm->t_state.client_info.dst_addr.sa);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_cache_lookup_url_canon(char *buf)
{
  int len = INK_MIN_ALIGN;

  validate_lookup_url();
  if (m_cache_lookup_url_canon_str == INVALID_STR) {
    // If the lookup URL isn't populated, we'll fall back to the request URL.
    len = marshal_client_req_url_canon(buf);
  } else {
    len = round_strlen(m_cache_lookup_url_canon_len + 1); // +1 for eos
    if (buf) {
      marshal_mem(buf, m_cache_lookup_url_canon_str, m_cache_lookup_url_canon_len, len);
    }
  }

  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_client_sni_server_name(char *buf)
{
  // NOTE:  For this string_view, data() must always be nul-terminated, but the nul character must not be included in
  // the length.
  //
  std::string_view server_name = "";

  if (m_http_sm) {
    auto txn = m_http_sm->get_ua_txn();
    if (txn) {
      auto ssn = txn->get_proxy_ssn();
      if (ssn) {
        auto ssl = ssn->ssl();
        if (ssl) {
          auto server_name_str = ssl->client_sni_server_name();
          if (server_name_str) {
            server_name = server_name_str;
          }
        }
      }
    }
  }
  int len = round_strlen(server_name.length() + 1);
  if (buf) {
    marshal_str(buf, server_name.data(), len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_client_provided_cert(char *buf)
{
  int provided_cert = 0;
  if (m_http_sm) {
    auto txn = m_http_sm->get_ua_txn();
    if (txn) {
      auto ssn = txn->get_proxy_ssn();
      if (ssn) {
        auto ssl = ssn->ssl();
        if (ssl) {
          provided_cert = ssl->client_provided_certificate();
        }
      }
    }
  }
  if (buf) {
    marshal_int(buf, provided_cert);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_proxy_provided_cert(char *buf)
{
  int provided_cert = 0;
  if (m_http_sm) {
    provided_cert = m_http_sm->server_connection_provided_cert;
  }
  if (buf) {
    marshal_int(buf, provided_cert);
  }
  return INK_MIN_ALIGN;
}
/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_version_build_number(char *buf)
{
  auto &version = AppVersionInfo::get_version();
  int   len     = LogAccess::strlen(version.build_number());
  if (buf) {
    marshal_str(buf, version.build_number(), len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_version_string(char *buf)
{
  auto &version = AppVersionInfo::get_version();
  int   len     = LogAccess::strlen(version.version());
  if (buf) {
    marshal_str(buf, version.version(), len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_protocol_version(char *buf)
{
  const char *version_str = nullptr;
  int         len         = INK_MIN_ALIGN;

  if (m_http_sm) {
    ProxyProtocolVersion ver = m_http_sm->t_state.pp_info.version;
    switch (ver) {
    case ProxyProtocolVersion::V1:
      version_str = "V1";
      break;
    case ProxyProtocolVersion::V2:
      version_str = "V2";
      break;
    case ProxyProtocolVersion::UNDEFINED:
    default:
      version_str = "-";
      break;
    }
    len = LogAccess::strlen(version_str);
  }

  if (buf) {
    marshal_str(buf, version_str, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_proxy_protocol_src_ip(char *buf)
{
  sockaddr const *ip = nullptr;
  if (m_http_sm && m_http_sm->t_state.pp_info.version != ProxyProtocolVersion::UNDEFINED) {
    ip = &m_http_sm->t_state.pp_info.src_addr.sa;
  }
  return marshal_ip(buf, ip);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_proxy_protocol_dst_ip(char *buf)
{
  sockaddr const *ip = nullptr;
  if (m_http_sm && m_http_sm->t_state.pp_info.version != ProxyProtocolVersion::UNDEFINED) {
    ip = &m_http_sm->t_state.pp_info.dst_addr.sa;
  }
  return marshal_ip(buf, ip);
}

int
LogAccess::marshal_proxy_protocol_authority(char *buf)
{
  if (buf && m_http_sm) {
    if (auto authority = m_http_sm->t_state.pp_info.get_tlv(PP2_TYPE_AUTHORITY)) {
      int len = static_cast<int>(authority->size());
      marshal_str(buf, authority->data(), len);
      return len;
    }
  }
  return 0;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_client_host_port(char *buf)
{
  if (buf) {
    uint16_t port = m_http_sm->t_state.client_info.src_addr.host_order_port();
    marshal_int(buf, port);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  user authenticated to the proxy (RFC931)
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_auth_user_name(char *buf)
{
  char *str = nullptr;
  int   len = INK_MIN_ALIGN;

  // Jira TS-40:
  // NOTE: Authentication related code and modules were removed/disabled.
  //       Uncomment code path below when re-added/enabled.
  /*if (m_http_sm->t_state.auth_params.user_name) {
    str = m_http_sm->t_state.auth_params.user_name;
    len = LogAccess::strlen(str);
    } */
  if (buf) {
    marshal_str(buf, str, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  Private utility function to validate m_client_req_unmapped_url_canon_str &
  m_client_req_unmapped_url_canon_len fields.
  -------------------------------------------------------------------------*/

void
LogAccess::validate_unmapped_url()
{
  if (m_client_req_unmapped_url_canon_str == nullptr) {
    // prevent multiple validations
    m_client_req_unmapped_url_canon_str = INVALID_STR;

    if (m_http_sm->t_state.unmapped_url.valid()) {
      int   unmapped_url_len;
      char *unmapped_url = m_http_sm->t_state.unmapped_url.string_get_ref(&unmapped_url_len);

      if (unmapped_url && unmapped_url[0] != 0) {
        m_client_req_unmapped_url_canon_str =
          Encoding::escapify_url(&m_arena, unmapped_url, unmapped_url_len, &m_client_req_unmapped_url_canon_len);
      }
    }
  }
}

/*-------------------------------------------------------------------------
  Private utility function to validate m_client_req_unmapped_url_path_str &
  m_client_req_unmapped_url_path_len fields.
  -------------------------------------------------------------------------*/

void
LogAccess::validate_unmapped_url_path()
{
  if (m_client_req_unmapped_url_path_str == nullptr && m_client_req_unmapped_url_host_str == nullptr) {
    // Use unmapped canonical URL as default
    m_client_req_unmapped_url_path_str = m_client_req_unmapped_url_canon_str;
    m_client_req_unmapped_url_path_len = m_client_req_unmapped_url_canon_len;
    // In case the code below fails, we prevent it from being used.
    m_client_req_unmapped_url_host_str = INVALID_STR;

    if (m_client_req_unmapped_url_path_len >= 6) { // xxx:// - minimum schema size
      int   len;
      char *c =
        static_cast<char *>(memchr((void *)m_client_req_unmapped_url_path_str, ':', m_client_req_unmapped_url_path_len - 1));

      if (c && (len = static_cast<int>(c - m_client_req_unmapped_url_path_str)) <= 5) { // 5 - max schema size
        if (len + 2 <= m_client_req_unmapped_url_canon_len && c[1] == '/' && c[2] == '/') {
          len                                += 3; // Skip "://"
          m_client_req_unmapped_url_host_str  = &m_client_req_unmapped_url_canon_str[len];
          m_client_req_unmapped_url_host_len  = m_client_req_unmapped_url_path_len - len;
          // Attempt to find first '/' in the path
          if (m_client_req_unmapped_url_host_len > 0 &&
              (c = static_cast<char *>(
                 memchr((void *)m_client_req_unmapped_url_host_str, '/', m_client_req_unmapped_url_host_len))) != nullptr) {
            m_client_req_unmapped_url_host_len = static_cast<int>(c - m_client_req_unmapped_url_host_str);
            m_client_req_unmapped_url_path_str = &m_client_req_unmapped_url_host_str[m_client_req_unmapped_url_host_len];
            m_client_req_unmapped_url_path_len = m_client_req_unmapped_url_path_len - len - m_client_req_unmapped_url_host_len;
          }
        }
      }
    }
  }
}

/*-------------------------------------------------------------------------
  Private utility function to validate m_cache_lookup_url_canon_str &
  m_cache_lookup__url_canon_len fields.
  -------------------------------------------------------------------------*/
void
LogAccess::validate_lookup_url()
{
  if (m_cache_lookup_url_canon_str == nullptr) {
    // prevent multiple validations
    m_cache_lookup_url_canon_str = INVALID_STR;

    if (m_http_sm->t_state.cache_info.lookup_url_storage.valid()) {
      int   lookup_url_len;
      char *lookup_url = m_http_sm->t_state.cache_info.lookup_url_storage.string_get_ref(&lookup_url_len);

      if (lookup_url && lookup_url[0] != 0) {
        m_cache_lookup_url_canon_str = Encoding::escapify_url(&m_arena, lookup_url, lookup_url_len, &m_cache_lookup_url_canon_len);
      }
    }
  }
}

/*-------------------------------------------------------------------------
  This is the method, url, and version all rolled into one.  Use the
  respective marshalling routines to do the job.
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_text(char *buf)
{
  int len = marshal_client_req_http_method(nullptr) + marshal_client_req_url(nullptr) + marshal_client_req_http_version(nullptr);

  if (buf) {
    int offset  = 0;
    offset     += marshal_client_req_http_method(&buf[offset]);
    offset     += marshal_client_req_url(&buf[offset]);
    offset     += marshal_client_req_http_version(&buf[offset]);
    len         = offset;
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_timestamp_sec(char *buf)
{
  return marshal_milestone_fmt_sec(TS_MILESTONE_UA_BEGIN, buf);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_timestamp_ms(char *buf)
{
  return marshal_milestone_fmt_ms(TS_MILESTONE_UA_BEGIN, buf);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_http_method(char *buf)
{
  std::string_view str;
  int              plen = INK_MIN_ALIGN;

  if (m_client_request) {
    str = m_client_request->method_get();

    // calculate the padded length only if the actual length
    // is not zero. We don't want the padded length to be zero
    // because marshal_mem should write the DEFAULT_STR to the
    // buffer if str is nil, and we need room for this.
    //
    if (!str.empty()) {
      plen = round_strlen(static_cast<int>(str.length()) + 1); // +1 for trailing 0
    }
  }

  if (buf) {
    marshal_mem(buf, str.data(), static_cast<int>(str.length()), plen);
  }
  return plen;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_url(char *buf)
{
  int len = round_strlen(m_client_req_url_len + 1); // +1 for trailing 0

  if (buf) {
    marshal_mem(buf, m_client_req_url_str, m_client_req_url_len, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_url_canon(char *buf)
{
  int len = round_strlen(m_client_req_url_canon_len + 1);

  if (buf) {
    marshal_mem(buf, m_client_req_url_canon_str, m_client_req_url_canon_len, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_unmapped_url_canon(char *buf)
{
  int len = INK_MIN_ALIGN;

  validate_unmapped_url();
  if (m_client_req_unmapped_url_canon_str == INVALID_STR) {
    // If the unmapped URL isn't populated, we'll fall back to the original
    // client URL. This helps for example server intercepts to continue to
    // log the requests, even when there is no remap rule for it.
    len = marshal_client_req_url_canon(buf);
  } else {
    len = round_strlen(m_client_req_unmapped_url_canon_len + 1); // +1 for eos
    if (buf) {
      marshal_mem(buf, m_client_req_unmapped_url_canon_str, m_client_req_unmapped_url_canon_len, len);
    }
  }

  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_unmapped_url_path(char *buf)
{
  int len = INK_MIN_ALIGN;

  validate_unmapped_url();
  validate_unmapped_url_path();

  if (m_client_req_unmapped_url_path_str == INVALID_STR) {
    len = marshal_client_req_url_path(buf);
  } else {
    len = round_strlen(m_client_req_unmapped_url_path_len + 1); // +1 for eos
    if (buf) {
      marshal_mem(buf, m_client_req_unmapped_url_path_str, m_client_req_unmapped_url_path_len, len);
    }
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_unmapped_url_host(char *buf)
{
  validate_unmapped_url();
  validate_unmapped_url_path();

  int len = round_strlen(m_client_req_unmapped_url_host_len + 1); // +1 for eos
  if (buf) {
    marshal_mem(buf, m_client_req_unmapped_url_host_str, m_client_req_unmapped_url_host_len, len);
  }

  return len;
}

int
LogAccess::marshal_client_req_url_path(char *buf)
{
  int len = round_strlen(m_client_req_url_path_len + 1);
  if (buf) {
    marshal_mem(buf, m_client_req_url_path_str, m_client_req_url_path_len, len);
  }
  return len;
}

int
LogAccess::marshal_client_req_url_scheme(char *buf)
{
  int         scheme = m_http_sm->t_state.orig_scheme;
  const char *str    = nullptr;
  int         alen;
  int         plen = INK_MIN_ALIGN;

  // If the transaction aborts very early, the scheme may not be set, or so ASAN reports.
  if (scheme >= 0) {
    str  = hdrtoken_index_to_wks(scheme);
    alen = hdrtoken_index_to_length(scheme);
  } else {
    str  = "UNKNOWN";
    alen = strlen(str);
  }

  // calculate the padded length only if the actual length
  // is not zero. We don't want the padded length to be zero
  // because marshal_mem should write the DEFAULT_STR to the
  // buffer if str is nil, and we need room for this.
  //
  if (alen) {
    plen = round_strlen(alen + 1); // +1 for trailing 0
  }

  if (buf) {
    marshal_mem(buf, str, alen, plen);
  }

  return plen;
}

/*-------------------------------------------------------------------------
  For this one we're going to marshal two INTs, one the first representing
  the major number and the second representing the minor.
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_http_version(char *buf)
{
  if (buf) {
    if (m_client_request) {
      HTTPVersion versionObject = m_client_request->version_get();
      marshal_int(buf, versionObject.get_major());
      marshal_int((buf + INK_MIN_ALIGN), versionObject.get_minor());
    } else {
      marshal_int(buf, 0);
      marshal_int((buf + INK_MIN_ALIGN), 0);
    }
  }
  return (2 * INK_MIN_ALIGN);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_protocol_version(char *buf)
{
  const char *protocol_str = m_http_sm->get_user_agent().get_client_protocol();
  int         len          = LogAccess::strlen(protocol_str);

  // Set major & minor versions when protocol_str is not "http/2".
  if (::strlen(protocol_str) == 4 && strncmp("http", protocol_str, 4) == 0) {
    if (m_client_request) {
      HTTPVersion versionObject = m_client_request->version_get();
      if (versionObject == HTTP_1_1) {
        protocol_str = "http/1.1";
      } else if (versionObject == HTTP_1_0) {
        protocol_str = "http/1.0";
      } // else invalid http version
    } else {
      protocol_str = "*";
    }

    len = LogAccess::strlen(protocol_str);
  }

  if (buf) {
    marshal_str(buf, protocol_str, len);
  }

  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_req_protocol_version(char *buf)
{
  const char *protocol_str = m_http_sm->server_protocol;
  int         len          = LogAccess::strlen(protocol_str);

  // Set major & minor versions when protocol_str is not "http/2".
  if (::strlen(protocol_str) == 4 && strncmp("http", protocol_str, 4) == 0) {
    if (m_proxy_request) {
      HTTPVersion versionObject = m_proxy_request->version_get();
      if (versionObject == HTTP_1_1) {
        protocol_str = "http/1.1";
      } else if (versionObject == HTTP_1_0) {
        protocol_str = "http/1.0";
      } // else invalid http version
    } else {
      protocol_str = "*";
    }

    len = LogAccess::strlen(protocol_str);
  }

  if (buf) {
    marshal_str(buf, protocol_str, len);
  }

  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_header_len(char *buf)
{
  if (buf) {
    int64_t len = 0;
    if (m_client_request) {
      len = m_client_request->length_get();
    }
    marshal_int(buf, len);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_content_len(char *buf)
{
  if (buf) {
    int64_t len = 0;
    if (m_client_request) {
      len = m_http_sm->client_request_body_bytes;
    }
    marshal_int(buf, len);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_client_req_squid_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_client_request) {
      val = m_client_request->length_get() + m_http_sm->client_request_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_tcp_reused(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->get_user_agent().get_client_tcp_reused() ? 1 : 0);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_client_req_is_ssl(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->get_user_agent().get_client_connection_is_ssl() ? 1 : 0);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_client_req_ssl_reused(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->get_user_agent().get_client_ssl_reused() ? 1 : 0);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_client_req_is_internal(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->is_internal ? 1 : 0);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_client_req_mptcp_state(char *buf)
{
  if (buf) {
    int val = -1;

    if (m_http_sm->mptcp_state.has_value()) {
      val = m_http_sm->mptcp_state.value() ? 1 : 0;
    } else {
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_finish_status_code(char *buf)
{
  if (buf) {
    int                        code           = LOG_FINISH_FIN;
    HttpTransact::AbortState_t cl_abort_state = m_http_sm->t_state.client_info.abort;
    if (cl_abort_state == HttpTransact::ABORTED) {
      // Check to see if the abort is due to a timeout
      if (m_http_sm->t_state.client_info.state == HttpTransact::ACTIVE_TIMEOUT ||
          m_http_sm->t_state.client_info.state == HttpTransact::INACTIVE_TIMEOUT) {
        code = LOG_FINISH_TIMEOUT;
      } else {
        code = LOG_FINISH_INTR;
      }
    }
    marshal_int(buf, code);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_id(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->sm_id);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_req_uuid(char *buf)
{
  char        str[TS_CRUUID_STRING_LEN + 1];
  const char *uuid = Machine::instance()->process_uuid.getString();
  int         len  = snprintf(str, sizeof(str), "%s-%" PRId64 "", uuid, m_http_sm->sm_id);

  ink_assert(len <= TS_CRUUID_STRING_LEN);
  len = round_strlen(len + 1);

  if (buf) {
    marshal_str(buf, str, len); // This will pad the remaining bytes properly ...
  }

  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

// 1 ('S'/'T' flag) + 8 (Error Code) + 1 ('\0')
static constexpr size_t MAX_PROXY_ERROR_CODE_SIZE = 10;

int
LogAccess::marshal_client_rx_error_code(char *buf)
{
  char error_code[MAX_PROXY_ERROR_CODE_SIZE] = {0};
  m_http_sm->t_state.client_info.rx_error_code.str(error_code, sizeof(error_code));
  int round_len = LogAccess::strlen(error_code);

  if (buf) {
    marshal_str(buf, error_code, round_len);
  }

  return round_len;
}

int
LogAccess::marshal_client_tx_error_code(char *buf)
{
  char error_code[MAX_PROXY_ERROR_CODE_SIZE] = {0};
  m_http_sm->t_state.client_info.tx_error_code.str(error_code, sizeof(error_code));
  int round_len = LogAccess::strlen(error_code);

  if (buf) {
    marshal_str(buf, error_code, round_len);
  }

  return round_len;
}

/*-------------------------------------------------------------------------
-------------------------------------------------------------------------*/
int
LogAccess::marshal_client_security_protocol(char *buf)
{
  const char *proto     = m_http_sm->get_user_agent().get_client_sec_protocol();
  int         round_len = LogAccess::strlen(proto);

  if (buf) {
    marshal_str(buf, proto, round_len);
  }

  return round_len;
}

int
LogAccess::marshal_client_security_cipher_suite(char *buf)
{
  const char *cipher    = m_http_sm->get_user_agent().get_client_cipher_suite();
  int         round_len = LogAccess::strlen(cipher);

  if (buf) {
    marshal_str(buf, cipher, round_len);
  }

  return round_len;
}

int
LogAccess::marshal_client_security_curve(char *buf)
{
  const char *curve     = m_http_sm->get_user_agent().get_client_curve();
  int         round_len = LogAccess::strlen(curve);

  if (buf) {
    marshal_str(buf, curve, round_len);
  }

  return round_len;
}

int
LogAccess::marshal_client_security_group(char *buf)
{
  const char *group     = m_http_sm->get_user_agent().get_client_security_group();
  int         round_len = LogAccess::strlen(group);

  if (buf) {
    marshal_str(buf, group, round_len);
  }

  return round_len;
}

int
LogAccess::marshal_client_security_alpn(char *buf)
{
  const char *alpn = "-";
  if (const int alpn_id = m_http_sm->get_user_agent().get_client_alpn_id(); alpn_id != SessionProtocolNameRegistry::INVALID) {
    swoc::TextView client_sec_alpn = globalSessionProtocolNameRegistry.nameFor(alpn_id);
    alpn                           = client_sec_alpn.data();
  }

  int round_len = LogAccess::strlen(alpn);

  if (buf) {
    marshal_str(buf, alpn, round_len);
  }

  return round_len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_content_type(char *buf)
{
  int len = round_strlen(m_proxy_resp_content_type_len + 1);
  if (buf) {
    marshal_mem(buf, m_proxy_resp_content_type_str, m_proxy_resp_content_type_len, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_reason_phrase(char *buf)
{
  int len = round_strlen(m_proxy_resp_reason_phrase_len + 1);
  if (buf) {
    marshal_mem(buf, m_proxy_resp_reason_phrase_str, m_proxy_resp_reason_phrase_len, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  Squid returns the content-length + header length as the total length.
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_squid_len(char *buf)
{
  if (buf) {
    int64_t val = m_http_sm->client_response_hdr_bytes + m_http_sm->client_response_body_bytes;
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_content_len(char *buf)
{
  if (buf) {
    int64_t val = m_http_sm->client_response_body_bytes;
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_status_code(char *buf)
{
  if (buf) {
    HTTPStatus status;
    if (m_proxy_response && m_client_request) {
      if (m_client_request->version_get() >= HTTPVersion(1, 0)) {
        status = m_proxy_response->status_get();
      }
      // INKqa10788
      // For bad/incomplete request, the request version may be 0.9.
      // However, we can still log the status code if there is one.
      else if (m_proxy_response->valid()) {
        status = m_proxy_response->status_get();
      } else {
        status = HTTPStatus::OK;
      }
    } else {
      status = HTTPStatus::NONE;
    }
    marshal_int(buf, static_cast<int64_t>(status));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_resp_header_len(char *buf)
{
  if (buf) {
    int64_t val = m_http_sm->client_response_hdr_bytes;
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_proxy_finish_status_code(char *buf)
{
  /* FIXME: Should there be no server transaction code if
     the result comes out of the cache.  Right now we default
     to FIN */
  if (buf) {
    int code = LOG_FINISH_FIN;
    if (m_http_sm->t_state.current.server) {
      switch (m_http_sm->t_state.current.server->state) {
      case HttpTransact::ACTIVE_TIMEOUT:
      case HttpTransact::INACTIVE_TIMEOUT:
        code = LOG_FINISH_TIMEOUT;
        break;
      case HttpTransact::CONNECTION_ERROR:
        code = LOG_FINISH_INTR;
        break;
      default:
        if (m_http_sm->t_state.current.server->abort == HttpTransact::ABORTED) {
          code = LOG_FINISH_INTR;
        }
        break;
      }
    }

    marshal_int(buf, code);
  }

  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
-------------------------------------------------------------------------*/
int
LogAccess::marshal_proxy_host_port(char *buf)
{
  if (buf) {
    uint16_t port = m_http_sm->t_state.request_data.incoming_port;
    marshal_int(buf, port);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_result_code(char *buf)
{
  if (buf) {
    SquidLogCode code = m_http_sm->t_state.squid_codes.log_code;
    marshal_int(buf, static_cast<int64_t>(code));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_result_subcode(char *buf)
{
  if (buf) {
    SquidSubcode code = m_http_sm->t_state.squid_codes.subcode;
    marshal_int(buf, static_cast<int64_t>(code));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_hit_miss(char *buf)
{
  if (buf) {
    SquidHitMissCode code = m_http_sm->t_state.squid_codes.hit_miss_code;
    marshal_int(buf, static_cast<int64_t>(code));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_req_header_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_proxy_request) {
      val = m_proxy_request->length_get();
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_req_content_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_proxy_request) {
      val = m_http_sm->server_request_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_proxy_req_squid_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_proxy_request) {
      val = m_proxy_request->length_get() + m_http_sm->server_request_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

// TODO: Change marshalling code to support both IPv4 and IPv6 addresses.
int
LogAccess::marshal_proxy_req_server_ip(char *buf)
{
  return marshal_ip(buf, m_http_sm->t_state.current.server != nullptr ? &m_http_sm->t_state.current.server->src_addr.sa : nullptr);
}

int
LogAccess::marshal_proxy_req_server_port(char *buf)
{
  if (buf) {
    uint16_t port =
      m_http_sm->t_state.current.server != nullptr ? m_http_sm->t_state.current.server->src_addr.host_order_port() : 0;
    marshal_int(buf, port);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_next_hop_ip(char *buf)
{
  return marshal_ip(buf, m_http_sm->t_state.current.server != nullptr ? &m_http_sm->t_state.current.server->dst_addr.sa : nullptr);
}

int
LogAccess::marshal_next_hop_port(char *buf)
{
  if (buf) {
    uint16_t port =
      m_http_sm->t_state.current.server != nullptr ? m_http_sm->t_state.current.server->dst_addr.host_order_port() : 0;
    marshal_int(buf, port);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_req_is_ssl(char *buf)
{
  if (buf) {
    int64_t is_ssl;
    is_ssl = m_http_sm->server_connection_is_ssl;
    marshal_int(buf, is_ssl);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_proxy_req_ssl_reused(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->server_ssl_reused ? 1 : 0);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_proxy_hierarchy_route(char *buf)
{
  if (buf) {
    SquidHierarchyCode code = m_http_sm->t_state.squid_codes.hier_code;
    marshal_int(buf, static_cast<int64_t>(code));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

// TODO: Change marshalling code to support both IPv4 and IPv6 addresses.
int
LogAccess::marshal_server_host_ip(char *buf)
{
  sockaddr const *ip = nullptr;
  ip                 = &m_http_sm->t_state.server_info.dst_addr.sa;
  if (!ats_is_ip(ip)) {
    if (m_http_sm->t_state.current.server) {
      ip = &m_http_sm->t_state.current.server->dst_addr.sa;
      if (!ats_is_ip(ip)) {
        ip = nullptr;
      }
    } else {
      ip = nullptr;
    }
  }
  return marshal_ip(buf, ip);
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_host_name(char *buf)
{
  char *str = nullptr;
  int   len = INK_MIN_ALIGN;

  if (m_http_sm->t_state.current.server) {
    str = m_http_sm->t_state.current.server->name;
    len = LogAccess::strlen(str);
  }

  if (buf) {
    marshal_str(buf, str, len);
  }
  return len;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_resp_status_code(char *buf)
{
  if (buf) {
    HTTPStatus status;
    if (m_server_response) {
      status = m_server_response->status_get();
    } else {
      status = HTTPStatus::NONE;
    }
    marshal_int(buf, static_cast<int64_t>(status));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_resp_content_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_server_response) {
      val = m_http_sm->server_response_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_resp_header_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_server_response) {
      val = m_server_response->length_get();
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_server_resp_squid_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_server_response) {
      val = m_server_response->length_get() + m_http_sm->server_response_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_server_resp_http_version(char *buf)
{
  if (buf) {
    int64_t major = 0;
    int64_t minor = 0;
    if (m_server_response) {
      major = m_server_response->version_get().get_major();
      minor = m_server_response->version_get().get_minor();
    }
    marshal_int(buf, major);
    marshal_int((buf + INK_MIN_ALIGN), minor);
  }
  return (2 * INK_MIN_ALIGN);
}

/*-------------------------------------------------------------------------
-------------------------------------------------------------------------*/
int
LogAccess::marshal_server_resp_time_ms(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->milestones.difference_msec(TS_MILESTONE_SERVER_CONNECT, TS_MILESTONE_SERVER_CLOSE));
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_server_resp_time_s(char *buf)
{
  if (buf) {
    marshal_int(buf,
                static_cast<int64_t>(m_http_sm->milestones.difference_sec(TS_MILESTONE_SERVER_CONNECT, TS_MILESTONE_SERVER_CLOSE)));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_transact_count(char *buf)
{
  if (buf) {
    int64_t count;
    count = m_http_sm->server_transact_count;
    marshal_int(buf, count);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_simple_retry_count(char *buf)
{
  if (buf) {
    const int64_t attempts = m_http_sm->t_state.current.simple_retry_attempts;
    marshal_int(buf, attempts);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_unavailable_retry_count(char *buf)
{
  if (buf) {
    const int64_t attempts = m_http_sm->t_state.current.unavailable_server_retry_attempts;
    marshal_int(buf, attempts);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_server_connect_attempts(char *buf)
{
  if (buf) {
    int64_t attempts = m_http_sm->t_state.current.retry_attempts.saved();
    marshal_int(buf, attempts);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_resp_status_code(char *buf)
{
  if (buf) {
    HTTPStatus status;
    if (m_cache_response) {
      status = m_cache_response->status_get();
    } else {
      status = HTTPStatus::NONE;
    }
    marshal_int(buf, static_cast<int64_t>(status));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_resp_content_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_cache_response) {
      val = m_http_sm->cache_response_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_cache_resp_squid_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_cache_response) {
      val = m_cache_response->length_get() + m_http_sm->cache_response_body_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_resp_header_len(char *buf)
{
  if (buf) {
    int64_t val = 0;
    if (m_cache_response) {
      val = m_http_sm->cache_response_hdr_bytes;
    }
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_cache_resp_http_version(char *buf)
{
  if (buf) {
    int64_t major = 0;
    int64_t minor = 0;
    if (m_cache_response) {
      major = m_cache_response->version_get().get_major();
      minor = m_cache_response->version_get().get_minor();
    }
    marshal_int(buf, major);
    marshal_int((buf + INK_MIN_ALIGN), minor);
  }
  return (2 * INK_MIN_ALIGN);
}

int
LogAccess::marshal_client_retry_after_time(char *buf)
{
  if (buf) {
    int64_t crat = m_http_sm->t_state.congestion_control_crat;
    marshal_int(buf, crat);
  }
  return INK_MIN_ALIGN;
}

static LogCacheWriteCodeType
convert_cache_write_code(HttpTransact::CacheWriteStatus_t t)
{
  LogCacheWriteCodeType code;
  switch (t) {
  case HttpTransact::CacheWriteStatus_t::NO_WRITE:
    code = LOG_CACHE_WRITE_NONE;
    break;
  case HttpTransact::CacheWriteStatus_t::LOCK_MISS:
    code = LOG_CACHE_WRITE_LOCK_MISSED;
    break;
  case HttpTransact::CacheWriteStatus_t::IN_PROGRESS:
    // Hack - the HttpSM doesn't record
    //   cache write aborts currently so
    //   if it's not complete declare it
    //   aborted
    code = LOG_CACHE_WRITE_LOCK_ABORTED;
    break;
  case HttpTransact::CacheWriteStatus_t::ERROR:
    code = LOG_CACHE_WRITE_ERROR;
    break;
  case HttpTransact::CacheWriteStatus_t::COMPLETE:
    code = LOG_CACHE_WRITE_COMPLETE;
    break;
  default:
    ink_assert(!"bad cache write code");
    code = LOG_CACHE_WRITE_NONE;
    break;
  }

  return code;
}

int
LogAccess::marshal_cache_write_code(char *buf)
{
  if (buf) {
    int code = convert_cache_write_code(m_http_sm->t_state.cache_info.write_status);
    marshal_int(buf, code);
  }

  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_cache_write_transform_code(char *buf)
{
  if (buf) {
    int code = convert_cache_write_code(m_http_sm->t_state.cache_info.transform_write_status);
    marshal_int(buf, code);
  }

  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_transfer_time_ms(char *buf)
{
  if (buf) {
    marshal_int(buf, m_http_sm->milestones.difference_msec(TS_MILESTONE_SM_START, TS_MILESTONE_SM_FINISH));
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_transfer_time_s(char *buf)
{
  if (buf) {
    marshal_int(buf, static_cast<int64_t>(m_http_sm->milestones.difference_sec(TS_MILESTONE_SM_START, TS_MILESTONE_SM_FINISH)));
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  Figure out the size of the object *on origin*. This is somewhat tricky
  since there are many variations on how this can be calculated.
  -------------------------------------------------------------------------*/
int
LogAccess::marshal_file_size(char *buf)
{
  if (buf) {
    MIMEField *fld;
    HTTPHdr   *hdr = m_server_response ? m_server_response : m_cache_response;

    if (hdr && (fld = hdr->field_find(static_cast<std::string_view>(MIME_FIELD_CONTENT_RANGE)))) {
      auto  value{fld->value_get()};
      int   len = value.length();
      char *str = const_cast<char *>(value.data());
      char *pos = static_cast<char *>(memchr(str, '/', len)); // Find the /

      // If the size is not /* (which means unknown) use it as the file_size.
      if (pos && !memchr(pos + 1, '*', len - (pos + 1 - str))) {
        marshal_int(buf, ink_atoi64(pos + 1, len - (pos + 1 - str)));
      }
    } else {
      // This is semi-broken when we serveq zero length objects. See TS-2213
      if (m_http_sm->server_response_body_bytes > 0) {
        marshal_int(buf, m_http_sm->server_response_body_bytes);
      } else if (m_http_sm->cache_response_body_bytes > 0) {
        marshal_int(buf, m_http_sm->cache_response_body_bytes);
      }
    }
  }
  // Else, we don't set the value at all (so, -)

  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_http_connection_id(char *buf)
{
  if (buf) {
    int64_t id = 0;
    if (m_http_sm) {
      id = m_http_sm->client_connection_id();
    }
    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_http_transaction_id(char *buf)
{
  if (buf) {
    int64_t id = 0;
    if (m_http_sm) {
      id = m_http_sm->client_transaction_id();
    }
    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_http_transaction_priority_weight(char *buf)
{
  if (buf) {
    int64_t id = 0;
    if (m_http_sm) {
      id = m_http_sm->client_transaction_priority_weight();
    }
    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_client_http_transaction_priority_dependence(char *buf)
{
  if (buf) {
    int64_t id = 0;
    if (m_http_sm) {
      id = m_http_sm->client_transaction_priority_dependence();
    }
    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_read_retries(char *buf)
{
  if (buf) {
    int64_t id = 0;
    if (m_http_sm) {
      id = m_http_sm->get_cache_sm().get_open_read_tries();
    }
    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}

/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_cache_write_retries(char *buf)
{
  if (buf) {
    int64_t id = 0;
    if (m_http_sm) {
      id = m_http_sm->get_cache_sm().get_open_write_tries();
    }
    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_cache_collapsed_connection_success(char *buf)
{
  if (buf) {
    int64_t id = 0; // default - no collapse attempt
    if (m_http_sm) {
      SquidLogCode code = m_http_sm->t_state.squid_codes.log_code;

      // We attempted an open write, but ended up with some sort of HIT which means we must have gone back to the read state
      if ((m_http_sm->get_cache_sm().get_open_write_tries() > (0)) &&
          ((code == SquidLogCode::TCP_HIT) || (code == SquidLogCode::TCP_MEM_HIT) || (code == SquidLogCode::TCP_DISK_HIT) ||
           (code == SquidLogCode::TCP_CF_HIT))) {
        // Attempted collapsed connection and got a hit, success
        id = 1;
      } else if (m_http_sm->get_cache_sm().get_open_write_tries() > (m_http_sm->t_state.txn_conf->max_cache_open_write_retries)) {
        // Attempted collapsed connection with no hit, failure, we can also get +2 retries in a failure state
        id = -1;
      }
    }

    marshal_int(buf, id);
  }
  return INK_MIN_ALIGN;
}
/*-------------------------------------------------------------------------
  -------------------------------------------------------------------------*/

int
LogAccess::marshal_http_header_field(LogField::Container container, char *field, char *buf)
{
  char    *str         = nullptr;
  int      padded_len  = INK_MIN_ALIGN;
  int      actual_len  = 0;
  bool     valid_field = false;
  HTTPHdr *header;

  switch (container) {
  case LogField::CQH:
    header = m_client_request;
    break;

  case LogField::PSH:
    header = m_proxy_response;
    break;

  case LogField::PQH:
    header = m_proxy_request;
    break;

  case LogField::SSH:
    header = m_server_response;
    break;

  case LogField::CSSH:
    header = m_cache_response;
    break;

  default:
    header = nullptr;
    break;
  }

  if (header) {
    MIMEField *fld = header->field_find(std::string_view{field});
    if (fld) {
      valid_field = true;

      // Loop over dups, marshalling each one into the buffer and
      // summing up their length
      //
      int running_len = 0;
      while (fld) {
        auto value{fld->value_get()};
        actual_len = value.length();
        str        = const_cast<char *>(value.data());
        if (buf) {
          memcpy(buf, str, actual_len);
          buf += actual_len;
        }
        running_len += actual_len;
        fld          = fld->m_next_dup;

        // Dups need to be comma separated.  So if there's another
        // dup, then add a comma and a space ...
        //
        if (fld != nullptr) {
          if (buf) {
            memcpy(buf, ", ", 2);
            buf += 2;
          }
          running_len += 2;
        }
      }

      // Done with all dups.  Ensure that the string is terminated
      // and that the running_len is padded.
      //
      if (buf) {
        *buf = '\0';
        buf++;
      }
      running_len += 1;
      padded_len   = round_strlen(running_len);

// Note: marshal_string fills the padding to
//  prevent purify UMRs so we do it here too
//  since we always pass the unpadded length on
//  our calls to marshal string
#ifdef DEBUG
      if (buf) {
        int pad_len = padded_len - running_len;
        for (int i = 0; i < pad_len; i++) {
          *buf = '$';
          buf++;
        }
      }
#endif
    }
  }

  if (valid_field == false) {
    padded_len = INK_MIN_ALIGN;
    if (buf) {
      marshal_str(buf, nullptr, padded_len);
    }
  }

  return (padded_len);
}

int
LogAccess::marshal_http_header_field_escapify(LogField::Container container, char *field, char *buf)
{
  char    *str = nullptr, *new_str = nullptr;
  int      padded_len = INK_MIN_ALIGN;
  int      actual_len = 0, new_len = 0;
  bool     valid_field = false;
  HTTPHdr *header;

  switch (container) {
  case LogField::ECQH:
    header = m_client_request;
    break;

  case LogField::EPSH:
    header = m_proxy_response;
    break;

  case LogField::EPQH:
    header = m_proxy_request;
    break;

  case LogField::ESSH:
    header = m_server_response;
    break;

  case LogField::ECSSH:
    header = m_cache_response;
    break;

  default:
    header = nullptr;
    break;
  }

  if (header) {
    MIMEField *fld = header->field_find(std::string_view{field});
    if (fld) {
      valid_field = true;

      // Loop over dups, marshalling each one into the buffer and
      // summing up their length
      //
      int running_len = 0;
      while (fld) {
        auto value{fld->value_get()};
        actual_len = value.length();
        str        = const_cast<char *>(value.data());
        new_str    = Encoding::escapify_url(&m_arena, str, actual_len, &new_len);
        if (buf) {
          memcpy(buf, new_str, new_len);
          buf += new_len;
        }
        running_len += new_len;
        fld          = fld->m_next_dup;

        // Dups need to be comma separated.  So if there's another
        // dup, then add a comma and an escapified space ...
        constexpr const char SEP[]   = ",%20";
        constexpr size_t     SEP_LEN = sizeof(SEP) - 1;
        if (fld != nullptr) {
          if (buf) {
            memcpy(buf, SEP, SEP_LEN);
            buf += SEP_LEN;
          }
          running_len += SEP_LEN;
        }
      }

      // Done with all dups.  Ensure that the string is terminated
      // and that the running_len is padded.
      //
      if (buf) {
        *buf = '\0';
        buf++;
      }
      running_len += 1;
      padded_len   = round_strlen(running_len);

// Note: marshal_string fills the padding to
//  prevent purify UMRs so we do it here too
//  since we always pass the unpadded length on
//  our calls to marshal string
#ifdef DEBUG
      if (buf) {
        int pad_len = padded_len - running_len;
        for (int i = 0; i < pad_len; i++) {
          *buf = '$';
          buf++;
        }
      }
#endif
    }
  }

  if (valid_field == false) {
    padded_len = INK_MIN_ALIGN;
    if (buf) {
      marshal_str(buf, nullptr, padded_len);
    }
  }

  return (padded_len);
}

int
LogAccess::marshal_milestone(TSMilestonesType ms, char *buf)
{
  if (buf) {
    int64_t val = ink_hrtime_to_msec(m_http_sm->milestones[ms]);
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_milestone_fmt_sec(TSMilestonesType type, char *buf)
{
  if (buf) {
    ink_hrtime tsec = ink_hrtime_to_sec(m_http_sm->milestones[type]);
    marshal_int(buf, tsec);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_milestone_fmt_ms(TSMilestonesType type, char *buf)
{
  if (buf) {
    ink_hrtime tmsec = ink_hrtime_to_msec(m_http_sm->milestones[type]);
    marshal_int(buf, tmsec);
  }
  return INK_MIN_ALIGN;
}

int
LogAccess::marshal_milestone_diff(TSMilestonesType ms1, TSMilestonesType ms2, char *buf)
{
  if (buf) {
    int64_t val = m_http_sm->milestones.difference_msec(ms2, ms1);
    marshal_int(buf, val);
  }
  return INK_MIN_ALIGN;
}

void
LogAccess::set_http_header_field(LogField::Container container, char *field, char *buf, int len)
{
  HTTPHdr *header;

  switch (container) {
  case LogField::CQH:
  case LogField::ECQH:
    header = m_client_request;
    break;

  case LogField::PSH:
  case LogField::EPSH:
    header = m_proxy_response;
    break;

  case LogField::PQH:
  case LogField::EPQH:
    header = m_proxy_request;
    break;

  case LogField::SSH:
  case LogField::ESSH:
    header = m_server_response;
    break;

  case LogField::CSSH:
  case LogField::ECSSH:
    header = m_cache_response;
    break;

  default:
    header = nullptr;
    break;
  }

  if (header && buf) {
    MIMEField *fld = header->field_find(std::string_view{field});
    if (fld) {
      // Loop over dups, update each of them
      //
      while (fld) {
        // make sure to reuse header heaps as otherwise
        // coalesce logic in header heap may free up
        // memory pointed to by cquuc or other log fields
        header->field_value_set(fld, std::string_view{buf, static_cast<std::string_view::size_type>(len)}, true);
        fld = fld->m_next_dup;
      }
    }
  }
}
