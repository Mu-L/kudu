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
//
// A DiskRowSet is a horizontal slice of a Kudu tablet.
// Each DiskRowSet contains data for a a disjoint set of keys.
// See src/kudu/tablet/README for a detailed description.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest_prod.h>

#include "kudu/common/rowid.h"
#include "kudu/common/schema.h"
#include "kudu/fs/block_id.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/delta_key.h"
#include "kudu/tablet/delta_tracker.h"
#include "kudu/tablet/rowset.h"
#include "kudu/tablet/tablet_mem_trackers.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/util/bloom_filter.h"
#include "kudu/util/faststring.h"
#include "kudu/util/locks.h"
#include "kudu/util/make_shared.h"
#include "kudu/util/status.h"

namespace kudu {

class MemTracker;
class MonoTime;
class RowBlock;
class RowChangeList;
class RowwiseIterator;
class Timestamp;

namespace tablet {
class RowSetMetadata;
}  // namespace tablet

namespace cfile {
class BloomFileWriter;
class CFileWriter;
}

namespace consensus {
class OpId;
}

namespace fs {
class BlockCreationTransaction;
struct IOContext;
}

namespace log {
class LogAnchorRegistry;
}

namespace tserver {
class TabletServerTest_SetEncodedKeysWhenStartingUp_Test;
} // namespace tserver

namespace tools {
Status DumpRowSetInternal(const fs::IOContext& ctx,
                          const std::shared_ptr<tablet::RowSetMetadata>& rs_meta,
                          int indent,
                          int64_t* rows_left);
}

namespace tablet {

class CFileSet;
class CompactionOrFlushInput;
class DeltaFileWriter;
class DeltaStats;
class HistoryGcOpts;
class MultiColumnWriter;
class Mutation;
class MvccSnapshot;
class OperationResultPB;

class DiskRowSetWriter final {
 public:
  // TODO(todd): document ownership of rowset_metadata
  DiskRowSetWriter(RowSetMetadata* rowset_metadata, const Schema* schema,
                   BloomFilterSizing bloom_sizing);

  ~DiskRowSetWriter();

  Status Open();

  // The block is written to all column writers as well as the bloom filter,
  // if configured.
  // Rows must be appended in ascending order.
  // 'live_row_count' means the number of live rows in this input block.
  Status AppendBlock(const RowBlock& block, int live_row_count = 0);

  // Closes the CFiles and their underlying writable blocks.
  // If no rows were written, returns Status::Aborted().
  Status Finish();

  // Closes the CFiles, finalizing the underlying blocks and releasing
  // them to 'transaction'. If no rows were written, returns Status::Aborted().
  Status FinishAndReleaseBlocks(fs::BlockCreationTransaction* transaction);

  // The base DiskRowSetWriter never rolls. This method is necessary for tests
  // which are templatized on the writer type.
  static Status RollIfNecessary() { return Status::OK(); }

  rowid_t written_count() const {
    DCHECK(finished_);
    return written_count_;
  }

  // Return the total number of bytes written so far to this DiskRowSet.
  // Additional bytes may be written by "Finish()", but this should provide
  // a reasonable estimate for the total data size.
  size_t written_size() const;

  const Schema& schema() const { return *schema_; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DiskRowSetWriter);

  Status InitBloomFileWriter();

  // Initializes the index writer required for compound keys
  // this index is written to a new file instead of embedded in the col_* files
  Status InitAdHocIndexWriter();

  // Return the cfile::Writer responsible for writing the key index.
  // (the ad-hoc writer for composite keys, otherwise the key column writer)
  cfile::CFileWriter* key_index_writer();

  RowSetMetadata* rowset_metadata_;
  const Schema* const schema_;

  BloomFilterSizing bloom_sizing_;

  bool finished_;
  rowid_t written_count_;
  std::unique_ptr<MultiColumnWriter> col_writer_;
  std::unique_ptr<cfile::BloomFileWriter> bloom_writer_;
  std::unique_ptr<cfile::CFileWriter> ad_hoc_index_writer_;

  // The last encoded key written.
  faststring last_encoded_key_;
};


// Wrapper around DiskRowSetWriter which "rolls" to a new DiskRowSet after
// a certain amount of data has been written. Each output rowset is suffixed
// with ".N" where N starts at 0 and increases as new rowsets are generated.
//
// See AppendBlock(...) for important usage information.
class RollingDiskRowSetWriter final {
 public:
  // Create a new rolling writer. The given 'tablet_metadata' must stay valid
  // for the lifetime of this writer, and is used to construct the new rowsets
  // that this RollingDiskRowSetWriter creates.
  RollingDiskRowSetWriter(TabletMetadata* tablet_metadata,
                          const Schema& schema,
                          BloomFilterSizing bloom_sizing,
                          size_t target_rowset_size);
  ~RollingDiskRowSetWriter();

  Status Open();

  // The block is written to all column writers as well as the bloom filter,
  // if configured.
  // Rows must be appended in ascending order.
  //
  // NOTE: data must be appended in a particular order: for each set of rows
  // you must append deltas using the APIs below *before* appending the block
  // of rows that they correspond to. This ensures that the output delta files
  // and data files are aligned.
  // 'live_row_count' means the number of live rows in this input block.
  Status AppendBlock(const RowBlock& block, int live_row_count = 0);

  // Appends a sequence of REDO deltas for the same row to the current redo
  // delta file. 'row_idx_in_block' is the positional index after the last
  // written block. The 'row_idx_in_drs' out parameter will be set with the row
  // index from the start of the DiskRowSet currently being written.
  Status AppendRedoDeltas(rowid_t row_idx_in_block,
                          Mutation* redo_delta_head,
                          rowid_t* row_idx_in_drs);

  // Appends a sequence of UNDO deltas for the same row to the current undo
  // delta file. 'row_idx_in_block' is the positional index after the last
  // written block. The 'row_idx_in_drs' out parameter will be set with the row
  // index from the start of the DiskRowSet currently being written.
  Status AppendUndoDeltas(rowid_t row_idx_in_block,
                          Mutation* undo_delta_head,
                          rowid_t* row_idx_in_drs);

  // Try to roll the output, if we've passed the configured threshold. This will
  // only roll if called immediately after an AppendBlock() call. The implementation
  // of AppendBlock() doesn't call it automatically, because it doesn't know if there
  // is any more data to be appended. It is safe to call this in other circumstances --
  // it will be ignored if it is not a good time to roll.
  Status RollIfNecessary();

  Status Finish();

  int64_t rows_written_count() const { return written_count_; }

  const Schema& schema() const { return schema_; }

  // Return the set of rowset paths that were written by this writer via
  // the 'metas' out parameter. This must only be called after Finish() returns
  // an OK result.
  const RowSetMetadataVector& GetWrittenRowSetMetadata() const;

  uint64_t written_size() const { return written_size_; }

  int64_t drs_written_count() const { return written_drs_metas_.size(); }

 private:
  Status RollWriter();

  // Close the current DRS and delta writers, releasing their finished blocks
  // into block_closer_.
  Status FinishCurrentWriter();

  template<DeltaType Type>
  Status AppendDeltas(rowid_t row_idx_in_block,
                      Mutation* delta_head,
                      rowid_t* row_idx,
                      DeltaFileWriter* writer,
                      DeltaStats* delta_stats);

  enum State {
    kInitialized,
    kStarted,
    kFinished
  };
  State state_;

  TabletMetadata* tablet_metadata_;
  const Schema schema_;
  std::shared_ptr<RowSetMetadata> cur_drs_metadata_;
  const BloomFilterSizing bloom_sizing_;
  const size_t target_rowset_size_;

  std::unique_ptr<DiskRowSetWriter> cur_writer_;

  // A delta writer to store the undos for each DRS
  std::unique_ptr<DeltaFileWriter> cur_undo_writer_;
  std::unique_ptr<DeltaStats> cur_undo_delta_stats_;
  // a delta writer to store the redos for each DRS
  std::unique_ptr<DeltaFileWriter> cur_redo_writer_;
  std::unique_ptr<DeltaStats> cur_redo_delta_stats_;
  BlockId cur_undo_ds_block_id_;
  BlockId cur_redo_ds_block_id_;

  uint64_t row_idx_in_cur_drs_;

  // True when we are allowed to roll. We can only roll when the delta writers
  // and data writers are aligned (i.e. just after we've appended a new block of data).
  bool can_roll_;

  // RowSetMetadata objects for diskrowsets which have been successfully
  // written out.
  RowSetMetadataVector written_drs_metas_;

  int64_t written_count_;
  uint64_t written_size_;

  // Syncs and commits all writes of outstanding blocks when the rolling
  // writer is destroyed.
  std::unique_ptr<fs::BlockCreationTransaction> block_transaction_;

  DISALLOW_COPY_AND_ASSIGN(RollingDiskRowSetWriter);
};

// A rowset's disk-space-occupying components are as follows:
// - cfile set
//   - base data
//   - bloom file
//   - ad hoc index
// - delta files
//   - UNDO deltas
//   - REDO deltas
// This struct is a container for the sizes of these components.
struct DiskRowSetSpace {
  uint64_t base_data_size;
  uint64_t bloom_size;
  uint64_t ad_hoc_index_size;
  uint64_t redo_deltas_size;
  uint64_t undo_deltas_size;

  // Helper method to compute the size of the diskrowset's underlying cfile set.
  uint64_t CFileSetOnDiskSize() {
    return base_data_size + bloom_size + ad_hoc_index_size;
  }
};

////////////////////////////////////////////////////////////
// DiskRowSet
////////////////////////////////////////////////////////////

class MajorDeltaCompaction;

class DiskRowSet :
    public RowSet,
    public enable_make_shared<DiskRowSet> {
 public:
  static constexpr const char* const kMinKeyMetaEntryName = "min_key";
  static constexpr const char* const kMaxKeyMetaEntryName = "max_key";

  // Open a rowset from disk.
  // If successful, sets *rowset to the newly open rowset
  static Status Open(const std::shared_ptr<RowSetMetadata>& rowset_metadata,
                     log::LogAnchorRegistry* log_anchor_registry,
                     const TabletMemTrackers& mem_trackers,
                     const fs::IOContext* io_context,
                     std::shared_ptr<DiskRowSet>* rowset);

  ////////////////////////////////////////////////////////////
  // "Management" functions
  ////////////////////////////////////////////////////////////

  // Flush all accumulated delta data to disk.
  Status FlushDeltas(const fs::IOContext* io_context) override;

  // Perform delta store minor compaction.
  // This compacts the delta files down to a single one.
  // If there is already only a single delta file, this does nothing.
  Status MinorCompactDeltaStores(const fs::IOContext* io_context) override;

  ////////////////////////////////////////////////////////////
  // RowSet implementation
  ////////////////////////////////////////////////////////////

  ////////////////////
  // Updates
  ////////////////////

  // Update the given row.
  // 'key' should be the key portion of the row -- i.e a contiguous
  // encoding of the key columns.
  Status MutateRow(Timestamp timestamp,
                   const RowSetKeyProbe& probe,
                   const RowChangeList& update,
                   const consensus::OpId& op_id,
                   const fs::IOContext* io_context,
                   ProbeStats* stats,
                   OperationResultPB* result) override;

  Status CheckRowPresent(const RowSetKeyProbe& probe,
                         const fs::IOContext* io_context,
                         bool* present, ProbeStats* stats) const override;

  ////////////////////
  // Read functions.
  ////////////////////
  Status NewRowIterator(const RowIteratorOptions& opts,
                        std::unique_ptr<RowwiseIterator>* out) const override;

  Status NewCompactionInput(const Schema* projection,
                            const MvccSnapshot& snap,
                            const fs::IOContext* io_context,
                            const std::shared_ptr<MemTracker>& parent_tracker,
                            const std::shared_ptr<MemTracker>& tracker,
                            std::shared_ptr<CompactionOrFlushInput>* out) const override;

  // Gets the number of rows in this rowset, checking 'num_rows_' first. If not
  // yet set, consults the base data and stores the result in 'num_rows_'.
  Status CountRows(const fs::IOContext* io_context, rowid_t* count) const final;

  // Count the number of live rows in this DRS.
  Status CountLiveRows(uint64_t* count) const override;
  // Because possible operations in DMS and will be ignored, mainly because there is no API
  // available in the old version(earlier than 1.10) of kudu to obtain this data, this API
  // can only obtain an approximate live row count.
  Status CountLiveRowsWithoutLiveRowCountStats(uint64_t* count) const;

  // See RowSet::GetBounds(...)
  Status GetBounds(std::string* min_encoded_key,
                   std::string* max_encoded_key) const override;

  void GetDiskRowSetSpaceUsage(DiskRowSetSpace* drss) const;

  uint64_t OnDiskSize() const override;

  uint64_t OnDiskBaseDataSize() const override;

  uint64_t OnDiskBaseDataColumnSize(const ColumnId& col_id) const override;

  uint64_t OnDiskBaseDataSizeWithRedos() const override;

  size_t DeltaMemStoreSize() const override;

  bool DeltaMemStoreInfo(size_t* size_bytes, MonoTime* creation_time) const override;

  bool DeltaMemStoreEmpty() const override;

  int64_t MinUnflushedLogIndex() const override;

  size_t CountDeltaStores() const;

  double DeltaStoresCompactionPerfImprovementScore(DeltaCompactionType type) const override;

  Status EstimateBytesInPotentiallyAncientUndoDeltas(
      Timestamp ancient_history_mark,
      EstimateType estimate_type,
      int64_t* bytes) override;

  Status IsDeletedAndFullyAncient(Timestamp ancient_history_mark,
                                  bool* deleted_and_ancient) override;

  Status InitUndoDeltas(Timestamp ancient_history_mark,
                        MonoTime deadline,
                        const fs::IOContext* io_context,
                        int64_t* delta_blocks_initialized,
                        int64_t* bytes_in_ancient_undos) override;

  Status DeleteAncientUndoDeltas(Timestamp ancient_history_mark, const fs::IOContext* io_context,
                                 int64_t* blocks_deleted, int64_t* bytes_deleted) override;

  // Major compacts all the delta files for all the columns.
  Status MajorCompactDeltaStores(const fs::IOContext* io_context, HistoryGcOpts history_gc_opts);

  std::mutex* compact_flush_lock() override {
    return &compact_flush_lock_;
  }

  bool has_been_compacted() const override {
    return has_been_compacted_.load();
  }

  void set_has_been_compacted() override {
    has_been_compacted_.store(true);
  }

  DeltaTracker* mutable_delta_tracker() {
    DCHECK(delta_tracker_);
    return delta_tracker_.get();
  }

  const DeltaTracker& delta_tracker() const {
    DCHECK(delta_tracker_);
    return *delta_tracker_;
  }

  std::shared_ptr<RowSetMetadata> metadata() override {
    return rowset_metadata_;
  }

  std::string ToString() const override {
    return rowset_metadata_->ToString();
  }

  std::string LogPrefix() const {
    return strings::Substitute("T $0 P $1: $2: ",
        rowset_metadata_->tablet_metadata()->tablet_id(),
        rowset_metadata_->fs_manager()->uuid(),
        ToString());
  }

 protected:
  DiskRowSet(std::shared_ptr<RowSetMetadata> rowset_metadata,
             log::LogAnchorRegistry* log_anchor_registry,
             TabletMemTrackers mem_trackers);

 private:
  FRIEND_TEST(TabletHistoryGcTest, TestMajorDeltaCompactionOnSubsetOfColumns);
  FRIEND_TEST(TestCompaction, TestOneToOne);
  FRIEND_TEST(TestRowSet, TestRowSetUpdate);
  FRIEND_TEST(TestRowSet, TestDMSFlush);
  FRIEND_TEST(tserver::TabletServerTest, SetEncodedKeysWhenStartingUp);

  friend class CompactionOrFlushInput;
  friend class Tablet;
  friend Status kudu::tools::DumpRowSetInternal(
      const fs::IOContext& ctx,
      const std::shared_ptr<tablet::RowSetMetadata>& rs_meta,
      int indent,
      int64_t* rows_left);

  Status Open(const fs::IOContext* io_context);

  // Create a new major delta compaction object to compact the specified columns.
  // TODO(aserbin): use the move semantics for RowIteratorOptions
  Status NewMajorDeltaCompaction(const std::vector<ColumnId>& col_ids,
                                 const RowIteratorOptions& opts,
                                 HistoryGcOpts history_gc_opts,
                                 std::unique_ptr<MajorDeltaCompaction>* out) const;

  // Major compacts all the delta files for the specified columns.
  Status MajorCompactDeltaStoresWithColumnIds(const std::vector<ColumnId>& col_ids,
                                              const fs::IOContext* io_context,
                                              HistoryGcOpts history_gc_opts);

  Status DebugDumpImpl(int64_t* rows_left, std::vector<std::string>* lines) override;

  std::shared_ptr<RowSetMetadata> rowset_metadata_;

  bool open_;

  log::LogAnchorRegistry* log_anchor_registry_;

  TabletMemTrackers mem_trackers_;

  // Base data for this rowset.
  mutable rw_spinlock component_lock_;
  std::shared_ptr<CFileSet> base_data_;
  std::unique_ptr<DeltaTracker> delta_tracker_;

  // Number of rows in the rowset. This may be unset (-1) if the rows in the
  // underlying cfile set have not been counted yet.
  mutable std::atomic<int64_t> num_rows_;

  // Lock governing this rowset's inclusion in a compact/flush. If locked,
  // no other compactor will attempt to include this rowset.
  std::mutex compact_flush_lock_;

  // Flag indicating whether the rowset has been removed from a rowset tree,
  // and thus should not be scheduled for further compactions.
  std::atomic<bool> has_been_compacted_;

  DISALLOW_COPY_AND_ASSIGN(DiskRowSet);
};

} // namespace tablet
} // namespace kudu
