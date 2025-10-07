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

#include <memory>
#include <string>
#include <vector>

#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/fs/fs_manager.h"
#include "kudu/master/master.h"
#include "kudu/master/mini_master.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/path_util.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_string(ip_config_mode);

namespace kudu {
namespace master {

using std::unique_ptr;

class MiniMasterTest : public KuduTest,
                       public ::testing::WithParamInterface<std::string> {
 protected:
  static std::string get_host() {
    IPMode mode;
    FLAGS_ip_config_mode = GetParam();
    CHECK_OK(ParseIPModeFlag(FLAGS_ip_config_mode, &mode));
    switch (mode) {
      case IPMode::IPV6:
        return "::1";
      case IPMode::DUAL:
        return "::";
      default:
        return "127.0.0.1";
    }
  }
};

// This is used to run all parameterized tests with different IP modes.
INSTANTIATE_TEST_SUITE_P(Parameters, MiniMasterTest,
                         testing::Values("ipv4", "ipv6", "dual"));

TEST_P(MiniMasterTest, TestMultiDirMaster) {
  // Specifying the number of data directories will create subdirectories under the test root.
  unique_ptr<MiniMaster> mini_master;
  FsManager* fs_manager;

  int kNumDataDirs = 3;
  mini_master.reset(new MiniMaster(GetTestPath("Master"), HostPort(get_host(), 0), kNumDataDirs));
  ASSERT_OK(mini_master->Start());
  fs_manager = mini_master->master()->fs_manager();
  ASSERT_STR_CONTAINS(DirName(fs_manager->GetWalsRootDir()), "wal");
  ASSERT_EQ(kNumDataDirs, fs_manager->GetDataRootDirs().size());
}
}  // namespace master
}  // namespace kudu
