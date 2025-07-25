/** @file

    Plugin to perform background fetches of certain content that would
    otherwise not be cached. For example, Range: requests / responses.

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
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#include <string>
#include <iostream>
#include <unordered_map>
#include <cinttypes>
#include <string_view>
#include <array>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ts/ts.h"
#include "ts/remap.h"
#include "ts/remap_version.h"
#include "background_fetch.h"
#include "configs.h"

static const char *
getCacheLookupResultName(TSCacheLookupResult result)
{
  switch (result) {
  case TS_CACHE_LOOKUP_MISS:
    return "TS_CACHE_LOOKUP_MISS";
    break;
  case TS_CACHE_LOOKUP_HIT_STALE:
    return "TS_CACHE_LOOKUP_HIT_STALE";
    break;
  case TS_CACHE_LOOKUP_HIT_FRESH:
    return "TS_CACHE_LOOKUP_HIT_FRESH";
    break;
  case TS_CACHE_LOOKUP_SKIPPED:
    return "TS_CACHE_LOOKUP_SKIPPED";
    break;
  default:
    return "UNKNOWN_CACHE_LOOKUP_EVENT";
    break;
  }
}

///////////////////////////////////////////////////////////////////////////
// create background fetch request if possible
//
static bool
cont_check_cacheable(TSHttpTxn txnp)
{
  if (TSHttpTxnIsInternal(txnp)) {
    return false;
  }
  int lookupStatus;
  if (TSHttpTxnCacheLookupStatusGet(txnp, &lookupStatus) == TS_ERROR) {
    TSError("[%s] Couldn't get cache status of object", PLUGIN_NAME);
    return false;
  }
  Dbg(dbg_ctl, "lookup status: %s", getCacheLookupResultName(static_cast<TSCacheLookupResult>(lookupStatus)));
  bool ret = false;
  if (TS_CACHE_LOOKUP_MISS == lookupStatus || TS_CACHE_LOOKUP_HIT_STALE == lookupStatus) {
    bool const nostore = TSHttpTxnServerRespNoStoreGet(txnp);

    Dbg(dbg_ctl, "is nostore set %d", nostore);
    if (!nostore) {
      TSMBuffer request;
      TSMLoc    req_hdr;
      if (TS_SUCCESS == TSHttpTxnClientReqGet(txnp, &request, &req_hdr)) {
        BgFetchData *data = new BgFetchData();
        // Initialize the data structure (can fail) and acquire a privileged lock on the URL
        if (data->initialize(request, req_hdr, txnp) && data->acquireUrl()) {
          Dbg(dbg_ctl, "scheduling background fetch");
          data->schedule();
          ret = true;
        } else {
          delete data;
        }
      }
      TSHandleMLocRelease(request, TS_NULL_MLOC, req_hdr);
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////
// Main "plugin", which is a global TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE hook. Before
// initiating a background fetch, this checks
// if a background fetch is allowed for this request
//
static int
cont_handle_cache(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn      txnp   = static_cast<TSHttpTxn>(edata);
  BgFetchConfig *config = static_cast<BgFetchConfig *>(TSContDataGet(contp));

  if (nullptr == config) {
    // something seriously wrong..
    TSError("[%s] Can't get configurations", PLUGIN_NAME);
  } else if (config->bgFetchAllowed(txnp)) {
    if (TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE == event) {
      bool const requested = cont_check_cacheable(txnp);
      if (requested) // Made a background fetch request, do not cache the response
      {
        Dbg(dbg_ctl, "setting no store");
        TSHttpTxnCntlSet(txnp, TS_HTTP_CNTL_SERVER_NO_STORE, true);
        TSHttpTxnCacheLookupStatusSet(txnp, TS_CACHE_LOOKUP_MISS);
      }

    } else {
      TSError("[%s] Unknown event for this plugin %d", PLUGIN_NAME, event);
      Dbg(dbg_ctl, "unknown event for this plugin %d", event);
    }
  }
  // Reenable and continue with the state machine.
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

///////////////////////////////////////////////////////////////////////////
// Setup Remap mode
///////////////////////////////////////////////////////////////////////////////
// Initialize the plugin.
//
TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
  CHECK_REMAP_API_COMPATIBILITY(api_info, errbuf, errbuf_size);
  Dbg(dbg_ctl, "cache fill remap is successfully initialized");
  return TS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// We don't have any specific "instances" here, at least not yet.
//
TSReturnCode
TSRemapNewInstance(int argc, char **argv, void **ih, char * /* errbuf ATS_UNUSED */, int /* errbuf_size ATS_UNUSED */)
{
  TSCont         cont    = TSContCreate(cont_handle_cache, nullptr);
  BgFetchConfig *config  = new BgFetchConfig(cont);
  bool           success = true;

  // The first two arguments are the "from" and "to" URL string. We need to
  // skip them, but we also require that there be an option to masquerade as
  // argv[0], so we increment the argument indexes by 1 rather than by 2.
  argc--;
  argv++;

  // This is for backwards compatibility, ugly! ToDo: Remove for ATS v9.0.0 IMO.
  if (argc > 1 && *argv[1] != '-') {
    Dbg(dbg_ctl, "config file %s", argv[1]);
    if (!config->readConfig(argv[1])) {
      success = false;
    }
  } else {
    if (!config->parseOptions(argc, const_cast<const char **>(argv))) {
      success = false;
    }
  }

  if (success) {
    *ih = config;

    return TS_SUCCESS;
  }

  // Something went wrong with the configuration setup.
  delete config;
  return TS_ERROR;
}

void
TSRemapDeleteInstance(void *ih)
{
  BgFetchConfig *config = static_cast<BgFetchConfig *>(ih);
  delete config;
}

///////////////////////////////////////////////////////////////////////////////
//// This is the main "entry" point for the plugin, called for every request.
////
TSRemapStatus
TSRemapDoRemap(void *ih, TSHttpTxn txnp, TSRemapRequestInfo * /* rri */)
{
  if (nullptr == ih) {
    return TSREMAP_NO_REMAP;
  }
  BgFetchConfig *config = static_cast<BgFetchConfig *>(ih);
  TSHttpTxnHookAdd(txnp, TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, config->getCont());
  Dbg(dbg_ctl, "TSRemapDoRemap() added hook");

  return TSREMAP_NO_REMAP;
}
