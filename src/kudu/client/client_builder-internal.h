// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kudu/client/client.h"
#include "kudu/client/replica_controller-internal.h"
#include "kudu/gutil/macros.h"
#include "kudu/util/monotime.h"

namespace kudu {

namespace client {

class KuduClientBuilder::Data {
 public:
  Data();
  ~Data();

  std::vector<std::string> master_server_addrs_;
  MonoDelta default_admin_operation_timeout_;
  MonoDelta default_rpc_timeout_;
  MonoDelta connection_negotiation_timeout_;
  std::string authn_creds_;
  std::string jwt_;
  internal::ReplicaController::Visibility replica_visibility_;
  std::optional<int64_t> rpc_max_message_size_;
  std::optional<int> num_reactors_;
  std::string sasl_protocol_name_;
  bool require_authentication_;
  EncryptionPolicy encryption_policy_;
  std::vector<std::string> trusted_certs_pem_;

  DISALLOW_COPY_AND_ASSIGN(Data);
};

} // namespace client
} // namespace kudu
