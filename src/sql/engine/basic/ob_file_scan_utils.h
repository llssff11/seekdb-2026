/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

// The logical type of a column inferred from file content.
// The order is also the widening order: a wider type wins when two rows
// disagree, and OB_FILE_TYPE_VARCHAR is the final fallback that absorbs
// everything else.
enum ObFileColType : int8_t
{
  OB_FILE_TYPE_NULL = 0,   // column holds only NULL so far
  OB_FILE_TYPE_BOOLEAN,    // true / false
  OB_FILE_TYPE_BIGINT,     // 64 bit signed integer
  OB_FILE_TYPE_DOUBLE,     // floating point number
  OB_FILE_TYPE_DATE,       // YYYY-MM-DD
  OB_FILE_TYPE_DATETIME,   // YYYY-MM-DD HH:MM:SS[.ffffff]
  OB_FILE_TYPE_VARCHAR,    // everything else
  OB_FILE_TYPE_MAX
};

const char *ob_file_col_type_name(const ObFileColType type);

// Widen two types into the smallest type able to hold both.
// NULL is the identity element, VARCHAR is the absorbing element.
ObFileColType ob_file_common_type(const ObFileColType lhs, const ObFileColType rhs);

struct ObFileScanOptions
{
  ObFileScanOptions()
    : field_delim_(','),
      quote_('"'),
      escape_('"'),
      has_header_(true),
      null_str_(),
      empty_as_null_(true),
      sample_size_(-1),
      newline_delimited_(true)
  {}

  char field_delim_;
  char quote_;
  char escape_;
  bool has_header_;
  // Text that represents SQL NULL; an empty string means the empty field is
  // NULL (this is what the tests expect for `,,`).
  std::string null_str_;
  bool empty_as_null_;
  int64_t sample_size_;
  bool newline_delimited_;
};

struct ObFileColumn
{
  ObFileColumn() : type_(OB_FILE_TYPE_NULL), nullable_(false) {}
  std::string name_;
  ObFileColType type_;
  // true when at least one row had no value for this column
  bool nullable_;
};

// Streaming CSV reader. Rows are decoded one at a time so that a file of any
// size can be scanned with a bounded amount of memory.
class ObCsvReader
{
public:
  ObCsvReader();
  ~ObCsvReader();

  int open(const std::string &path, const ObFileScanOptions &options);
  void close();
  bool is_open() const { return NULL != fp_; }

  // Reads the next logical record. Returns 1 when a record was produced,
  // 0 at end of file, and a negative value on error.
  // Handles quoted fields, doubled quotes (""), embedded newlines and CRLF.
  int next_record(std::vector<std::string> &fields, std::string &err_msg);

  int64_t record_no() const { return record_no_; }

private:
  int fill_buffer();
  int read_char(char &c);
  int parse_record(std::vector<std::string> &fields, std::string &err_msg);

  FILE *fp_;
  ObFileScanOptions options_;
  std::vector<char> buf_;
  size_t buf_pos_;
  size_t buf_len_;
  bool eof_;
  int64_t record_no_;
};

// Reads the header line (when has_header) and infers a full-file schema.
// sample_size < 0 means "scan the whole file" which is the only mode the
// competition requires; a positive sample size stops early.
int ob_file_infer_csv_schema(const std::string &path,
                             const ObFileScanOptions &options,
                             std::vector<ObFileColumn> &columns,
                             std::string &err_msg);

// Converts one field to a column type. Returns false when the text cannot be
// represented by the type.
bool ob_file_field_matches(const char *str, const int64_t len, const ObFileColType type);
bool ob_file_parse_bool(const char *str, const int64_t len, bool &value);
bool ob_file_parse_int(const char *str, const int64_t len, int64_t &value);
bool ob_file_parse_double(const char *str, const int64_t len, double &value);
// Accepts YYYY-MM-DD.
bool ob_file_parse_date(const char *str, const int64_t len, int32_t &days);
// Accepts YYYY-MM-DD[ T]HH:MM:SS[.fraction].
bool ob_file_parse_datetime(const char *str, const int64_t len, int64_t &seconds, int64_t &micros);

// Derives unique column names the way the tests expect: a repeated header name
// gets the "__<ordinal>" suffix, e.g. the second `id` becomes `id__2`.
void ob_file_make_unique_names(std::vector<std::string> &names);

// Supported file formats of a file table.
enum ObFileFormat : int32_t
{
  OB_FILE_FORMAT_CSV = 0,
  OB_FILE_FORMAT_JSONL = 1,
  OB_FILE_FORMAT_PARQUET = 2,
  OB_FILE_FORMAT_GLOB = 3,
  OB_FILE_FORMAT_MAX
};

const char *ob_file_format_name(const int32_t format);
// Maps a format spelling (csv/json/newline_delimited/parquet) to its enum.
// Returns OB_FILE_FORMAT_MAX for an unknown spelling.
int32_t ob_file_format_from_name(const char *name, const int64_t len);

// Flat JSON object reader used by the JSONL format. Only top level members are
// supported; a nested object or array makes the line rejected, which is what
// the "reject nested" requirement asks for.
int ob_file_infer_jsonl_schema(const std::string &path,
                               const ObFileScanOptions &options,
                               std::vector<ObFileColumn> &columns,
                               std::string &err_msg);

int ob_file_infer_parquet_schema(const std::string &path,
                                 std::vector<ObFileColumn> &columns,
                                 std::string &err_msg);

// Expands one glob pattern into a sorted list of regular file paths.
// Supported wildcards: '*' and '?' inside a path component, plus '**'.
int ob_file_glob(const std::string &pattern,
                 std::vector<std::string> &files,
                 std::string &err_msg);

// Uniform streaming reader over the three formats plus glob.
// A row is materialised as text values so that the type conversion stays in
// one place; the reader itself never loads a whole file into memory.
class ObFileScanner
{
public:
  ObFileScanner();
  ~ObFileScanner();

  // Opens the file, infers the schema and rewinds to the first data row.
  int open(const std::string &path,
           const int32_t format,
           const ObFileScanOptions &options,
           std::vector<ObFileColumn> &columns,
           std::string &err_msg);
  void close();
  bool is_open() const;

  // Returns 1 when a row was produced, 0 at end of file, negative on error.
  // `values` holds one entry per column; an empty string with `is_null` set
  // means SQL NULL.
  int next_row(std::vector<std::string> &values,
               std::vector<bool> &is_null,
               std::string &err_msg);

private:
  class Impl;
  Impl *impl_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
