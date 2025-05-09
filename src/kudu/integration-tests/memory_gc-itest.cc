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

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/integration-tests/cluster_itest_util.h"
#include "kudu/integration-tests/external_mini_cluster-itest-base.h"
#include "kudu/integration-tests/test_workload.h"
#include "kudu/mini-cluster/external_mini_cluster.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using std::string;
using std::vector;

METRIC_DECLARE_entity(server);
METRIC_DECLARE_gauge_uint64(generic_current_allocated_bytes);
METRIC_DECLARE_gauge_uint64(tcmalloc_pageheap_free_bytes);
METRIC_DECLARE_gauge_uint64(spinlock_contention_time);
METRIC_DECLARE_gauge_uint64(tcmalloc_max_total_thread_cache_bytes);
namespace kudu {

using cluster::ExternalMiniClusterOptions;
using cluster::ExternalTabletServer;

namespace {
void GetOverheadRatio(ExternalTabletServer* ets, double* ratio) {
  CHECK(ratio);

  int64_t generic_current_allocated_bytes = 0;
  int64_t tcmalloc_pageheap_free_bytes = 0;
  ASSERT_OK(itest::GetTsCounterValue(ets, &METRIC_generic_current_allocated_bytes,
                                     &generic_current_allocated_bytes));
  ASSERT_OK(itest::GetTsCounterValue(ets, &METRIC_tcmalloc_pageheap_free_bytes,
                                     &tcmalloc_pageheap_free_bytes));
  ASSERT_GT(generic_current_allocated_bytes, 0);
  ASSERT_GE(tcmalloc_pageheap_free_bytes, 0);
  *ratio = static_cast<double>(tcmalloc_pageheap_free_bytes) / generic_current_allocated_bytes;
}
} // namespace

class MemoryGcITest : public ExternalMiniClusterITestBase {
};

TEST_F(MemoryGcITest, TestPeriodicGc) {
  vector<string> ts_flags;
  // Set GC interval seconds short enough, so the test case could complete sooner.
  ts_flags.emplace_back("--gc_tcmalloc_memory_interval_seconds=5");
  // Disable tcmalloc memory GC by memory tracker, but periodical tcmalloc memory
  // GC is still enabled.
  ts_flags.emplace_back("--disable_tcmalloc_gc_by_memory_tracker_for_testing=true");

  ExternalMiniClusterOptions opts;
  opts.extra_tserver_flags = std::move(ts_flags);
  opts.num_tablet_servers = 3;
  NO_FATALS(StartClusterWithOpts(opts));

  // Disable periodical tcmalloc memory GC for tserver-0 and tserver-2.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(0),
                              "gc_tcmalloc_memory_interval_seconds", "0"));
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(1),
                              "gc_tcmalloc_memory_interval_seconds", "1"));
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(2),
                              "gc_tcmalloc_memory_interval_seconds", "0"));

  // Write some data to be scanned later on.
  {
    TestWorkload workload(cluster_.get());
    workload.set_num_tablets(60);
    workload.set_num_replicas(1);
    workload.set_num_write_threads(4);
    workload.set_write_batch_size(10);
    workload.set_payload_bytes(1024);
    workload.Setup();
    workload.Start();
    ASSERT_EVENTUALLY([&]() {
      ASSERT_GE(workload.rows_inserted(), 30000);
    });
    workload.StopAndJoin();
  }

  // Additional memory is allocated during scan operations below.
  {
    TestWorkload workload(cluster_.get());
    workload.set_num_write_threads(0);
    workload.set_num_read_threads(8);
    workload.Setup();
    workload.Start();
    // Run the scan workload for a long time to let it allocate/deallocate a lot of memory.
    SleepFor(MonoDelta::FromSeconds(8));
    workload.StopAndJoin();
  }

  // Check result.
  ASSERT_EVENTUALLY([&]() {
    NO_FATALS(
      double ratio;
      GetOverheadRatio(cluster_->tablet_server(0), &ratio);
      ASSERT_GE(ratio, 0.1) << "tserver-0";
      GetOverheadRatio(cluster_->tablet_server(1), &ratio);
      ASSERT_LE(ratio, 0.1) << "tserver-1";
      GetOverheadRatio(cluster_->tablet_server(2), &ratio);
      ASSERT_GE(ratio, 0.1) << "tserver-2";
    );
  });
}

// Test if the lock contention decreases if increasing the flag
// tcmalloc_max_total_thread_cache_bytes.
TEST_F(MemoryGcITest, TestLockContentionInVariousThreadCacheSize) {
  SKIP_IF_SLOW_NOT_ALLOWED();

  ExternalMiniClusterOptions opts;
  // Set the max_total_thread_cache_bytes to 1MB.
  vector<string> ts_flags;
  ts_flags.emplace_back("--tcmalloc_max_total_thread_cache_bytes=1048576");
  opts.num_tablet_servers = 3;
  opts.extra_tserver_flags = std::move(ts_flags);
  NO_FATALS(StartClusterWithOpts(opts));

  // Set --max_total_thread_cache_bytes to 8MB for the second tablet server.
  auto *ts = cluster_->tablet_server(1);
  ts->mutable_flags()->emplace_back("--tcmalloc_max_total_thread_cache_bytes=8388608");
  ts->Shutdown();
  ASSERT_OK(ts->Restart());

  // Set --max_total_thread_cache_bytes to 64MB for the third tablet server.
  ts = cluster_->tablet_server(2);
  ts->mutable_flags()->emplace_back("--tcmalloc_max_total_thread_cache_bytes=67108864");
  ts->Shutdown();
  ASSERT_OK(ts->Restart());

  // Make sure the flag works.
  int64_t total_size = 0;
  ASSERT_OK(itest::GetInt64Metric(cluster_->tablet_server(0)->bound_http_hostport(),
                                  &METRIC_ENTITY_server,
                                  "kudu.tabletserver",
                                  &METRIC_tcmalloc_max_total_thread_cache_bytes,
                                  "value",
                                  &total_size));
  ASSERT_EQ(1048576, total_size);
  ASSERT_OK(itest::GetInt64Metric(cluster_->tablet_server(1)->bound_http_hostport(),
                                  &METRIC_ENTITY_server,
                                  "kudu.tabletserver",
                                  &METRIC_tcmalloc_max_total_thread_cache_bytes,
                                  "value",
                                  &total_size));
  ASSERT_EQ(8388608, total_size);
  ASSERT_OK(itest::GetInt64Metric(cluster_->tablet_server(2)->bound_http_hostport(),
                                  &METRIC_ENTITY_server,
                                  "kudu.tabletserver",
                                  &METRIC_tcmalloc_max_total_thread_cache_bytes,
                                  "value",
                                  &total_size));
  ASSERT_EQ(67108864, total_size);

  // Write some data to be scanned later on.
  {
    TestWorkload workload(cluster_.get());
    workload.set_num_tablets(60);
    workload.set_num_replicas(3);
    workload.set_num_write_threads(20);
    workload.set_write_batch_size(100);
    workload.set_payload_bytes(1024);
    workload.Setup();
    workload.Start();
    ASSERT_EVENTUALLY([&]() {
      ASSERT_GE(workload.rows_inserted(), 30000);
    });
    workload.StopAndJoin();
  }

  // Additional memory is allocated during scan operations below.
  {
    TestWorkload workload(cluster_.get());
    workload.set_num_write_threads(0);
    workload.set_num_read_threads(20);
    workload.Setup();
    workload.Start();
    // Run the scan workload for a long time to let it allocate/deallocate a lot of memory.
    SleepFor(MonoDelta::FromSeconds(8));
    workload.StopAndJoin();
  }

  // Compare the lock contention.
  int64_t contention_0 = 0;
  ASSERT_OK(itest::GetInt64Metric(cluster_->tablet_server(0)->bound_http_hostport(),
                                  &METRIC_ENTITY_server,
                                  "kudu.tabletserver",
                                  &METRIC_spinlock_contention_time,
                                  "value",
                                  &contention_0));
  int64_t contention_1 = 1;
  ASSERT_OK(itest::GetInt64Metric(cluster_->tablet_server(1)->bound_http_hostport(),
                                  &METRIC_ENTITY_server,
                                  "kudu.tabletserver",
                                  &METRIC_spinlock_contention_time,
                                  "value",
                                  &contention_1));
  int64_t contention_2 = 2;
  ASSERT_OK(itest::GetInt64Metric(cluster_->tablet_server(2)->bound_http_hostport(),
                                  &METRIC_ENTITY_server,
                                  "kudu.tabletserver",
                                  &METRIC_spinlock_contention_time,
                                  "value",
                                  &contention_2));
  LOG(INFO) << "The lock contention metric of Tablet server 0 is " << contention_0;
  LOG(INFO) << "The lock contention metric of Tablet server 1 is " << contention_1;
  LOG(INFO) << "The lock contention metric of Tablet server 2 is " << contention_2;
}

} // namespace kudu
