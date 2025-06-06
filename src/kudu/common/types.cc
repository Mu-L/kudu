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

#include "kudu/common/types.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>

#include "kudu/gutil/singleton.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/walltime.h"
#include "kudu/util/logging.h"

using std::string;
using std::unique_ptr;
using std::unordered_map;

namespace kudu {

using strings::Substitute;

template<typename TypeTraitsClass>
TypeInfo::TypeInfo(TypeTraitsClass /*t*/)
    : type_(TypeTraitsClass::type),
      physical_type_(TypeTraitsClass::physical_type),
      name_(TypeTraitsClass::name()),
      size_(TypeTraitsClass::size),
      min_value_(TypeTraitsClass::min_value()),
      max_value_(TypeTraitsClass::max_value()),
      is_virtual_(TypeTraitsClass::IsVirtual()),
      append_func_(TypeTraitsClass::AppendDebugStringForValue),
      compare_func_(TypeTraitsClass::Compare),
      are_consecutive_func_(TypeTraitsClass::AreConsecutive) {
}

void TypeInfo::AppendDebugStringForValue(const void* ptr, string* str) const {
  if (KUDU_SHOULD_REDACT()) {
    str->append(kRedactionMessage);
  } else {
    append_func_(ptr, str);
  }
}

int TypeInfo::Compare(const void* lhs, const void* rhs) const {
  return compare_func_(lhs, rhs);
}

bool TypeInfo::AreConsecutive(const void* a, const void* b) const {
  return are_consecutive_func_(a, b);
}

class TypeInfoResolver {
 public:
  const TypeInfo* GetTypeInfo(DataType t) {
    const TypeInfo* type_info = mapping_[t].get();
    return CHECK_NOTNULL(type_info);
  }

 private:
  friend class Singleton<TypeInfoResolver>;

  TypeInfoResolver() {
    AddMapping<UINT8>();
    AddMapping<INT8>();
    AddMapping<UINT16>();
    AddMapping<INT16>();
    AddMapping<UINT32>();
    AddMapping<INT32>();
    AddMapping<UINT64>();
    AddMapping<INT64>();
    AddMapping<UNIXTIME_MICROS>();
    AddMapping<DATE>();
    AddMapping<STRING>();
    AddMapping<BOOL>();
    AddMapping<FLOAT>();
    AddMapping<DOUBLE>();
    AddMapping<BINARY>();
    AddMapping<INT128>();
    AddMapping<DECIMAL32>();
    AddMapping<DECIMAL64>();
    AddMapping<DECIMAL128>();
    AddMapping<IS_DELETED>();
    AddMapping<VARCHAR>();
  }

  template<DataType type> void AddMapping() {
    TypeTraits<type> traits;
    mapping_.emplace(type, new TypeInfo(traits));
  }

  unordered_map<DataType,
                unique_ptr<const TypeInfo>,
                std::hash<size_t>> mapping_;

  DISALLOW_COPY_AND_ASSIGN(TypeInfoResolver);
};

const TypeInfo* GetTypeInfo(DataType type) {
  return Singleton<TypeInfoResolver>::get()->GetTypeInfo(type);
}

void DataTypeTraits<DATE>::AppendDebugStringForValue(const void* val, string* str) {
  constexpr static const char* const kDateFormat = "%F"; // the ISO 8601 date format
  static constexpr time_t kSecondsInDay = 24 * 60 * 60;

  int32_t days_since_unix_epoch = *reinterpret_cast<const int32_t*>(val);
  if (IsValidValue(days_since_unix_epoch)) {
    time_t seconds = static_cast<time_t>(days_since_unix_epoch) * kSecondsInDay;
    StringAppendStrftime(str, kDateFormat, seconds, false);
  } else {
    str->append(Substitute("value $0 out of range for DATE type", days_since_unix_epoch));
  }
}

} // namespace kudu
