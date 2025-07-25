/* @file
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

#pragma once

#include "mgmt/rpc/jsonrpc/JsonRPCManager.h"

namespace rpc::handlers::server
{
swoc::Rv<YAML::Node> server_start_drain(std::string_view const &id, YAML::Node const &params);
swoc::Rv<YAML::Node> server_stop_drain(std::string_view const &id, YAML::Node const &);
void                 server_shutdown(YAML::Node const &);
swoc::Rv<YAML::Node> get_server_status(std::string_view const &id, YAML::Node const &);
swoc::Rv<YAML::Node> get_connection_tracker_info(std::string_view const &id, YAML::Node const &params);

} // namespace rpc::handlers::server
