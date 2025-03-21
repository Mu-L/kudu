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

#include "kudu/cfile/binary_dict_block.h"

#include <functional>
#include <limits>
#include <ostream>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "kudu/cfile/block_handle.h"
#include "kudu/cfile/block_pointer.h"
#include "kudu/cfile/bshuf_block.h"
#include "kudu/cfile/cfile.pb.h"
#include "kudu/cfile/cfile_reader.h"
#include "kudu/cfile/cfile_writer.h"
#include "kudu/common/column_materialization_context.h"
#include "kudu/common/column_predicate.h"
#include "kudu/common/columnblock.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/rowblock.h"
#include "kudu/common/rowblock_memory.h"
#include "kudu/common/types.h"
#include "kudu/gutil/casts.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/stringpiece.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/bitmap.h"
#include "kudu/util/coding.h"
#include "kudu/util/coding-inl.h"
#include "kudu/util/memory/arena.h"

using std::vector;
using strings::Substitute;

namespace kudu {
namespace cfile {

BinaryDictBlockBuilder::BinaryDictBlockBuilder(const WriterOptions* options)
    : options_(options),
      dict_block_(options_),
      dictionary_strings_arena_(1024),
      mode_(kCodeWordMode) {
  data_builder_.reset(new BShufBlockBuilder<UINT32>(options_));
  // We use this invalid StringPiece for the "empty key". It's safe to build such
  // a string and use it in equality comparisons.
  dictionary_.set_empty_key(StringPiece(static_cast<const char*>(nullptr),
                                        std::numeric_limits<int>::max()));
  Reset();
}

void BinaryDictBlockBuilder::Reset() {
  if (mode_ == kCodeWordMode &&
      dict_block_.IsBlockFull()) {
    mode_ = kPlainBinaryMode;
    data_builder_.reset(new BinaryPlainBlockBuilder(options_));
  } else {
    data_builder_->Reset();
  }

  finished_ = false;
}

void BinaryDictBlockBuilder::Finish(rowid_t ordinal_pos, vector<Slice>* slices) {
  finished_ = true;

  header_buffer_.resize(sizeof(int32_t));
  InlineEncodeFixed32(&header_buffer_[0], mode_);

  vector<Slice> data_slices;
  data_builder_->Finish(ordinal_pos, &data_slices);
  data_slices.insert(data_slices.begin(), Slice(header_buffer_));
  *slices = std::move(data_slices);
}

// The current block is considered full when the the size of data block
// exceeds limit or when the size of dictionary block exceeds the
// CFile block size.
//
// If it is the latter case, all the subsequent data blocks will switch to
// StringPlainBlock automatically.
bool BinaryDictBlockBuilder::IsBlockFull() const {
  if (data_builder_->IsBlockFull()) return true;
  if (dict_block_.IsBlockFull() && (mode_ == kCodeWordMode)) return true;
  return false;
}

int BinaryDictBlockBuilder::AddCodeWords(const uint8_t* vals, size_t count) {
  DCHECK(!finished_);
  DCHECK_GT(count, 0);
  size_t i;

  const Slice* src = reinterpret_cast<const Slice*>(vals);
  if (data_builder_->Count() == 0) {
    first_key_.assign_copy(src->data(), src->size());
  }

  for (i = 0; i < count; i++, src++) {
    const char* c_str = reinterpret_cast<const char*>(src->data());
    StringPiece current_item(c_str, src->size());
    uint32_t codeword;

    if (PREDICT_FALSE(!FindCopy(dictionary_, current_item, &codeword))) {
      // Not already in dictionary, try to add it if there is space.
      if (PREDICT_FALSE(!AddToDict(*src, &codeword))) {
        break;
      }
    }
    if (PREDICT_FALSE(data_builder_->Add(reinterpret_cast<const uint8_t*>(&codeword), 1) == 0)) {
      // The data block is full
      break;
    }
  }
  return i;
}

bool BinaryDictBlockBuilder::AddToDict(Slice val, uint32_t* codeword) {
  if (PREDICT_FALSE(dict_block_.Add(reinterpret_cast<const uint8_t*>(&val), 1) == 0)) {
    // The dictionary block is full
    return false;
  }
  const uint8_t* s_ptr = CHECK_NOTNULL(dictionary_strings_arena_.AddSlice(val));
  const char* s_content = reinterpret_cast<const char*>(s_ptr);
  *codeword = dict_block_.Count() - 1;
  InsertOrDie(&dictionary_, StringPiece(s_content, val.size()), *codeword);
  return true;
}

int BinaryDictBlockBuilder::Add(const uint8_t* vals, size_t count) {
  if (mode_ == kCodeWordMode) {
    return AddCodeWords(vals, count);
  } else {
    DCHECK_EQ(mode_, kPlainBinaryMode);
    return data_builder_->Add(vals, count);
  }
}

Status BinaryDictBlockBuilder::AppendExtraInfo(CFileWriter* c_writer, CFileFooterPB* footer) {
  vector<Slice> dict_v;
  dict_block_.Finish(0, &dict_v);

  BlockPointer ptr;
  Status s = c_writer->AppendDictBlock(std::move(dict_v), &ptr, "Append dictionary block");
  if (!s.ok()) {
    LOG(WARNING) << "Unable to append block to file: " << s.ToString();
    return s;
  }
  ptr.CopyToPB(footer->mutable_dict_block_ptr());
  return Status::OK();
}

size_t BinaryDictBlockBuilder::Count() const {
  return data_builder_->Count();
}

Status BinaryDictBlockBuilder::GetFirstKey(void* key_void) const {
  if (mode_ == kCodeWordMode) {
    DCHECK(finished_);
    Slice* slice = reinterpret_cast<Slice*>(key_void);
    *slice = Slice(first_key_);
    return Status::OK();
  }
  DCHECK_EQ(mode_, kPlainBinaryMode);
  return data_builder_->GetFirstKey(key_void);
}

Status BinaryDictBlockBuilder::GetLastKey(void* key_void) const {
  if (mode_ == kCodeWordMode) {
    DCHECK(finished_);
    uint32_t last_codeword;
    RETURN_NOT_OK(data_builder_->GetLastKey(reinterpret_cast<void*>(&last_codeword)));
    return dict_block_.GetKeyAtIdx(key_void, last_codeword);
  } else {
    DCHECK_EQ(mode_, kPlainBinaryMode);
    return data_builder_->GetLastKey(key_void);
  }
}

////////////////////////////////////////////////////////////
// Decoding
////////////////////////////////////////////////////////////

BinaryDictBlockDecoder::BinaryDictBlockDecoder(scoped_refptr<BlockHandle> block,
                                               CFileIterator* iter)
    : block_(std::move(block)),
      data_(block_->data()),
      parsed_(false),
      dict_decoder_(iter->GetDictDecoder()),
      parent_cfile_iter_(iter) {
}

Status BinaryDictBlockDecoder::ParseHeader() {
  DCHECK(!parsed_);

  if (PREDICT_FALSE(data_.size() < kMinHeaderSize)) {
    return Status::Corruption(Substitute(
        "not enough bytes for header: dictionary block header "
        "size ($0) less than minimum possible header length ($1)",
        data_.size(), kMinHeaderSize));
  }

  if (PREDICT_FALSE(!tight_enum_test_cast<DictEncodingMode>(
          DecodeFixed32(&data_[0]), &mode_))) {
    return Status::Corruption("header Mode information corrupted");
  }
  auto sub_block = block_->SubrangeBlock(4, data_.size() - 4);

  if (mode_ == kCodeWordMode) {
    data_decoder_.reset(new BShufBlockDecoder<UINT32>(std::move(sub_block)));
  } else {
    if (PREDICT_FALSE(mode_ != kPlainBinaryMode)) {
      return Status::Corruption("Unrecognized Dictionary encoded data block header");
    }
    data_decoder_.reset(new BinaryPlainBlockDecoder(std::move(sub_block)));
  }

  RETURN_NOT_OK(data_decoder_->ParseHeader());
  parsed_ = true;
  return Status::OK();
}

void BinaryDictBlockDecoder::SeekToPositionInBlock(uint pos) {
  data_decoder_->SeekToPositionInBlock(pos);
}

Status BinaryDictBlockDecoder::SeekAtOrAfterValue(const void* value_void, bool* exact) {
  if (mode_ == kCodeWordMode) {
    DCHECK(value_void != nullptr);
    Status s = dict_decoder_->SeekAtOrAfterValue(value_void, exact);
    if (!s.ok()) {
      // This case means the value_void is larger that the largest key
      // in the dictionary block. Therefore, it is impossible to be in
      // the current data block, and we adjust the index to be the end
      // of the block
      data_decoder_->SeekToPositionInBlock(data_decoder_->Count() - 1);
      return s;
    }

    size_t index = dict_decoder_->GetCurrentIndex();
    bool tmp;
    return data_decoder_->SeekAtOrAfterValue(&index, &tmp);
  } else {
    DCHECK_EQ(mode_, kPlainBinaryMode);
    return data_decoder_->SeekAtOrAfterValue(value_void, exact);
  }
}

// TODO: implement CopyNextAndEval for more blocks. Eg. other blocks can
// store their min/max values. CopyNextAndEval in these blocks could
// short-circuit if the query can does not search for values within the
// min/max range, or copy all and evaluate otherwise.
Status BinaryDictBlockDecoder::CopyNextAndEval(size_t* n,
                                               ColumnMaterializationContext* ctx,
                                               SelectionVectorView* sel,
                                               ColumnDataView* dst) {
  ctx->SetDecoderEvalSupported();
  if (mode_ == kPlainBinaryMode) {
    // Copy all strings and evaluate them Slice-by-Slice.
    return data_decoder_->CopyNextAndEval(n, ctx, sel, dst);
  }

  // Predicates that have no matching words should return no data.
  SelectionVector* codewords_matching_pred = parent_cfile_iter_->GetCodeWordsMatchingPredicate();
  DCHECK(codewords_matching_pred != nullptr);
  if (!codewords_matching_pred->AnySelected()) {
    // If nothing is selected, move the data_decoder_ pointer forward and clear
    // the corresponding bits in the selection vector.
    int skip = static_cast<int>(*n);
    data_decoder_->SeekForward(&skip);
    *n = static_cast<size_t>(skip);
    sel->ClearBits(*n);
    return Status::OK();
  }

  // IsNotNull predicates should return all data.
  if (ctx->pred()->predicate_type() == PredicateType::IsNotNull) {
    return CopyNextDecodeStrings(n, dst);
  }

  bool retain_dict = false;

  // Load the rows' codeword values into a buffer for scanning.
  BShufBlockDecoder<UINT32>* d_bptr = down_cast<BShufBlockDecoder<UINT32>*>(data_decoder_.get());
  codeword_buf_.resize(*n * sizeof(uint32_t));
  RETURN_NOT_OK(d_bptr->CopyNextValuesToArray(n, codeword_buf_.data()));
  Slice* out = reinterpret_cast<Slice*>(dst->data());
  for (size_t i = 0; i < *n; i++, out++) {
    // Check with the SelectionVectorView to see whether the data has already
    // been cleared, in which case we can skip evaluation.
    if (!sel->TestBit(i)) {
      continue;
    }
    uint32_t codeword = *reinterpret_cast<uint32_t*>(&codeword_buf_[i * sizeof(uint32_t)]);
    if (BitmapTest(codewords_matching_pred->bitmap(), codeword)) {
      // Row is included in predicate: point the cell in the block
      // to the entry in the dictionary.
      *out = dict_decoder_->string_at_index(codeword);
      retain_dict = true;
    } else {
      // Mark that the row will not be returned.
      sel->ClearBit(i);
    }
  }
  if (retain_dict) {
    dst->memory()->RetainReference(dict_decoder_->block_handle());
  }
  return Status::OK();
}

Status BinaryDictBlockDecoder::CopyNextDecodeStrings(size_t* n, ColumnDataView* dst) {
  DCHECK(parsed_);
  DCHECK_EQ(dst->type_info()->physical_type(), BINARY);
  DCHECK_LE(*n, dst->nrows());
  DCHECK_EQ(dst->stride(), sizeof(Slice));

  Slice* out = reinterpret_cast<Slice*>(dst->data());

  codeword_buf_.resize((*n)*sizeof(uint32_t));

  // Copy the codewords into a temporary buffer first.
  BShufBlockDecoder<UINT32>* d_bptr = down_cast<BShufBlockDecoder<UINT32>*>(data_decoder_.get());
  RETURN_NOT_OK(d_bptr->CopyNextValuesToArray(n, codeword_buf_.data()));

  // Now point the cells in the destination block to the string data in the dictionary
  // block.
  for (int i = 0; i < *n; i++) {
    uint32_t codeword = *reinterpret_cast<uint32_t*>(&codeword_buf_[i * sizeof(uint32_t)]);
    *out++ = dict_decoder_->string_at_index(codeword);
  }
  dst->memory()->RetainReference(dict_decoder_->block_handle());
  return Status::OK();
}

Status BinaryDictBlockDecoder::CopyNextValues(size_t* n, ColumnDataView* dst) {
  if (mode_ == kCodeWordMode) {
    return CopyNextDecodeStrings(n, dst);
  } else {
    DCHECK_EQ(mode_, kPlainBinaryMode);
    return data_decoder_->CopyNextValues(n, dst);
  }
}

} // namespace cfile
} // namespace kudu
