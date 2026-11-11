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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_OP_H_

#include "sql/engine/ob_operator.h"
#include "lib/allocator/page_arena.h"
#include <vector>
#include <string>

namespace oceanbase
{
namespace sql
{

// Physical operator that scans a local file as a table. Rows are produced one
// at a time, the file is never loaded into memory as a whole.
class ObFileScanSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileScanSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      path_(),
      format_(0),
      has_header_(true),
      field_delim_(','),
      null_str_(),
      col_names_(alloc),
      col_types_(alloc),
      column_exprs_(alloc) {}
  virtual ~ObFileScanSpec() {}

  int64_t get_col_count() const { return col_names_.count(); }

  ObString path_;
  int32_t format_;
  bool has_header_;
  char field_delim_;
  ObString null_str_;
  common::ObFixedArray<ObString, common::ObIAllocator> col_names_;
  // logical type of each column, see ObFileColType
  common::ObFixedArray<int32_t, common::ObIAllocator> col_types_;
  // output expression of each column
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
};

class ObFileScanOp : public ObOperator
{
public:
  ObFileScanOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      scanner_(NULL),
      row_alloc_()
  {}
  virtual ~ObFileScanOp() {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int open_scanner();
  int fill_row(const std::vector<std::string> &values,
               const std::vector<bool> &is_null);

private:
  void *scanner_;
  common::ObArenaAllocator row_alloc_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_OP_H_ */
