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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kudu/cfile/cfile_util.h"
#include "kudu/common/rowid.h"
#include "kudu/fs/block_id.h"
#include "kudu/fs/block_manager.h"
#include "kudu/gutil/macros.h"
#include "kudu/util/compression/compression.pb.h"
#include "kudu/util/faststring.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"

namespace kudu {

class TypeInfo;

namespace cfile {

class BlockBuilder;
class BlockPointer;
class CompressedBlockBuilder;
class FileMetadataPairPB;
class IndexTreeBuilder;
class TypeEncodingInfo;

// Magic used in header/footer
extern const char kMagicStringV1[];
extern const char kMagicStringV2[];
extern const int kMagicLength;
extern const size_t kChecksumSize;

class ArrayElemNumBuilder;
class NonNullBitmapBuilder;

// Main class used to write a CFile.
class CFileWriter final {
 public:
  explicit CFileWriter(WriterOptions options,
                       const TypeInfo* typeinfo,
                       bool is_nullable,
                       std::unique_ptr<fs::WritableBlock> block);

  ~CFileWriter();

  Status Start();

  // Close the CFile and close the underlying writable block.
  Status Finish();

  // Close the CFile, finalizing the underlying block and releasing
  // it to 'transaction'.
  Status FinishAndReleaseBlock(fs::BlockCreationTransaction* transaction);

  bool finished() const noexcept {
    return state_ == kWriterFinished;
  }

  // Add a key-value pair of metadata to the file. Keys should be human-readable,
  // values may be arbitrary binary.
  //
  // If this is called prior to Start(), then the metadata pairs will be added in
  // the header. Otherwise, the pairs will be added in the footer during Finish().
  void AddMetadataPair(const Slice& key, const Slice& value);

  // Return the metadata value associated with the given key.
  //
  // If no such metadata has been added yet, logs a FATAL error.
  std::string GetMetaValueOrDie(Slice key) const;

  // Append a set of values to the file.
  Status AppendEntries(const void* entries, size_t count);

  // Append a set of values to the file with the relative null bitmap.
  // "entries" is not "compact" - ie if you're appending 10 rows, and 9 are NULL,
  // 'entries' still will have 10 elements in it
  Status AppendNullableEntries(const uint8_t* bitmap, const void* entries, size_t count);

  // Similar to AppendNullableEntries above, but for appending array-type
  // column blocks.
  //
  // The 'entries' is a pointer to a C-style array of Slice elements,
  // where each Slice element represents a cell of an array-type column.
  // The 'entries' may contain NULL array cells as well, and the validity
  // of a cell (i.e. whether it's a non-NULL cell) is determined by
  // corresponding bit in 'bitmap': 1 means that the cell contains an array
  // (NOTE: the array may be empty, i.e. contain no elements), and 0 means
  // the cell is nil (NULL).
  //
  // The information on the validity of elemenents in each of the array cells
  // is encoded in the cell's data.
  Status AppendNullableArrayEntries(const uint8_t* bitmap,
                                    const void* entries,
                                    size_t count);

  // Append a raw block to the file, adding it to the various indexes.
  //
  // The Slices in 'data_slices' are concatenated to form the block.
  //
  // validx_key and validx_prev may be NULL if this file writer has not been
  // configured with value indexing.
  //
  // validx_prev should be a Slice pointing to the last key of the previous block.
  // It will be used to optimize the value index entry for the block.
  Status AppendRawBlock(std::vector<Slice> data_slices,
                        size_t ordinal_pos,
                        const void* validx_curr,
                        const Slice& validx_prev,
                        const char* name_for_log);


  // Return the amount of data written so far to this CFile.
  // More data may be written by Finish(), but this is an approximation.
  size_t written_size() const {
    // This is a low estimate, but that's OK -- this is checked after every block
    // write during flush/compact, so better to give a fast slightly-inaccurate result
    // than spend a lot of effort trying to improve accuracy by a few KB.
    return off_;
  }

  // Return the number of values written to the file.
  // This includes NULL cells, but does not include any "raw" blocks
  // appended.
  uint32_t written_value_count() const {
    return value_count_;
  }

  std::string ToString() const { return block_->id().ToString(); }

  const fs::WritableBlock* block() const { return block_.get(); }

  // Wrapper for AddBlock() to append the dictionary block to the end of a Cfile.
  Status AppendDictBlock(std::vector<Slice> data_slices,
                         BlockPointer* block_ptr,
                         const char* name_for_log) {
    return AddBlock(std::move(data_slices), block_ptr, name_for_log);
  }

 private:
  friend class IndexTreeBuilder;

  // Append the given block into the file.
  //
  // Sets *block_ptr to correspond to the newly inserted block.
  Status AddBlock(std::vector<Slice> data_slices,
                  BlockPointer* block_ptr,
                  const char* name_for_log);

  Status WriteRawData(const std::vector<Slice>& data);

  Status FinishCurDataBlock();
  Status FinishCurArrayDataBlock();

  // Flush the current unflushed_metadata_ entries into the given protobuf
  // field, clearing the buffer.
  void FlushMetadataToPB(google::protobuf::RepeatedPtrField<FileMetadataPairPB>* field);

  WriterOptions options_;

  // Block being written.
  std::unique_ptr<fs::WritableBlock> block_;

  // Current file offset.
  uint64_t off_;

  // Current number of values that have been appended. It's accumulated
  // across all the blocks that are to be written into this CFile.
  rowid_t value_count_;

  // Type of data being written
  bool is_nullable_;
  CompressionType compression_;
  const TypeInfo* typeinfo_;
  const TypeEncodingInfo* type_encoding_info_;
  const bool is_array_;

  // The last key written to the block.
  // Only set if the writer is writing an embedded value index.
  faststring last_key_;

  // a temporary buffer for encoding
  faststring tmp_buf_;

  // Metadata which has been added to the writer but not yet flushed.
  std::vector<std::pair<std::string, std::string>> unflushed_metadata_;

  std::unique_ptr<BlockBuilder> data_block_;
  std::unique_ptr<IndexTreeBuilder> posidx_builder_;
  std::unique_ptr<IndexTreeBuilder> validx_builder_;
  std::unique_ptr<NonNullBitmapBuilder> non_null_bitmap_builder_;
  std::unique_ptr<NonNullBitmapBuilder> array_non_null_bitmap_builder_;
  std::unique_ptr<ArrayElemNumBuilder> array_elem_num_builder_;
  std::unique_ptr<CompressedBlockBuilder> block_compressor_;

  enum State {
    kWriterInitialized,
    kWriterWriting,
    kWriterFinished
  };
  State state_;

  DISALLOW_COPY_AND_ASSIGN(CFileWriter);
};

} // namespace cfile
} // namespace kudu
