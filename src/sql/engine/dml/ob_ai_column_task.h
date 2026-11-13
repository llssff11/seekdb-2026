/**
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

#ifndef OB_AI_COLUMN_TASK_H_
#define OB_AI_COLUMN_TASK_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

class ObAlterTableStmt;

/**
 * @brief AI column task rows and the transaction of the user statement
 *
 * The rows of the AI column task table are written by an autonomous
 * transaction, they are therefore not covered by the rollback of the
 * transaction of the user statement.  The DML hook registers the task rows
 * that it creates together with the id of the transaction of the user
 * statement, this entry point is called when the transaction of the user
 * statement is rolled back.  The rollback removes the task rows of the
 * transaction again, a committed transaction keeps its task rows, they are
 * published by the worker.
 */
int ai_column_task_on_end_trans(uint64_t session_id, int64_t tx_id, bool is_rollback);

/**
 * @brief run a statement on an autonomous transaction
 *
 * The statement is used to write the rows of the AI column task table and to
 * invalidate the result of an AI column.  It runs on its own transaction, the
 * rows it writes are therefore not covered by the transaction of the user
 * statement.
 */
int ai_execute_inner_write_raw(const common::ObString &stmt, int64_t &affected_rows);

/**
 * @brief build the update statement of ALTER TABLE ... RETRY AI COLUMN
 *
 * The retry statement is not a schema change, it invalidates the result of an
 * AI column for the rows that match the predicate.  The predicate is taken
 * from the text of the statement, the status pseudo column is replaced by the
 * state of the result column.
 */
int ai_build_ai_column_retry_sql(const common::ObString &db_name,
                                 const common::ObString &table_name,
                                 const common::ObString &column_name,
                                 const common::ObString &sql_text,
                                 common::ObIAllocator &allocator,
                                 common::ObString &retry_sql);

} // namespace sql
} // namespace oceanbase
#endif // OB_AI_COLUMN_TASK_H_
