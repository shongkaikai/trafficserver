/** @file

  A brief file description

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

#define LARGE_FILE 10 * 1024 * 1024
#define SMALL_FILE 10 * 1024

#include "main.h"
#include "../P_CacheInternal.h"

using namespace std::literals;

int  cache_vols           = 1;
bool reuse_existing_cache = false;

// delete dir
Dir dir = {};

class CacheAltReadAgain2 : public CacheTestHandler
{
public:
  CacheAltReadAgain2(size_t size, const char *url) : CacheTestHandler()
  {
    auto rt = new CacheReadTest(size, this, url);

    rt->mutex = this->mutex;

    rt->info.destroy();

    rt->info.create();
    build_hdrs(rt->info, url, "application/x-javascript");

    this->_rt = rt;

    SET_HANDLER(&CacheAltReadAgain2::start_test);
  }

  int
  start_test(int event, void * /* e ATS_UNUSED */)
  {
    REQUIRE(event == EVENT_IMMEDIATE);
    this_ethread()->schedule_imm(this->_rt);
    return 0;
  }

  void
  handle_cache_event(int event, CacheTestBase *base) override
  {
    switch (event) {
    case CACHE_EVENT_OPEN_READ:
      base->do_io_read();
      validate_content_type(base);
      break;
    case VC_EVENT_READ_READY:
      base->reenable();
      break;
    case VC_EVENT_READ_COMPLETE:
      base->close();
      delete this;
      break;
    default:
      REQUIRE(false);
      break;
    }
  }

  void
  validate_content_type(CacheTestBase *base)
  {
    auto rt = dynamic_cast<CacheReadTest *>(base);
    REQUIRE(rt);
    MIMEField *field = rt->read_http_info->m_alt->m_response_hdr.field_find(static_cast<std::string_view>(MIME_FIELD_CONTENT_TYPE));
    REQUIRE(field);
    auto value{field->value_get()};
    REQUIRE(value == "application/x-javascript"sv);
  }
};

class CacheAltReadAgain : public CacheTestHandler
{
public:
  CacheAltReadAgain(size_t size, const char *url) : CacheTestHandler()
  {
    this->_rt        = new CacheReadTest(size, this, url);
    this->_rt->mutex = this->mutex;

    SET_HANDLER(&CacheAltReadAgain::start_test);
  }

  int
  start_test(int event, void * /* e ATS_UNUSED */)
  {
    REQUIRE(event == EVENT_IMMEDIATE);
    this_ethread()->schedule_imm(this->_rt);
    return 0;
  }

  void
  handle_cache_event(int event, CacheTestBase * /* base ATS_UNUSED */) override
  {
    switch (event) {
    case CACHE_EVENT_OPEN_READ_FAILED:
      this->_rt->close();
      delete this;
      break;
    default:
      REQUIRE(false);
      break;
    }
  }

  void
  validate_content_type(CacheTestBase *base)
  {
    auto rt = dynamic_cast<CacheReadTest *>(base);
    REQUIRE(rt);
    MIMEField *field = rt->read_http_info->m_alt->m_response_hdr.field_find(static_cast<std::string_view>(MIME_FIELD_CONTENT_TYPE));
    REQUIRE(field);
    auto value{field->value_get()};
    REQUIRE(value == "text/html;charset=utf-8"sv);
  }
};

class test_Alternate_S_to_L_remove_S : public CacheTestHandler
{
public:
  test_Alternate_S_to_L_remove_S(size_t size, const char *url) : CacheTestHandler()
  {
    auto rt = new CacheReadTest(size, this, url);
    auto wt = new CacheWriteTest(size, this, url);

    rt->info.destroy();
    wt->info.destroy();

    rt->info.create();
    wt->info.create();

    build_hdrs(rt->info, url, "application/x-javascript");
    build_hdrs(wt->info, url, "application/x-javascript");

    this->_rt = rt;
    this->_wt = wt;

    this->_rt->mutex = this->mutex;
    this->_wt->mutex = this->mutex;

    SET_HANDLER(&test_Alternate_S_to_L_remove_S::start_test);
  }

  int
  start_test(int event, void * /* e ATS_UNUSED */)
  {
    REQUIRE(event == EVENT_IMMEDIATE);
    this_ethread()->schedule_imm(this->_wt);
    return 0;
  }

  void
  handle_cache_event(int event, CacheTestBase *base) override
  {
    switch (event) {
    case CACHE_EVENT_OPEN_WRITE:
      base->do_io_write();
      break;
    case VC_EVENT_WRITE_READY:
      base->reenable();
      break;
    case VC_EVENT_WRITE_COMPLETE:
      this->_wt->close();
      this->_wt = nullptr;
      // to make sure writer successfully write the final doc done. we need to schedule
      // to wait for some while. This time should be large than cache_config_mutex_retry_delay
      this_ethread()->schedule_in(this->_rt, 4 * HRTIME_SECONDS(cache_config_mutex_retry_delay));
      break;
    case CACHE_EVENT_OPEN_READ:
      base->do_io_read();
      validate_content_type(base);
      break;
    case VC_EVENT_READ_READY:
      base->reenable();
      break;
    case VC_EVENT_READ_COMPLETE:
      delete_earliest_dir(base->vc);
      base->close();
      delete this;
      break;
    default:
      REQUIRE(false);
      break;
    }
  }

  void
  validate_content_type(CacheTestBase *base)
  {
    auto rt = dynamic_cast<CacheReadTest *>(base);
    REQUIRE(rt);
    MIMEField *field = rt->read_http_info->m_alt->m_response_hdr.field_find(static_cast<std::string_view>(MIME_FIELD_CONTENT_TYPE));
    REQUIRE(field);
    auto value{field->value_get()};
    REQUIRE(value == "application/x-javascript"sv);
  }

  void
  delete_earliest_dir(CacheVC *vc)
  {
    CacheKey key            = {};
    Dir     *last_collision = nullptr;
    SCOPED_MUTEX_LOCK(lock, vc->stripe->mutex, this->mutex->thread_holding);
    vc->vector.data[0].alternate.object_key_get(&key);
    REQUIRE(vc->stripe->directory.probe(&key, vc->stripe, &dir, &last_collision) != 0);
    REQUIRE(vc->stripe->directory.remove(&key, vc->stripe, &dir));
  }
};

class CacheAltInit : public CacheInit
{
public:
  CacheAltInit() {}
  int
  cache_init_success_callback(int /* event ATS_UNUSED */, void * /* e ATS_UNUSED */) override
  {
    CacheTestHandler               *h     = new CacheTestHandler(SMALL_FILE, "http://www.scw11.com");
    test_Alternate_S_to_L_remove_S *ls    = new test_Alternate_S_to_L_remove_S(LARGE_FILE, "http://www.scw11.com");
    CacheAltReadAgain              *read  = new CacheAltReadAgain(SMALL_FILE, "http://www.scw11.com");
    CacheAltReadAgain2             *read2 = new CacheAltReadAgain2(LARGE_FILE, "http://www.scw11.com");
    TerminalTest                   *tt    = new TerminalTest;

    h->add(ls);
    h->add(read); // read again
    h->add(read2);
    h->add(tt);
    this_ethread()->schedule_imm(h);
    delete this;
    return 0;
  }
};

TEST_CASE("cache write -> read", "cache")
{
  init_cache(256 * 1024 * 1024);
  // large write test
  CacheAltInit *init = new CacheAltInit;

  this_ethread()->schedule_imm(init);
  this_thread()->execute();
}
