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

#include "kudu/fs/fs_manager.h"

#include <sys/stat.h>
#include <unistd.h>
// IWYU pragma: no_include <bits/struct_stat.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>
#if !defined(NO_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#endif

#include "kudu/fs/block_manager.h"
#include "kudu/fs/data_dirs.h"
#include "kudu/fs/default_key_provider.h"
#include "kudu/fs/dir_manager.h"
#include "kudu/fs/dir_util.h"
#include "kudu/fs/fs.pb.h"
#include "kudu/fs/fs_report.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/flags.h"
#include "kudu/util/oid_generator.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/random.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using kudu::pb_util::ReadPBContainerFromPath;
using kudu::pb_util::SecureDebugString;
using std::make_tuple;
using std::nullopt;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using strings::Substitute;



DECLARE_bool(crash_on_eio);
DECLARE_bool(encrypt_data_at_rest);
DECLARE_bool(enable_multi_tenancy);
DECLARE_bool(log_container_rdb_paranoid_checks);
DECLARE_bool(log_container_rdb_skip_stats_update_on_db_open);
DECLARE_double(env_inject_eio);
DECLARE_string(block_manager);
DECLARE_string(env_inject_eio_globs);
DECLARE_string(env_inject_lock_failure_globs);
DECLARE_string(umask);
DECLARE_uint32(log_container_rdb_max_background_jobs);
DECLARE_uint64(log_container_rdb_write_buffer_size);

namespace kudu {
namespace fs {

static constexpr const char* const kTenantSelectors[] = {
  "00000000000000000000000000000000", // "default_tenant_kudu"
  "00000000000000000000000000000001", // "test_tenant_kudu"
};

static constexpr const char* const kEncryptionType[] = {
  "kNonEncryption",         // FLAGS_encrypt_data_at_rest = false
                            // FLAGS_enable_multi_tenancy = false
  "kServerEncryption",      // FLAGS_encrypt_data_at_rest = true
                            // FLAGS_enable_multi_tenancy = false
  "kMultiTenantEncryption", // FLAGS_encrypt_data_at_rest = true
                            // FLAGS_enable_multi_tenancy = true
};

static constexpr const char* const kTestTenantName = "test_tenant_kudu";
static constexpr const char* const kTestTenantKey = "00010203040506070809101112131442";
static constexpr const char* const kTestTenantKeyIv = "42141312111009080706050403020100";
static constexpr const char* const kTestTenantKeyVersion = "kudutenantkey@0";

class FsManagerTestBase : public KuduTest,
                          public testing::WithParamInterface<std::tuple<string, const char*>> {
 public:
  FsManagerTestBase()
     : fs_root_(GetTestPath("fs_root")) {
  }

  void SetUp() override {
    KuduTest::SetUp();
    FLAGS_block_manager = std::get<0>(GetParam());
    auto encryption_type = std::get<1>(GetParam());
    if (encryption_type == kEncryptionType[0]) {
      tenant_id_ = kTenantSelectors[0];
      FLAGS_encrypt_data_at_rest = false;
      FLAGS_enable_multi_tenancy = false;
    } else if (encryption_type == kEncryptionType[1]) {
      tenant_id_ = kTenantSelectors[0];
      FLAGS_encrypt_data_at_rest = true;
      FLAGS_enable_multi_tenancy = false;
    } else if (encryption_type == kEncryptionType[2]) {
      tenant_id_ = kTenantSelectors[1];
      FLAGS_encrypt_data_at_rest = true;
      FLAGS_enable_multi_tenancy = true;
    } else {
      ASSERT_TRUE(0);
    }

    // Initialize File-System Layout
    ReinitFsManager();
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
    if (tenant_id() != kTenantSelectors[0]) {
      // Init tenant for non default tenant.
      ASSERT_OK(fs_manager_->AddTenant(kTestTenantName,
                                       tenant_id(),
                                       kTestTenantKey,
                                       kTestTenantKeyIv,
                                       kTestTenantKeyVersion));
    }
  }

  Env* GetEnv() const {
    // TODO(kedeng):
    //    Different tenants should use their own environments, but currently
    // the data loading patches for multi-tenants have not been merged, which
    // results in ignoring tenant information when re-opening the FS manager.
    // This method is temporarily used to ensure that the single test can run
    // successfully, and the implementation here will need to be modified in
    // the future.
    return fs_manager()->GetEnv(kTenantSelectors[0]);
  }

  void AddNonDefaultTanent() {
    if (tenant_id() != kTenantSelectors[0]) {
      // Init tenant for non default tenant.
      ASSERT_OK(fs_manager_->AddTenant(kTestTenantName,
                                       tenant_id(),
                                       kTestTenantKey,
                                       kTestTenantKeyIv,
                                       kTestTenantKeyVersion));
    }
  }

  void ReinitFsManager() {
    ReinitFsManagerWithPaths(fs_root_, { fs_root_ });
  }

  void ReinitFsManagerWithPaths(string wal_path, vector<string> data_paths) {
    FsManagerOpts opts;
    opts.wal_root = std::move(wal_path);
    opts.data_roots = std::move(data_paths);
    ReinitFsManagerWithOpts(std::move(opts));
  }

  void ReinitFsManagerWithOpts(FsManagerOpts opts) {
    fs_manager_.reset(new FsManager(env_, std::move(opts)));
  }

  void TestReadWriteDataFile(const Slice& data) {
    uint8_t buffer[64];
    DCHECK_LT(data.size(), sizeof(buffer));

    // Test Write
    unique_ptr<WritableBlock> writer;
    ASSERT_OK(fs_manager()->CreateNewBlock({}, &writer, tenant_id()));
    ASSERT_OK(writer->Append(data));
    ASSERT_OK(writer->Close());

    // Test Read
    Slice result(buffer, data.size());
    unique_ptr<ReadableBlock> reader;
    ASSERT_OK(fs_manager()->OpenBlock(writer->id(), &reader, tenant_id()));
    ASSERT_OK(reader->Read(0, result));
    ASSERT_EQ(data, result);
  }

  FsManager *fs_manager() const { return fs_manager_.get(); }

  const string& tenant_id() const { return tenant_id_; }

 protected:
  const string fs_root_;

 private:
  unique_ptr<FsManager> fs_manager_;
  string tenant_id_;
};

INSTANTIATE_TEST_SUITE_P(BlockManagerTypes, FsManagerTestBase,
// TODO(yingchun): When --enable_multi_tenancy is set, the data directories are still shared by
//  all tenants, which will cause some errors when --block_manager=logr. This will be fixed in the
//  future after the TODO "The new tenant should have its own dd manager instead of sharing" in
//  src/kudu/fs/fs_manager.cc is done. We can enable all the following test cases when the TODO
//  is addressed.
//
//    ::testing::ValuesIn(BlockManager::block_manager_types()),
//    ::testing::ValuesIn(kEncryptionType))
    ::testing::Values(
#if defined(__linux__)
        make_tuple("log", kEncryptionType[0]),
        make_tuple("log", kEncryptionType[1]),
        make_tuple("log", kEncryptionType[2]),
#if !defined(NO_ROCKSDB)
        make_tuple("logr", kEncryptionType[0]),
        make_tuple("logr", kEncryptionType[1]),
#endif
#endif
        make_tuple("file", kEncryptionType[0]),
        make_tuple("file", kEncryptionType[1]),
        make_tuple("file", kEncryptionType[2])
    )
);

TEST_P(FsManagerTestBase, TestBaseOperations) {
  fs_manager()->DumpFileSystemTree(std::cout, tenant_id());

  TestReadWriteDataFile(Slice("test0"));
  TestReadWriteDataFile(Slice("test1"));

  fs_manager()->DumpFileSystemTree(std::cout, tenant_id());
}

TEST_P(FsManagerTestBase, TestTenantAccountOperation) {
  int tenant_num = fs_manager()->tenants_count();
  const string non_exist_tenant_name = "non_exist_tenant_name";
  const string non_exist_tenant = "10000000000000000000000000000000";
  const string default_tenant_id = "00000000000000000000000000000000";
  auto encryption_type = std::get<1>(GetParam());
  if (encryption_type != kEncryptionType[2]) {
    if (encryption_type == kEncryptionType[0]) {
      // Multi-tenancy is disabled.
      ASSERT_FALSE(FLAGS_enable_multi_tenancy);
      ASSERT_FALSE(FLAGS_encrypt_data_at_rest);
    } else if (encryption_type == kEncryptionType[1]) {
      // Multi-tenancy is disabled but data at rest encryption is enabled.
      ASSERT_FALSE(FLAGS_enable_multi_tenancy);
      ASSERT_TRUE(FLAGS_encrypt_data_at_rest);
    }
    ASSERT_EQ(0, tenant_num);
    ASSERT_FALSE(fs_manager()->is_tenants_exist());

    // Add tenant is not allowed.
    ASSERT_DEATH({ ASSERT_OK(fs_manager()->AddTenant(
                       non_exist_tenant_name,
                       non_exist_tenant,
                       nullopt,
                       nullopt,
                       nullopt)); }, "");
    ASSERT_FALSE(fs_manager()->VertifyTenant(non_exist_tenant));
    ASSERT_TRUE(fs_manager()->VertifyTenant(default_tenant_id));
    ASSERT_EQ(0, fs_manager()->GetDataRootDirs(non_exist_tenant).size());
    ASSERT_NE(0, fs_manager()->GetDataRootDirs(default_tenant_id).size());
    ASSERT_FALSE(fs_manager()->block_manager(non_exist_tenant));
    ASSERT_TRUE(fs_manager()->block_manager(default_tenant_id));

    // Remove tenant is not allowed.
    const auto s = fs_manager()->RemoveTenant(non_exist_tenant);
    ASSERT_TRUE(s.IsNotSupported()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(),
        Substitute("Not support for removing tenant for id: $0.", non_exist_tenant));
  } else {
    // Multi-tenancy is enabled.
    ASSERT_TRUE(FLAGS_enable_multi_tenancy);
    ASSERT_TRUE(FLAGS_encrypt_data_at_rest);
    ASSERT_EQ(2, tenant_num);
    ASSERT_TRUE(fs_manager()->is_tenants_exist());
    for (const auto& tenant : kTenantSelectors) {
      ASSERT_TRUE(fs_manager()->is_tenant_exist(tenant));
      ASSERT_TRUE(fs_manager()->VertifyTenant(tenant));
      // Re-add a tenant which was already exist will fail.
      string new_tenant = "new_tenant_name";
      const auto s = fs_manager()->AddTenant(new_tenant, tenant, nullopt,
                                             nullopt, nullopt);
      ASSERT_TRUE(s.IsAlreadyPresent()) << s.ToString();
      ASSERT_STR_CONTAINS(s.ToString(),
          Substitute("Tenant $0 already exists.", tenant));
    }

    ASSERT_FALSE(fs_manager()->is_tenant_exist(non_exist_tenant));

    // Test add tenant.
    string new_tenant_name = "new_tenant_name";
    string new_tenant = "00000000000000000000000000000011";
    // Make sure the new tenant does not exist.
    ASSERT_FALSE(fs_manager()->is_tenant_exist(new_tenant));
    // Generate key info to do tenant init.
    security::DefaultKeyProvider key_provider;
    string encrypted_key;
    string iv;
    string version;
    ASSERT_OK(key_provider.GenerateEncryptionKey(&encrypted_key, &iv, &version));
    ASSERT_OK(fs_manager()->AddTenant(new_tenant_name, new_tenant,
                                      encrypted_key, iv, version));

    // The key info we get need equal to what we set.
    ASSERT_EQ(new_tenant_name, fs_manager()->tenant_name(new_tenant));
    ASSERT_EQ(encrypted_key, fs_manager()->tenant_key(new_tenant));
    ASSERT_EQ(iv, fs_manager()->tenant_key_iv(new_tenant));
    ASSERT_EQ(version, fs_manager()->tenant_key_version(new_tenant));

    // The new tenant need exist after 'AddTenant'.
    ASSERT_TRUE(fs_manager()->is_tenant_exist(new_tenant));
    ASSERT_EQ(3, fs_manager()->tenants_count());
    for (auto& tenant : fs_manager()->GetAllTenants()) {
      ASSERT_TRUE(fs_manager()->is_tenant_exist(tenant));
      ASSERT_TRUE(fs_manager()->VertifyTenant(tenant));
      ASSERT_NE(0, fs_manager()->GetDataRootDirs(tenant).size());
    }

    // Test remove tenant.
    ASSERT_TRUE(fs_manager()->is_tenant_exist(new_tenant));
    ASSERT_OK(fs_manager()->RemoveTenant(new_tenant));
    ASSERT_FALSE(fs_manager()->is_tenant_exist(new_tenant));
    ASSERT_EQ(2, fs_manager()->tenants_count());
    for (auto& tenant : fs_manager()->GetAllTenants()) {
      ASSERT_TRUE(fs_manager()->is_tenant_exist(tenant));
    }

    // Remove default tenant is not allowed.
    const auto s = fs_manager()->RemoveTenant(default_tenant_id);
    ASSERT_TRUE(s.IsNotSupported()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "Remove default tenant is not allowed.");
  }
}

TEST_P(FsManagerTestBase, TestIllegalPaths) {
  vector<string> illegal = { "", "asdf", "/foo\n\t" };
  for (const string& path : illegal) {
    ReinitFsManagerWithPaths(path, { path });
    ASSERT_TRUE(fs_manager()->CreateInitialFileSystemLayout().IsIOError());
  }
}

TEST_P(FsManagerTestBase, TestMultiplePaths) {
  string wal_path = GetTestPath("a");
  vector<string> data_paths = { GetTestPath("a"), GetTestPath("b"), GetTestPath("c") };
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
}

TEST_P(FsManagerTestBase, TestMatchingPathsWithMismatchedSlashes) {
  string wal_path = GetTestPath("foo");
  vector<string> data_paths = { wal_path + "/" };
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
}

TEST_P(FsManagerTestBase, TestDuplicatePaths) {
  string path = GetTestPath("foo");
  ReinitFsManagerWithPaths(path, { path, path, path });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
  NO_FATALS(AddNonDefaultTanent());
  ASSERT_EQ(vector<string>({ JoinPathSegments(path, fs_manager()->kDataDirName) }),
            fs_manager()->GetDataRootDirs(tenant_id()));
}

TEST_P(FsManagerTestBase, TestListTablets) {
  vector<string> tablet_ids;
  ASSERT_OK(fs_manager()->ListTabletIds(&tablet_ids));
  ASSERT_EQ(0, tablet_ids.size());

  string path = fs_manager()->GetTabletMetadataDir();
  unique_ptr<WritableFile> writer;
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, "foo.kudutmp"), &writer));
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, "foo.kudutmp.abc123"), &writer));
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, "foo.bak"), &writer));
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, "foo.bak.abc123"), &writer));
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, ".hidden"), &writer));
  // An uncanonicalized id.
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, "6ba7b810-9dad-11d1-80b4-00c04fd430c8"), &writer));
  // 1 valid tablet id.
  ASSERT_OK(GetEnv()->NewWritableFile(
      JoinPathSegments(path, "922ff7ed14c14dbca4ee16331dfda42a"), &writer));

  ASSERT_OK(fs_manager()->ListTabletIds(&tablet_ids));
  ASSERT_EQ(1, tablet_ids.size()) << tablet_ids;
}

TEST_P(FsManagerTestBase, TestCannotUseNonEmptyFsRoot) {
  string path = GetTestPath("new_fs_root");
  ASSERT_OK(GetEnv()->CreateDir(path));
  {
    unique_ptr<WritableFile> writer;
    ASSERT_OK(GetEnv()->NewWritableFile(
        JoinPathSegments(path, "some_file"), &writer));
  }

  // Try to create the FS layout. It should fail.
  ReinitFsManagerWithPaths(path, { path });
  ASSERT_TRUE(fs_manager()->CreateInitialFileSystemLayout().IsAlreadyPresent());
}

TEST_P(FsManagerTestBase, TestEmptyWALPath) {
  ReinitFsManagerWithPaths("", {});
  Status s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_TRUE(s.IsIOError()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "directory (fs_wal_dir) not provided");
}

TEST_P(FsManagerTestBase, TestOnlyWALPath) {
  string path = GetTestPath("new_fs_root");
  ASSERT_OK(GetEnv()->CreateDir(path));

  ReinitFsManagerWithPaths(path, {});
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_TRUE(HasPrefixString(fs_manager()->GetWalsRootDir(), path));
  ASSERT_TRUE(HasPrefixString(fs_manager()->GetConsensusMetadataDir(), path));
  ASSERT_TRUE(HasPrefixString(fs_manager()->GetTabletMetadataDir(), path));
  vector<string> data_dirs = fs_manager()->GetDataRootDirs();
  ASSERT_EQ(1, data_dirs.size());
  ASSERT_TRUE(HasPrefixString(data_dirs[0], path));
}

TEST_P(FsManagerTestBase, TestFormatWithSpecificUUID) {
  string path = GetTestPath("new_fs_root");
  ReinitFsManagerWithPaths(path, {});

  // Use an invalid uuid at first.
  string uuid = "not_a_valid_uuid";
  Status s = fs_manager()->CreateInitialFileSystemLayout(uuid);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), Substitute("invalid uuid $0", uuid));

  // Now use a valid one.
  ObjectIdGenerator oid_generator;
  uuid = oid_generator.Next();
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout(uuid));
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(uuid, fs_manager()->uuid());
}

TEST_P(FsManagerTestBase, TestMetadataDirInWALRoot) {
  // By default, the FsManager should put metadata in the wal root.
  FsManagerOpts opts;
  opts.wal_root = GetTestPath("wal");
  opts.data_roots = { GetTestPath("data") };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
  ASSERT_STR_CONTAINS(fs_manager()->GetTabletMetadataDir(),
                      JoinPathSegments("wal", FsManager::kTabletMetadataDirName));

  // Reinitializing the FS layout with any other configured metadata root
  // should fail, as a non-empty metadata root will be used verbatim.
  opts.metadata_root = GetTestPath("asdf");
  ReinitFsManagerWithOpts(opts);
  Status s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();

  // The above comment also applies to the default value before Kudu 1.6: the
  // first configured data directory. Let's check that too.
  opts.metadata_root = opts.data_roots[0];
  ReinitFsManagerWithOpts(opts);
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();

  // We should be able to verify that the metadata is in the WAL root.
  opts.metadata_root = opts.wal_root;
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
}

TEST_P(FsManagerTestBase, TestMetadataDirInDataRoot) {
  FsManagerOpts opts;
  opts.wal_root = GetTestPath("wal");
  opts.data_roots = { GetTestPath("data1") };

  // Creating a brand new FS layout configured with metadata in the first data
  // directory emulates the default behavior in Kudu 1.6 and below.
  opts.metadata_root = opts.data_roots[0];
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
  const string& meta_root_suffix = JoinPathSegments("data1", FsManager::kTabletMetadataDirName);
  ASSERT_STR_CONTAINS(fs_manager()->GetTabletMetadataDir(), meta_root_suffix);

  // Opening the FsManager with an empty fs_metadata_dir flag should account
  // for the old default and use the first data directory for metadata.
  opts.metadata_root.clear();
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_STR_CONTAINS(fs_manager()->GetTabletMetadataDir(), meta_root_suffix);

  // Now let's test adding data directories with metadata in the data root.
  // Adding data directories is not supported by the file block manager.
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "Skipping the rest of test, file block manager not supported";
  }

  // Adding a data dir to the front of the FS root list (i.e. such that the
  // metadata root is no longer at the front) will prevent Kudu from starting.
  opts.data_roots = { GetTestPath("data2"), GetTestPath("data1") };
  ReinitFsManagerWithOpts(opts);
  Status s = fs_manager()->Open();
  ASSERT_STR_CONTAINS(s.ToString(), "could not verify required directory");
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_FALSE(GetEnv()->FileExists(opts.data_roots[0]));
  ASSERT_TRUE(GetEnv()->FileExists(opts.data_roots[1]));

  // Now allow the reordering by specifying the expected metadata root.
  opts.metadata_root = opts.data_roots[1];
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_STR_CONTAINS(fs_manager()->GetTabletMetadataDir(), meta_root_suffix);
}

TEST_P(FsManagerTestBase, TestIsolatedMetadataDir) {
  FsManagerOpts opts;
  opts.wal_root = GetTestPath("wal");
  opts.data_roots = { GetTestPath("data") };

  // Creating a brand new FS layout configured to a directory outside the WAL
  // or data directories is supported.
  opts.metadata_root = GetTestPath("asdf");
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
  ASSERT_STR_CONTAINS(fs_manager()->GetTabletMetadataDir(),
                      JoinPathSegments("asdf", FsManager::kTabletMetadataDirName));
  ASSERT_NE(DirName(fs_manager()->GetTabletMetadataDir()),
            DirName(fs_manager()->GetWalsRootDir()));
  ASSERT_NE(DirName(fs_manager()->GetTabletMetadataDir()),
            DirName(fs_manager()->GetDataRootDirs()[0]));

  // If the user henceforth forgets to specify the metadata root, the FsManager
  // will fail to open.
  opts.metadata_root.clear();
  ReinitFsManagerWithOpts(opts);
  Status s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
}

Status CountTmpFiles(Env* env, const string& path, const vector<string>& children,
                     unordered_set<string>* checked_dirs, int* count) {
  int n = 0;
  vector<string> sub_objects;
  for (const string& name : children) {
    if (name == "." || name == "..") continue;

    string sub_path;
    RETURN_NOT_OK(env->Canonicalize(JoinPathSegments(path, name), &sub_path));
    bool is_directory;
    RETURN_NOT_OK(env->IsDirectory(sub_path, &is_directory));
    if (is_directory) {
      if (!ContainsKey(*checked_dirs, sub_path)) {
        checked_dirs->insert(sub_path);
        RETURN_NOT_OK(env->GetChildren(sub_path, &sub_objects));
        int subdir_count = 0;
        RETURN_NOT_OK(CountTmpFiles(env, sub_path, sub_objects, checked_dirs, &subdir_count));
        n += subdir_count;
      }
    } else if (name.find(kTmpInfix) != string::npos) {
      n++;
    }
  }
  *count = n;
  return Status::OK();
}

Status CountTmpFiles(Env* env, const vector<string>& roots, int* count) {
  unordered_set<string> checked_dirs;
  int n = 0;
  for (const string& root : roots) {
    vector<string> children;
    RETURN_NOT_OK(env->GetChildren(root, &children));
    int dir_count;
    RETURN_NOT_OK(CountTmpFiles(env, root, children, &checked_dirs, &dir_count));
    n += dir_count;
  }
  *count = n;
  return Status::OK();
}

TEST_P(FsManagerTestBase, TestCreateWithFailedDirs) {
  string wal_path = GetTestPath("wals");
  // Create some top-level paths to place roots in.
  vector<string> data_paths = { GetTestPath("data1"), GetTestPath("data2"), GetTestPath("data3") };
  for (const string& path : data_paths) {
    ASSERT_OK(GetEnv()->CreateDir(path));
  }
  // Initialize the FS layout with roots in subdirectories of data_paths. When
  // we canonicalize paths, we canonicalize the dirname of each path (e.g.
  // data1) to ensure it exists. With this, we can inject failures in
  // canonicalization by failing the dirname.
  vector<string> data_roots = JoinPathSegmentsV(data_paths, "root");

  FLAGS_crash_on_eio = false;
  FLAGS_env_inject_eio = 1.0;

  // Fail a directory, avoiding the metadata directory.
  FLAGS_env_inject_eio_globs = data_paths[1];
  ReinitFsManagerWithPaths(wal_path, data_roots);
  Status s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_STR_MATCHES(s.ToString(), "cannot create FS layout; at least one directory "
                                   "failed to canonicalize");
}

// Test that if an operator tries to copy an instance file, Kudu will refuse to
// start up.
TEST_P(FsManagerTestBase, TestOpenWithDuplicateInstanceFiles) {
  // First, make a copy of some instance files.
  WritableFileOptions wr_opts;
  wr_opts.mode = Env::MUST_CREATE;
  const string duplicate_test_root = GetTestPath("fs_dup");
  ASSERT_OK(GetEnv()->CreateDir(duplicate_test_root));
  const string duplicate_instance = JoinPathSegments(
      duplicate_test_root, FsManager::kInstanceMetadataFileName);
  ASSERT_OK(env_util::CopyFile(GetEnv(), fs_manager()->GetInstanceMetadataPath(fs_root_),
                               duplicate_instance, wr_opts));

  // Make a copy of the per-directory instance file.
  const string duplicate_test_dir = JoinPathSegments(duplicate_test_root, kDataDirName);
  ASSERT_OK(GetEnv()->CreateDir(duplicate_test_dir));
  const string duplicate_dir_instance = JoinPathSegments(
      duplicate_test_dir, kInstanceMetadataFileName);
  ASSERT_OK(env_util::CopyFile(GetEnv(),
        fs_manager()->dd_manager()->FindDirByUuidIndex(0)->instance()->path(),
        duplicate_dir_instance, wr_opts));

  // This is disallowed, as each directory should have its own unique UUID.
  // NOTE: the failure case looks slightly different depending on the block
  // manager type, so just check there is an error, rather than the specific
  // error type.
  ReinitFsManagerWithPaths(fs_root_, { fs_root_, duplicate_test_root });
  Status s = fs_manager()->Open();
  ASSERT_FALSE(s.ok());
  ASSERT_STR_CONTAINS(s.ToString(), "instance files contain duplicate UUIDs");
}

TEST_P(FsManagerTestBase, TestOpenWithNoBlockManagerInstances) {
  // Open a healthy FS layout, sharing the WAL directory with a data directory.
  const string wal_path = GetTestPath("wals");
  FsManagerOpts opts;
  opts.wal_root = wal_path;
  const auto block_manager_type = opts.block_manager_type;
  ReinitFsManagerWithOpts(std::move(opts));
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());

  // Now try moving the data directory out of WAL directory.
  // We must be able to find an existing block manager instance to open the
  // FsManager successfully.
  for (auto check_behavior : { UpdateInstanceBehavior::DONT_UPDATE,
                               UpdateInstanceBehavior::UPDATE_AND_IGNORE_FAILURES }) {
    FsManagerOpts new_opts;
    new_opts.wal_root = wal_path;
    new_opts.data_roots = { GetTestPath("data") };
    new_opts.update_instances = check_behavior;
    ReinitFsManagerWithOpts(new_opts);
    Status s = fs_manager()->Open();
    ASSERT_STR_CONTAINS(s.ToString(), "no healthy directories found");
    ASSERT_TRUE(s.IsNotFound()) << s.ToString();

    // Once we supply the WAL directory as a data directory, we can open successfully.
    new_opts.data_roots.emplace_back(wal_path);
    ReinitFsManagerWithOpts(std::move(new_opts));
    s = fs_manager()->Open();
    if (block_manager_type == "file") {
      ASSERT_TRUE(s.IsCorruption()) << s.ToString();
      ASSERT_STR_CONTAINS(s.ToString(), "2 unique UUIDs expected, got 1");
    } else {
      ASSERT_OK(s);
    }
  }
}

// Test the behavior when we fail to open a data directory for some reason (its
// mountpoint failed, it's missing, etc). Kudu should allow this and open up
// with failed data directories listed.
TEST_P(FsManagerTestBase, TestOpenWithUnhealthyDataDir) {
  // Successfully create a multi-directory FS layout.
  const string new_root = GetTestPath("new_root");
  FsManagerOpts opts;
  opts.wal_root = fs_root_;
  opts.data_roots = { fs_root_, new_root };
  ReinitFsManagerWithOpts(opts);
  string new_root_uuid;
  auto s = fs_manager()->Open();
  if (opts.block_manager_type == "file") {
    ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "2 unique UUIDs expected, got 1");
  } else {
    ASSERT_OK(s);
    ASSERT_TRUE(fs_manager()->dd_manager()->FindUuidByRoot(new_root,
                                                           &new_root_uuid));
  }

  // Fail the new directory. Kudu should have no problem starting up with this
  // and should list one as failed.
  FLAGS_env_inject_eio_globs = JoinPathSegments(new_root, "**");
  FLAGS_env_inject_eio = 1.0;
  ReinitFsManagerWithOpts(opts);
  s = fs_manager()->Open();
  if (opts.block_manager_type == "file") {
    ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "2 unique UUIDs expected, got 1");
    LOG(INFO) << "Skipping the rest of test, file block manager not supported";
    return;
  }

  ASSERT_OK(s);
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());

  // Now remove the new directory from disk. Kudu should start up with the
  // empty disk and attempt to use it. Upon opening the FS layout, we should
  // see no failed directories.
  FLAGS_env_inject_eio = 0;
  ASSERT_OK(GetEnv()->DeleteRecursively(new_root));
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());

  // Even at the same mountpoint, the directory will be assigned a new UUID.
  string new_root_uuid_post_update;
  ASSERT_TRUE(fs_manager()->dd_manager()->FindUuidByRoot(new_root, &new_root_uuid_post_update));
  ASSERT_NE(new_root_uuid, new_root_uuid_post_update);

  // Now let's try failing all the directories. Kudu should yield an error,
  // complaining it couldn't find any healthy data directories.
  FLAGS_env_inject_eio_globs = JoinStrings(JoinPathSegmentsV(opts.data_roots, "**"), ",");
  FLAGS_env_inject_eio = 1.0;
  ReinitFsManagerWithOpts(opts);
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "could not find a healthy instance file");

  // Upon returning from FsManager::Open() with a NotFound error, Kudu will
  // attempt to create a new FS layout. With bad mountpoints, this should fail.
  s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_TRUE(s.IsIOError()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "cannot create FS layout");

  // The above behavior should be seen if the data directories are missing...
  FLAGS_env_inject_eio = 0;
  for (const auto& root : opts.data_roots) {
    ASSERT_OK(GetEnv()->DeleteRecursively(root));
  }
  ReinitFsManagerWithOpts(opts);
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "could not find a healthy instance file");

  // ...except we should be able to successfully create a new FS layout.
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());
}

// When we canonicalize a directory, we actually canonicalize the directory's
// parent directory; as such, canonicalization can fail if the parent directory
// can't be read (e.g. due to a disk error or because it's flat out missing).
// In such cases, we should still be able to open the FS layout.
TEST_P(FsManagerTestBase, TestOpenWithCanonicalizationFailure) {
  // Create some parent directories and subdirectories.
  const string dir1 = GetTestPath("test1");
  const string dir2 = GetTestPath("test2");
  ASSERT_OK(GetEnv()->CreateDir(dir1));
  ASSERT_OK(GetEnv()->CreateDir(dir2));
  const string subdir1 = GetTestPath("test1/subdir");
  const string subdir2 = GetTestPath("test2/subdir");
  FsManagerOpts opts;
  opts.wal_root = subdir1;
  opts.data_roots = { subdir1, subdir2 };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());

  // Fail the canonicalization by injecting errors to a parent directory.
  ReinitFsManagerWithOpts(opts);
  FLAGS_env_inject_eio_globs = JoinPathSegments(dir2, "**");
  FLAGS_env_inject_eio = 1.0;
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());
  FLAGS_env_inject_eio = 0;

  // Now fail the canonicalization by deleting a parent directory. This
  // simulates the mountpoint disappearing.
  ASSERT_OK(GetEnv()->DeleteRecursively(dir2));
  ReinitFsManagerWithOpts(opts);
  Status s = fs_manager()->Open();
  ASSERT_OK(s);
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());
  if (opts.block_manager_type == "file") {
    LOG(INFO) << "Skipping the rest of test, file block manager not supported";
    return;
  }

  // Let's try that again, but with the appropriate mountpoint/directory.
  ASSERT_OK(GetEnv()->CreateDir(dir2));
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());
}

TEST_P(FsManagerTestBase, TestTmpFilesCleanup) {
  string wal_path = GetTestPath("wals");
  vector<string> data_paths = { GetTestPath("data1"), GetTestPath("data2"), GetTestPath("data3") };
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());

  // Create a few tmp files here
  shared_ptr<WritableFile> tmp_writer;

  string tmp_path = JoinPathSegments(fs_manager()->GetWalsRootDir(), "wal.kudutmp.file");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->GetEnv(), tmp_path, &tmp_writer));

  tmp_path = JoinPathSegments(fs_manager()->GetDataRootDirs()[0], "data1.kudutmp.file");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->GetEnv(), tmp_path, &tmp_writer));

  tmp_path = JoinPathSegments(fs_manager()->GetConsensusMetadataDir(), "12345.kudutmp.asdfg");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->GetEnv(), tmp_path, &tmp_writer));

  tmp_path = JoinPathSegments(fs_manager()->GetTabletMetadataDir(), "12345.kudutmp.asdfg");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->GetEnv(), tmp_path, &tmp_writer));

  // Not a misprint here: checking for just ".kudutmp" as well
  tmp_path = JoinPathSegments(fs_manager()->GetDataRootDirs()[1], "data2.kudutmp");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->GetEnv(), tmp_path, &tmp_writer));

  // Try with nested directory
  string nested_dir_path = JoinPathSegments(fs_manager()->GetDataRootDirs()[2], "data4");
  ASSERT_OK(env_util::CreateDirIfMissing(fs_manager()->GetEnv(), nested_dir_path));
  tmp_path = JoinPathSegments(nested_dir_path, "data4.kudutmp.file");
  ASSERT_OK(env_util::OpenFileForWrite(fs_manager()->GetEnv(), tmp_path, &tmp_writer));

  // Add a loop using symlink
  string data3_link = JoinPathSegments(nested_dir_path, "data3-link");
  int symlink_error = symlink(fs_manager()->GetDataRootDirs()[2].c_str(), data3_link.c_str());
  ASSERT_EQ(0, symlink_error);

  vector<string> lookup_dirs = fs_manager()->GetDataRootDirs();
  lookup_dirs.emplace_back(fs_manager()->GetWalsRootDir());
  lookup_dirs.emplace_back(fs_manager()->GetConsensusMetadataDir());
  lookup_dirs.emplace_back(fs_manager()->GetTabletMetadataDir());

  int n_tmp_files = 0;
  ASSERT_OK(CountTmpFiles(fs_manager()->GetEnv(), lookup_dirs, &n_tmp_files));
  ASSERT_EQ(6, n_tmp_files);

  // The FsManager should not delete any tmp files if it fails to acquire
  // a lock on the data dir.
  string bm_instance = JoinPathSegments(fs_manager()->GetDataRootDirs()[1],
                                        "block_manager_instance");
  {
    google::FlagSaver saver;
    FLAGS_env_inject_lock_failure_globs = bm_instance;
    ReinitFsManagerWithPaths(wal_path, data_paths);
    Status s = fs_manager()->Open();
    ASSERT_STR_MATCHES(s.ToString(), "Could not lock.*");
    ASSERT_OK(CountTmpFiles(fs_manager()->GetEnv(), lookup_dirs, &n_tmp_files));
    ASSERT_EQ(6, n_tmp_files);
  }

  // Now start up without the injected lock failure, and ensure that tmp files are deleted.
  ReinitFsManagerWithPaths(wal_path, data_paths);
  ASSERT_OK(fs_manager()->Open());

  n_tmp_files = 0;
  ASSERT_OK(CountTmpFiles(fs_manager()->GetEnv(), lookup_dirs, &n_tmp_files));
  ASSERT_EQ(0, n_tmp_files);
}

namespace {

string FilePermsAsString(const string& path) {
  struct stat s;
  CHECK_ERR(stat(path.c_str(), &s));
  return StringPrintf("%03o", s.st_mode & ACCESSPERMS);
}

} // anonymous namespace

TEST_P(FsManagerTestBase, TestUmask) {
  // With the default umask, we should create files with permissions 600
  // and directories with permissions 700.
  ASSERT_EQ(077, g_parsed_umask) << "unexpected default value";
  string root = GetTestPath("fs_root");
  EXPECT_EQ("700", FilePermsAsString(root));
  EXPECT_EQ("700", FilePermsAsString(fs_manager()->GetConsensusMetadataDir()));
  EXPECT_EQ("600", FilePermsAsString(fs_manager()->GetInstanceMetadataPath(root)));

  // With umask 007, we should create files with permissions 660
  // and directories with 770.
  FLAGS_umask = "007";
  HandleCommonFlags();
  ASSERT_EQ(007, g_parsed_umask);
  root = GetTestPath("new_root");
  ReinitFsManagerWithPaths(root, { root });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  EXPECT_EQ("770", FilePermsAsString(root));
  EXPECT_EQ("770", FilePermsAsString(fs_manager()->GetConsensusMetadataDir()));
  EXPECT_EQ("660", FilePermsAsString(fs_manager()->GetInstanceMetadataPath(root)));

  // If we change the umask back to being restrictive and re-open the filesystem,
  // the permissions on the root dir should be fixed accordingly.
  FLAGS_umask = "077";
  HandleCommonFlags();
  ASSERT_EQ(077, g_parsed_umask);
  ReinitFsManagerWithPaths(root, { root });
  ASSERT_OK(fs_manager()->Open());
  EXPECT_EQ("700", FilePermsAsString(root));
}

TEST_P(FsManagerTestBase, TestOpenFailsWhenMissingImportantDir) {
  const string kWalRoot = fs_manager()->GetWalsRootDir();

  ASSERT_OK(GetEnv()->DeleteDir(kWalRoot));
  ReinitFsManager();
  Status s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "could not verify required directory");

  unique_ptr<WritableFile> f;
  ASSERT_OK(GetEnv()->NewWritableFile(kWalRoot, &f));
  s = fs_manager()->Open();
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "exists but is not a directory");
}

TEST_P(FsManagerTestBase, TestAddRemoveDataDirs) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "Skipping test, file block manager not supported";
  }

  // Try to open with a new data dir in the list to be opened; Kudu should
  // allow for this to happen, creating the necessary data directory.
  const string new_path1 = GetTestPath("new_path1");
  FsManagerOpts opts;
  opts.wal_root = fs_root_;
  opts.data_roots = { fs_root_, new_path1 };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());

  // Try to open with a data dir removed; this should succeed, and Kudu should
  // open with only a single data directory.
  opts.data_roots = { fs_root_ };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetDirs().size());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());

  // We should be able to add new directories anywhere in the list.
  const string new_path2 = GetTestPath("new_path2");
  opts.data_roots = { new_path2, fs_root_ };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());

  // Open the FS layout with an existing, failed data dir; this should be fine,
  // but should report a single failed directory.
  FLAGS_env_inject_eio = 1.0;
  FLAGS_env_inject_eio_globs = JoinPathSegments(new_path2, "**");
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());
}

TEST_P(FsManagerTestBase, TestEIOWhileChangingDirs) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "The file block manager doesn't support updating directories";
  }
  const string kTestPathBase = GetTestPath("testpath");
  const int kMaxDirs = 10;
  vector<string> all_dirs;
  for (int i = 0; i < kMaxDirs; i++) {
    const string dir = Substitute("$0$1", kTestPathBase, i);
    all_dirs.emplace_back(dir);
    ASSERT_OK(GetEnv()->CreateDir(dir));
  }
  FsManagerOpts opts;
  opts.wal_root = all_dirs[0];
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  // Inject failures into the directories. This shouldn't prevent any updating
  // of instances, and it definitely shouldn't affect startup.
  vector<string> all_dirs_but_first(++all_dirs.begin(), all_dirs.end());
  FLAGS_env_inject_eio_globs = JoinStrings(JoinPathSegmentsV(all_dirs_but_first, "**"), ",");
  FLAGS_env_inject_eio = 0.1;
  for (int i = 1; i <= kMaxDirs; i++) {
    // Use an increasing number of dirs so we build up to using all of them.
    opts.data_roots = all_dirs;
    opts.data_roots.resize(i);
    ReinitFsManagerWithOpts(opts);
    ASSERT_OK(fs_manager()->Open());
  }
}

// Unlike the case where we're opening the FsManager for deployment, when
// running the update_dirs tool (i.e. UPDATE_AND_ERROR_ON_FAILURE mode), Kudu
// should fail and return an error in the event of a disk failure. When that
// happens, we should ensure that the our failures to update get rolled back.
TEST_P(FsManagerTestBase, TestEIOWhileRunningUpdateDirsTool) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "The file block manager doesn't support updating directories";
  }
  const string kTestPathBase = GetTestPath("testpath");
  // Helper to get a new root.
  auto create_root = [&] (int i) {
    const string dir = Substitute("$0$1", kTestPathBase, i);
    CHECK_OK(GetEnv()->CreateDir(dir));
    return dir;
  };

  // Helper to collect the contents of the InstanceMetadataPB and
  // DirInstanceMetadataPBs we expect to see in 'data_roots'. We'll read the
  // contents of each instance file from disk and compare them before and after
  // a botched update of the FsManager.
  auto get_added_instance_files = [&] (const vector<string>& data_roots,
                                       unordered_map<string, string>* instance_names_to_contents) {
    unordered_map<string, string> instances;
    instance_names_to_contents->clear();
    // Skip the first root, since we'll be injecting errors into the first
    // directory, meaning we don't have any guarantees on what that directory's
    // instance files will be.
    for (int i = 1; i < data_roots.size(); i++) {
      const auto& root = data_roots[i];
      // Collect the contents of the InstanceMetadataPB objects.
      const auto instance_path = JoinPathSegments(root, FsManager::kInstanceMetadataFileName);
      unique_ptr<InstanceMetadataPB> pb(new InstanceMetadataPB);
      Status s = ReadPBContainerFromPath(GetEnv(), instance_path, pb.get(), pb_util::NOT_SENSITIVE);
      if (s.IsNotFound()) {
        InsertOrDie(&instances, instance_path, "");
      } else {
        RETURN_NOT_OK(s);
        InsertOrDie(&instances, instance_path, SecureDebugString(*pb));
      }

      // Collect the contents of the DirInstanceMetadataPB objects.
      unique_ptr<DirInstanceMetadataPB> bmi_pb(new DirInstanceMetadataPB);
      const auto block_manager_instance = JoinPathSegments(
          JoinPathSegments(root, kDataDirName), kInstanceMetadataFileName);
      s = ReadPBContainerFromPath(GetEnv(), block_manager_instance,
                                  bmi_pb.get(), pb_util::NOT_SENSITIVE);
      if (s.IsNotFound()) {
        InsertOrDie(&instances, block_manager_instance, "");
      } else {
        RETURN_NOT_OK(s);
        InsertOrDie(&instances, block_manager_instance, SecureDebugString(*bmi_pb));
      }
    }
    *instance_names_to_contents = std::move(instances);
    return Status::OK();
  };

  vector<string> all_roots = { create_root(0) };
  FsManagerOpts opts;
  opts.wal_root = all_roots[0];
  opts.data_roots = all_roots;
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());

  // Start injecting failures into the first directory as we try updating.
  FLAGS_env_inject_eio_globs = JoinPathSegments(all_roots[0], "**");
  FLAGS_env_inject_eio = 0.05;
  unordered_map<string, string> instance_files_before_update;
  ASSERT_EVENTUALLY([&] {
    {
      // First, collect the contents of our instance files so we can compare
      // against their contents after failing.
      google::FlagSaver saver;
      FLAGS_env_inject_eio = 0;
      all_roots.emplace_back(create_root(all_roots.size()));
      ASSERT_OK(get_added_instance_files(all_roots, &instance_files_before_update));
    }
    // Then try to update the directories. We'll keep trying until we fail.
    opts.update_instances = UpdateInstanceBehavior::UPDATE_AND_ERROR_ON_FAILURE;
    opts.data_roots = all_roots;
    ReinitFsManagerWithOpts(opts);
    Status s = fs_manager()->Open();
    ASSERT_FALSE(s.ok());
  });

  // Now that we've failed to add a new directory, let's compare the contents
  // of the instnace files to ensure that they're unchanged from the point
  // right before the update.
  FLAGS_env_inject_eio = 0;
  unordered_map<string, string> instance_files_after_update;
  ASSERT_OK(get_added_instance_files(all_roots, &instance_files_after_update));
  ASSERT_EQ(instance_files_before_update, instance_files_after_update);
}

TEST_P(FsManagerTestBase, TestReAddRemovedDataDir) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "Skipping test, file block manager not supported";
  }

  // Add a new data directory, remove it, and add it back.
  const string new_path1 = GetTestPath("new_path1");
  FsManagerOpts opts;
  opts.wal_root = fs_root_;
  unordered_map<string, string> path_to_uuid;
  for (const auto& data_roots : vector<vector<string>>({{ fs_root_, new_path1 },
                                                        { fs_root_ },
                                                        { fs_root_, new_path1 }})) {
    opts.data_roots = data_roots;
    ReinitFsManagerWithOpts(opts);
    ASSERT_OK(fs_manager()->Open());
    auto dd_manager = fs_manager()->dd_manager();
    ASSERT_EQ(data_roots.size(), dd_manager->GetDirs().size());

    // Since we haven't deleted any directories or instance files, ensure that
    // our UUIDs match across startups.
    for (const auto& data_root : data_roots) {
      string uuid;
      ASSERT_TRUE(dd_manager->FindUuidByRoot(data_root, &uuid));
      string* existing_uuid = FindOrNull(path_to_uuid, data_root);
      if (existing_uuid) {
        ASSERT_EQ(*existing_uuid, uuid) <<
            Substitute("Expected $0 to have UUID $1, got $2",
                       data_root, *existing_uuid, uuid);
      } else {
        InsertOrDie(&path_to_uuid, data_root, uuid);
      }
    }
  }
}

TEST_P(FsManagerTestBase, TestCannotRemoveDataDirServingAsMetadataDir) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "Skipping test, file block manager not supported";
  }

  // Create a new fs layout with a metadata root explicitly set to the first
  // data root.
  ASSERT_OK(GetEnv()->DeleteRecursively(fs_root_));
  ASSERT_OK(GetEnv()->CreateDir(fs_root_));

  FsManagerOpts opts;
  opts.data_roots = { JoinPathSegments(fs_root_, "data1"),
                      JoinPathSegments(fs_root_, "data2") };
  opts.metadata_root = opts.data_roots[0];
  opts.wal_root = JoinPathSegments(fs_root_, "wal");
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());

  // Stop specifying the metadata root. The FsManager will automatically look
  // for and find it in the first data root.
  opts.metadata_root.clear();
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());

  // Now try to remove the first data root. This should fail because in the
  // absence of a defined metadata root, the FsManager will try looking for it
  // in the wal root (not found), and the first data dir (not found).
  opts.data_roots = { opts.data_roots[1] };
  ReinitFsManagerWithOpts(opts);
  Status s = fs_manager()->Open();
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "could not verify required directory");
}

TEST_P(FsManagerTestBase, TestAddRemoveSpeculative) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "Skipping test, file block manager not supported";
  }

  // Add a second data directory.
  const string new_path1 = GetTestPath("new_path1");
  FsManagerOpts opts;
  opts.wal_root = fs_root_;
  opts.data_roots = { fs_root_, new_path1 };
  opts.update_instances = UpdateInstanceBehavior::UPDATE_AND_IGNORE_FAILURES;
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());

  // Create a 'speculative' FsManager with the second data directory removed.
  opts.data_roots = { fs_root_ };
  opts.update_instances = UpdateInstanceBehavior::DONT_UPDATE;
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetDirs().size());

  // Do the same thing, but with a new data directory added.
  const string new_path2 = GetTestPath("new_path2");
  opts.data_roots = { fs_root_, new_path1, new_path2 };
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(3, fs_manager()->dd_manager()->GetDirs().size());
  ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());

  // Neither of those attempts should have changed the on-disk state. Verify
  // this by retrying all three combinations again.
  // With three directories, we should see a failed directory still.
  vector<vector<string>> data_roots_list = { { fs_root_ },
                                             { fs_root_, new_path1 },
                                             { fs_root_, new_path1, new_path2 } };
  for (const auto& data_roots : data_roots_list) {
    opts.data_roots = data_roots;
    ReinitFsManagerWithOpts(opts);
    ASSERT_OK(fs_manager()->Open());
    ASSERT_EQ(data_roots.size() == 3 ? 1 : 0,
              fs_manager()->dd_manager()->GetFailedDirs().size());
  }

  // When we allow ourselves to update the disk instances, each open will
  // update the on-disk layout.
  for (const auto& data_roots : data_roots_list) {
    opts.update_instances = UpdateInstanceBehavior::UPDATE_AND_IGNORE_FAILURES;
    opts.data_roots = data_roots;
    ReinitFsManagerWithOpts(opts);
    ASSERT_OK(fs_manager()->Open());
    ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());

    // Since the on-disk state has been updated, we should be able to open the
    // speculative directory with no issues.
    opts.update_instances = UpdateInstanceBehavior::DONT_UPDATE;
    ReinitFsManagerWithOpts(opts);
    ASSERT_OK(fs_manager()->Open());
    ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());
  }
}

TEST_P(FsManagerTestBase, TestAddRemoveDataDirsFuzz) {
  if (FLAGS_block_manager == "file") {
    GTEST_SKIP() << "Skipping test, file block manager not supported";
  }

#if defined(THREAD_SANITIZER) || defined(ADDRESS_SANITIZER)
  // When using a sanitizer, reduce the loop times to get a more stable result.
  const int kNumAttempts = 10;
#else
  // In some situations, the tests would take too long, so we reduce number of times the loop
  // runs if slow tests are not allowed. Since logr will open multiple RocksDB instances and it
  // takes longer than log block manager, it will only run the loop 100 times if slow
  // tests are allowed.
  const int kNumAttempts = AllowSlowTests() ? (FLAGS_block_manager == "logr" ? 25 : 1000) : 10;
#endif

#if !defined(NO_ROCKSDB)
  // In case of the "logr" block manager, it's quite expensive to run paranoid
  // checks, allocate big RocksDB memtables, spawn many threads, and update
  // compaction stats on startup just to do that again next iteration
  // when almost no data is being written. To speed up the test, let's change
  // a few configuration settings to speed up this test scenario while still
  // having a meaningful configuration for the embedded RocksDB instance.
  FLAGS_log_container_rdb_paranoid_checks = false;
  FLAGS_log_container_rdb_skip_stats_update_on_db_open = true;
  FLAGS_log_container_rdb_max_background_jobs = 2;
  FLAGS_log_container_rdb_write_buffer_size = 1 << 20;
#endif // #if !defined(NO_ROCKSDB)

  Random rng_(SeedRandom());

  FsManagerOpts fs_opts;
  fs_opts.wal_root = fs_root_;
  fs_opts.data_roots = { fs_root_ };
  for (int i = 0; i < kNumAttempts; i++) {
    // Randomly create a directory to add, or choose an existing one to remove.
    //
    // Note: we skip removing the last data directory because the FsManager
    // treats that as a signal to use the wal root as the sole data root.
    vector<string> old_data_roots = fs_opts.data_roots;
    bool action_was_add;
    string fs_root;
    if (rng_.Uniform(2) == 0 || fs_opts.data_roots.size() == 1) {
      action_was_add = true;
      fs_root = GetTestPath(Substitute("new_data_$0", i));
      fs_opts.data_roots.emplace_back(fs_root);
    } else {
      action_was_add = false;
      DCHECK_GT(fs_opts.data_roots.size(), 1);
      int removed_idx = rng_.Uniform(fs_opts.data_roots.size());
      fs_root = fs_opts.data_roots[removed_idx];
      auto iter = fs_opts.data_roots.begin();
      std::advance(iter, removed_idx);
      fs_opts.data_roots.erase(iter);
    }

    // Try to add or remove it with failure injection enabled.
    SCOPED_LOG_TIMING(INFO, Substitute("$0ing $1", action_was_add ? "add" : "remov", fs_root));
    bool update_succeeded;
    {
      google::FlagSaver saver;
      FLAGS_crash_on_eio = false;

      // This value isn't arbitrary: most attempts fail and only some succeed.
      FLAGS_env_inject_eio = 0.01;

      ReinitFsManagerWithOpts(fs_opts);
      update_succeeded = fs_manager()->Open().ok();
    }

    // Reopen regardless, to ensure that failures didn't corrupt anything.
    ReinitFsManagerWithOpts(fs_opts);
    Status open_status = fs_manager()->Open();
    if (update_succeeded) {
      ASSERT_OK(open_status);
    }

    // The rollback logic built into the update operation isn't robust enough
    // to handle every possible sequence of injected failures. Let's see if we
    // need to apply a "workaround" in order to fix the filesystem.

    if (!open_status.ok()) {
      // Perhaps a new fs root and data directory were created, but there was
      // an injected failure later on, which led to a rollback and the removal
      // of the new fs instance file.
      //
      // Fix this as a user might (by copying the original fs instance file
      // into the new fs root) then retry.
      string source_instance = fs_manager()->GetInstanceMetadataPath(fs_root_);
      bool is_dir;
      Status s = GetEnv()->IsDirectory(fs_root, &is_dir);
      if (s.ok()) {
        ASSERT_TRUE(is_dir);
        string new_instance = fs_manager()->GetInstanceMetadataPath(fs_root);
        if (!GetEnv()->FileExists(new_instance)) {
          WritableFileOptions wr_opts;
          wr_opts.mode = Env::MUST_CREATE;
          ASSERT_OK(env_util::CopyFile(GetEnv(), source_instance, new_instance, wr_opts));
          ReinitFsManagerWithOpts(fs_opts);
          open_status = fs_manager()->Open();
        }
      }
    }
    if (!open_status.ok()) {
      // Still failing? Unfortunately, there's not enough information to know
      // whether the injected failure occurred during the update or just
      // afterwards, when the DataDirManager reloaded the data directory
      // instance files. If the former, the failure should resolve itself if we
      // restore the old data roots.
      fs_opts.data_roots = old_data_roots;
      ReinitFsManagerWithOpts(fs_opts);
      open_status = fs_manager()->Open();
    }
    if (!open_status.ok()) {
      // We're still failing? Okay, there's only one legitimate case left, and
      // that's if an error was injected during the update of existing data
      // directory instance files AND during the restoration phase of cleanup.
      //
      // Fix this as a user might (by completing the restoration phase
      // manually), then retry.
      ASSERT_TRUE(open_status.IsIOError());
      bool repaired = false;
      for (const auto& root : fs_opts.data_roots) {
        string data_dir = JoinPathSegments(root, kDataDirName);
        string instance = JoinPathSegments(data_dir,
                                           kInstanceMetadataFileName);
        ASSERT_TRUE(GetEnv()->FileExists(instance));
        string copy = instance + kTmpInfix;
        if (GetEnv()->FileExists(copy)) {
          ASSERT_OK(GetEnv()->RenameFile(copy, instance));
          repaired = true;
        }
      }
      if (repaired) {
        ReinitFsManagerWithOpts(fs_opts);
        open_status = fs_manager()->Open();
      }
    }

    // We've exhausted all of our manual repair options; if this still fails,
    // something else is wrong.
    ASSERT_OK(open_status);
  }
}

TEST_P(FsManagerTestBase, TestAncillaryDirsReported) {
  FsManagerOpts opts;
  opts.wal_root = GetTestPath("wal");
  opts.data_roots = { GetTestPath("data") };
  opts.metadata_root = GetTestPath("metadata");
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  FsReport report;
  ASSERT_OK(fs_manager()->Open(&report));
  string report_str = report.ToString();
  ASSERT_STR_CONTAINS(report_str, "wal directory: " + opts.wal_root);
  ASSERT_STR_CONTAINS(report_str, "metadata directory: " + opts.metadata_root);
}

// Regression test for KUDU-3522.
TEST_P(FsManagerTestBase, TestFailToStartWithoutEncryptionKeys) {
  if (!FLAGS_encrypt_data_at_rest) {
    // Skipping this test if encryption is not enabled.
    GTEST_SKIP();
  }
  // Disable encryption while creating file system.
  FLAGS_encrypt_data_at_rest = false;
  const auto& path = GetTestPath("unencrypted");
  ReinitFsManagerWithPaths(path, { path });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());

  // Re-enable encryption and attempt to open the FS.
  FLAGS_encrypt_data_at_rest = true;
  ASSERT_TRUE(fs_manager()->Open().IsIllegalState());
}

#if !defined(NO_ROCKSDB)
TEST_P(FsManagerTestBase, TestOpenDirectoryWithRdbMissing) {
  if (FLAGS_block_manager != "logr") {
    GTEST_SKIP() << "Skipping 'logr'-specific test";
  }

  // Add a new data dir.
  const string new_path = GetTestPath("new_path");
  FsManagerOpts opts;
  opts.wal_root = fs_root_;
  opts.data_roots = { fs_root_, new_path };
  ReinitFsManagerWithOpts(opts);
  // Opening the fs manager succeeds, both of the 2 dirs are healthy.
  ASSERT_OK(fs_manager()->Open());
  ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
  ASSERT_EQ(0, fs_manager()->dd_manager()->GetFailedDirs().size());

  // Write some data and reopen the fs manager, then some *.sst files will be generated.
  for (int i = 0; i < 1000; i++) {
    TestReadWriteDataFile(Slice("test0"));
  }
  ReinitFsManagerWithOpts(opts);
  ASSERT_OK(fs_manager()->Open());

  // 1. Corrupt the content of the RocksDB directory (by removing one *.sst file) in 'new_path'.
  {
    const auto rdb_dir = JoinPathSegments(JoinPathSegments(new_path, kDataDirName),
                                          kRocksDBDirName);
    std::vector<std::string> children;
    ASSERT_OK(GetEnv()->GetChildren(rdb_dir, &children));
    ASSERT_FALSE(children.empty());
    std::vector<std::string> sst_files;
    for (const auto& child : children) {
      if (HasSuffixString(child, ".sst")) {
        sst_files.push_back(child);
      }
    }
    ASSERT_FALSE(sst_files.empty());
    // Delete a *.sst file to corrupt the RocksDB.
    ASSERT_OK(GetEnv()->DeleteFile(JoinPathSegments(rdb_dir, sst_files[0])));

    ReinitFsManagerWithOpts(opts);
    // Opening the fs manager succeeds, but 1 dir is failed with RocksDB related error.
    ASSERT_OK(fs_manager()->Open());
    ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
    ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());
    const auto uuid_idx = *(fs_manager()->dd_manager()->GetFailedDirs().begin());
    const auto* failed_dir = fs_manager()->dd_manager()->FindDirByUuidIndex(uuid_idx);
    ASSERT_STR_CONTAINS(failed_dir->dir(), new_path);
    ASSERT_STR_CONTAINS(failed_dir->instance()->health_status().ToString(),
                        "Corruption: IO error: No such file or directory: "
                        "While open a file for random read");
  }

  // 2. Remove all the content under RocksDB top-level directory in 'new_path'.
  {
    const auto rdb_dir = JoinPathSegments(JoinPathSegments(new_path, kDataDirName),
                                          kRocksDBDirName);
    ASSERT_OK(GetEnv()->DeleteRecursively(rdb_dir));
    ASSERT_OK(GetEnv()->CreateDir(rdb_dir));
    ReinitFsManagerWithOpts(opts);
    // Opening the fs manager succeeds, but 1 dir is failed with RocksDB related error.
    ASSERT_OK(fs_manager()->Open());
    ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
    ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());
    const auto uuid_idx = *(fs_manager()->dd_manager()->GetFailedDirs().begin());
    const auto* failed_dir = fs_manager()->dd_manager()->FindDirByUuidIndex(uuid_idx);
    ASSERT_STR_CONTAINS(failed_dir->dir(), new_path);
    ASSERT_STR_CONTAINS(failed_dir->instance()->health_status().ToString(),
                        "rdb/CURRENT: does not exist (create_if_missing is false)");
  }

  // 3. Remove the RocksDB top-level directory in 'new_path'.
  {
    const auto rdb_dir = JoinPathSegments(JoinPathSegments(new_path, kDataDirName),
                                          kRocksDBDirName);
    ASSERT_OK(GetEnv()->DeleteRecursively(rdb_dir));
    ReinitFsManagerWithOpts(opts);
    // Opening the fs manager succeeds, but 1 dir is failed with RocksDB related error.
    ASSERT_OK(fs_manager()->Open());
    ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
    ASSERT_EQ(1, fs_manager()->dd_manager()->GetFailedDirs().size());
    const auto uuid_idx = *(fs_manager()->dd_manager()->GetFailedDirs().begin());
    const auto* failed_dir = fs_manager()->dd_manager()->FindDirByUuidIndex(uuid_idx);
    ASSERT_STR_CONTAINS(failed_dir->dir(), new_path);
    ASSERT_STR_CONTAINS(failed_dir->instance()->health_status().ToString(),
                        "rdb/CURRENT: does not exist (create_if_missing is false)");
  }

  // 4. Remove the RocksDB top-level directory in 'fs_root_' as well.
  {
    const auto rdb_dir = JoinPathSegments(JoinPathSegments(fs_root_, kDataDirName),
                                          kRocksDBDirName);
    ASSERT_OK(GetEnv()->DeleteRecursively(rdb_dir));
    ReinitFsManagerWithOpts(opts);
    // Opening the fs manager failed, both of the 2 dirs are failed with RocksDB related error.
    const auto s = fs_manager()->Open();
    ASSERT_TRUE(s.IsIOError()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "All data dirs failed to open");
    ASSERT_EQ(2, fs_manager()->dd_manager()->GetDirs().size());
    ASSERT_EQ(2, fs_manager()->dd_manager()->GetFailedDirs().size());
    for (const auto &uuid_idx : fs_manager()->dd_manager()->GetFailedDirs()) {
      const auto* failed_dir = fs_manager()->dd_manager()->FindDirByUuidIndex(uuid_idx);
      ASSERT_STR_CONTAINS(failed_dir->instance()->health_status().ToString(),
                          "rdb/CURRENT: does not exist (create_if_missing is false)");
    }
  }
}

// This test is similar to FsManagerTestBase.TestCannotUseNonEmptyFsRoot,
// but this one is 'logr'-specific.
TEST_P(FsManagerTestBase, TestInitialOpenDirectoryWithRdbPresent) {
  if (FLAGS_block_manager != "logr") {
    GTEST_SKIP() << "Skipping 'logr'-specific test";
  }

  // Use a new data dir.
  const auto& new_path = GetTestPath("new_path");
  ReinitFsManagerWithPaths(new_path, { new_path });

  // Create the RocksDB dir before opening.
  const auto rdb_dir = JoinPathSegments(new_path, kDataDirName);
  ASSERT_OK(GetEnv()->CreateDir(new_path));
  ASSERT_OK(GetEnv()->CreateDir(rdb_dir));
  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.error_if_exists = true;
  rocksdb::DB* db = nullptr;
  ASSERT_OK(FromRdbStatus(rocksdb::DB::Open(opts, rdb_dir, &db)));
  delete db;

  auto s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_TRUE(s.IsAlreadyPresent()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "FSManager roots already exist");
}
#endif

class OpenFsTypeTest : public KuduTest,
                       public ::testing::WithParamInterface<std::tuple<string, bool, bool>> {
 public:
  OpenFsTypeTest()
     : fs_root_(GetTestPath("fs_root")) {
  }

  void SetUp() override {
    KuduTest::SetUp();
    FLAGS_block_manager = std::get<0>(GetParam());
    FLAGS_encrypt_data_at_rest = std::get<1>(GetParam());
    FLAGS_enable_multi_tenancy = std::get<2>(GetParam());

    // '--enable_multi_tenancy' should set with '--encrypt_data_at_rest'.
    if (!FLAGS_encrypt_data_at_rest && FLAGS_enable_multi_tenancy) {
      GTEST_SKIP();
    }

    FsManagerOpts opts;
    opts.wal_root = GetTestPath("wal");
    opts.data_roots = { GetTestPath("data") };
    opts.metadata_root = GetTestPath("metadata");
    ReinitFsManagerWithOpts(opts);

    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
  }

  FsManager *fs_manager() const { return fs_manager_.get(); }

  static bool is_multi_tenancy_enable() {
    return FLAGS_encrypt_data_at_rest && FLAGS_enable_multi_tenancy;
  }

  static bool is_only_encryption_enable() {
    return FLAGS_encrypt_data_at_rest && !FLAGS_enable_multi_tenancy;
  }

  static bool is_encryption_disable() {
    return !FLAGS_encrypt_data_at_rest && !FLAGS_enable_multi_tenancy;
  }

 protected:
  void ReinitFsManagerWithPaths(string wal_path, vector<string> data_paths) {
    FsManagerOpts opts;
    opts.wal_root = std::move(wal_path);
    opts.data_roots = std::move(data_paths);
    ReinitFsManagerWithOpts(std::move(opts));
  }

  void ReinitFsManagerWithOpts(FsManagerOpts opts) {
    fs_manager_.reset(new FsManager(env_, std::move(opts)));
  }

 private:
  const string fs_root_;
  unique_ptr<FsManager> fs_manager_;
};

INSTANTIATE_TEST_SUITE_P(, OpenFsTypeTest,
                         ::testing::Combine(
                             ::testing::ValuesIn(BlockManager::block_manager_types()),
                             ::testing::Bool() /* --encrypt_data_at_rest */,
                             ::testing::Bool() /* --enable_multi_tenancy */));

TEST_P(OpenFsTypeTest, TestDifferentTypesOpen) {
  if (is_encryption_disable()) {
    ASSERT_TRUE(fs_manager()->server_key().empty());
    ASSERT_FALSE(fs_manager()->is_tenants_exist());
  }

  if (is_only_encryption_enable()) {
    ASSERT_FALSE(fs_manager()->server_key().empty());
    ASSERT_FALSE(fs_manager()->is_tenants_exist());
  }

  if (is_multi_tenancy_enable()) {
    // This isn't upgrade case, so no server key exist.
    ASSERT_TRUE(fs_manager()->server_key().empty());
    ASSERT_TRUE(fs_manager()->is_tenants_exist());
  }
}

} // namespace fs
} // namespace kudu
