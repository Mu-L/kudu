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

#include "kudu/common/schema.h"

#include <algorithm>
#include <unordered_set>

#include "kudu/common/row.h"
#include "kudu/common/rowblock.h" // IWYU pragma: keep
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/malloc.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/status.h"
#include "kudu/util/string_case.h"

using std::string;
using std::unordered_set;
using std::vector;
using strings::Substitute;

namespace kudu {

// In a new schema, we typically would start assigning column IDs at 0. However, this
// makes it likely that in many test cases, the column IDs and the column indexes are
// equal to each other, and it's easy to accidentally pass an index where we meant to pass
// an ID, without having any issues. So, in DEBUG builds, we start assigning columns at ID
// 10, ensuring that if we accidentally mix up IDs and indexes, we're likely to fire an
// assertion or bad memory access.
#ifdef NDEBUG
static const ColumnId kFirstColumnId(0);
#else
static const ColumnId  kFirstColumnId(10);
#endif

namespace {

Status FindFirstIsDeletedVirtualColumnIdx(
    const vector<ColumnSchema>& cols, int* idx) {
  for (int i = 0; i < cols.size(); i++) {
    const auto& col = cols[i];
    if (col.type_info()->type() == IS_DELETED) {
      // Enforce some properties on the virtual column that simplify our
      // implementation.
      // TODO(KUDU-2692): Consider removing these requirements.
      if (col.is_nullable()) {
        return Status::InvalidArgument(Substitute(
            "Virtual column $0 $1 must not be nullable",
            col.name(), col.TypeToString()));
      }
      if (!col.has_read_default()) {
        return Status::InvalidArgument(Substitute(
            "Virtual column $0 $1 must have a default value for read",
            col.name(), col.TypeToString()));
      }
      *idx = i;
      return Status::OK();
    }
  }
  *idx = Schema::kColumnNotFound;
  return Status::OK();
}

} // anonymous namespace

bool ColumnTypeAttributes::EqualsForType(ColumnTypeAttributes other,
                                         DataType type) const {
  switch (type) {
    case DECIMAL32:
    case DECIMAL64:
    case DECIMAL128:
      return precision == other.precision && scale == other.scale;
    case VARCHAR:
      return length == other.length;
    default:
      return true; // true because unhandled types don't use ColumnTypeAttributes.
  }
}

string ColumnTypeAttributes::ToStringForType(DataType type) const {
  switch (type) {
    case DECIMAL32:
    case DECIMAL64:
    case DECIMAL128:
      return Substitute("($0, $1)", precision, scale);
    case VARCHAR:
      return Substitute("($0)", length);
    default:
      return "";
  }
}

string ColumnStorageAttributes::ToString() const {
  const string cfile_block_size_str =
      cfile_block_size == 0 ? "" : Substitute(" $0", cfile_block_size);
  return Substitute("$0 $1$2",
                    EncodingType_Name(encoding),
                    CompressionType_Name(compression),
                    cfile_block_size_str);
}

Status ColumnSchema::ApplyDelta(const ColumnSchemaDelta& col_delta) {
  // This method does all validation up-front before making any changes to
  // the schema, so that if we return an error then we are guaranteed to
  // have had no effect
  if (type_info()->physical_type() != BINARY) {
    if (col_delta.default_value && col_delta.default_value->size() < type_info()->size()) {
      return Status::InvalidArgument("wrong size for default value");
    }
  }

  if (col_delta.new_name) {
    name_ = *col_delta.new_name;
  }

  if (col_delta.default_value) {
    const void* value = type_info()->physical_type() == BINARY ?
                        reinterpret_cast<const void*>(&(*col_delta.default_value)) :
                        reinterpret_cast<const void*>(col_delta.default_value->data());
    write_default_ = std::make_shared<Variant>(type_info()->type(), value);
  }

  if (col_delta.remove_default) {
    write_default_ = nullptr;
  }

  if (col_delta.encoding) {
    storage_attributes_.encoding = *col_delta.encoding;
  }
  if (col_delta.compression) {
    storage_attributes_.compression = *col_delta.compression;
  }
  if (col_delta.cfile_block_size) {
    storage_attributes_.cfile_block_size = *col_delta.cfile_block_size;
  }
  if (col_delta.new_comment) {
    comment_ = col_delta.new_comment.value();
  }
  if (col_delta.immutable) {
    is_immutable_ = col_delta.immutable.value();
  }
  return Status::OK();
}

string ColumnSchema::ToString(uint8_t mode) const {
  return Substitute("$0 $1$2$3",
                    name_,
                    TypeToString(),
                    mode & ToStringMode::WITH_ATTRIBUTES ?
                    " " + AttrToString() : "",
                    mode & ToStringMode::WITH_COMMENTS
                    && comment_.length() ? " " + comment_ : "");
}

string ColumnSchema::TypeToString() const {
  string type_name = type_info_->name();
  ToUpperCase(type_name, &type_name);
  return Substitute("$0$1 $2$3",
                    type_name,
                    type_attributes().ToStringForType(type_info()->type()),
                    is_nullable_ ? "NULLABLE" : "NOT NULL",
                    is_immutable_ ? " IMMUTABLE" : "");
}

string ColumnSchema::AttrToString() const {
  return Substitute("$0 $1 $2",
                    storage_attributes_.ToString(),
                    has_read_default() ? Stringify(read_default_value()) : "-",
                    has_write_default() ? Stringify(write_default_value()) : "-");
}

size_t ColumnSchema::memory_footprint_excluding_this() const {
  // Rough approximation.
  return name_.capacity();
}

size_t ColumnSchema::memory_footprint_including_this() const {
  return kudu_malloc_usable_size(this) + memory_footprint_excluding_this();
}

Schema::Schema(const Schema& other)
    : name_to_index_(other.num_columns()) {
      name_to_index_.set_empty_key(StringPiece());
  CopyFrom(other);
}

Schema& Schema::operator=(const Schema& other) {
  if (&other != this) {
    CopyFrom(other);
  }
  return *this;
}

void Schema::CopyFrom(const Schema& other) {
  DCHECK_NE(this, &other);
  num_key_columns_ = other.num_key_columns_;
  cols_ = other.cols_;
  col_ids_ = other.col_ids_;
  max_col_id_ = other.max_col_id_;
  col_offsets_ = other.col_offsets_;
  id_to_index_ = other.id_to_index_;

  // We can't simply copy name_to_index_ since the StringPiece keys
  // reference the other Schema's ColumnSchema objects.
  name_to_index_.clear_no_resize();
  size_t i = 0;
  for (const auto& col : cols_) {
    // The map uses the 'name' string from within the ColumnSchema object.
    name_to_index_[col.name()] = i++;
  }

  first_is_deleted_virtual_column_idx_ = other.first_is_deleted_virtual_column_idx_;
  has_nullables_ = other.has_nullables_;
  auto_incrementing_col_idx_ = other.auto_incrementing_col_idx_;
}

Schema::Schema(Schema&& other) noexcept
    : cols_(std::move(other.cols_)),
      num_key_columns_(other.num_key_columns_),
      col_ids_(std::move(other.col_ids_)),
      max_col_id_(other.max_col_id_),
      col_offsets_(std::move(other.col_offsets_)),
      name_to_index_(std::move(other.name_to_index_)),
      id_to_index_(std::move(other.id_to_index_)),
      first_is_deleted_virtual_column_idx_(other.first_is_deleted_virtual_column_idx_),
      has_nullables_(other.has_nullables_),
      auto_incrementing_col_idx_(other.auto_incrementing_col_idx_) {
}

Schema& Schema::operator=(Schema&& other) noexcept {
  if (&other != this) {
    cols_ = std::move(other.cols_);
    num_key_columns_ = other.num_key_columns_;
    col_ids_ = std::move(other.col_ids_);
    max_col_id_ = other.max_col_id_;
    col_offsets_ = std::move(other.col_offsets_);
    id_to_index_ = std::move(other.id_to_index_);
    first_is_deleted_virtual_column_idx_ = other.first_is_deleted_virtual_column_idx_;
    has_nullables_ = other.has_nullables_;
    name_to_index_ = std::move(other.name_to_index_);
    auto_incrementing_col_idx_ = other.auto_incrementing_col_idx_;
  }
  return *this;
}

Status Schema::Reset(vector<ColumnSchema> cols,
                     vector<ColumnId> ids,
                     int key_columns) {
  cols_ = std::move(cols);
  num_key_columns_ = key_columns;

  if (PREDICT_FALSE(key_columns > cols_.size())) {
    return Status::InvalidArgument(
      "Bad schema", "More key columns than columns");
  }

  if (PREDICT_FALSE(key_columns < 0)) {
    return Status::InvalidArgument(
      "Bad schema", "Cannot specify a negative number of key columns");
  }

  if (PREDICT_FALSE(!ids.empty() && ids.size() != cols_.size())) {
    return Status::InvalidArgument("Bad schema",
      "The number of ids does not match with the number of columns");
  }

  // Verify that the key columns are not nullable
  int auto_incrementing_col_idx = kColumnNotFound;
  for (int i = 0; i < key_columns; ++i) {
    if (PREDICT_FALSE(cols_[i].is_nullable())) {
      return Status::InvalidArgument(
        "Bad schema", Substitute("Nullable key columns are not supported: $0",
                                 cols_[i].name()));
    }
    if (cols_[i].is_auto_incrementing()) {
      // Schemas can have at most one auto-incrementing column
      DCHECK_EQ(auto_incrementing_col_idx, kColumnNotFound);
      DCHECK_EQ(cols_[i].type_info()->type(), INT64);
      DCHECK(!cols_[i].is_nullable());
      DCHECK(!cols_[i].is_immutable());
      auto_incrementing_col_idx = i;
    }
  }

  auto_incrementing_col_idx_ = auto_incrementing_col_idx;
  // Calculate the offset of each column in the row format.
  col_offsets_.clear();
  col_offsets_.reserve(cols_.size() + 1);  // Include space for total byte size at the end.
  size_t off = 0;
  size_t i = 0;
  name_to_index_.clear_no_resize();
  for (const ColumnSchema& col : cols_) {
    if (col.name().empty()) {
      return Status::InvalidArgument("column names must be non-empty");
    }
    // We have to check for the number of key columns here as
    // ColumnSchema.getStrippedColumnSchema() would trigger the exception
    // otherwise.
    if (col.name() == Schema::GetAutoIncrementingColumnName() &&
        !col.is_auto_incrementing() && num_key_columns_ != 0) {
      return Status::InvalidArgument(Substitute(
          "$0 is a reserved column name", Schema::GetAutoIncrementingColumnName()));
    }
    // The map uses the 'name' string from within the ColumnSchema object.
    if (!InsertIfNotPresent(&name_to_index_, col.name(), i++)) {
      return Status::InvalidArgument("Duplicate column name", col.name());
    }

    col_offsets_.push_back(off);
    off += col.type_info()->size();
  }

  // Add an extra element on the end for the total
  // byte size
  col_offsets_.push_back(off);

  // Initialize IDs mapping
  col_ids_ = std::move(ids);
  id_to_index_.clear();
  max_col_id_ = 0;
  for (size_t i = 0; i < col_ids_.size(); ++i) {
    if (col_ids_[i] > max_col_id_) {
      max_col_id_ = col_ids_[i];
    }
    id_to_index_.set(col_ids_[i], i);
  }

  RETURN_NOT_OK(FindFirstIsDeletedVirtualColumnIdx(
      cols_, &first_is_deleted_virtual_column_idx_));

  // Determine whether any column is nullable
  has_nullables_ = false;
  for (const ColumnSchema& col : cols_) {
    if (col.is_nullable()) {
      has_nullables_ = true;
      break;
    }
  }

  return Status::OK();
}

Status Schema::FindColumn(Slice col_name, int* idx) const {
  DCHECK(idx);
  StringPiece sp(reinterpret_cast<const char*>(col_name.data()), col_name.size());
  *idx = find_column(sp);
  if (PREDICT_FALSE(*idx == kColumnNotFound)) {
    return Status::NotFound("No such column", col_name);
  }
  return Status::OK();
}

Status Schema::CreateProjectionByNames(const vector<StringPiece>& col_names,
                                       Schema* out) const {
  vector<ColumnId> ids;
  vector<ColumnSchema> cols;
  for (const StringPiece& name : col_names) {
    const int idx = find_column(name);
    if (idx == kColumnNotFound) {
      return Status::NotFound("column not found", name);
    }
    if (has_column_ids()) {
      ids.push_back(column_id(idx));
    }
    cols.push_back(column(idx));
  }
  return out->Reset(std::move(cols), std::move(ids), 0);
}

Status Schema::CreateProjectionByIdsIgnoreMissing(const vector<ColumnId>& col_ids,
                                                  Schema* out) const {
  vector<ColumnSchema> cols;
  vector<ColumnId> filtered_col_ids;
  for (ColumnId id : col_ids) {
    const auto idx = find_column_by_id(id);
    if (idx == kColumnNotFound) {
      continue;
    }
    cols.push_back(column(idx));
    filtered_col_ids.push_back(id);
  }
  return out->Reset(std::move(cols), std::move(filtered_col_ids), 0);
}

Schema Schema::CopyWithColumnIds() const {
  CHECK(!has_column_ids());
  vector<ColumnId> ids;
  ids.reserve(num_columns());
  for (int32_t i = 0; i < num_columns(); i++) {
    ids.emplace_back(kFirstColumnId + i);
  }
  return Schema(cols_, ids, num_key_columns_);
}

Schema Schema::CopyWithoutColumnIds() const {
  return Schema(cols_, num_key_columns_);
}

Status Schema::VerifyProjectionCompatibility(const Schema& projection) const {
  DCHECK(has_column_ids()) "The server schema must have IDs";

  if (projection.has_column_ids()) {
    return Status::InvalidArgument("User requests should not have Column IDs");
  }

  vector<string> missing_columns;
  for (const ColumnSchema& pcol : projection.columns()) {
    if (pcol.type_info()->is_virtual()) {
      // Virtual columns may appear in a projection without appearing in the
      // schema being projected onto.
      continue;
    }
    int index = find_column(pcol.name());
    if (index < 0) {
      missing_columns.push_back(pcol.name());
    } else if (!pcol.EqualsType(cols_[index])) {
      // TODO(matteo): We don't support query with type adaptors yet.
      return Status::InvalidArgument("The column '" + pcol.name() + "' must have type " +
                                     cols_[index].TypeToString() + " found " + pcol.TypeToString());
    }
  }

  if (!missing_columns.empty()) {
    return Status::InvalidArgument("Some columns are not present in the current schema",
                                   JoinStrings(missing_columns, ", "));
  }
  return Status::OK();
}


Status Schema::GetMappedReadProjection(const Schema& projection,
                                       Schema* mapped_projection) const {
  // - The user projection may have different columns from the ones on the tablet
  // - User columns non present in the tablet are considered errors
  // - The user projection is not supposed to have the defaults or the nullable
  //   information on each field. The current tablet schema is supposed to.
  // - Each CFileSet may have a different schema and each CFileSet::Iterator
  //   must use projection from the CFileSet schema to the mapped user schema.
  RETURN_NOT_OK(VerifyProjectionCompatibility(projection));

  // Get the Projection Mapping
  vector<ColumnSchema> mapped_cols;
  vector<ColumnId> mapped_ids;

  mapped_cols.reserve(projection.num_columns());
  mapped_ids.reserve(projection.num_columns());

  int32_t proj_max_col_id = max_col_id_;
  for (const ColumnSchema& col : projection.columns()) {
    int index = find_column(col.name());
    if (col.type_info()->is_virtual()) {
      DCHECK_EQ(kColumnNotFound, index) << "virtual column not expected in tablet schema";
      DCHECK(!col.is_nullable()); // enforced by Schema constructor
      DCHECK(col.has_read_default()); // enforced by Schema constructor
      mapped_cols.push_back(col);
      // Generate a "fake" column id for virtual columns.
      mapped_ids.emplace_back(++proj_max_col_id);
      continue;
    }
    DCHECK_GE(index, 0) << col.name();
    mapped_cols.push_back(cols_[index]);
    mapped_ids.push_back(col_ids_[index]);
  }

  CHECK_OK(mapped_projection->Reset(mapped_cols, mapped_ids, projection.num_key_columns()));
  return Status::OK();
}

string Schema::ToString(uint8_t mode) const {
  if (cols_.empty()) return "()";

  vector<string> pk_strs;
  pk_strs.reserve(num_key_columns_);
  for (size_t i = 0; i < num_key_columns_; ++i) {
    pk_strs.push_back(cols_[i].name());
  }

  uint8_t col_mode = ColumnSchema::ToStringMode::WITHOUT_ATTRIBUTES;
  if (mode & ToStringMode::WITH_COLUMN_ATTRIBUTES) {
    col_mode |= ColumnSchema::ToStringMode::WITH_ATTRIBUTES;
  }
  if (mode & ToStringMode::WITH_COLUMN_COMMENTS) {
    col_mode |= ColumnSchema::ToStringMode::WITH_COMMENTS;
  }
  vector<string> col_strs;
  if (has_column_ids() && mode & ToStringMode::WITH_COLUMN_IDS) {
    for (size_t i = 0; i < cols_.size(); ++i) {
      col_strs.push_back(Substitute("$0:$1", col_ids_[i], cols_[i].ToString(col_mode)));
    }
  } else {
    for (const ColumnSchema& col : cols_) {
      col_strs.push_back(col.ToString(col_mode));
    }
  }

  return StrCat("(\n    ",
                JoinStrings(col_strs, ",\n    "),
                ",\n    ",
                "PRIMARY KEY (",
                JoinStrings(pk_strs, ", "),
                ")",
                "\n)");
}

template <class RowType>
Status Schema::DecodeRowKey(Slice encoded_key,
                            RowType* row,
                            Arena* arena) const {
  for (size_t col_idx = 0; col_idx < num_key_columns(); ++col_idx) {
    const ColumnSchema& col = column(col_idx);
    const KeyEncoder<faststring>& key_encoder = GetKeyEncoder<faststring>(col.type_info());
    bool is_last = col_idx == (num_key_columns() - 1);
    RETURN_NOT_OK_PREPEND(key_encoder.Decode(&encoded_key,
                                             is_last,
                                             arena,
                                             row->mutable_cell_ptr(col_idx)),
                          Substitute("Error decoding composite key component '$0'",
                                     col.name()));
  }
  return Status::OK();
}

string Schema::DebugEncodedRowKey(Slice encoded_key, StartOrEnd start_or_end) const {
  if (encoded_key.empty()) {
    switch (start_or_end) {
      case START_KEY: return "<start of table>";
      case END_KEY:   return "<end of table>";
    }
  }

  Arena arena(256);
  uint8_t* buf = reinterpret_cast<uint8_t*>(arena.AllocateBytes(key_byte_size()));
  ContiguousRow row(this, buf);
  Status s = DecodeRowKey(encoded_key, &row, &arena);
  if (!s.ok()) {
    return "<invalid key: " + s.ToString() + ">";
  }
  return DebugRowKey(row);
}

size_t Schema::memory_footprint_excluding_this() const {
  size_t size = 0;
  for (const ColumnSchema& col : cols_) {
    size += col.memory_footprint_excluding_this();
  }

  if (cols_.capacity() > 0) {
    size += kudu_malloc_usable_size(cols_.data());
  }
  if (col_ids_.capacity() > 0) {
    size += kudu_malloc_usable_size(col_ids_.data());
  }
  if (col_offsets_.capacity() > 0) {
    size += kudu_malloc_usable_size(col_offsets_.data());
  }
  size += name_to_index_.bucket_count() * sizeof(NameToIndexMap::value_type);
  size += id_to_index_.memory_footprint_excluding_this();

  return size;
}

size_t Schema::memory_footprint_including_this() const {
  return kudu_malloc_usable_size(this) + memory_footprint_excluding_this();
}

// Explicit specialization for callers outside this compilation unit.
template
Status Schema::DecodeRowKey(Slice encoded_key, RowBlockRow* row, Arena* arena) const;

// ============================================================================
//  Schema Builder
// ============================================================================
void SchemaBuilder::Reset() {
  cols_.clear();
  col_ids_.clear();
  col_names_.clear();
  num_key_columns_ = 0;
  next_id_ = kFirstColumnId;
}

void SchemaBuilder::Reset(const Schema& schema) {
  cols_ = schema.cols_;
  col_ids_ = schema.col_ids_;
  num_key_columns_ = schema.num_key_columns_;
  for (const auto& column : cols_) {
    col_names_.insert(column.name());
  }

  if (col_ids_.empty()) {
    for (int32_t i = 0; i < cols_.size(); ++i) {
      col_ids_.emplace_back(kFirstColumnId + i);
    }
  }
  if (col_ids_.empty()) {
    next_id_ = kFirstColumnId;
  } else {
    next_id_ = *std::max_element(col_ids_.begin(), col_ids_.end()) + 1;
  }
}

Status SchemaBuilder::AddColumn(const ColumnSchema& column, bool is_key) {
  if (!InsertIfNotPresent(&col_names_, column.name())) {
    return Status::AlreadyPresent("The column already exists", column.name());
  }
  if (is_key) {
    cols_.insert(cols_.begin() + num_key_columns_, column);
    col_ids_.insert(col_ids_.begin() + num_key_columns_, next_id_);
    ++num_key_columns_;
  } else {
    cols_.push_back(column);
    col_ids_.push_back(next_id_);
  }

  next_id_ = ColumnId(next_id_ + 1);
  return Status::OK();
}

Status SchemaBuilder::RemoveColumn(const string& name) {
  unordered_set<string>::const_iterator it_names = col_names_.find(name);
  if (it_names == col_names_.end()) {
    return Status::NotFound("The specified column does not exist", name);
  }

  col_names_.erase(it_names);
  for (int i = 0; i < cols_.size(); ++i) {
    if (name == cols_[i].name()) {
      cols_.erase(cols_.begin() + i);
      col_ids_.erase(col_ids_.begin() + i);
      if (i < num_key_columns_) {
        num_key_columns_--;
      }
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
  return Status::Corruption("Unable to remove existing column");
}

Status SchemaBuilder::RenameColumn(const string& old_name, const string& new_name) {
  if (new_name.empty()) {
    return Status::InvalidArgument("column name must be non-empty");
  }
  // check if 'new_name' is already in use
  if (col_names_.find(new_name) != col_names_.end()) {
    return Status::AlreadyPresent("The column already exists", new_name);
  }

  // check if the 'old_name' column exists
  unordered_set<string>::const_iterator it_names = col_names_.find(old_name);
  if (it_names == col_names_.end()) {
    return Status::NotFound("The specified column does not exist", old_name);
  }

  col_names_.erase(it_names);   // TODO(wdb): Should this one stay and marked as alias?
  col_names_.insert(new_name);

  for (ColumnSchema& col_schema : cols_) {
    if (old_name == col_schema.name()) {
      col_schema.set_name(new_name);
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
  return Status::IllegalState("Unable to rename existing column");
}

Status SchemaBuilder::ApplyColumnSchemaDelta(const ColumnSchemaDelta& col_delta) {
  // if the column will be renamed, check if 'new_name' is already in use
  if (col_delta.new_name && ContainsKey(col_names_, *col_delta.new_name)) {
    return Status::AlreadyPresent("The column already exists", *col_delta.new_name);
  }

  // check if the column exists
  unordered_set<string>::const_iterator it_names = col_names_.find(col_delta.name);
  if (it_names == col_names_.end()) {
    return Status::NotFound("The specified column does not exist", col_delta.name);
  }

  for (ColumnSchema& col_schema : cols_) {
    if (col_delta.name == col_schema.name()) {
      RETURN_NOT_OK(col_schema.ApplyDelta(col_delta));
      if (col_delta.new_name) {
        // TODO(wdb): Should the old one stay, marked as an alias?
        col_names_.erase(it_names);
        col_names_.insert(*col_delta.new_name);
      }
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
}

} // namespace kudu
