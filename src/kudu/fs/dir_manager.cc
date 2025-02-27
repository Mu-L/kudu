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

#include "kudu/fs/dir_manager.h"

#include <algorithm>
#include <cerrno>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(NO_ROCKSDB)
#include <gflags/gflags.h>
#else
#include <gflags/gflags_declare.h>
#endif
#include <glog/logging.h>
#if !defined(NO_ROCKSDB)
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/status.h>
#include <rocksdb/table.h>
#endif

#include "kudu/fs/dir_util.h"
#include "kudu/fs/fs.pb.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#if !defined(NO_ROCKSDB)
#include "kudu/util/flag_tags.h"
#endif
#include "kudu/util/oid_generator.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/random_util.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/stopwatch.h"
#if !defined(NO_ROCKSDB)
#include "kudu/util/test_util_prod.h"
#endif
#include "kudu/util/threadpool.h"

using std::set;
using std::shared_lock;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using strings::Substitute;

DECLARE_int32(fs_data_dirs_available_space_cache_seconds);
DECLARE_int64(fs_data_dirs_reserved_bytes);
DECLARE_string(block_manager);

#if !defined(NO_ROCKSDB)
DEFINE_double(log_container_rdb_bits_per_key, 9.9,
              "Average number of bits allocated per key in RocksDB bloom filter, for details see "
              "https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter. It is only effective "
              "when --block_manager='logr'");
TAG_FLAG(log_container_rdb_bits_per_key, advanced);
TAG_FLAG(log_container_rdb_bits_per_key, experimental);

DEFINE_uint32(log_container_rdb_block_cache_capacity_mb, 10,
              "The block cache capacity of RocksDB in MiB, it is shared by all RocksDB instances "
              "in the process. It is only effective when --block_manager='logr'");
TAG_FLAG(log_container_rdb_block_cache_capacity_mb, advanced);
TAG_FLAG(log_container_rdb_block_cache_capacity_mb, experimental);

DEFINE_uint32(log_container_rdb_max_background_jobs, 8,
              "The maximum number of concurrent background jobs (compactions and flushes) shared "
              "between RocksDB instances. It is only effective when --block_manager='logr'");
TAG_FLAG(log_container_rdb_max_background_jobs, advanced);
TAG_FLAG(log_container_rdb_max_background_jobs, experimental);

DEFINE_uint32(log_container_rdb_max_write_buffer_number, 2,
              "The maximum number of write buffers that are built up in memory of each RocksDB "
              "instance. It is only effective when --block_manager='logr'");
TAG_FLAG(log_container_rdb_max_write_buffer_number, advanced);
TAG_FLAG(log_container_rdb_max_write_buffer_number, experimental);

DEFINE_double(log_container_rdb_memtable_prefix_bloom_size_ratio, 0.1,
              "Enables a dynamic bloom filter of RocksDB memtable to optimize many queries that "
              "must go beyond the memtable if it is larger than 0. The size in bytes of the filter "
              "is --log_container_rdb_write_buffer_size * "
              "--log_container_rdb_memtable_prefix_bloom_size_ratio. It is only effective when "
              "--block_manager='logr'");
TAG_FLAG(log_container_rdb_memtable_prefix_bloom_size_ratio, advanced);
TAG_FLAG(log_container_rdb_memtable_prefix_bloom_size_ratio, experimental);

DEFINE_uint64(log_container_rdb_write_buffer_size, 64 << 20,
              "The amount of data in RocksDB to build up in memory (backed by an unsorted log on "
              "disk) before converting to a sorted on-disk file. It is only effective when "
              "--block_manager='logr'");
TAG_FLAG(log_container_rdb_write_buffer_size, advanced);
TAG_FLAG(log_container_rdb_write_buffer_size, experimental);

DEFINE_string(log_container_rdb_db_log_dir, "",
              "This specifies the info log dir of RocksDB. If it is empty, the log files are in "
              "the same dir as data (i.e. each dir in --fs_data_dirs). If it is not empty, the log "
              "files will be in the specified dir, and the --fs_data_dirs absolute path will "
              "be used as the log file name's prefixes. It is only effective when "
              "--block_manager='logr'");
TAG_FLAG(log_container_rdb_db_log_dir, advanced);
TAG_FLAG(log_container_rdb_db_log_dir, experimental);

DEFINE_uint64(log_container_rdb_max_log_file_size, 8 << 20,
              "Maximum byte size of the RocksDB info log file. If the log file is larger "
              "than specified, a new info log file will be created. If it is 0, all logs will be "
              "written to one log file. It is only effective when --block_manager='logr'");
TAG_FLAG(log_container_rdb_max_log_file_size, advanced);
TAG_FLAG(log_container_rdb_max_log_file_size, experimental);

DEFINE_uint64(log_container_rdb_keep_log_file_num, 10,
              "Maximum number of RocksDB info log files to keep. It is only effective when "
              "--block_manager='logr'");
TAG_FLAG(log_container_rdb_keep_log_file_num, advanced);
TAG_FLAG(log_container_rdb_keep_log_file_num, experimental);

DEFINE_uint64(log_container_rdb_max_manifest_file_size, 64 << 20,
              "The RocksDB manifest file is rolled over on reaching this byte limit. It is only "
              "effective when --block_manager='logr'");
TAG_FLAG(log_container_rdb_max_manifest_file_size, advanced);
TAG_FLAG(log_container_rdb_max_manifest_file_size, experimental);

DEFINE_int32(log_container_rdb_level0_file_num_compaction_trigger, 4,
             "Number of files to trigger level-0 compaction in RocksDB. A value <0 means that "
             "level-0 compaction will not be triggered by the number of files at all. It is only "
             "effective when --block_manager='logr'");
TAG_FLAG(log_container_rdb_level0_file_num_compaction_trigger, advanced);
TAG_FLAG(log_container_rdb_level0_file_num_compaction_trigger, experimental);

DEFINE_bool(log_container_rdb_paranoid_checks, true,
            "If true, RocksDB aggressively checks consistency of its data. "
            "Effective only when --block_manager='logr'");
TAG_FLAG(log_container_rdb_paranoid_checks, advanced);
TAG_FLAG(log_container_rdb_paranoid_checks, experimental);

DEFINE_bool(log_container_rdb_skip_stats_update_on_db_open, false,
            "Whether to skip updating the RocksDB's stats for compaction "
            "decision upon startup. Effective only when --block_manager='logr'");
TAG_FLAG(log_container_rdb_skip_stats_update_on_db_open, advanced);
TAG_FLAG(log_container_rdb_skip_stats_update_on_db_open, experimental);
#endif

namespace kudu {
namespace {
// Wrapper for env_util::DeleteTmpFilesRecursively that is suitable for parallel
// execution on a data directory's thread pool (which requires the return value
// be void).
void DeleteTmpFilesRecursively(Env* env, const string& path) {
  WARN_NOT_OK(env_util::DeleteTmpFilesRecursively(env, path),
              "Error while deleting temp files");
}
} // anonymous namespace

namespace fs {
#if !defined(NO_ROCKSDB)
Status FromRdbStatus(const rocksdb::Status& s) {
  switch (s.code()) {
    case rocksdb::Status::kOk:
      return Status::OK();
    case rocksdb::Status::kNotFound:
      return Status::NotFound(s.ToString());
    case rocksdb::Status::kCorruption:
      return Status::Corruption(s.ToString());
    case rocksdb::Status::kNotSupported:
      return Status::NotSupported(s.ToString());
    case rocksdb::Status::kInvalidArgument:
      return Status::InvalidArgument(s.ToString());
    case rocksdb::Status::kIOError:
      return Status::IOError(s.ToString());
    case rocksdb::Status::kIncomplete:
      return Status::Incomplete(s.ToString());
    case rocksdb::Status::kAborted:
      return Status::Aborted(s.ToString());
    default:
      return Status::RuntimeError(s.ToString());
  }
}
#endif

Dir::Dir(Env* env,
         DirMetrics* metrics,
         FsType fs_type,
         string dir,
         unique_ptr<DirInstanceMetadataFile> metadata_file,
         unique_ptr<ThreadPool> pool)
    : env_(env),
      metrics_(metrics),
      fs_type_(fs_type),
      dir_(std::move(dir)),
      metadata_file_(std::move(metadata_file)),
      pool_(std::move(pool)),
      is_shutdown_(false),
      is_full_(false),
      available_bytes_(0) {
}

Dir::~Dir() {
  Shutdown();
}

void Dir::Shutdown() {
  if (is_shutdown_) {
    return;
  }

  WaitOnClosures();
  pool_->Shutdown();
  is_shutdown_ = true;
}

void Dir::ExecClosure(const std::function<void()>& task) {
  Status s = pool_->Submit(task);
  if (!s.ok()) {
    WARN_NOT_OK(
        s, "Could not submit task to thread pool, running it synchronously");
    task();
  }
}

void Dir::WaitOnClosures() {
  pool_->Wait();
}

Status Dir::RefreshAvailableSpace(RefreshMode mode) {
  switch (mode) {
    case RefreshMode::EXPIRED_ONLY: {
      std::lock_guard l(lock_);
      DCHECK(last_space_check_.Initialized());
      MonoTime expiry = last_space_check_ + MonoDelta::FromSeconds(
          available_space_cache_secs());
      if (MonoTime::Now() < expiry) {
        break;
      }
      [[fallthrough]]; // Root was previously full, check again.
    }
    case RefreshMode::ALWAYS: {
      int64_t available_bytes_new;
      Status s = env_util::VerifySufficientDiskSpace(
          env_, dir_, 0, reserved_bytes(), &available_bytes_new);
      bool is_full_new;
      if (PREDICT_FALSE(s.IsIOError() && s.posix_code() == ENOSPC)) {
        LOG(WARNING) << Substitute(
            "Insufficient disk space under path $0: will retry after $1 seconds: $2",
            dir_, available_space_cache_secs(), s.ToString());
        s = Status::OK();
        is_full_new = true;
      } else {
        is_full_new = false;
      }
      RETURN_NOT_OK_PREPEND(s, "Could not refresh fullness"); // Catch other types of IOErrors, etc.
      {
        std::lock_guard l(lock_);
        if (metrics_ && is_full_ != is_full_new) {
          metrics_->dirs_full->IncrementBy(is_full_new ? 1 : -1);
        }
        is_full_ = is_full_new;
        last_space_check_ = MonoTime::Now();
        available_bytes_ = available_bytes_new;
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown check mode";
  }
  return Status::OK();
}

int Dir::available_space_cache_secs() {
  return FLAGS_fs_data_dirs_available_space_cache_seconds;
}

int Dir::reserved_bytes() {
  return FLAGS_fs_data_dirs_reserved_bytes;
}

#if !defined(NO_ROCKSDB)
shared_ptr<rocksdb::Cache> RdbDir::s_block_cache_;
RdbDir::RdbDir(Env* env, DirMetrics* metrics,
               FsType fs_type,
               bool newly_created,
               string dir,
               unique_ptr<DirInstanceMetadataFile> metadata_file,
               unique_ptr<ThreadPool> pool)
    : Dir(env, metrics, fs_type, std::move(dir), std::move(metadata_file), std::move(pool)) {
  if (!metadata_file_->healthy()) {
    LOG(WARNING) << Substitute("Skip initializing rocksdb instance for the non-healthy "
                               "directory $0",
                               dir_);
    return;
  }

  // Initialize the directory only if it's healthy.
  // Note: the unhealthy directories will be kept, but will be skipped when opening block manager.
  auto s = InitRocksDBInstance(newly_created);
  if (!s.ok()) {
    s = s.CloneAndPrepend(Substitute("could not initialize $0", dir_));
    LOG(WARNING) << s.ToString();
    // Mark the directory as failed if it could not be initialized.
    DCHECK(metadata_file_->healthy());
    metadata_file_->SetInstanceFailed(s);
  }
}

Status RdbDir::InitRocksDBInstance(bool newly_created) {
  DCHECK_STREQ(FLAGS_block_manager.c_str(), "logr");
  if (db_) {
    // Some unit tests (e.g. BlockManagerTest.PersistenceTest) reopen the block manager,
    // 'db_' is possible to be non-nullptr.
    // In non-test environments, 'db_' is always to be nullptr, log a fatal error if not.
    DCHECK(IsGTest()) <<
        Substitute("It's not allowed to reopen the RocksDB $0 except in tests", dir_);
    return Status::OK();
  }

  // See the rocksdb::Options details:
  // https://github.com/facebook/rocksdb/blob/main/include/rocksdb/options.h
  rocksdb::Options opts;
  // A RocksDB instance is created if it does not exist when opening the Dir.
  if (newly_created) {
      opts.create_if_missing = true;
      opts.error_if_exists = true;
  } else {
      opts.create_if_missing = false;
      opts.error_if_exists = false;
  }
  opts.db_log_dir = FLAGS_log_container_rdb_db_log_dir;
  opts.max_log_file_size = FLAGS_log_container_rdb_max_log_file_size;
  opts.keep_log_file_num = FLAGS_log_container_rdb_keep_log_file_num;
  opts.write_buffer_size = FLAGS_log_container_rdb_write_buffer_size;
  opts.max_write_buffer_number = FLAGS_log_container_rdb_max_write_buffer_number;
  opts.max_background_jobs = FLAGS_log_container_rdb_max_background_jobs;
  opts.max_manifest_file_size = FLAGS_log_container_rdb_max_manifest_file_size;
  opts.level0_file_num_compaction_trigger =
      FLAGS_log_container_rdb_level0_file_num_compaction_trigger;
  opts.paranoid_checks = FLAGS_log_container_rdb_paranoid_checks;
  opts.skip_stats_update_on_db_open =
      FLAGS_log_container_rdb_skip_stats_update_on_db_open;

  static std::once_flag flag;
  std::call_once(flag, [&]() {
    s_block_cache_ = rocksdb::NewLRUCache(FLAGS_log_container_rdb_block_cache_capacity_mb << 20);
  });
  rocksdb::BlockBasedTableOptions tbl_opts;
  tbl_opts.block_cache = s_block_cache_;
  tbl_opts.whole_key_filtering = false;
  tbl_opts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(FLAGS_log_container_rdb_bits_per_key));
  opts.table_factory.reset(NewBlockBasedTableFactory(tbl_opts));
  // Take advantage of Prefix-Seek, see https://github.com/facebook/rocksdb/wiki/Prefix-Seek.
  opts.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(ObjectIdGenerator::IdLength()));
  opts.memtable_prefix_bloom_size_ratio = FLAGS_log_container_rdb_memtable_prefix_bloom_size_ratio;

  rdb_dir_ = JoinPathSegments(dir_, kRocksDBDirName);
  rocksdb::DB* db_temp = nullptr;
  rocksdb::Status rdb_s = rocksdb::DB::Open(opts, *rdb_dir_, &db_temp);
  RETURN_NOT_OK_PREPEND(FromRdbStatus(rdb_s),
                        Substitute("open RocksDB failed, path: $0", *rdb_dir_));
  db_.reset(db_temp);
  return Status::OK();
}

void RdbDir::Shutdown() {
  if (is_shutdown_) {
    return;
  }

  // Shut down the thread pool before closing RocksDB to make sure there aren't any in-flight
  // write operations.
  WaitOnClosures();
  pool_->Shutdown();

  // The 'db_' is nullptr if the Dir open failed.
  if (db_) {
    // Flushing memtable before closing RocksDB reduces bootstrapping time upon next start-up.
    // Call Flush() rather than WaitForCompact(), because it's enough to wait for the flush jobs
    // to finish, compactions jobs may take more time, which results in longer times to shut down
    // a server.
    rocksdb::FlushOptions options;
    options.wait = true;
    WARN_NOT_OK(FromRdbStatus(db_->Flush(options)),
                Substitute("Flush RocksDB failed, path: $0", *rdb_dir_));
    WARN_NOT_OK(FromRdbStatus(db_->Close()),
                Substitute("Closed RocksDB with error, path: $0", *rdb_dir_));
    db_.reset();
  }

  is_shutdown_ = true;
}

rocksdb::DB* RdbDir::rdb() {
  DCHECK_STREQ(FLAGS_block_manager.c_str(), "logr");
  DCHECK(db_);
  return db_.get();
}
#endif

DirManagerOptions::DirManagerOptions(string dir_type,
                                     string tid)
    : dir_type(std::move(dir_type)),
      tenant_id(std::move(tid)),
      read_only(false),
      update_instances(UpdateInstanceBehavior::UPDATE_AND_IGNORE_FAILURES) {}


vector<string> DirManager::GetRootNames(const CanonicalizedRootsList& root_list) {
  vector<string> roots;
  std::transform(root_list.begin(), root_list.end(), std::back_inserter(roots),
    [&] (const CanonicalizedRootAndStatus& r) { return r.path; });
  return roots;
}

vector<string> DirManager::GetRoots() const {
  return GetRootNames(canonicalized_fs_roots_);
}

vector<string> DirManager::GetDirs() const {
  return JoinPathSegmentsV(GetRoots(), dir_name());
}

DirManager::DirManager(Env* env,
                       unique_ptr<DirMetrics> dir_metrics,
                       int num_threads_per_dir,
                       const DirManagerOptions& opts,
                       CanonicalizedRootsList canonicalized_data_roots)
    : env_(env),
      num_threads_per_dir_(num_threads_per_dir),
      opts_(opts),
      canonicalized_fs_roots_(std::move(canonicalized_data_roots)),
      metrics_(std::move(dir_metrics)),
      rng_(GetRandomSeed32()) {
  DCHECK_GT(canonicalized_fs_roots_.size(), 0);
  DCHECK(opts_.update_instances == UpdateInstanceBehavior::DONT_UPDATE || !opts_.read_only);
  DCHECK(!opts_.dir_type.empty());
}

DirManager::~DirManager() {
  Shutdown();
}

void DirManager::WaitOnClosures() {
  for (const auto& dir : dirs_) {
    dir->WaitOnClosures();
  }
}

void DirManager::Shutdown() {
  // We may be waiting here for a while on outstanding closures.
  LOG_SLOW_EXECUTION(INFO, 1000,
                     Substitute("waiting on $0 block manager thread pools",
                                dirs_.size())) {
    for (const auto& dir : dirs_) {
      dir->Shutdown();
    }
  }
}

Status DirManager::Create() {
  CHECK(!opts_.read_only);

  vector<string> all_uuids;
  for (const auto& r : canonicalized_fs_roots_) {
    RETURN_NOT_OK_PREPEND(r.status, "Could not create directory manager with disks failed");
  }
  vector<unique_ptr<DirInstanceMetadataFile>> loaded_instances;
  bool has_healthy_instances = true;
  RETURN_NOT_OK(LoadInstances(&loaded_instances, &has_healthy_instances));
  if (has_healthy_instances) {
    return Status::AlreadyPresent("instance files already exist");
  }

  // If none of the instances exist, we can assume this is a new deployment and
  // we should try creating some a new set of instance files.
  RETURN_NOT_OK_PREPEND(CreateNewDirectoriesAndUpdateInstances(std::move(loaded_instances)),
                        "could not create new data directories");
  return Status::OK();
}

Status DirManager::CreateNewDirectoriesAndUpdateInstances(
    vector<unique_ptr<DirInstanceMetadataFile>> instances) {
  CHECK(!opts_.read_only);
  CHECK_NE(UpdateInstanceBehavior::DONT_UPDATE, opts_.update_instances);

  vector<string> created_dirs;
  vector<string> created_files;
  auto deleter = MakeScopedCleanup([&]() {
    // Delete files first so that the directories will be empty when deleted.
    for (const auto& f : created_files) {
      WARN_NOT_OK(env_->DeleteFile(f), "Could not delete file " + f);
    }
    // Delete directories in reverse order since parent directories will have
    // been added before child directories.
    for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); it++) {
      WARN_NOT_OK(env_->DeleteDir(*it), "Could not delete dir " + *it);
    }
  });

  // First, de-duplicate the instance UUIDs. If we have duplicates, something's
  // wrong. Maybe an operator manually duplicated some instance files.
  set<string> all_uuids;
  for (const auto& instance : instances) {
    InsertIfNotPresent(&all_uuids, instance->uuid());
  }
  if (all_uuids.size() != instances.size()) {
    return Status::InvalidArgument(
        Substitute("instance files contain duplicate UUIDs: $0 directories provided, "
                   "$1 unique UUIDs found ($2)", instances.size(), all_uuids.size(),
                   JoinStrings(all_uuids, ", ")));
  }

  // Determine which instance files are healthy (and can thus be updated), and
  // which don't exist. Create any that don't exist.
  //
  // Note: we don't bother trying to create/update the instance if the file is
  // otherwise unhealthy.
  vector<unique_ptr<DirInstanceMetadataFile>> healthy_instances;
  for (auto& instance : instances) {
    if (instance->healthy()) {
      healthy_instances.emplace_back(std::move(instance));
      continue;
    }
    if (instance->health_status().IsNotFound()) {
      bool created_dir = false;
      RETURN_NOT_OK(instance->Create(all_uuids, &created_dir));
      if (created_dir) {
        created_dirs.emplace_back(instance->dir());
      }
      created_files.emplace_back(instance->path());
    }
  }

  // Go through the healthy instances and look for instances that don't have
  // the full complete set of instance UUIDs.
  vector<unique_ptr<DirInstanceMetadataFile>> instances_to_update;
  for (auto& instance : healthy_instances) {
    DCHECK(instance->healthy());
    const auto& dir_set = instance->metadata()->dir_set();
    set<string> instance_uuids;
    for (int i = 0; i < dir_set.all_uuids_size(); i++) {
      InsertIfNotPresent(&instance_uuids, dir_set.all_uuids(i));
    }
    // If an instance file disagrees with the expected UUIDs, rewrite it.
    if (all_uuids != instance_uuids) {
      instances_to_update.emplace_back(std::move(instance));
    }
  }

  // If any of the instance files need to be updated because they didn't match
  // the expected set of UUIDs, update them now.
  // Note: Having a consistent set of instance files isn't a correctness
  // requirement, but it can be useful for degbugging.
  if (!instances_to_update.empty()) {
    RETURN_NOT_OK(UpdateHealthyInstances(instances_to_update, all_uuids));
  }

  // Ensure newly created directories are synchronized to disk.
  if (sync_dirs()) {
    WARN_NOT_OK(env_util::SyncAllParentDirs(env_, created_dirs, created_files),
                "could not sync newly created data directories");
  }

  // Success: don't delete any files.
  deleter.cancel();
  std::move(created_dirs.begin(), created_dirs.end(),
            std::inserter(created_fs_dir_paths_, created_fs_dir_paths_.end()));
  return Status::OK();
}

Status DirManager::UpdateHealthyInstances(
    const vector<unique_ptr<DirInstanceMetadataFile>>& instances_to_update,
    const set<string>& new_all_uuids) {
  unordered_map<string, string> copies_to_restore;
  unordered_set<string> copies_to_delete;
  auto cleanup = MakeScopedCleanup([&] {
    for (const auto& f : copies_to_delete) {
      WARN_NOT_OK(env_->DeleteFile(f), Substitute("Could not delete file $0", f));
    }
    for (const auto& copy_and_original : copies_to_restore) {
      const auto& copy_filename = copy_and_original.first;
      const auto& original_filename = copy_and_original.second;
      WARN_NOT_OK(env_->RenameFile(copy_filename, original_filename),
          Substitute("Could not restore file $0 from $1", original_filename, copy_filename));
    }
  });
  // Make a copy of every existing instance metadata file. This is done before
  // performing any updates, so that if there's a failure while copying,
  // there's no metadata to restore.
  //
  // We'll keep track of the copies so we can delete them on success, or use
  // them to restore on failure.
  WritableFileOptions opts;
  opts.sync_on_close = true;
  for (const auto& instance : instances_to_update) {
    if (!instance->healthy()) {
      continue;
    }
    const string& instance_filename = instance->path();
    string copy_filename = instance_filename + kTmpInfix;
    Status s = env_util::CopyFile(env_, instance_filename, copy_filename, opts);
    if (PREDICT_FALSE(!s.ok())) {
      s = s.CloneAndPrepend("unable to backup existing instance file");
      instance->SetInstanceFailed(s);
      LOG(WARNING) << s.ToString();
      continue;
    }
    InsertOrDie(&copies_to_delete, copy_filename);
  }

  // Update the instance metadata files with the new set of UUIDs.
  for (const auto& instance : instances_to_update) {
    if (!instance->healthy()) {
      continue;
    }
    const string& instance_filename = instance->path();
    string copy_filename = instance_filename + kTmpInfix;

    // Put together the PB and perform the update.
    DirInstanceMetadataPB new_pb = *instance->metadata();
    new_pb.mutable_dir_set()->mutable_all_uuids()->Clear();
    for (const auto& uuid : new_all_uuids) {
      new_pb.mutable_dir_set()->add_all_uuids(uuid);
    }

    // We're about to update the file; if we fail midway, we should try to
    // restore them from our backups if we can.
    InsertOrDie(&copies_to_restore, copy_filename, instance_filename);
    CHECK_EQ(1, copies_to_delete.erase(copy_filename));
    Status s = pb_util::WritePBContainerToPath(
        env_, instance_filename, new_pb, pb_util::OVERWRITE,
        sync_dirs() ? pb_util::SYNC : pb_util::NO_SYNC,
        pb_util::NOT_SENSITIVE);
    // We've failed to update for some reason, so restore our original file.
    // Since we're renaming our copy, we don't have to delete it.
    if (PREDICT_FALSE(!s.ok())) {
      s = s.CloneAndPrepend("unable to update instance file");
      instance->SetInstanceFailed(s);
      LOG(WARNING) << Substitute("unable to overwrite existing instance file $0: $1",
                                 instance_filename, s.ToString());
    }
  }

  // If we are not tolerating errors (e.g. we're running the update_dirs tool)
  // and we've hit an error, return now and clean up what we've changed.
  if (opts_.update_instances == UpdateInstanceBehavior::UPDATE_AND_ERROR_ON_FAILURE) {
    for (const auto& instance : instances_to_update) {
      RETURN_NOT_OK_PREPEND(instance->health_status(),
          "at least one instance file failed to update");
    }
  }

  // Success; we only need to delete our copies.
  InsertKeysFromMap(copies_to_restore, &copies_to_delete);
  copies_to_restore.clear();
  return Status::OK();
}

Status DirManager::LoadInstances(
    vector<unique_ptr<DirInstanceMetadataFile>>* instance_files,
    bool* has_healthy_instances) {
  DCHECK(instance_files);

  LockMode lock_mode;
  if (!lock_dirs()) {
    lock_mode = LockMode::NONE;
  } else if (opts_.read_only) {
    lock_mode = LockMode::OPTIONAL;
  } else {
    lock_mode = LockMode::MANDATORY;
  }
  vector<unique_ptr<DirInstanceMetadataFile>> loaded_instances;
  ObjectIdGenerator gen;
  for (const auto& root : canonicalized_fs_roots_) {
    string dir = JoinPathSegments(root.path, dir_name());
    string instance_filename = JoinPathSegments(dir, instance_metadata_filename());

    // Initialize the instance with a backup UUID. In case the load fails, this
    // will be the UUID for our instance.
    string backup_uuid = gen.Next();
    unique_ptr<DirInstanceMetadataFile> instance(
        new DirInstanceMetadataFile(env_, std::move(backup_uuid), opts_.dir_type,
                                    instance_filename));
    if (PREDICT_FALSE(!root.status.ok())) {
      instance->SetInstanceFailed(root.status);
    } else {
      // This may return OK and mark 'instance' as unhealthy if the file could
      // not be loaded (e.g. not found, disk errors).
      RETURN_NOT_OK_PREPEND(instance->LoadFromDisk(),
                            Substitute("could not load $0", instance_filename));
    }

    // Try locking the instance.
    if (instance->healthy() && lock_mode != LockMode::NONE) {
      // This may return OK and mark 'instance' as unhealthy if the file could
      // not be locked due to non-locking issues (e.g. disk errors).
      Status s = instance->Lock();
      if (!s.ok()) {
        if (lock_mode == LockMode::OPTIONAL) {
          LOG(WARNING) << s.ToString();
          LOG(WARNING) << "Proceeding without lock";
        } else {
          DCHECK(LockMode::MANDATORY == lock_mode);
          return s;
        }
      }
    }
    loaded_instances.emplace_back(std::move(instance));
  }

  int num_healthy_instances = 0;
  for (const auto& instance : loaded_instances) {
    if (instance->healthy()) {
      num_healthy_instances++;
    }
  }
  if (has_healthy_instances) {
    *has_healthy_instances = num_healthy_instances > 0;
  }
  *instance_files = std::move(loaded_instances);
  return Status::OK();
}

Status DirManager::PopulateDirectoryMaps(const vector<unique_ptr<Dir>>& dirs) {
  // Go through our instances and assign them each a UUID index.
  for (int idx = 0; idx < dirs.size(); idx++) {
    Dir* dir = dirs[idx].get();
    InsertToMaps(dir->instance()->uuid(), idx, dir);
  }
  return Status::OK();
}

void DirManager::InsertToMaps(const string& uuid, int idx, Dir* dir) {
  if (!dir->instance()->healthy()) {
    if (metrics_) {
      metrics_->dirs_failed->IncrementBy(1);
    }
    InsertOrDie(&failed_dirs_, idx);
  }
  InsertOrDie(&uuid_by_root_, DirName(dir->dir()), uuid);
  InsertOrDie(&uuid_by_idx_, idx, uuid);
  InsertOrDie(&idx_by_uuid_, uuid, idx);
  InsertOrDie(&dir_by_uuid_idx_, idx, dir);
  InsertOrDie(&uuid_idx_by_dir_, dir, idx);
  InsertOrDie(&tablets_by_uuid_idx_map_, idx, {});
}

Status DirManager::Open() {
  if (canonicalized_fs_roots_.size() > max_dirs()) {
    return Status::InvalidArgument(Substitute("too many directories provided $0, max is $1",
                                              canonicalized_fs_roots_.size(), max_dirs()));
  }

  // Load the instance files from disk.
  bool has_healthy_instances = true;
  vector<unique_ptr<DirInstanceMetadataFile>> loaded_instances;
  RETURN_NOT_OK_PREPEND(LoadInstances(&loaded_instances, &has_healthy_instances),
      "failed to load instance files");
  if (!has_healthy_instances) {
    return Status::NotFound(
        "could not open directory manager, no healthy directories found");
  }
  // Note: the file block manager should not be updated because its block
  // indexing algorithm depends on a fixed set of directories.
  if (!opts_.read_only && opts_.dir_type != "file" &&
      opts_.update_instances != UpdateInstanceBehavior::DONT_UPDATE) {
    RETURN_NOT_OK_PREPEND(
        CreateNewDirectoriesAndUpdateInstances(std::move(loaded_instances)),
        "could not add new directories");
    vector<unique_ptr<DirInstanceMetadataFile>> new_loaded_instances;
    RETURN_NOT_OK_PREPEND(LoadInstances(&new_loaded_instances, &has_healthy_instances),
                          "failed to load instance files after updating");
    if (!has_healthy_instances) {
      return Status::IOError(
          "could not open directory manager, no healthy directories found");
    }
    loaded_instances = std::move(new_loaded_instances);
  }

  // All instances are present and accounted for. Time to create the in-memory
  // directory structures.
  vector<unique_ptr<Dir>> dirs;
  for (int i = 0; i < loaded_instances.size(); i++) {
    auto& instance = loaded_instances[i];
    const string dir = instance->dir();

    // Figure out what filesystem the directory is on.
    FsType fs_type = FsType::OTHER;
    if (instance->healthy()) {
      bool result = false;
      Status fs_check = env_->IsOnExtFilesystem(dir, &result);
      if (fs_check.ok()) {
        if (result) {
          fs_type = FsType::EXT;
        } else {
          fs_check = env_->IsOnXfsFilesystem(dir, &result);
          if (fs_check.ok() && result) {
            fs_type = FsType::XFS;
          }
        }
      }
      // If we hit a disk error, consider the directory failed.
      if (PREDICT_FALSE(fs_check.IsDiskFailure())) {
        instance->SetInstanceFailed(fs_check.CloneAndPrepend("failed to check FS type"));
      } else {
        RETURN_NOT_OK(fs_check);
      }
    }

    // Create a per-dir thread pool.
    unique_ptr<ThreadPool> pool;
    RETURN_NOT_OK(ThreadPoolBuilder(Substitute("dir $0", dir))
                  .set_max_threads(num_threads_per_dir_)
                  .set_trace_metric_prefix("dirs")
                  .Build(&pool));
    unique_ptr<Dir> new_dir = CreateNewDir(env_, metrics_.get(), fs_type, dir,
                                           std::move(instance), std::move(pool));
    dirs.emplace_back(std::move(new_dir));
  }

  // Use the per-dir thread pools to delete temporary files in parallel.
  for (const auto& dir : dirs) {
    if (dir->instance()->healthy()) {
      auto* d = dir.get();
      dir->ExecClosure([this, d]() { DeleteTmpFilesRecursively(this->env_, d->dir()); });
    }
  }
  for (const auto& dir : dirs) {
    dir->WaitOnClosures();
  }

  RETURN_NOT_OK(PopulateDirectoryMaps(dirs));
  dirs_ = std::move(dirs);

  // From this point onwards, the in-memory maps are the source of truth about
  // the state of each dir.

  // Initialize the 'fullness' status of the directories.
  for (const auto& dd : dirs_) {
    int uuid_idx;
    CHECK(FindUuidIndexByDir(dd.get(), &uuid_idx));
    if (ContainsKey(failed_dirs_, uuid_idx)) {
      continue;
    }
    Status refresh_status = dd->RefreshAvailableSpace(Dir::RefreshMode::ALWAYS);
    if (PREDICT_FALSE(!refresh_status.ok())) {
      if (refresh_status.IsDiskFailure()) {
        RETURN_NOT_OK(MarkDirFailed(uuid_idx, refresh_status.ToString()));
        continue;
      }
      return refresh_status;
    }
  }
  return Status::OK();
}

Dir* DirManager::FindDirByUuidIndex(int uuid_idx) const {
  DCHECK_LT(uuid_idx, dirs_.size());
  return FindPtrOrNull(dir_by_uuid_idx_, uuid_idx);
}

Dir* DirManager::FindDirByFullPathForTests(const std::string& full_path) const {
  for (const auto& d : dirs_) {
    if (HasPrefixString(full_path, d->dir())) {
      return d.get();
    }
  }
  return nullptr;
}

bool DirManager::FindUuidIndexByDir(const Dir* dir, int* uuid_idx) const {
  return FindCopy(uuid_idx_by_dir_, dir, uuid_idx);
}

bool DirManager::FindUuidIndexByRoot(const string& root, int* uuid_idx) const {
  string uuid;
  if (FindUuidByRoot(root, &uuid)) {
    return FindUuidIndexByUuid(uuid, uuid_idx);
  }
  return false;
}

bool DirManager::FindUuidIndexByUuid(const string& uuid, int* uuid_idx) const {
  return FindCopy(idx_by_uuid_, uuid, uuid_idx);
}

bool DirManager::FindUuidByRoot(const string& root, string* uuid) const {
  return FindCopy(uuid_by_root_, root, uuid);
}

set<string> DirManager::FindTabletsByDirUuidIdx(int uuid_idx) const {
  DCHECK_LT(uuid_idx, dirs_.size());
  shared_lock lock(dir_group_lock_.get_lock());
  const set<string>* tablet_set_ptr = FindOrNull(tablets_by_uuid_idx_map_, uuid_idx);
  if (tablet_set_ptr) {
    return *tablet_set_ptr;
  }
  return {};
}

void DirManager::MarkDirFailedByUuid(const std::string& uuid) {
  int uuid_idx;
  CHECK(FindUuidIndexByUuid(uuid, &uuid_idx));
  WARN_NOT_OK(MarkDirFailed(uuid_idx), "Failed to handle disk failure");
}

Status DirManager::MarkDirFailed(int uuid_idx, const string& error_message) {
  DCHECK_LT(uuid_idx, dirs_.size());
  std::lock_guard lock(dir_group_lock_);
  Dir* dir = FindDirByUuidIndex(uuid_idx);
  DCHECK(dir);
  if (InsertIfNotPresent(&failed_dirs_, uuid_idx)) {
    if (failed_dirs_.size() == dirs_.size()) {
      // TODO(awong): pass 'error_message' as a Status instead of an string so
      // we can avoid returning this artificial status.
      return Status::IOError(Substitute("All dirs have failed: ", error_message));
    }
    if (metrics_) {
      metrics_->dirs_failed->IncrementBy(1);
    }
    string error_prefix = "";
    if (!error_message.empty()) {
      error_prefix = Substitute("$0: ", error_message);
    }
    LOG(ERROR) << error_prefix << Substitute("Directory $0 marked as failed", dir->dir());
  }
  return Status::OK();
}

bool DirManager::IsDirFailed(int uuid_idx) const {
  DCHECK_LT(uuid_idx, dirs_.size());
  shared_lock lock(dir_group_lock_.get_lock());
  return ContainsKey(failed_dirs_, uuid_idx);
}

bool DirManager::IsTabletInFailedDir(const string& tablet_id) const {
  const set<int> failed_dirs = GetFailedDirs();
  for (int failed_dir : failed_dirs) {
    if (ContainsKey(FindTabletsByDirUuidIdx(failed_dir), tablet_id)) {
      return true;
    }
  }
  return false;
}

} // namespace fs
} // namespace kudu
