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

#include <memory>

#include "kudu/common/common.pb.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/status.h"

namespace kudu {
class TypeInfo;

namespace cfile {
class BlockBuilder;
class BlockDecoder;
class BlockHandle;
class CFileIterator;
struct WriterOptions;

// Runtime Information for type encoding/decoding
// including the ability to build BlockDecoders and BlockBuilders
// for each supported encoding
// Mimicked after common::TypeInfo et al
class TypeEncodingInfo {
 public:
  static Status Get(const TypeInfo* typeinfo,
                    EncodingType encoding,
                    const TypeEncodingInfo** out);

  static EncodingType GetDefaultEncoding(const TypeInfo* typeinfo);

  EncodingType encoding_type() const {
    return encoding_type_;
  }

  std::unique_ptr<BlockBuilder> CreateBlockBuilder(const WriterOptions* options) const;

  // Create a BlockDecoder. Returns the newly created decoder.
  // The 'parent_cfile_iter' parameter is only used in case of dictionary encoding.
  std::unique_ptr<BlockDecoder> CreateBlockDecoder(scoped_refptr<BlockHandle> block,
                                                   CFileIterator* parent_cfile_iter) const;

 private:
  friend class TypeEncodingResolver;

  template<typename TypeEncodingTraitsClass>
  explicit TypeEncodingInfo(TypeEncodingTraitsClass unused);

  const EncodingType encoding_type_;

  typedef std::unique_ptr<BlockBuilder> (*CreateBlockBuilderFunc)(const WriterOptions*);
  const CreateBlockBuilderFunc create_builder_func_;

  typedef std::unique_ptr<BlockDecoder> (*CreateBlockDecoderFunc)(
      scoped_refptr<BlockHandle>, CFileIterator*);
  const CreateBlockDecoderFunc create_decoder_func_;

  DISALLOW_COPY_AND_ASSIGN(TypeEncodingInfo);
};

} // namespace cfile
} // namespace kudu
