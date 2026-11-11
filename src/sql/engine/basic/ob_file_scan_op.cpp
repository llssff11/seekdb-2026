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

#include "sql/engine/basic/ob_file_scan_op.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/ob_operator_reg.h"
#include "share/object/ob_obj_cast.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileScanSpec, ObOpSpec),
                    path_,
                    format_,
                    has_header_,
                    field_delim_,
                    null_str_,
                    col_names_,
                    col_types_,
                    column_exprs_);

int ObFileScanOp::open_scanner()
{
  int ret = OB_SUCCESS;
  ObFileScanner *scanner = NULL;
  if (OB_NOT_NULL(scanner_)) {
    static_cast<ObFileScanner *>(scanner_)->close();
    static_cast<ObFileScanner *>(scanner_)->~ObFileScanner();
    scanner_ = NULL;
  }
  void *buf = get_exec_ctx().get_allocator().alloc(sizeof(ObFileScanner));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate file scanner", K(ret), K(sizeof(ObFileScanner)));
  } else {
    scanner = new (buf) ObFileScanner();
    std::string err_msg;
    std::string path(MY_SPEC.path_.ptr(), static_cast<size_t>(MY_SPEC.path_.length()));
    ObFileScanOptions options;
    options.has_header_ = MY_SPEC.has_header_;
    options.field_delim_ = MY_SPEC.field_delim_;
    options.sample_size_ = -1;
    if (!MY_SPEC.null_str_.empty()) {
      options.null_str_.assign(MY_SPEC.null_str_.ptr(),
                               static_cast<size_t>(MY_SPEC.null_str_.length()));
      options.empty_as_null_ = false;
    }
    std::vector<ObFileColumn> columns;
    if (0 != scanner->open(path, MY_SPEC.format_, options, columns, err_msg)) {
      LOG_WARN("failed to open file table", K(ret), K(path.c_str()), K(err_msg.c_str()));
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid file table");
    } else {
      scanner_ = scanner;
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

// The CSV/JSON readers hand back the raw text, so the literals used for a
// boolean column have to be recognized before the generic cast is attempted.
static bool parse_bool_text(const ObString &text, bool &value)
{
  bool found = true;
  if (0 == text.case_compare("true") || 0 == text.case_compare("t")) {
    value = true;
  } else if (0 == text.case_compare("false") || 0 == text.case_compare("f")) {
    value = false;
  } else {
    found = false;
  }
  return found;
}

int ObFileScanOp::fill_row(const std::vector<std::string> &values,
                           const std::vector<bool> &is_null)
{
  int ret = OB_SUCCESS;
  // The previous row has already been consumed by the parent operator when we
  // are called again, so the memory of the previous row can be released here.
  row_alloc_.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column expr is null", K(ret), K(i));
    } else {
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      const ObObjType dst_type = expr->datum_meta_.type_;
      const bool null_value = i >= static_cast<int64_t>(is_null.size()) || is_null[i];
      if (null_value || i >= static_cast<int64_t>(values.size())) {
        datum.set_null();
      } else if (ob_is_string_type(dst_type)) {
        // The text lives in a temporary std::string, it must be copied into the
        // row memory, otherwise the datum would point to released memory.
        const std::string &text = values[i];
        if (text.empty()) {
          datum.set_string("", 0);
        } else {
          char *buf = static_cast<char *>(row_alloc_.alloc(text.size()));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("failed to allocate memory for string column", K(ret), K(i), K(text.size()));
          } else {
            MEMCPY(buf, text.data(), text.size());
            datum.set_string(buf, static_cast<uint32_t>(text.size()));
          }
        }
      } else {
        const std::string &text = values[i];
        ObString str(static_cast<int32_t>(text.size()), const_cast<char *>(text.data()));
        bool bool_value = false;
        const bool is_bool_column =
            MY_SPEC.col_types_.count() == MY_SPEC.column_exprs_.count() &&
            OB_FILE_TYPE_BOOLEAN == MY_SPEC.col_types_.at(i);
        ObObj in_obj;
        in_obj.set_varchar(str);
        in_obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        ObObj out_obj;
        ObCastCtx cast_ctx(&row_alloc_, NULL, CM_NONE, CS_TYPE_UTF8MB4_GENERAL_CI);
        if (is_bool_column && parse_bool_text(str, bool_value)) {
          datum.set_bool(bool_value);
        } else if (OB_FAIL(ObObjCaster::to_type(dst_type, cast_ctx, in_obj, out_obj))) {
          LOG_WARN("failed to cast file value", K(ret), K(i), K(text.c_str()), K(dst_type));
        } else if (OB_FAIL(datum.from_obj(out_obj))) {
          LOG_WARN("failed to store datum", K(ret), K(i));
        }
      }
      if (OB_SUCC(ret)) {
        expr->get_eval_info(eval_ctx_).evaluated_ = true;
      }
    }
  }
  return ret;
}

int ObFileScanOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(open_scanner())) {
    LOG_WARN("failed to open file scan operator", K(ret));
  }
  return ret;
}

int ObFileScanOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (OB_FAIL(open_scanner())) {
  }
  return ret;
}

int ObFileScanOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  std::vector<std::string> values;
  std::vector<bool> is_null;
  std::string err_msg;
  if (OB_ISNULL(scanner_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file scanner is null", K(ret));
  } else {
    clear_evaluated_flag();
    int rc = static_cast<ObFileScanner *>(scanner_)->next_row(values, is_null, err_msg);
    if (1 == rc) {
      if (OB_FAIL(fill_row(values, is_null))) {
      }
    } else if (0 == rc) {
      ret = OB_ITER_END;
    } else {
      LOG_WARN("failed to read file table row", K(ret), K(err_msg.c_str()));
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int ObFileScanOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(scanner_)) {
    static_cast<ObFileScanner *>(scanner_)->close();
  }
  row_alloc_.reset();
  return ret;
}

void ObFileScanOp::destroy()
{
  if (OB_NOT_NULL(scanner_)) {
    static_cast<ObFileScanner *>(scanner_)->close();
    static_cast<ObFileScanner *>(scanner_)->~ObFileScanner();
    scanner_ = NULL;
  }
  row_alloc_.reset();
  ObOperator::destroy();
}

} // end namespace sql
} // end namespace oceanbase
