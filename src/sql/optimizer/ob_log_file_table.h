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

#ifndef _OB_LOG_FILE_TABLE_H
#define _OB_LOG_FILE_TABLE_H
#include "sql/optimizer/ob_logical_operator.h"

namespace oceanbase
{
namespace sql
{
// Logical operator reading a local file (read_csv / read_json / read_parquet
// / glob) as a table.
class ObLogFileTable : public ObLogicalOperator
{
public:
  ObLogFileTable(ObLogPlan &plan)
    : ObLogicalOperator(plan),
      table_id_(OB_INVALID_ID),
      access_exprs_() {}
  virtual ~ObLogFileTable() {}
  virtual uint64_t hash(uint64_t seed) const override;
  int generate_access_exprs();
  ObIArray<ObRawExpr *> &get_access_exprs() { return access_exprs_; }
  int get_op_exprs(ObIArray<ObRawExpr *> &all_exprs);
  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  void set_table_id(uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_table_id() const { return table_id_; }

private:
  uint64_t table_id_;
  common::ObSEArray<ObRawExpr *, 4, common::ModulePageAllocator, true> access_exprs_;

  DISALLOW_COPY_AND_ASSIGN(ObLogFileTable);
};

} // end namespace sql
} // end namespace oceanbase
#endif
