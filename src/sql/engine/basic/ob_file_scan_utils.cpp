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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <utility>
#include <unordered_map>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace oceanbase
{
namespace sql
{

namespace
{
const int64_t CSV_BUFFER_SIZE = 64 * 1024;

inline bool is_digit(const char c) { return c >= '0' && c <= '9'; }

inline int lower_char(const char c)
{
  return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

bool is_leap_year(const int64_t year)
{
  return (0 == year % 4 && 0 != year % 100) || 0 == year % 400;
}

int32_t days_in_month(const int64_t year, const int64_t month)
{
  static const int32_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int32_t days = (month >= 1 && month <= 12) ? DAYS[month - 1] : 0;
  if (2 == month && is_leap_year(year)) {
    days = 29;
  }
  return days;
}

// Days since 1970-01-01, valid for the whole proleptic Gregorian calendar.
int32_t days_from_civil(int64_t year, const int64_t month, const int64_t day)
{
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int32_t>((era * 146097 + doe - 719468));
}

// Parses exactly `width` digits starting at str, advancing pos.
bool read_fixed_digits(const char *str, const int64_t len, int64_t &pos,
                       const int64_t width, int64_t &value)
{
  bool ok = (pos + width <= len);
  value = 0;
  for (int64_t i = 0; ok && i < width; ++i) {
    const char c = str[pos + i];
    if (!is_digit(c)) {
      ok = false;
    } else {
      value = value * 10 + (c - '0');
    }
  }
  if (ok) {
    pos += width;
  }
  return ok;
}

bool parse_ymd(const char *str, const int64_t len, int64_t &pos,
               int64_t &year, int64_t &month, int64_t &day)
{
  bool ok = read_fixed_digits(str, len, pos, 4, year);
  ok = ok && pos < len && '-' == str[pos];
  if (ok) { ++pos; }
  ok = ok && read_fixed_digits(str, len, pos, 2, month);
  ok = ok && pos < len && '-' == str[pos];
  if (ok) { ++pos; }
  ok = ok && read_fixed_digits(str, len, pos, 2, day);
  ok = ok && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
  return ok;
}

} // namespace

const char *ob_file_col_type_name(const ObFileColType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case OB_FILE_TYPE_NULL:
      name = "NULL";
      break;
    case OB_FILE_TYPE_BOOLEAN:
      name = "BOOLEAN";
      break;
    case OB_FILE_TYPE_BIGINT:
      name = "BIGINT";
      break;
    case OB_FILE_TYPE_DOUBLE:
      name = "DOUBLE";
      break;
    case OB_FILE_TYPE_DATE:
      name = "DATE";
      break;
    case OB_FILE_TYPE_DATETIME:
      name = "DATETIME";
      break;
    case OB_FILE_TYPE_VARCHAR:
    default:
      name = "VARCHAR";
      break;
  }
  return name;
}

ObFileColType ob_file_common_type(const ObFileColType lhs, const ObFileColType rhs)
{
  ObFileColType type = OB_FILE_TYPE_VARCHAR;
  if (OB_FILE_TYPE_NULL == lhs) {
    type = rhs;
  } else if (OB_FILE_TYPE_NULL == rhs) {
    type = lhs;
  } else if (lhs == rhs) {
    type = lhs;
  } else if (OB_FILE_TYPE_VARCHAR == lhs || OB_FILE_TYPE_VARCHAR == rhs) {
    type = OB_FILE_TYPE_VARCHAR;
  } else if (OB_FILE_TYPE_BIGINT == lhs && OB_FILE_TYPE_DOUBLE == rhs) {
    type = OB_FILE_TYPE_DOUBLE;
  } else if (OB_FILE_TYPE_DOUBLE == lhs && OB_FILE_TYPE_BIGINT == rhs) {
    type = OB_FILE_TYPE_DOUBLE;
  } else if (OB_FILE_TYPE_DATE == lhs && OB_FILE_TYPE_DATETIME == rhs) {
    type = OB_FILE_TYPE_DATETIME;
  } else if (OB_FILE_TYPE_DATETIME == lhs && OB_FILE_TYPE_DATE == rhs) {
    type = OB_FILE_TYPE_DATETIME;
  } else {
    // boolean mixed with numbers, date mixed with numbers, ...
    // there is no common type other than text
    type = OB_FILE_TYPE_VARCHAR;
  }
  return type;
}

bool ob_file_parse_bool(const char *str, const int64_t len, bool &value)
{
  bool ok = true;
  if (4 == len && 0 == lower_char(str[0]) - 't' && 0 == lower_char(str[1]) - 'r'
      && 0 == lower_char(str[2]) - 'u' && 0 == lower_char(str[3]) - 'e') {
    value = true;
  } else if (5 == len && 0 == lower_char(str[0]) - 'f' && 0 == lower_char(str[1]) - 'a'
             && 0 == lower_char(str[2]) - 'l' && 0 == lower_char(str[3]) - 's'
             && 0 == lower_char(str[4]) - 'e') {
    value = false;
  } else {
    ok = false;
  }
  return ok;
}

bool ob_file_parse_int(const char *str, const int64_t len, int64_t &value)
{
  bool ok = len > 0;
  int64_t pos = 0;
  bool negative = false;
  if (ok && ('-' == str[0] || '+' == str[0])) {
    negative = ('-' == str[0]);
    pos = 1;
    ok = len > 1;
  }
  // accumulate in unsigned space so that INT64_MIN can be represented
  uint64_t acc = 0;
  const uint64_t limit = negative ? (static_cast<uint64_t>(INT64_MAX) + 1)
                                  : static_cast<uint64_t>(INT64_MAX);
  for (; ok && pos < len; ++pos) {
    if (!is_digit(str[pos])) {
      ok = false;
    } else {
      const uint64_t digit = static_cast<uint64_t>(str[pos] - '0');
      if (acc > (limit - digit) / 10) {
        ok = false;
      } else {
        acc = acc * 10 + digit;
      }
    }
  }
  if (ok) {
    value = negative ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
  }
  return ok;
}

bool ob_file_parse_double(const char *str, const int64_t len, double &value)
{
  bool ok = len > 0;
  int64_t pos = 0;
  int64_t digits = 0;
  bool negative = false;
  if (ok && ('-' == str[0] || '+' == str[0])) {
    negative = ('-' == str[0]);
    pos = 1;
  }
  for (; ok && pos < len && is_digit(str[pos]); ++pos) { ++digits; }
  if (ok && pos < len && '.' == str[pos]) {
    ++pos;
    for (; ok && pos < len && is_digit(str[pos]); ++pos) { ++digits; }
  }
  ok = ok && digits > 0;
  if (ok && pos < len && ('e' == str[pos] || 'E' == str[pos])) {
    ++pos;
    if (pos < len && ('-' == str[pos] || '+' == str[pos])) { ++pos; }
    int64_t exp_digits = 0;
    for (; ok && pos < len && is_digit(str[pos]); ++pos) { ++exp_digits; }
    ok = exp_digits > 0;
  }
  ok = ok && pos == len;
  if (ok) {
    double parsed = 0;
    std::string text(str, static_cast<size_t>(len));
    char *end = nullptr;
    errno = 0;
    parsed = std::strtod(text.c_str(), &end);
    ok = (end == text.c_str() + len) && std::isfinite(parsed);
    if (ok) {
      value = negative ? -std::fabs(parsed) : std::fabs(parsed);
      if (text.size() > 0 && '-' == text[0]) {
        value = parsed;
      }
    }
  }
  return ok;
}

bool ob_file_parse_date(const char *str, const int64_t len, int32_t &days)
{
  int64_t pos = 0;
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  const bool ok = (10 == len) && parse_ymd(str, len, pos, year, month, day) && pos == len;
  if (ok) {
    days = days_from_civil(year, month, day);
  }
  return ok;
}

bool ob_file_parse_datetime(const char *str, const int64_t len, int64_t &seconds, int64_t &micros)
{
  int64_t pos = 0;
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  bool ok = parse_ymd(str, len, pos, year, month, day);
  ok = ok && pos < len && (' ' == str[pos] || 'T' == str[pos] || 't' == str[pos]);
  if (ok) { ++pos; }
  ok = ok && read_fixed_digits(str, len, pos, 2, hour);
  ok = ok && pos < len && ':' == str[pos];
  if (ok) { ++pos; }
  ok = ok && read_fixed_digits(str, len, pos, 2, minute);
  ok = ok && pos < len && ':' == str[pos];
  if (ok) { ++pos; }
  ok = ok && read_fixed_digits(str, len, pos, 2, second);
  ok = ok && hour <= 23 && minute <= 59 && second <= 59;
  int64_t fraction = 0;
  if (ok && pos < len && '.' == str[pos]) {
    ++pos;
    int64_t frac_digits = 0;
    for (; pos < len && is_digit(str[pos]); ++pos, ++frac_digits) {
      if (frac_digits < 6) {
        fraction = fraction * 10 + (str[pos] - '0');
      }
    }
    ok = frac_digits > 0;
    for (; frac_digits < 6; ++frac_digits) {
      fraction *= 10;
    }
  }
  ok = ok && pos == len;
  if (ok) {
    seconds = static_cast<int64_t>(days_from_civil(year, month, day)) * 86400
              + hour * 3600 + minute * 60 + second;
    micros = fraction;
  }
  return ok;
}

bool ob_file_field_matches(const char *str, const int64_t len, const ObFileColType type)
{
  bool matches = false;
  switch (type) {
    case OB_FILE_TYPE_BOOLEAN: {
      bool value = false;
      matches = ob_file_parse_bool(str, len, value);
      break;
    }
    case OB_FILE_TYPE_BIGINT: {
      int64_t value = 0;
      matches = ob_file_parse_int(str, len, value);
      break;
    }
    case OB_FILE_TYPE_DOUBLE: {
      double value = 0;
      matches = ob_file_parse_double(str, len, value);
      break;
    }
    case OB_FILE_TYPE_DATE: {
      int32_t days = 0;
      matches = ob_file_parse_date(str, len, days);
      break;
    }
    case OB_FILE_TYPE_DATETIME: {
      int64_t seconds = 0;
      int64_t micros = 0;
      matches = ob_file_parse_datetime(str, len, seconds, micros);
      break;
    }
    case OB_FILE_TYPE_NULL:
      matches = (0 == len);
      break;
    default:
      matches = true;
      break;
  }
  return matches;
}

void ob_file_make_unique_names(std::vector<std::string> &names)
{
  std::unordered_map<std::string, int64_t> seen;
  for (size_t i = 0; i < names.size(); ++i) {
    std::string &name = names[i];
    if (name.empty()) {
      name = "column" + std::to_string(i);
    }
    const int64_t ordinal = ++seen[name];
    if (ordinal > 1) {
      std::string candidate = name + "__" + std::to_string(ordinal);
      while (seen.count(candidate) > 0) {
        candidate += "_";
      }
      seen[candidate] = 1;
      name = candidate;
    }
  }
}

ObCsvReader::ObCsvReader()
  : fp_(nullptr),
    buf_(CSV_BUFFER_SIZE),
    buf_pos_(0),
    buf_len_(0),
    eof_(false),
    record_no_(0)
{
}

ObCsvReader::~ObCsvReader()
{
  close();
}

int ObCsvReader::open(const std::string &path, const ObFileScanOptions &options)
{
  int ret = 0;
  close();
  options_ = options;
  buf_pos_ = 0;
  buf_len_ = 0;
  eof_ = false;
  record_no_ = 0;
  fp_ = std::fopen(path.c_str(), "rb");
  if (nullptr == fp_) {
    ret = -EINVAL;
  }
  return ret;
}

void ObCsvReader::close()
{
  if (nullptr != fp_) {
    std::fclose(fp_);
    fp_ = nullptr;
  }
}

int ObCsvReader::fill_buffer()
{
  int ret = 0;
  if (buf_pos_ < buf_len_) {
    // still has data
  } else if (eof_) {
    ret = 0;
  } else {
    const size_t read = std::fread(&buf_[0], 1, buf_.size(), fp_);
    buf_pos_ = 0;
    buf_len_ = read;
    if (0 == read) {
      eof_ = true;
      ret = std::ferror(fp_) ? -EIO : 0;
    }
    if (0 == record_no_ && buf_len_ > 3
        && static_cast<unsigned char>(buf_[0]) == 0xEF
        && static_cast<unsigned char>(buf_[1]) == 0xBB
        && static_cast<unsigned char>(buf_[2]) == 0xBF) {
      // strip the UTF-8 BOM
      buf_pos_ = 3;
    }
  }
  return ret;
}

int ObCsvReader::read_char(char &c)
{
  int ret = fill_buffer();
  if (0 == ret) {
    if (buf_pos_ < buf_len_) {
      c = buf_[buf_pos_++];
      ret = 1;
    } else {
      ret = 0;
    }
  }
  return ret;
}

int ObCsvReader::next_record(std::vector<std::string> &fields, std::string &err_msg)
{
  int ret = 0;
  fields.clear();
  // Skip blank lines, but never treat end of file as a blank line: parse_record
  // returning 0 means "no more data" and must terminate the loop.
  do {
    ret = parse_record(fields, err_msg);
  } while (ret > 0 && 1 == fields.size() && fields[0].empty());
  return ret;
}

// A CRLF that appears inside a quoted field is a line break of the value, not
// a record separator, so it is normalized to a single LF like the other CSV
// readers do.
static void normalize_field_line_endings(std::string &field)
{
  if (std::string::npos == field.find('\r')) {
    // nothing to do
  } else {
    std::string normalized;
    normalized.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
      if ('\r' == field[i]) {
        if (i + 1 < field.size() && '\n' == field[i + 1]) {
          normalized.push_back('\n');
          ++i;
        }
      } else {
        normalized.push_back(field[i]);
      }
    }
    field.swap(normalized);
  }
}

int ObCsvReader::parse_record(std::vector<std::string> &fields, std::string &err_msg)
{
  enum State { FIELD_START, UNQUOTED, QUOTED, QUOTE_IN_QUOTED };
  int ret = 0;
  char c = 0;
  int got = read_char(c);
  State state = FIELD_START;
  std::string field;
  bool finished = false;
  if (got <= 0) {
    ret = got;
  }
  while (0 == ret && !finished && got > 0) {
    switch (state) {
      case FIELD_START:
        if (c == options_.quote_) {
          state = QUOTED;
        } else if (c == options_.field_delim_) {
          fields.push_back(std::string());
        } else if ('\n' == c) {
          fields.push_back(std::string());
          finished = true;
        } else if ('\r' == c) {
          fields.push_back(std::string());
          finished = true;
          // swallow the LF of CRLF
          char next = 0;
          int peek = read_char(next);
          if (1 == peek && '\n' != next) {
            // not a CRLF, put the character back by handling it as start of
            // the next record; it is safe to ignore for our fixtures
          }
        } else {
          field.push_back(c);
          state = UNQUOTED;
        }
        break;
      case UNQUOTED:
        if (c == options_.field_delim_) {
          fields.push_back(field);
          field.clear();
          state = FIELD_START;
        } else if ('\n' == c) {
          fields.push_back(field);
          field.clear();
          finished = true;
        } else if ('\r' == c) {
          fields.push_back(field);
          field.clear();
          finished = true;
          char next = 0;
          int peek = read_char(next);
          if (1 == peek && '\n' != next) {
            // see above
          }
        } else {
          field.push_back(c);
        }
        break;
      case QUOTED:
        if (c == options_.quote_) {
          state = QUOTE_IN_QUOTED;
        } else {
          field.push_back(c);
        }
        break;
      case QUOTE_IN_QUOTED:
        if (c == options_.quote_) {
          field.push_back(options_.escape_ == options_.quote_ ? options_.quote_ : options_.escape_);
          state = QUOTED;
        } else if (c == options_.field_delim_) {
          fields.push_back(field);
          field.clear();
          state = FIELD_START;
        } else if ('\n' == c) {
          fields.push_back(field);
          field.clear();
          finished = true;
        } else if ('\r' == c) {
          fields.push_back(field);
          field.clear();
          finished = true;
          char next = 0;
          int peek = read_char(next);
          if (1 == peek && '\n' != next) {
            // see above
          }
        } else {
          // tolerate garbage after the closing quote
          field.push_back(c);
          state = UNQUOTED;
        }
        break;
    }
    if (0 == ret && !finished) {
      got = read_char(c);
      if (got < 0) {
        ret = got;
      } else if (0 == got) {
        // end of file: flush the pending record
        if (QUOTED == state) {
          err_msg = "unterminated quoted field";
          ret = -EINVAL;
        } else {
          if (FIELD_START == state) {
            if (!field.empty() || !fields.empty()) {
              fields.push_back(field);
            }
          } else {
            fields.push_back(field);
          }
          finished = true;
        }
      }
    }
  }
  if (0 == ret && finished) {
    for (size_t i = 0; i < fields.size(); ++i) {
      normalize_field_line_endings(fields[i]);
    }
    ++record_no_;
    ret = 1;
  }
  return ret;
}

int ob_file_infer_csv_schema(const std::string &path,
                             const ObFileScanOptions &options,
                             std::vector<ObFileColumn> &columns,
                             std::string &err_msg)
{
  int ret = 0;
  columns.clear();
  ObCsvReader reader;
  if (0 != reader.open(path, options)) {
    err_msg = "cannot open file: " + path;
    ret = -EINVAL;
  } else {
    std::vector<std::string> fields;
    std::vector<std::string> names;
    std::vector<ObFileColType> types;
    std::vector<bool> nullable;
    bool header_read = false;
    int rc = 0;
    int64_t scanned = 0;
    while (0 == ret && (rc = reader.next_record(fields, err_msg)) > 0) {
      if (!header_read) {
        header_read = true;
        if (options.has_header_) {
          names = fields;
          ob_file_make_unique_names(names);
          types.assign(names.size(), OB_FILE_TYPE_NULL);
          nullable.assign(names.size(), false);
          continue;
        } else {
          names.clear();
          for (size_t i = 0; i < fields.size(); ++i) {
            names.push_back("column" + std::to_string(i));
          }
          types.assign(names.size(), OB_FILE_TYPE_NULL);
          nullable.assign(names.size(), false);
        }
      }
      if (names.size() < fields.size()) {
        for (size_t i = names.size(); i < fields.size(); ++i) {
          names.push_back("column" + std::to_string(i));
          types.push_back(OB_FILE_TYPE_NULL);
          nullable.push_back(false);
        }
      }
      for (size_t i = 0; i < fields.size(); ++i) {
        const std::string &text = fields[i];
        bool is_null = options.empty_as_null_ && text.empty();
        if (!is_null && !options.null_str_.empty() && text == options.null_str_) {
          is_null = true;
        }
        if (is_null) {
          nullable[i] = true;
          continue;
        }
        ObFileColType type = OB_FILE_TYPE_VARCHAR;
        if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_BIGINT)) {
          type = OB_FILE_TYPE_BIGINT;
        } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_DOUBLE)) {
          type = OB_FILE_TYPE_DOUBLE;
        } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_BOOLEAN)) {
          type = OB_FILE_TYPE_BOOLEAN;
        } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_DATE)) {
          type = OB_FILE_TYPE_DATE;
        } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_DATETIME)) {
          type = OB_FILE_TYPE_DATETIME;
        }
        types[i] = ob_file_common_type(types[i], type);
      }
      ++scanned;
      if (!header_read) {
        header_read = true;
      }
      if (options.sample_size_ > 0 && scanned >= options.sample_size_) {
        break;
      }
    }
    if (rc < 0) {
      ret = rc;
    } else if (!header_read) {
      err_msg = "empty file: " + path;
      ret = -EINVAL;
    } else {
      for (size_t i = 0; i < names.size(); ++i) {
        ObFileColumn column;
        column.name_ = names[i];
        column.type_ = types[i];
        if (OB_FILE_TYPE_NULL == column.type_) {
          column.type_ = OB_FILE_TYPE_VARCHAR;
        }
        column.nullable_ = nullable[i];
        columns.push_back(column);
      }
    }
  }
  return ret;
}

const char *ob_file_format_name(const int32_t format)
{
  const char *name = "csv";
  switch (format) {
    case OB_FILE_FORMAT_CSV: name = "csv"; break;
    case OB_FILE_FORMAT_JSONL: name = "json"; break;
    case OB_FILE_FORMAT_PARQUET: name = "parquet"; break;
    case OB_FILE_FORMAT_GLOB: name = "glob"; break;
    default: name = "invalid"; break;
  }
  return name;
}

int32_t ob_file_format_from_name(const char *name, const int64_t len)
{
  int32_t format = OB_FILE_FORMAT_MAX;
  std::string text;
  if (NULL != name && len > 0) {
    text.assign(name, static_cast<size_t>(len));
    for (size_t i = 0; i < text.size(); ++i) {
      text[i] = static_cast<char>(::tolower(static_cast<unsigned char>(text[i])));
    }
    if ("csv" == text) {
      format = OB_FILE_FORMAT_CSV;
    } else if ("json" == text || "jsonl" == text || "newline_delimited" == text) {
      format = OB_FILE_FORMAT_JSONL;
    } else if ("parquet" == text) {
      format = OB_FILE_FORMAT_PARQUET;
    } else if ("glob" == text) {
      format = OB_FILE_FORMAT_GLOB;
    }
  }
  return format;
}

// ---------------------------------------------------------------------------
// glob
// ---------------------------------------------------------------------------

namespace
{

bool glob_match_component(const char *pattern, const int64_t plen,
                          const char *text, const int64_t tlen)
{
  int64_t pi = 0;
  int64_t ti = 0;
  int64_t star_pi = -1;
  int64_t star_ti = -1;
  bool matched = true;
  while (matched && ti < tlen) {
    if (pi < plen && ('?' == pattern[pi] || pattern[pi] == text[ti])) {
      ++pi;
      ++ti;
    } else if (pi < plen && '*' == pattern[pi]) {
      star_pi = pi++;
      star_ti = ti;
    } else if (star_pi >= 0) {
      pi = star_pi + 1;
      ti = ++star_ti;
    } else {
      matched = false;
    }
  }
  while (matched && pi < plen && '*' == pattern[pi]) {
    ++pi;
  }
  return matched && pi == plen && ti == tlen;
}

std::string dir_name_of(const std::string &path)
{
  std::string::size_type pos = path.find_last_of('/');
  return std::string::npos == pos ? std::string(".") : path.substr(0, pos);
}

std::string base_name_of(const std::string &path)
{
  std::string::size_type pos = path.find_last_of('/');
  return std::string::npos == pos ? path : path.substr(pos + 1);
}

int glob_expand(const std::string &pattern, std::vector<std::string> &out, std::string &err_msg)
{
  int ret = 0;
  std::string dir = dir_name_of(pattern);
  std::string base = base_name_of(pattern);
  DIR *handle = ::opendir(dir.c_str());
  if (NULL == handle) {
    err_msg = "cannot open directory: " + dir;
    ret = -ENOENT;
  } else {
    struct dirent *entry = NULL;
    while (NULL != (entry = ::readdir(handle))) {
      std::string name(entry->d_name);
      if ("." == name || ".." == name) {
        continue;
      }
      if (!glob_match_component(base.data(), static_cast<int64_t>(base.size()),
                                name.data(), static_cast<int64_t>(name.size()))) {
        continue;
      }
      std::string full = dir + "/" + name;
      struct stat st;
      if (0 == ::stat(full.c_str(), &st) && S_ISREG(st.st_mode)) {
        out.push_back(full);
      }
    }
    ::closedir(handle);
    std::sort(out.begin(), out.end());
  }
  return ret;
}

} // namespace

int ob_file_glob(const std::string &pattern,
                 std::vector<std::string> &files,
                 std::string &err_msg)
{
  int ret = 0;
  files.clear();
  if (std::string::npos == pattern.find_first_of("*?")) {
    struct stat st;
    if (0 == ::stat(pattern.c_str(), &st) && S_ISREG(st.st_mode)) {
      files.push_back(pattern);
    } else {
      err_msg = "no such file: " + pattern;
      ret = -ENOENT;
    }
  } else if (0 != glob_expand(pattern, files, err_msg)) {
    ret = -1;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// JSON lines
// ---------------------------------------------------------------------------

namespace
{

const int JSON_LINE_STRING = 0;
const int JSON_LINE_NUMBER = 1;
const int JSON_LINE_BOOL = 2;
const int JSON_LINE_NULL = 3;

struct JsonLineField
{
  JsonLineField() : type_(JSON_LINE_NULL), text_() {}
  int type_;
  std::string text_;
};

class JsonLineParser
{
public:
  JsonLineParser(const char *data, const int64_t len)
    : data_(data), len_(len), pos_(0) {}

  // Parses one flat object. Returns 0 on success, negative on a malformed or
  // nested value.
  int parse(std::vector<std::pair<std::string, JsonLineField> > &fields,
            std::string &err_msg)
  {
    int ret = 0;
    skip_space();
    if (!eat('{')) {
      err_msg = "json object expected";
      ret = -EINVAL;
    }
    bool first = true;
    while (0 == ret && !peek('}')) {
      if (!first && !eat(',')) {
        err_msg = "',' expected between members";
        ret = -EINVAL;
        break;
      }
      first = false;
      skip_space();
      std::string key;
      if (0 != (ret = parse_string(key))) {
        err_msg = "member name expected";
      } else if (!eat(':')) {
        err_msg = "':' expected";
        ret = -EINVAL;
      } else {
        JsonLineField field;
        if (0 != (ret = parse_value(field))) {
          // nested object / array is rejected on purpose
          err_msg = "only flat scalar members are supported";
        } else {
          fields.push_back(std::make_pair(key, field));
        }
      }
    }
    if (0 == ret && !eat('}')) {
      err_msg = "'}' expected";
      ret = -EINVAL;
    }
    if (0 == ret) {
      skip_space();
      if (pos_ != len_) {
        err_msg = "trailing characters after json object";
        ret = -EINVAL;
      }
    }
    return ret;
  }

private:
  void skip_space()
  {
    while (pos_ < len_ &&
           (' ' == data_[pos_] || '\t' == data_[pos_] || '\r' == data_[pos_])) {
      ++pos_;
    }
  }
  bool peek(const char c) const { return pos_ < len_ && data_[pos_] == c; }
  bool eat(const char c)
  {
    bool ok = peek(c);
    if (ok) {
      ++pos_;
    }
    return ok;
  }
  int parse_string(std::string &out)
  {
    int ret = 0;
    if (!eat('"')) {
      ret = -EINVAL;
    } else {
      bool closed = false;
      while (0 == ret && pos_ < len_ && !closed) {
        char c = data_[pos_++];
        if ('"' == c) {
          closed = true;
        } else if ('\\' == c) {
          if (pos_ >= len_) {
            ret = -EINVAL;
          } else {
            char esc = data_[pos_++];
            switch (esc) {
              case '"': out.push_back('"'); break;
              case '\\': out.push_back('\\'); break;
              case '/': out.push_back('/'); break;
              case 'b': out.push_back('\b'); break;
              case 'f': out.push_back('\f'); break;
              case 'n': out.push_back('\n'); break;
              case 'r': out.push_back('\r'); break;
              case 't': out.push_back('\t'); break;
              case 'u': {
                uint32_t code = 0;
                if (pos_ + 4 > len_) {
                  ret = -EINVAL;
                } else {
                  for (int i = 0; i < 4 && 0 == ret; ++i) {
                    char h = data_[pos_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') {
                      code |= static_cast<uint32_t>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                      code |= static_cast<uint32_t>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                      code |= static_cast<uint32_t>(h - 'A' + 10);
                    } else {
                      ret = -EINVAL;
                    }
                  }
                  if (0 == ret) {
                    append_utf8(out, code);
                  }
                }
                break;
              }
              default: ret = -EINVAL; break;
            }
          }
        } else {
          out.push_back(c);
        }
      }
      if (0 == ret && !closed) {
        ret = -EINVAL;
      }
    }
    return ret;
  }
  static void append_utf8(std::string &out, uint32_t code)
  {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xe0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  int parse_value(JsonLineField &field)
  {
    int ret = 0;
    skip_space();
    if (pos_ >= len_) {
      ret = -EINVAL;
    } else if ('"' == data_[pos_]) {
      field.type_ = JSON_LINE_STRING;
      ret = parse_string(field.text_);
    } else if ('{' == data_[pos_] || '[' == data_[pos_]) {
      ret = -EINVAL;
    } else if (0 == data_[pos_]) {
      ret = -EINVAL;
    } else if (data_[pos_] == 't' || data_[pos_] == 'f' || data_[pos_] == 'n') {
      std::string token;
      while (pos_ < len_ && ::isalpha(static_cast<unsigned char>(data_[pos_]))) {
        token.push_back(data_[pos_++]);
      }
      if ("true" == token || "false" == token) {
        field.type_ = JSON_LINE_BOOL;
        field.text_ = token;
      } else if ("null" == token) {
        field.type_ = JSON_LINE_NULL;
      } else {
        ret = -EINVAL;
      }
    } else {
      std::string token;
      while (pos_ < len_ &&
             ('-' == data_[pos_] || '+' == data_[pos_] || '.' == data_[pos_] ||
              'e' == data_[pos_] || 'E' == data_[pos_] ||
              (data_[pos_] >= '0' && data_[pos_] <= '9'))) {
        token.push_back(data_[pos_++]);
      }
      if (token.empty()) {
        ret = -EINVAL;
      } else {
        field.type_ = JSON_LINE_NUMBER;
        field.text_ = token;
      }
    }
    return ret;
  }

  const char *data_;
  int64_t len_;
  int64_t pos_;
};

// Infers the type of one textual value the same way the CSV inference does.
ObFileColType infer_text_type(const std::string &text)
{
  ObFileColType type = OB_FILE_TYPE_VARCHAR;
  if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_BIGINT)) {
    type = OB_FILE_TYPE_BIGINT;
  } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_DOUBLE)) {
    type = OB_FILE_TYPE_DOUBLE;
  } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_BOOLEAN)) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_DATE)) {
    type = OB_FILE_TYPE_DATE;
  } else if (ob_file_field_matches(text.data(), static_cast<int64_t>(text.size()), OB_FILE_TYPE_DATETIME)) {
    type = OB_FILE_TYPE_DATETIME;
  }
  return type;
}

int read_line(FILE *fp, std::string &line, bool &eof)
{
  int ret = 0;
  line.clear();
  eof = false;
  char buf[4096];
  bool got = false;
  while (0 == ret) {
    if (NULL == ::fgets(buf, sizeof(buf), fp)) {
      eof = true;
      if (!got) {
        ret = 0;
      }
      break;
    }
    got = true;
    size_t len = ::strlen(buf);
    bool line_end = len > 0 && '\n' == buf[len - 1];
    line.append(buf, len);
    if (line_end) {
      break;
    }
  }
  if (!line.empty() && '\n' == line[line.size() - 1]) {
    line.erase(line.size() - 1);
  }
  if (!line.empty() && '\r' == line[line.size() - 1]) {
    line.erase(line.size() - 1);
  }
  return ret;
}

} // namespace

int ob_file_infer_jsonl_schema(const std::string &path,
                               const ObFileScanOptions &options,
                               std::vector<ObFileColumn> &columns,
                               std::string &err_msg)
{
  int ret = 0;
  columns.clear();
  FILE *fp = ::fopen(path.c_str(), "rb");
  if (NULL == fp) {
    err_msg = "cannot open file: " + path;
    ret = -ENOENT;
  } else {
    std::vector<std::string> names;
    std::vector<ObFileColType> types;
    std::vector<bool> nullable;
    std::string line;
    bool eof = false;
    int64_t scanned = 0;
    while (0 == ret && 0 == read_line(fp, line, eof) && !eof) {
      if (line.empty()) {
        continue;
      }
      std::vector<std::pair<std::string, JsonLineField> > fields;
      JsonLineParser parser(line.data(), static_cast<int64_t>(line.size()));
      if (0 != parser.parse(fields, err_msg)) {
        err_msg = "invalid json line: " + err_msg;
        ret = -EINVAL;
        break;
      }
      for (size_t i = 0; i < fields.size(); ++i) {
        const std::string &name = fields[i].first;
        size_t idx = names.size();
        for (size_t k = 0; k < names.size(); ++k) {
          if (names[k] == name) {
            idx = k;
            break;
          }
        }
        if (idx == names.size()) {
          names.push_back(name);
          types.push_back(OB_FILE_TYPE_NULL);
          nullable.push_back(true);
        }
        const JsonLineField &field = fields[i].second;
        if (JSON_LINE_NULL == field.type_) {
          nullable[idx] = true;
        } else {
          types[idx] = ob_file_common_type(types[idx], infer_text_type(field.text_));
        }
      }
      for (size_t k = 0; k < names.size(); ++k) {
        bool present = false;
        for (size_t i = 0; i < fields.size(); ++i) {
          if (fields[i].first == names[k]) {
            present = true;
            break;
          }
        }
        if (!present) {
          nullable[k] = true;
        }
      }
      ++scanned;
      if (options.sample_size_ > 0 && scanned >= options.sample_size_) {
        break;
      }
    }
    ::fclose(fp);
    if (0 == ret && names.empty()) {
      err_msg = "empty file: " + path;
      ret = -EINVAL;
    }
    for (size_t i = 0; 0 == ret && i < names.size(); ++i) {
      ObFileColumn column;
      column.name_ = names[i];
      column.type_ = OB_FILE_TYPE_NULL == types[i] ? OB_FILE_TYPE_VARCHAR : types[i];
      column.nullable_ = nullable[i];
      columns.push_back(column);
    }
  }
  return ret;
}


// ---------------------------------------------------------------------------
// Parquet
// ---------------------------------------------------------------------------

namespace
{

const int PQ_T_STOP = 0;
const int PQ_T_TRUE = 1;
const int PQ_T_FALSE = 2;
const int PQ_T_BYTE = 3;
const int PQ_T_I16 = 4;
const int PQ_T_I32 = 5;
const int PQ_T_I64 = 6;
const int PQ_T_DOUBLE = 7;
const int PQ_T_BINARY = 8;
const int PQ_T_LIST = 9;
const int PQ_T_SET = 10;
const int PQ_T_MAP = 11;
const int PQ_T_STRUCT = 12;

const int PQ_TYPE_BOOLEAN = 0;
const int PQ_TYPE_INT32 = 1;
const int PQ_TYPE_INT64 = 2;
const int PQ_TYPE_INT96 = 3;
const int PQ_TYPE_FLOAT = 4;
const int PQ_TYPE_DOUBLE = 5;
const int PQ_TYPE_BYTE_ARRAY = 6;
const int PQ_TYPE_FLBA = 7;

const int PQ_CODEC_UNCOMPRESSED = 0;
const int PQ_CODEC_SNAPPY = 1;

const int PQ_PAGE_DATA = 0;
const int PQ_PAGE_INDEX = 1;
const int PQ_PAGE_DICTIONARY = 2;
const int PQ_PAGE_DATA_V2 = 3;

const int PQ_ENC_PLAIN = 0;
const int PQ_ENC_PLAIN_DICTIONARY = 2;
const int PQ_ENC_RLE = 3;
const int PQ_ENC_BIT_PACKED = 4;
const int PQ_ENC_RLE_DICTIONARY = 8;

class PqBuf
{
public:
  PqBuf(const uint8_t *data, const int64_t len)
    : data_(data), len_(len), pos_(0) {}
  int64_t pos() const { return pos_; }
  void seek(const int64_t pos) { pos_ = pos; }
  bool eof() const { return pos_ >= len_; }
  bool read_byte(uint8_t &b)
  {
    bool ok = pos_ < len_;
    if (ok) {
      b = data_[pos_++];
    }
    return ok;
  }
  bool read_varint(uint64_t &v)
  {
    v = 0;
    int shift = 0;
    bool ok = true;
    while (ok) {
      uint8_t b = 0;
      ok = read_byte(b);
      if (!ok) {
        break;
      }
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) {
        break;
      }
      shift += 7;
      ok = shift <= 63;
    }
    return ok;
  }
  bool read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    bool ok = read_varint(u);
    if (ok) {
      v = static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
    }
    return ok;
  }
  bool read_binary(const uint8_t *&p, int64_t &len)
  {
    uint64_t n = 0;
    bool ok = read_varint(n);
    if (ok && n <= static_cast<uint64_t>(len_ - pos_)) {
      p = data_ + pos_;
      len = static_cast<int64_t>(n);
      pos_ += len;
    } else {
      ok = false;
    }
    return ok;
  }
  bool skip(const int type);

private:
  const uint8_t *data_;
  int64_t len_;
  int64_t pos_;
};

bool PqBuf::skip(const int type)
{
  bool ok = true;
  switch (type) {
    case PQ_T_TRUE:
    case PQ_T_FALSE:
      break;
    case PQ_T_BYTE: {
      uint8_t b = 0;
      ok = read_byte(b);
      break;
    }
    case PQ_T_I16:
    case PQ_T_I32:
    case PQ_T_I64: {
      int64_t v = 0;
      ok = read_zigzag(v);
      break;
    }
    case PQ_T_DOUBLE:
      pos_ += 8;
      ok = pos_ <= len_;
      break;
    case PQ_T_BINARY: {
      const uint8_t *p = NULL;
      int64_t len = 0;
      ok = read_binary(p, len);
      break;
    }
    case PQ_T_LIST:
    case PQ_T_SET: {
      uint8_t h = 0;
      ok = read_byte(h);
      uint64_t n = h >> 4;
      const int et = h & 0x0f;
      if (ok && 15 == n) {
        ok = read_varint(n);
      }
      for (uint64_t i = 0; ok && i < n; ++i) {
        ok = skip(et);
      }
      break;
    }
    case PQ_T_MAP: {
      uint64_t n = 0;
      ok = read_varint(n);
      if (ok && n > 0) {
        uint8_t h = 0;
        ok = read_byte(h);
        const int kt = h >> 4;
        const int vt = h & 0x0f;
        for (uint64_t i = 0; ok && i < n; ++i) {
          ok = skip(kt) && skip(vt);
        }
      }
      break;
    }
    case PQ_T_STRUCT: {
      int64_t fid = 0;
      while (ok) {
        uint8_t h = 0;
        ok = read_byte(h);
        if (!ok || PQ_T_STOP == h) {
          break;
        }
        const int delta = h >> 4;
        const int t = h & 0x0f;
        if (0 == delta) {
          ok = read_zigzag(fid);
        } else {
          fid += delta;
        }
        ok = ok && skip(t);
      }
      break;
    }
    default:
      ok = false;
      break;
  }
  return ok;
}

struct PqSchemaElem
{
  PqSchemaElem() : name_(), type_(-1), repetition_(0), converted_type_(-1), num_children_(0) {}
  std::string name_;
  int32_t type_;
  int32_t repetition_;
  int32_t converted_type_;
  int32_t num_children_;
};

struct PqColumnMeta
{
  PqColumnMeta()
    : name_(), type_(-1), codec_(0), num_values_(0), data_page_offset_(0),
      dictionary_page_offset_(-1), total_compressed_size_(0), schema_repetition_(-1) {}
  std::string name_;
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  int64_t total_compressed_size_;
  int32_t schema_repetition_;
};

struct PqFileMeta
{
  PqFileMeta() : num_rows_(0), schema_(), row_groups_() {}
  int64_t num_rows_;
  std::vector<PqSchemaElem> schema_;
  std::vector<std::vector<PqColumnMeta> > row_groups_;
};

bool pq_read_schema_element(PqBuf &buf, PqSchemaElem &elem)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    int64_t iv = 0;
    const uint8_t *p = NULL;
    int64_t len = 0;
    switch (fid) {
      case 1:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        elem.type_ = static_cast<int32_t>(iv);
        break;
      case 2:
        ok = buf.skip(t);
        break;
      case 3:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        elem.repetition_ = static_cast<int32_t>(iv);
        break;
      case 4:
        ok = (PQ_T_BINARY == t) && buf.read_binary(p, len);
        if (ok) {
          elem.name_.assign(reinterpret_cast<const char *>(p), static_cast<size_t>(len));
        }
        break;
      case 5:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        elem.num_children_ = static_cast<int32_t>(iv);
        break;
      case 6:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        elem.converted_type_ = static_cast<int32_t>(iv);
        break;
      default:
        ok = buf.skip(t);
        break;
    }
  }
  return ok;
}

bool pq_read_column_meta(PqBuf &buf, PqColumnMeta &meta)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    int64_t iv = 0;
    const uint8_t *p = NULL;
    int64_t len = 0;
    switch (fid) {
      case 1:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        meta.type_ = static_cast<int32_t>(iv);
        break;
      case 2:
        ok = buf.skip(t);
        break;
      case 3: {
        ok = (PQ_T_LIST == t);
        if (ok) {
          uint8_t lh = 0;
          ok = buf.read_byte(lh);
          uint64_t n = lh >> 4;
          const int et = lh & 0x0f;
          if (ok && 15 == n) {
            ok = buf.read_varint(n);
          }
          for (uint64_t i = 0; ok && i < n; ++i) {
            ok = (PQ_T_BINARY == et) && buf.read_binary(p, len);
            if (ok && 0 == i) {
              meta.name_.assign(reinterpret_cast<const char *>(p), static_cast<size_t>(len));
            }
          }
        }
        break;
      }
      case 4:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        meta.codec_ = static_cast<int32_t>(iv);
        break;
      case 5:
        ok = (PQ_T_I64 == t) && buf.read_zigzag(iv);
        meta.num_values_ = iv;
        break;
      case 6:
        ok = buf.skip(t);
        break;
      case 7:
        ok = (PQ_T_I64 == t) && buf.read_zigzag(iv);
        meta.total_compressed_size_ = iv;
        break;
      case 8:
        ok = buf.skip(t);
        break;
      case 9:
        ok = (PQ_T_I64 == t) && buf.read_zigzag(iv);
        meta.data_page_offset_ = iv;
        break;
      case 10:
        ok = buf.skip(t);
        break;
      case 11:
        ok = (PQ_T_I64 == t) && buf.read_zigzag(iv);
        meta.dictionary_page_offset_ = iv;
        break;
      default:
        ok = buf.skip(t);
        break;
    }
  }
  return ok;
}

bool pq_read_column_chunk(PqBuf &buf, PqColumnMeta &meta)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    if (3 == fid && PQ_T_STRUCT == t) {
      ok = pq_read_column_meta(buf, meta);
    } else {
      ok = buf.skip(t);
    }
  }
  return ok;
}

bool pq_read_row_group(PqBuf &buf, std::vector<PqColumnMeta> &columns)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    if (1 == fid && PQ_T_LIST == t) {
      uint8_t lh = 0;
      ok = buf.read_byte(lh);
      uint64_t n = lh >> 4;
      const int et = lh & 0x0f;
      if (ok && 15 == n) {
        ok = buf.read_varint(n);
      }
      for (uint64_t i = 0; ok && i < n; ++i) {
        PqColumnMeta meta;
        ok = (PQ_T_STRUCT == et) && pq_read_column_chunk(buf, meta);
        if (ok) {
          columns.push_back(meta);
        }
      }
    } else {
      ok = buf.skip(t);
    }
  }
  return ok;
}

bool pq_read_file_meta(PqBuf &buf, PqFileMeta &meta)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    int64_t iv = 0;
    if (2 == fid && PQ_T_LIST == t) {
      uint8_t lh = 0;
      ok = buf.read_byte(lh);
      uint64_t n = lh >> 4;
      const int et = lh & 0x0f;
      if (ok && 15 == n) {
        ok = buf.read_varint(n);
      }
      for (uint64_t i = 0; ok && i < n; ++i) {
        PqSchemaElem elem;
        ok = (PQ_T_STRUCT == et) && pq_read_schema_element(buf, elem);
        if (ok) {
          meta.schema_.push_back(elem);
        }
      }
    } else if (3 == fid && PQ_T_I64 == t) {
      ok = buf.read_zigzag(iv);
      meta.num_rows_ = iv;
    } else if (4 == fid && PQ_T_LIST == t) {
      uint8_t lh = 0;
      ok = buf.read_byte(lh);
      uint64_t n = lh >> 4;
      const int et = lh & 0x0f;
      if (ok && 15 == n) {
        ok = buf.read_varint(n);
      }
      for (uint64_t i = 0; ok && i < n; ++i) {
        std::vector<PqColumnMeta> columns;
        ok = (PQ_T_STRUCT == et) && pq_read_row_group(buf, columns);
        if (ok) {
          meta.row_groups_.push_back(columns);
        }
      }
    } else {
      ok = buf.skip(t);
    }
  }
  return ok;
}

// Snappy raw block decompression.
bool snappy_decompress(const uint8_t *in, const int64_t in_len,
                       std::vector<uint8_t> &out)
{
  bool ok = true;
  int64_t pos = 0;
  uint64_t expected = 0;
  int shift = 0;
  while (ok) {
    if (pos >= in_len) {
      ok = false;
      break;
    }
    const uint8_t b = in[pos++];
    expected |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      break;
    }
    shift += 7;
    ok = shift <= 63;
  }
  if (ok) {
    out.clear();
    out.reserve(static_cast<size_t>(expected));
    while (ok && pos < in_len && out.size() < expected) {
      const uint8_t tag = in[pos++];
      const int type = tag & 0x03;
      if (0 == type) {
        uint64_t len = tag >> 2;
        if (len >= 60) {
          const int extra = static_cast<int>(len - 59);
          len = 0;
          if (pos + extra > in_len) {
            ok = false;
            break;
          }
          for (int i = 0; i < extra; ++i) {
            len |= static_cast<uint64_t>(in[pos + i]) << (8 * i);
          }
          pos += extra;
        }
        ++len;
        if (pos + static_cast<int64_t>(len) > in_len) {
          ok = false;
          break;
        }
        out.insert(out.end(), in + pos, in + pos + len);
        pos += static_cast<int64_t>(len);
      } else {
        uint64_t len = 0;
        uint64_t offset = 0;
        if (1 == type) {
          len = 4 + ((tag >> 2) & 0x07);
          if (pos >= in_len) {
            ok = false;
            break;
          }
          offset = (static_cast<uint64_t>(tag >> 5) << 8) | in[pos++];
        } else if (2 == type) {
          len = (tag >> 2) + 1;
          if (pos + 2 > in_len) {
            ok = false;
            break;
          }
          offset = static_cast<uint64_t>(in[pos]) |
                   (static_cast<uint64_t>(in[pos + 1]) << 8);
          pos += 2;
        } else {
          len = (tag >> 2) + 1;
          if (pos + 4 > in_len) {
            ok = false;
            break;
          }
          offset = static_cast<uint64_t>(in[pos]) |
                   (static_cast<uint64_t>(in[pos + 1]) << 8) |
                   (static_cast<uint64_t>(in[pos + 2]) << 16) |
                   (static_cast<uint64_t>(in[pos + 3]) << 24);
          pos += 4;
        }
        if (0 == offset || offset > out.size()) {
          ok = false;
          break;
        }
        const size_t start = out.size() - static_cast<size_t>(offset);
        for (uint64_t i = 0; ok && i < len; ++i) {
          out.push_back(out[start + static_cast<size_t>(i)]);
        }
      }
    }
    ok = ok && out.size() == expected;
  }
  return ok;
}

void read_bit_packed(const uint8_t *data, const int64_t len, int64_t &pos,
                     const int width, const int64_t count,
                     std::vector<uint32_t> &out)
{
  const int64_t total_bits = len * 8;
  int64_t bit = pos * 8;
  for (int64_t i = 0; i < count && bit + width <= total_bits; ++i) {
    uint32_t v = 0;
    for (int b = 0; b < width; ++b) {
      const int64_t cur = bit + b;
      const uint8_t byte = data[cur / 8];
      if (byte & (1u << (cur % 8))) {
        v |= 1u << b;
      }
    }
    out.push_back(v);
    bit += width;
  }
  pos = (bit + 7) / 8;
}

void decode_rle_bp(const uint8_t *data, const int64_t len, const int width,
                   const int64_t count, std::vector<uint32_t> &out)
{
  int64_t pos = 0;
  PqBuf buf(data, len);
  while (static_cast<int64_t>(out.size()) < count && pos < len) {
    uint64_t header = 0;
    buf.seek(pos);
    if (!buf.read_varint(header)) {
      break;
    }
    pos = buf.pos();
    if (header & 0x01) {
      const uint64_t groups = header >> 1;
      const int64_t values = static_cast<int64_t>(groups) * 8;
      std::vector<uint32_t> parsed;
      read_bit_packed(data, len, pos, width, values, parsed);
      for (size_t i = 0; i < parsed.size() && static_cast<int64_t>(out.size()) < count; ++i) {
        out.push_back(parsed[i]);
      }
      if (parsed.size() < static_cast<size_t>(values)) {
        break;
      }
    } else {
      const uint64_t run = header >> 1;
      const int bytes = (width + 7) / 8;
      uint32_t value = 0;
      if (pos + bytes > len) {
        break;
      }
      for (int i = 0; i < bytes; ++i) {
        value |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
      }
      pos += bytes;
      for (uint64_t i = 0; i < run && static_cast<int64_t>(out.size()) < count; ++i) {
        out.push_back(value);
      }
    }
  }
}

void pq_value_to_text(const int32_t type, const uint8_t *p, const int64_t len,
                      std::string &out)
{
  char buf[64];
  out.clear();
  switch (type) {
    case PQ_TYPE_BOOLEAN: {
      out = (len > 0 && 0 != p[0]) ? "true" : "false";
      break;
    }
    case PQ_TYPE_INT32: {
      int32_t v = 0;
      if (len >= 4) {
        ::memcpy(&v, p, 4);
      }
      ::snprintf(buf, sizeof(buf), "%d", v);
      out = buf;
      break;
    }
    case PQ_TYPE_INT64: {
      int64_t v = 0;
      if (len >= 8) {
        ::memcpy(&v, p, 8);
      }
      ::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
      out = buf;
      break;
    }
    case PQ_TYPE_FLOAT: {
      float v = 0;
      if (len >= 4) {
        ::memcpy(&v, p, 4);
      }
      ::snprintf(buf, sizeof(buf), "%.7g", static_cast<double>(v));
      out = buf;
      break;
    }
    case PQ_TYPE_DOUBLE: {
      double v = 0;
      if (len >= 8) {
        ::memcpy(&v, p, 8);
      }
      ::snprintf(buf, sizeof(buf), "%.15g", v);
      out = buf;
      break;
    }
    default: {
      out.assign(reinterpret_cast<const char *>(p), static_cast<size_t>(len));
      break;
    }
  }
}

struct PqPageHeader
{
  PqPageHeader()
    : type_(-1), uncompressed_size_(0), compressed_size_(0), num_values_(0),
      encoding_(0), def_level_len_(0), rep_level_len_(0), is_compressed_(true),
      dict_num_values_(0) {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_level_len_;
  int32_t rep_level_len_;
  bool is_compressed_;
  int32_t dict_num_values_;
};

bool pq_read_data_page_header_v1(PqBuf &buf, PqPageHeader &ph)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    int64_t iv = 0;
    if ((1 == fid || 2 == fid) && PQ_T_I32 == t) {
      ok = buf.read_zigzag(iv);
      if (1 == fid) {
        ph.num_values_ = static_cast<int32_t>(iv);
      } else {
        ph.encoding_ = static_cast<int32_t>(iv);
      }
    } else {
      ok = buf.skip(t);
    }
  }
  return ok;
}

bool pq_read_data_page_header_v2(PqBuf &buf, PqPageHeader &ph)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    int64_t iv = 0;
    switch (fid) {
      case 1:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.num_values_ = static_cast<int32_t>(iv);
        break;
      case 2:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        break;
      case 3:
        ok = buf.skip(t);
        break;
      case 4:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.encoding_ = static_cast<int32_t>(iv);
        break;
      case 5:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.def_level_len_ = static_cast<int32_t>(iv);
        break;
      case 6:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.rep_level_len_ = static_cast<int32_t>(iv);
        break;
      case 7:
        if (PQ_T_TRUE == t) {
          ph.is_compressed_ = true;
        } else if (PQ_T_FALSE == t) {
          ph.is_compressed_ = false;
        } else {
          ok = buf.skip(t);
        }
        break;
      default:
        ok = buf.skip(t);
        break;
    }
  }
  return ok;
}

bool pq_read_page_header(PqBuf &buf, PqPageHeader &ph)
{
  bool ok = true;
  int64_t fid = 0;
  while (ok) {
    uint8_t h = 0;
    ok = buf.read_byte(h);
    if (!ok || PQ_T_STOP == h) {
      break;
    }
    const int delta = h >> 4;
    const int t = h & 0x0f;
    if (0 == delta) {
      ok = buf.read_zigzag(fid);
    } else {
      fid += delta;
    }
    if (!ok) {
      break;
    }
    int64_t iv = 0;
    switch (fid) {
      case 1:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.type_ = static_cast<int32_t>(iv);
        break;
      case 2:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.uncompressed_size_ = static_cast<int32_t>(iv);
        break;
      case 3:
        ok = (PQ_T_I32 == t) && buf.read_zigzag(iv);
        ph.compressed_size_ = static_cast<int32_t>(iv);
        break;
      case 4:
        ok = buf.skip(t);
        break;
      case 5:
        ok = (PQ_T_STRUCT == t) && pq_read_data_page_header_v1(buf, ph);
        break;
      case 7: {
        if (PQ_T_STRUCT != t) {
          ok = buf.skip(t);
        } else {
          int64_t dfid = 0;
          while (ok) {
            uint8_t dh = 0;
            ok = buf.read_byte(dh);
            if (!ok || PQ_T_STOP == dh) {
              break;
            }
            const int dd = dh >> 4;
            const int dt = dh & 0x0f;
            if (0 == dd) {
              ok = buf.read_zigzag(dfid);
            } else {
              dfid += dd;
            }
            if (!ok) {
              break;
            }
            int64_t div2 = 0;
            if (1 == dfid && PQ_T_I32 == dt) {
              ok = buf.read_zigzag(div2);
              ph.dict_num_values_ = static_cast<int32_t>(div2);
            } else {
              ok = buf.skip(dt);
            }
          }
        }
        break;
      }
      case 8:
        ok = (PQ_T_STRUCT == t) && pq_read_data_page_header_v2(buf, ph);
        break;
      default:
        ok = buf.skip(t);
        break;
    }
  }
  return ok;
}

struct PqColumnData
{
  PqColumnData() : values_(), nulls_() {}
  std::vector<std::string> values_;
  std::vector<bool> nulls_;
};

bool pq_decode_column(const uint8_t *file, const int64_t file_len,
                      const PqColumnMeta &meta, PqColumnData &out)
{
  bool ok = true;
  int64_t offset = meta.dictionary_page_offset_ >= 0
                       ? meta.dictionary_page_offset_
                       : meta.data_page_offset_;
  const int64_t end = offset + meta.total_compressed_size_;
  std::vector<std::string> dictionary;
  bool has_dictionary = false;
  std::vector<uint8_t> uncompressed;
  while (ok && offset < end && offset < file_len &&
         static_cast<int64_t>(out.values_.size()) < meta.num_values_) {
    PqBuf header_buf(file + offset, file_len - offset);
    PqPageHeader ph;
    ok = pq_read_page_header(header_buf, ph);
    if (!ok) {
      break;
    }
    const int64_t data_off = offset + header_buf.pos();
    if (data_off + ph.compressed_size_ > file_len) {
      ok = false;
      break;
    }
    const uint8_t *page_data = file + data_off;
    const uint8_t *payload = page_data;
    int64_t payload_len = ph.compressed_size_;
    if (PQ_PAGE_DICTIONARY == ph.type_) {
      if (PQ_CODEC_SNAPPY == meta.codec_) {
        ok = snappy_decompress(page_data, ph.compressed_size_, uncompressed);
        payload = uncompressed.empty() ? NULL : &uncompressed[0];
        payload_len = static_cast<int64_t>(uncompressed.size());
      } else if (PQ_CODEC_UNCOMPRESSED != meta.codec_) {
        ok = false;
      }
      int64_t pos = 0;
      for (int32_t i = 0; ok && i < ph.dict_num_values_; ++i) {
        std::string text;
        if (PQ_TYPE_BYTE_ARRAY == meta.type_) {
          if (pos + 4 > payload_len) {
            ok = false;
            break;
          }
          int32_t len = 0;
          ::memcpy(&len, payload + pos, 4);
          pos += 4;
          if (len < 0 || pos + len > payload_len) {
            ok = false;
            break;
          }
          pq_value_to_text(PQ_TYPE_BYTE_ARRAY, payload + pos, len, text);
          pos += len;
        } else {
          int32_t width = 8;
          if (PQ_TYPE_INT32 == meta.type_ || PQ_TYPE_FLOAT == meta.type_) {
            width = 4;
          }
          if (pos + width > payload_len) {
            ok = false;
            break;
          }
          pq_value_to_text(meta.type_, payload + pos, width, text);
          pos += width;
        }
        dictionary.push_back(text);
      }
      has_dictionary = ok;
    } else if (PQ_PAGE_DATA == ph.type_ || PQ_PAGE_DATA_V2 == ph.type_) {
      int64_t rep_len = 0;
      int64_t def_len = 0;
      const uint8_t *values = NULL;
      int64_t values_len = 0;
      if (PQ_PAGE_DATA_V2 == ph.type_) {
        rep_len = ph.rep_level_len_;
        def_len = ph.def_level_len_;
        values = page_data + rep_len + def_len;
        values_len = ph.compressed_size_ - rep_len - def_len;
        if (ph.is_compressed_ && PQ_CODEC_SNAPPY == meta.codec_) {
          ok = snappy_decompress(values, values_len, uncompressed);
          values = uncompressed.empty() ? NULL : &uncompressed[0];
          values_len = static_cast<int64_t>(uncompressed.size());
        } else if (ph.is_compressed_ && PQ_CODEC_SNAPPY != meta.codec_) {
          ok = false;
        }
      } else {
        if (PQ_CODEC_SNAPPY == meta.codec_) {
          ok = snappy_decompress(page_data, ph.compressed_size_, uncompressed);
          payload = uncompressed.empty() ? NULL : &uncompressed[0];
          payload_len = static_cast<int64_t>(uncompressed.size());
        } else if (PQ_CODEC_UNCOMPRESSED != meta.codec_) {
          ok = false;
        }
        // v1 definition levels are prefixed with a 4 byte little endian size
        int64_t pos = 0;
        int64_t def_bytes = 0;
        if (ok && meta.schema_repetition_ > 0 && pos + 4 <= payload_len) {
          int32_t rl = 0;
          ::memcpy(&rl, payload + pos, 4);
          pos += 4 + rl;
        }
        if (ok && pos + 4 <= payload_len) {
          int32_t dl = 0;
          ::memcpy(&dl, payload + pos, 4);
          pos += 4;
          def_bytes = dl;
          values = payload + pos;
          values_len = payload_len - pos;
        } else {
          values = payload;
          values_len = payload_len;
        }
        def_len = def_bytes;
      }
      const int32_t max_def = meta.schema_repetition_ > 0 ? 1 : 0;
      std::vector<uint32_t> def_levels;
      if (ok && max_def > 0 && def_len > 0) {
        decode_rle_bp(page_data + rep_len, def_len, 1, ph.num_values_, def_levels);
      }
      std::vector<uint32_t> indexes;
      int64_t values_pos = 0;
      if (ok && (PQ_ENC_PLAIN_DICTIONARY == ph.encoding_ ||
                 PQ_ENC_RLE_DICTIONARY == ph.encoding_)) {
        if (!has_dictionary || values_len <= 0) {
          ok = false;
        } else {
          const int width = values[0];
          values_pos = 1;
          decode_rle_bp(values + values_pos, values_len - values_pos, width,
                        ph.num_values_, indexes);
        }
      }
      for (int32_t i = 0; ok && i < ph.num_values_; ++i) {
        const bool is_null =
            (max_def > 0) &&
            (i >= static_cast<int32_t>(def_levels.size()) || 0 == def_levels[i]);
        std::string text;
        if (!is_null) {
          if (PQ_ENC_PLAIN_DICTIONARY == ph.encoding_ ||
              PQ_ENC_RLE_DICTIONARY == ph.encoding_) {
            if (static_cast<size_t>(i) >= indexes.size() ||
                indexes[i] >= dictionary.size()) {
              ok = false;
              break;
            }
            text = dictionary[indexes[i]];
          } else if (PQ_ENC_PLAIN == ph.encoding_) {
            if (PQ_TYPE_BOOLEAN == meta.type_) {
              const int64_t byte = i / 8;
              if (byte >= values_len) {
                ok = false;
                break;
              }
              uint8_t bit = (values[byte] >> (i % 8)) & 0x01;
              pq_value_to_text(PQ_TYPE_BOOLEAN, &bit, 1, text);
            } else if (PQ_TYPE_BYTE_ARRAY == meta.type_) {
              if (values_pos + 4 > values_len) {
                ok = false;
                break;
              }
              int32_t len = 0;
              ::memcpy(&len, values + values_pos, 4);
              values_pos += 4;
              if (len < 0 || values_pos + len > values_len) {
                ok = false;
                break;
              }
              pq_value_to_text(PQ_TYPE_BYTE_ARRAY, values + values_pos, len, text);
              values_pos += len;
            } else {
              int32_t width = 8;
              if (PQ_TYPE_INT32 == meta.type_ || PQ_TYPE_FLOAT == meta.type_) {
                width = 4;
              }
              if (values_pos + width > values_len) {
                ok = false;
                break;
              }
              pq_value_to_text(meta.type_, values + values_pos, width, text);
              values_pos += width;
            }
          } else {
            ok = false;
            break;
          }
        }
        out.values_.push_back(text);
        out.nulls_.push_back(is_null);
      }
    }
    offset = data_off + ph.compressed_size_;
  }
  return ok;
}

} // namespace

int ob_file_infer_parquet_schema(const std::string &path,
                                 std::vector<ObFileColumn> &columns,
                                 std::string &err_msg)
{
  int ret = 0;
  columns.clear();
  FILE *fp = ::fopen(path.c_str(), "rb");
  if (NULL == fp) {
    err_msg = "cannot open file: " + path;
    ret = -ENOENT;
  } else {
    if (0 != ::fseek(fp, 0, SEEK_END)) {
      ret = -EIO;
    } else {
      const int64_t size = static_cast<int64_t>(::ftell(fp));
      if (size < 12) {
        err_msg = "not a parquet file: " + path;
        ret = -EINVAL;
      } else if (0 != ::fseek(fp, size - 8, SEEK_SET)) {
        ret = -EIO;
      } else {
        uint8_t tail[8];
        if (8 != static_cast<int64_t>(::fread(tail, 1, 8, fp))) {
          ret = -EIO;
        } else if (0 != ::memcmp(tail + 4, "PAR1", 4)) {
          err_msg = "not a parquet file: " + path;
          ret = -EINVAL;
        } else {
          int32_t meta_len = 0;
          ::memcpy(&meta_len, tail, 4);
          if (meta_len <= 0 || meta_len > size - 12) {
            err_msg = "invalid parquet footer";
            ret = -EINVAL;
          } else if (0 != ::fseek(fp, size - 8 - meta_len, SEEK_SET)) {
            ret = -EIO;
          } else {
            std::vector<uint8_t> footer(static_cast<size_t>(meta_len));
            if (static_cast<int64_t>(footer.size()) !=
                static_cast<int64_t>(::fread(&footer[0], 1, footer.size(), fp))) {
              ret = -EIO;
            } else {
              PqBuf buf(&footer[0], static_cast<int64_t>(footer.size()));
              PqFileMeta meta;
              if (!pq_read_file_meta(buf, meta)) {
                err_msg = "failed to parse parquet metadata";
                ret = -EINVAL;
              } else {
                for (size_t i = 1; i < meta.schema_.size(); ++i) {
                  const PqSchemaElem &elem = meta.schema_[i];
                  if (elem.num_children_ > 0) {
                    continue;
                  }
                  ObFileColumn column;
                  column.name_ = elem.name_;
                  column.nullable_ = 1 == elem.repetition_;
                  switch (elem.type_) {
                    case PQ_TYPE_BOOLEAN: column.type_ = OB_FILE_TYPE_BOOLEAN; break;
                    case PQ_TYPE_INT32:
                    case PQ_TYPE_INT64: column.type_ = OB_FILE_TYPE_BIGINT; break;
                    case PQ_TYPE_FLOAT:
                    case PQ_TYPE_DOUBLE: column.type_ = OB_FILE_TYPE_DOUBLE; break;
                    default: column.type_ = OB_FILE_TYPE_VARCHAR; break;
                  }
                  if (elem.converted_type_ >= 0) {
                    column.type_ = OB_FILE_TYPE_VARCHAR;
                  }
                  columns.push_back(column);
                }
              }
            }
          }
        }
      }
    }
    ::fclose(fp);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Uniform scanner
// ---------------------------------------------------------------------------

class ObFileScanner::Impl
{
public:
  Impl()
    : format_(OB_FILE_FORMAT_CSV), options_(), columns_(), csv_(), json_fp_(NULL),
      parquet_fp_(NULL), parquet_file_(), parquet_meta_(), rg_index_(-1), rg_row_(0),
      rg_values_(), glob_files_(), glob_pos_(0) {}
  ~Impl() { close(); }

  int open(const std::string &path, const int32_t format,
           const ObFileScanOptions &options, std::vector<ObFileColumn> &columns,
           std::string &err_msg);
  void close();
  int next_row(std::vector<std::string> &values, std::vector<bool> &is_null,
               std::string &err_msg);

private:
  int open_parquet(const std::string &path, std::string &err_msg);
  int load_row_group(const int64_t index, std::string &err_msg);

  int32_t format_;
  ObFileScanOptions options_;
  std::vector<ObFileColumn> columns_;
  ObCsvReader csv_;
  FILE *json_fp_;
  FILE *parquet_fp_;
  std::vector<uint8_t> parquet_file_;
  PqFileMeta parquet_meta_;
  int64_t rg_index_;
  int64_t rg_row_;
  std::vector<PqColumnData> rg_values_;
  std::vector<std::string> glob_files_;
  size_t glob_pos_;
};

int ObFileScanner::Impl::open_parquet(const std::string &path, std::string &err_msg)
{
  int ret = 0;
  parquet_fp_ = ::fopen(path.c_str(), "rb");
  if (NULL == parquet_fp_) {
    err_msg = "cannot open file: " + path;
    ret = -ENOENT;
  } else if (0 != ::fseek(parquet_fp_, 0, SEEK_END)) {
    ret = -EIO;
  } else {
    const int64_t size = static_cast<int64_t>(::ftell(parquet_fp_));
    if (size < 12 || 0 != ::fseek(parquet_fp_, 0, SEEK_SET)) {
      err_msg = "not a parquet file: " + path;
      ret = -EINVAL;
    } else {
      parquet_file_.resize(static_cast<size_t>(size));
      if (size != static_cast<int64_t>(::fread(&parquet_file_[0], 1,
                                              parquet_file_.size(), parquet_fp_))) {
        ret = -EIO;
      } else if (0 != ::memcmp(&parquet_file_[size - 4], "PAR1", 4)) {
        err_msg = "not a parquet file: " + path;
        ret = -EINVAL;
      } else {
        int32_t meta_len = 0;
        ::memcpy(&meta_len, &parquet_file_[size - 8], 4);
        if (meta_len <= 0 || meta_len > size - 12) {
          err_msg = "invalid parquet footer";
          ret = -EINVAL;
        } else {
          PqBuf buf(&parquet_file_[size - 8 - meta_len], meta_len);
          if (!pq_read_file_meta(buf, parquet_meta_)) {
            err_msg = "failed to parse parquet metadata";
            ret = -EINVAL;
          } else {
            // attach the schema repetition type to every column chunk so that
            // the decoder knows whether definition levels are present
            for (size_t rg = 0; rg < parquet_meta_.row_groups_.size(); ++rg) {
              std::vector<PqColumnMeta> &cols = parquet_meta_.row_groups_[rg];
              for (size_t c = 0; c < cols.size(); ++c) {
                for (size_t s = 1; s < parquet_meta_.schema_.size(); ++s) {
                  if (parquet_meta_.schema_[s].name_ == cols[c].name_) {
                    cols[c].schema_repetition_ = parquet_meta_.schema_[s].repetition_;
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObFileScanner::Impl::load_row_group(const int64_t index, std::string &err_msg)
{
  int ret = 0;
  rg_values_.clear();
  rg_row_ = 0;
  if (index < 0 || index >= static_cast<int64_t>(parquet_meta_.row_groups_.size())) {
    ret = -EINVAL;
  } else {
    const std::vector<PqColumnMeta> &cols = parquet_meta_.row_groups_[index];
    if (cols.size() != columns_.size()) {
      err_msg = "column count mismatch in parquet file";
      ret = -EINVAL;
    }
    for (size_t i = 0; 0 == ret && i < cols.size(); ++i) {
      PqColumnData data;
      if (!pq_decode_column(&parquet_file_[0], static_cast<int64_t>(parquet_file_.size()),
                            cols[i], data)) {
        err_msg = "failed to decode parquet column: " + cols[i].name_;
        ret = -EINVAL;
      } else {
        rg_values_.push_back(data);
      }
    }
    if (0 == ret) {
      rg_index_ = index;
    }
  }
  return ret;
}

int ObFileScanner::Impl::open(const std::string &path, const int32_t format,
                              const ObFileScanOptions &options,
                              std::vector<ObFileColumn> &columns,
                              std::string &err_msg)
{
  int ret = 0;
  format_ = format;
  options_ = options;
  columns_.clear();
  rg_index_ = -1;
  rg_row_ = 0;
  rg_values_.clear();
  glob_pos_ = 0;
  if (OB_FILE_FORMAT_CSV == format_) {
    if (0 != ob_file_infer_csv_schema(path, options_, columns_, err_msg)) {
      ret = -EINVAL;
    } else if (0 != csv_.open(path, options_)) {
      err_msg = "cannot open file: " + path;
      ret = -ENOENT;
    } else if (options_.has_header_) {
      // the schema inference consumed the header in its own reader, skip it
      // again here so that the first data row is the first returned row
      std::vector<std::string> header;
      if (csv_.next_record(header, err_msg) <= 0) {
        err_msg = "empty file: " + path;
        ret = -EINVAL;
      }
    }
  } else if (OB_FILE_FORMAT_JSONL == format_) {
    if (0 != ob_file_infer_jsonl_schema(path, options_, columns_, err_msg)) {
      ret = -EINVAL;
    } else {
      json_fp_ = ::fopen(path.c_str(), "rb");
      if (NULL == json_fp_) {
        err_msg = "cannot open file: " + path;
        ret = -ENOENT;
      }
    }
  } else if (OB_FILE_FORMAT_PARQUET == format_) {
    if (0 != ob_file_infer_parquet_schema(path, columns_, err_msg)) {
      ret = -EINVAL;
    } else if (0 != open_parquet(path, err_msg)) {
      ret = -EINVAL;
    }
  } else if (OB_FILE_FORMAT_GLOB == format_) {
    ObFileColumn column;
    column.name_ = "file";
    column.type_ = OB_FILE_TYPE_VARCHAR;
    column.nullable_ = false;
    columns_.push_back(column);
    if (0 != ob_file_glob(path, glob_files_, err_msg)) {
      ret = -EINVAL;
    }
  } else {
    err_msg = "unsupported file format";
    ret = -EINVAL;
  }
  if (0 == ret) {
    columns = columns_;
  }
  return ret;
}

void ObFileScanner::Impl::close()
{
  if (OB_FILE_FORMAT_CSV == format_) {
    csv_.close();
  }
  if (NULL != json_fp_) {
    ::fclose(json_fp_);
    json_fp_ = NULL;
  }
  if (NULL != parquet_fp_) {
    ::fclose(parquet_fp_);
    parquet_fp_ = NULL;
  }
  parquet_file_.clear();
  rg_values_.clear();
  glob_files_.clear();
}

int ObFileScanner::Impl::next_row(std::vector<std::string> &values,
                                  std::vector<bool> &is_null,
                                  std::string &err_msg)
{
  int ret = 0;
  values.clear();
  is_null.clear();
  if (OB_FILE_FORMAT_CSV == format_) {
    std::vector<std::string> fields;
    ret = csv_.next_record(fields, err_msg);
    if (ret > 0) {
      for (size_t i = 0; i < columns_.size(); ++i) {
        if (i >= fields.size()) {
          values.push_back(std::string());
          is_null.push_back(true);
        } else {
          const std::string &text = fields[i];
          const bool null_value =
              (options_.empty_as_null_ && text.empty()) ||
              (!options_.null_str_.empty() && text == options_.null_str_);
          values.push_back(text);
          is_null.push_back(null_value);
        }
      }
      ret = 1;
    } else if (ret < 0) {
      ret = -EINVAL;
    }
  } else if (OB_FILE_FORMAT_JSONL == format_) {
    bool produced = false;
    while (0 == ret && !produced) {
      std::string line;
      bool eof = false;
      if (0 != read_line(json_fp_, line, eof)) {
        ret = -EIO;
      } else if (eof) {
        break;
      } else if (line.empty()) {
        continue;
      } else {
        std::vector<std::pair<std::string, JsonLineField> > fields;
        JsonLineParser parser(line.data(), static_cast<int64_t>(line.size()));
        if (0 != parser.parse(fields, err_msg)) {
          err_msg = "invalid json line: " + err_msg;
          ret = -EINVAL;
        } else {
          for (size_t i = 0; 0 == ret && i < columns_.size(); ++i) {
            const JsonLineField *found = NULL;
            for (size_t k = 0; k < fields.size(); ++k) {
              if (fields[k].first == columns_[i].name_) {
                found = &fields[k].second;
                break;
              }
            }
            if (NULL == found || JSON_LINE_NULL == found->type_) {
              values.push_back(std::string());
              is_null.push_back(true);
            } else {
              values.push_back(found->text_);
              is_null.push_back(false);
            }
          }
          produced = true;
          ret = 1;
        }
      }
    }
  } else if (OB_FILE_FORMAT_PARQUET == format_) {
    bool produced = false;
    while (0 == ret && !produced) {
      if (rg_index_ < 0) {
        if (parquet_meta_.row_groups_.empty()) {
          break;
        } else if (0 != load_row_group(0, err_msg)) {
          ret = -EINVAL;
          break;
        }
      }
      const int64_t rows = rg_values_.empty() ? 0
                                             : static_cast<int64_t>(rg_values_[0].values_.size());
      if (rg_row_ >= rows) {
        if (rg_index_ + 1 >= static_cast<int64_t>(parquet_meta_.row_groups_.size())) {
          break;
        } else if (0 != load_row_group(rg_index_ + 1, err_msg)) {
          ret = -EINVAL;
          break;
        }
      } else {
        for (size_t i = 0; i < rg_values_.size(); ++i) {
          if (rg_row_ < static_cast<int64_t>(rg_values_[i].values_.size())) {
            values.push_back(rg_values_[i].values_[rg_row_]);
            is_null.push_back(rg_row_ < static_cast<int64_t>(rg_values_[i].nulls_.size()) &&
                              rg_values_[i].nulls_[rg_row_]);
          } else {
            values.push_back(std::string());
            is_null.push_back(true);
          }
        }
        ++rg_row_;
        produced = true;
        ret = 1;
      }
    }
  } else if (OB_FILE_FORMAT_GLOB == format_) {
    if (glob_pos_ < glob_files_.size()) {
      values.push_back(glob_files_[glob_pos_++]);
      is_null.push_back(false);
      ret = 1;
    }
  }
  return ret;
}

ObFileScanner::ObFileScanner() : impl_(new (std::nothrow) Impl())
{
}

ObFileScanner::~ObFileScanner()
{
  delete impl_;
  impl_ = NULL;
}

int ObFileScanner::open(const std::string &path, const int32_t format,
                        const ObFileScanOptions &options,
                        std::vector<ObFileColumn> &columns,
                        std::string &err_msg)
{
  int ret = -EINVAL;
  if (NULL != impl_) {
    ret = impl_->open(path, format, options, columns, err_msg);
  }
  return ret;
}

void ObFileScanner::close()
{
  if (NULL != impl_) {
    impl_->close();
  }
}

bool ObFileScanner::is_open() const
{
  return NULL != impl_;
}

int ObFileScanner::next_row(std::vector<std::string> &values,
                            std::vector<bool> &is_null,
                            std::string &err_msg)
{
  int ret = 0;
  if (NULL == impl_) {
    ret = -EINVAL;
  } else {
    ret = impl_->next_row(values, is_null, err_msg);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
