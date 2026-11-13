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

#define USING_LOG_PREFIX SERVER
#include "config_bridge.h"
#include "share/config/ob_config_helper.h"
#include "ob_server_duty_task.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "observer/omt/ob_server_runtime.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace oceanbase {
using namespace share;
using namespace share::schema;
using namespace sql;
namespace observer {

ObServerDutyTask::ObServerDutyTask()
  : allocator_(ObModIds::OB_DUTY_TASK)
{
}

void ObServerDutyTask::runTimerTask()
{
  allocator_.reset_remain_one_page();
  update_runtime_settings();
}

int ObServerDutyTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObServerDutyTask::update_runtime_settings()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(update_ctx_memory_throttle())) {
  }
}

int ObServerDutyTask::update_ctx_memory_throttle()
{
  int ret = OB_SUCCESS;
  {
    ObCtxMemoryLimitChecker checker;
    uint64_t ctx_id = 0;
    int64_t limit = 0;
    ObMallocAllocator *alloc = ObMallocAllocator::get_instance();
    rust::String limits = config::_ctx_memory_limit();
    if (!checker.check(limits.c_str(), ctx_id, limit)) {
      // do nothing
    } else {
      if (limits.empty()) {
        ctx_id = ObCtxIds::MAX_CTX_ID;
        limit = INT64_MAX; // empty str means no limit, and not care ctx_id.
      }
      for (int i = 0; i < ObCtxIds::MAX_CTX_ID; i++) {
        if (ObCtxIds::WORK_AREA == i ||
            ObCtxIds::META_OBJ_CTX_ID == i) {
          continue;
        }
        auto allocator = alloc->get_ctx_allocator(i);
        if (OB_NOT_NULL(allocator)) {
          if (OB_FAIL(allocator->set_limit(ctx_id == i ? limit : INT64_MAX))) {
          }
        }
      }
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

int ObSqlMemoryTimerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObSqlMemoryTimerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  {
    SERVER_MODULE_SCOPE {
      ObSqlMemoryManager *sql_mem_mgr = ::oceanbase::share::server_service<::oceanbase::sql::ObSqlMemoryManager>();
      if (OB_UNLIKELY(nullptr == sql_mem_mgr)) {
      } else if (OB_FAIL(sql_mem_mgr->calculate_global_bound_size())) {
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////////////////////
// AI column asynchronous task worker

namespace {

const char *const AI_COLUMN_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";
const int64_t AI_COLUMN_TASK_BATCH_SIZE = 32;
const int64_t AI_COLUMN_TASK_CLAIM_LEASE_SEC = 30;
const int64_t AI_COLUMN_TASK_RETRY_DELAY_SEC = 1;
const int64_t AI_COLUMN_TASK_ERROR_MSG_LEN = 512;
// The task rows of a transaction that has not been committed yet are kept in
// this state, the worker waits for the transaction to become visible.
const int64_t AI_TASK_STATE_UNCOMMITTED = 4;

struct AiColumnTaskRow
{
  int64_t table_id_;
  int64_t request_id_;
  int64_t column_id_;
  int64_t retry_count_;
  int64_t state_;
  int64_t result_type_;
  int64_t max_retries_;
  common::ObString rowkey_predicate_;
  common::ObString column_name_;
  common::ObString model_name_;
  common::ObString prompt_expr_;
  common::ObString system_hint_;
  common::ObString table_name_;
  common::ObString db_name_;
  AiColumnTaskRow()
    : table_id_(0), request_id_(0), column_id_(0), retry_count_(0), state_(0),
      result_type_(0), max_retries_(0), rowkey_predicate_(), column_name_(),
      model_name_(), prompt_expr_(), system_hint_(), table_name_(), db_name_()
  {}
};

struct AiColumnTaskValue
{
  int64_t column_id_;
  common::ObString column_name_;
  common::ObString value_;
  int64_t data_type_;
  bool is_success_;
  AiColumnTaskValue()
    : column_id_(0), column_name_(), value_(), data_type_(0), is_success_(false)
  {}
};

bool ai_is_integer_obj_type(const ObObjType type)
{
  bool bret = false;
  switch (type) {
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType:
    case ObUTinyIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type:
      bret = true;
      break;
    default:
      bret = false;
      break;
  }
  return bret;
}

int ai_append_char(common::ObSqlString &sql, const char c)
{
  return sql.append(&c, 1);
}

int ai_append_escaped(common::ObSqlString &sql, const common::ObString &value)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c || '\\' == c) {
      if (OB_FAIL(ai_append_char(sql, '\\'))) {
        LOG_WARN("ai column task, append escape char failed", KR(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(ai_append_char(sql, c))) {
      LOG_WARN("ai column task, append literal char failed", KR(ret));
    }
  }
  return ret;
}

int ai_append_column_value(common::ObSqlString &sql,
                           const common::ObString &value,
                           const int64_t data_type)
{
  int ret = OB_SUCCESS;
  const ObObjType type = static_cast<ObObjType>(data_type);
  if (ObJsonType == type) {
    if (OB_FAIL(sql.append("cast('"))) {
      LOG_WARN("ai column task, append json prefix failed", KR(ret));
    } else if (OB_FAIL(ai_append_escaped(sql, value))) {
    } else if (OB_FAIL(sql.append("' as json)"))) {
      LOG_WARN("ai column task, append json suffix failed", KR(ret));
    }
  } else if (ai_is_integer_obj_type(type)) {
    if (OB_FAIL(sql.append("cast('"))) {
      LOG_WARN("ai column task, append int prefix failed", KR(ret));
    } else if (OB_FAIL(ai_append_escaped(sql, value))) {
    } else if (OB_FAIL(sql.append("' as signed)"))) {
      LOG_WARN("ai column task, append int suffix failed", KR(ret));
    }
  } else {
    if (OB_FAIL(ai_append_char(sql, '\''))) {
      LOG_WARN("ai column task, append string prefix failed", KR(ret));
    } else if (OB_FAIL(ai_append_escaped(sql, value))) {
    } else if (OB_FAIL(ai_append_char(sql, '\''))) {
      LOG_WARN("ai column task, append string suffix failed", KR(ret));
    }
  }
  return ret;
}

int ai_load_tasks(common::ObIAllocator &alloc, common::ObIArray<AiColumnTaskRow> &tasks)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt(
               "select t.table_id, t.request_id, t.column_id, t.rowkey_predicate, t.retry_count, t.state,"
               " a.column_name as ai_column_name, a.model_name as ai_model_name,"
               " a.prompt_expr as ai_prompt_expr, a.system_hint as ai_system_hint,"
               " a.result_type as ai_result_type, a.max_retries as ai_max_retries,"
               " tb.table_name as ai_table_name, db.database_name as ai_database_name"
               " from %s t"
               " join oceanbase.__all_ai_column a"
               " on a.table_id = t.table_id and a.column_id = t.column_id"
               " join oceanbase.__all_table tb on tb.table_id = t.table_id"
               " join oceanbase.__all_database db on db.database_id = tb.database_id"
               " where (t.state in (0, 3) or t.state = 1 or t.state = 4)"
               " and t.eligible_ts <= unix_timestamp()"
               " order by t.state, t.table_id, t.request_id, t.column_id limit %ld",
               AI_COLUMN_TASK_TABLE_NAME, AI_COLUMN_TASK_BATCH_SIZE))) {
    LOG_WARN("ai column task, build task query failed", KR(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("ai column task, read task rows failed", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column task, null mysql result", KR(ret));
      } else {
        while (OB_SUCC(ret)) {
          ret = result->next();
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          } else if (OB_FAIL(ret)) {
            LOG_WARN("ai column task, iterate task rows failed", KR(ret));
          } else {
            AiColumnTaskRow row;
            common::ObString str;
            if (OB_FAIL(result->get_int("table_id", row.table_id_))) {
              LOG_WARN("ai column task, get table id failed", KR(ret));
            } else if (OB_FAIL(result->get_int("request_id", row.request_id_))) {
              LOG_WARN("ai column task, get request id failed", KR(ret));
            } else if (OB_FAIL(result->get_int("column_id", row.column_id_))) {
              LOG_WARN("ai column task, get column id failed", KR(ret));
            } else if (OB_FAIL(result->get_int("retry_count", row.retry_count_))) {
              LOG_WARN("ai column task, get retry count failed", KR(ret));
            } else if (OB_FAIL(result->get_int("state", row.state_))) {
              LOG_WARN("ai column task, get state failed", KR(ret));
            } else if (OB_FAIL(result->get_int("ai_result_type", row.result_type_))) {
              LOG_WARN("ai column task, get result type failed", KR(ret));
            } else if (OB_FAIL(result->get_int("ai_max_retries", row.max_retries_))) {
              LOG_WARN("ai column task, get max retries failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("rowkey_predicate", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.rowkey_predicate_))) {
              LOG_WARN("ai column task, get rowkey predicate failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("ai_column_name", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.column_name_))) {
              LOG_WARN("ai column task, get column name failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("ai_model_name", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.model_name_))) {
              LOG_WARN("ai column task, get model name failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("ai_prompt_expr", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.prompt_expr_))) {
              LOG_WARN("ai column task, get prompt expr failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("ai_system_hint", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.system_hint_))) {
              LOG_WARN("ai column task, get system hint failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("ai_table_name", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.table_name_))) {
              LOG_WARN("ai column task, get table name failed", KR(ret));
            } else if (OB_FAIL(result->get_varchar("ai_database_name", str))
                       || OB_FAIL(ob_write_string(alloc, str, row.db_name_))) {
              LOG_WARN("ai column task, get database name failed", KR(ret));
            } else if (OB_FAIL(tasks.push_back(row))) {
              LOG_WARN("ai column task, push task row failed", KR(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ai_build_group_sql(common::ObSqlString &sql,
                       const char *const prefix,
                       const char *const suffix,
                       const common::ObIArray<AiColumnTaskRow> &tasks,
                       const int64_t begin,
                       const int64_t end)
{
  int ret = OB_SUCCESS;
  if (begin >= end) {
  } else if (OB_FAIL(sql.append(prefix))) {
    LOG_WARN("ai column task, append sql prefix failed", KR(ret));
  } else if (OB_FAIL(sql.append_fmt("table_id = %ld and request_id = %ld and column_id in (",
                                    tasks.at(begin).table_id_,
                                    tasks.at(begin).request_id_))) {
    LOG_WARN("ai column task, append group condition failed", KR(ret));
  } else {
    for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
      if (OB_FAIL(sql.append_fmt("%s%ld", (i == begin ? "" : ", "), tasks.at(i).column_id_))) {
        LOG_WARN("ai column task, append column id failed", KR(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(suffix))) {
      LOG_WARN("ai column task, append sql suffix failed", KR(ret));
    }
  }
  return ret;
}

int ai_claim_task_group(const common::ObIArray<AiColumnTaskRow> &tasks,
                        const int64_t begin,
                        const int64_t end,
                        int64_t &affected)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  affected = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("update %s set state = 1, eligible_ts = unix_timestamp() + %ld where ",
                                    AI_COLUMN_TASK_TABLE_NAME,
                                    AI_COLUMN_TASK_CLAIM_LEASE_SEC))) {
    LOG_WARN("ai column task, build claim sql failed", KR(ret));
  } else if (OB_FAIL(ai_build_group_sql(sql, "", ")", tasks, begin, end))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
    LOG_WARN("ai column task, claim tasks failed", KR(ret), K(sql));
  }
  return ret;
}

int ai_delete_task_group(const common::ObIArray<AiColumnTaskRow> &tasks,
                         const int64_t begin,
                         const int64_t end,
                         int64_t &affected)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  affected = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("delete from %s where ", AI_COLUMN_TASK_TABLE_NAME))) {
    LOG_WARN("ai column task, build delete sql failed", KR(ret));
  } else if (OB_FAIL(ai_build_group_sql(sql, "", "", tasks, begin, end))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
    LOG_WARN("ai column task, delete tasks failed", KR(ret), K(sql));
  }
  return ret;
}

int ai_eval_prompt(common::ObIAllocator &alloc,
                   const common::ObString &db_name,
                   const common::ObString &table_name,
                   const common::ObString &prompt_expr,
                   const common::ObString &predicate,
                   common::ObString &prompt)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("select "))) {
    LOG_WARN("ai column task, build prompt sql failed", KR(ret));
  } else if (OB_FAIL(sql.append(prompt_expr))) {
    LOG_WARN("ai column task, append prompt expr failed", KR(ret));
  } else if (OB_FAIL(sql.append_fmt(" as __ai_prompt from `%.*s`.`%.*s` where ",
                                    db_name.length(), db_name.ptr(),
                                    table_name.length(), table_name.ptr()))) {
    LOG_WARN("ai column task, append prompt table failed", KR(ret));
  } else if (OB_FAIL(sql.append(predicate))) {
    LOG_WARN("ai column task, append prompt predicate failed", KR(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      common::ObString value;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("ai column task, read prompt value failed", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column task, null prompt result", KR(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("ai column task, fetch prompt row failed", KR(ret), K(sql));
      } else if (OB_FAIL(result->get_varchar("__ai_prompt", value))) {
        LOG_WARN("ai column task, get prompt value failed", KR(ret), K(sql));
      } else if (OB_FAIL(ob_write_string(alloc, value, prompt))) {
        LOG_WARN("ai column task, copy prompt value failed", KR(ret));
      }
    }
  }
  return ret;
}

int ai_call_model(common::ObIAllocator &alloc,
                  const common::ObString &model_name,
                  const common::ObString &system_hint,
                  const common::ObString &prompt,
                  common::ObString &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *func_info = NULL;
  share::ObAiModelEndpointInfo endpoint_info;
  query::ObIAiEndpointResolver *resolver = NULL;
  if (OB_ISNULL(resolver = ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column task, ai endpoint resolver is null", KR(ret));
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, model_name, func_info))) {
    LOG_WARN("ai column task, get ai func info failed", KR(ret), K(model_name));
  } else if (OB_ISNULL(func_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column task, null ai func info", KR(ret), K(model_name));
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, alloc, endpoint_info))) {
    LOG_WARN("ai column task, resolve model endpoint failed", KR(ret), K(model_name));
  } else {
    ObAIFuncModel model(alloc, *func_info, endpoint_info);
    if (ObAIFuncUtils::is_dense_embedding_type(func_info)) {
      // An embedding model computes the vector of the prompt, it does not
      // answer with a completion.
      ObString content = prompt;
      if (OB_FAIL(model.call_dense_embedding(content, NULL, result))) {
        LOG_WARN("ai column task, call ai model failed", KR(ret), K(model_name));
      }
    } else if (OB_FAIL(model.call_completion_with_system(system_hint, prompt, NULL, result))) {
      LOG_WARN("ai column task, call ai model failed", KR(ret), K(model_name));
    }
  }
  return ret;
}

int ai_publish_task_group(common::ObIArray<AiColumnTaskValue> &values,
                          const common::ObIArray<AiColumnTaskRow> &tasks,
                          const int64_t begin,
                          const int64_t end,
                          const common::ObString &db_name,
                          const common::ObString &table_name,
                          const common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  common::ObSqlString del_sql;
  common::ObSqlString upd_sql;
  ObMySQLTransaction trans;
  int64_t affected = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ai_build_group_sql(
               del_sql,
               "delete from oceanbase.__all_ai_column_task where ",
               ")",
               tasks,
               begin,
               end))) {
    LOG_WARN("ai column task, build publish delete sql failed", KR(ret));
  } else if (OB_FAIL(upd_sql.append_fmt("update `%.*s`.`%.*s` set ",
                                        db_name.length(), db_name.ptr(),
                                        table_name.length(), table_name.ptr()))) {
    LOG_WARN("ai column task, build publish update sql failed", KR(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
      AiColumnTaskValue &value = values.at(i);
      if (OB_FAIL(upd_sql.append_fmt("%s`%.*s` = ",
                                     (0 == i ? "" : ", "),
                                     value.column_name_.length(),
                                     value.column_name_.ptr()))) {
        LOG_WARN("ai column task, append result column failed", KR(ret));
      } else if (OB_FAIL(ai_append_column_value(upd_sql, value.value_, value.data_type_))) {
        LOG_WARN("ai column task, append result value failed", KR(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(upd_sql.append_fmt(" where "))) {
        LOG_WARN("ai column task, append update where failed", KR(ret));
      } else if (OB_FAIL(upd_sql.append(predicate))) {
        LOG_WARN("ai column task, append update predicate failed", KR(ret));
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_, true))) {
    LOG_WARN("ai column task, start publish transaction failed", KR(ret));
  } else if (OB_FAIL(trans.write(del_sql.ptr(), affected))) {
    LOG_WARN("ai column task, delete published tasks failed", KR(ret), K(del_sql));
  } else if (affected != values.count()) {
    // the task rows were replaced by a newer change of the same row, the result
    // must not be published
    ret = OB_EAGAIN;
    LOG_INFO("ai column task, stale task group, skip publishing the result",
             K(affected), K(values.count()), K(del_sql));
  } else if (OB_FAIL(trans.write(upd_sql.ptr(), affected))) {
    LOG_WARN("ai column task, publish result failed", KR(ret), K(upd_sql));
  } else if (affected > 1) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column task, publish result affected too many rows", KR(ret), K(affected), K(upd_sql));
  }
  if (trans.is_started()) {
    const bool need_commit = OB_SUCC(ret);
    int tmp_ret = trans.end(need_commit);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("ai column task, end publish transaction failed", K(tmp_ret), K(need_commit));
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }
  return ret;
}

int ai_fail_task_group(common::ObIArray<AiColumnTaskValue> &values,
                       const common::ObIArray<AiColumnTaskRow> &tasks,
                       const int64_t begin,
                       const int64_t end,
                       const int64_t error_code)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
    const AiColumnTaskRow &row = tasks.at(i);
    const int64_t next_retry = row.retry_count_ + 1;
    sql.reset();
    if (next_retry > row.max_retries_) {
      // the task reaches its terminal state: no pending task is left and the
      // result column keeps NULL
      if (OB_FAIL(sql.append_fmt("delete from %s where table_id = %ld and request_id = %ld and column_id = %ld",
                                 AI_COLUMN_TASK_TABLE_NAME,
                                 row.table_id_, row.request_id_, row.column_id_))) {
        LOG_WARN("ai column task, build terminal delete sql failed", KR(ret));
      }
    } else {
      if (OB_FAIL(sql.append_fmt(
              "update %s set retry_count = %ld, state = 3, eligible_ts = unix_timestamp() + %ld,"
              " error_msg = 'ai model call failed, error code %ld'"
              " where table_id = %ld and request_id = %ld and column_id = %ld",
              AI_COLUMN_TASK_TABLE_NAME,
              next_retry,
              AI_COLUMN_TASK_RETRY_DELAY_SEC,
              error_code,
              row.table_id_, row.request_id_, row.column_id_))) {
        LOG_WARN("ai column task, build retry sql failed", KR(ret));
      }
    }
    if (OB_SUCC(ret)) {
      int64_t affected = 0;
      if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
        LOG_WARN("ai column task, update task state failed", KR(ret), K(sql));
      }
    }
  }
  UNUSED(values);
  return ret;
}

// A task is only meaningful while the target result column is still NULL. When
// the change that produced it was rolled back (or a newer change already
// published a value), the task row is stale: drop it without calling the model
// so that no request is ever sent for a value the user rolled back.
bool ai_result_column_needed(const AiColumnTaskRow &row)
{
  bool needed = true;
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res)
  {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(sql.append_fmt("select `%.*s` is null as ai_need from `%.*s`.`%.*s` where %.*s",
                               static_cast<int32_t>(row.column_name_.length()), row.column_name_.ptr(),
                               static_cast<int32_t>(row.db_name_.length()), row.db_name_.ptr(),
                               static_cast<int32_t>(row.table_name_.length()), row.table_name_.ptr(),
                               static_cast<int32_t>(row.rowkey_predicate_.length()),
                               row.rowkey_predicate_.ptr()))) {
      LOG_WARN("ai column task, build result probe sql failed", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("ai column task, read result probe failed", K(ret), "sql", sql.ptr());
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        needed = false;
        LOG_INFO("ai column task, target row is gone, drop task", K(row.table_id_),
                 K(row.request_id_), K(row.column_id_));
      }
    } else {
      int64_t need = 0;
      if (OB_FAIL(result->get_int("ai_need", need))) {
      } else if (0 == need) {
        needed = false;
        LOG_INFO("ai column task, result is already ready, drop task", K(row.table_id_),
                 K(row.request_id_), K(row.column_id_));
      }
    }
  }
  if (OB_SUCCESS != ret) {
    return true;
  }
  if (!needed) {
    ObSqlString del;
    int64_t affected = 0;
    if (OB_FAIL(del.append_fmt("delete from oceanbase.__all_ai_column_task where table_id = %ld "
                               "and request_id = %ld and column_id = %ld",
                               row.table_id_, row.request_id_, row.column_id_))
        || OB_FAIL(GCTX.sql_proxy_->write(del.ptr(), affected))) {
      LOG_WARN("ai column task, drop stale task failed", K(ret), K(del.ptr()));
    }
  }
  return needed;
}

// The task rows of a transaction that has not been committed yet must not be
// executed: the worker would read the value of the row before the change and
// would publish a result that is based on the outdated data.  Such a task
// becomes ready once the transaction is committed, the invalidated result
// column is then visible and NULL.
bool ai_task_is_ready(const AiColumnTaskRow &row)
{
  if (AI_TASK_STATE_UNCOMMITTED != row.state_) {
    return true;
  }
  bool ready = false;
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res)
  {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(sql.append_fmt("select `%.*s` is null as ai_need from `%.*s`.`%.*s` where %.*s",
                               static_cast<int32_t>(row.column_name_.length()), row.column_name_.ptr(),
                               static_cast<int32_t>(row.db_name_.length()), row.db_name_.ptr(),
                               static_cast<int32_t>(row.table_name_.length()), row.table_name_.ptr(),
                               static_cast<int32_t>(row.rowkey_predicate_.length()),
                               row.rowkey_predicate_.ptr()))) {
      LOG_WARN("ai column task, build commit probe sql failed", K(ret));
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("ai column task, read commit probe failed", K(ret), "sql", sql.ptr());
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END != ret) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else {
      int64_t need = 0;
      if (OB_FAIL(result->get_int("ai_need", need))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (1 == need) {
        // the transaction is committed and the result column is invalidated
        ready = true;
      }
    }
  }
  if (OB_SUCCESS != ret) {
    // the state of the transaction is unknown, retry in the next round
    return false;
  }
  if (!ready) {
    LOG_INFO("ai column task, wait for the commit of the transaction",
             K(row.table_id_), K(row.request_id_), K(row.column_id_));
  }
  return ready;
}

int ai_process_task_group(common::ObIAllocator &alloc,
                          const common::ObIArray<AiColumnTaskRow> &tasks,
                          const int64_t begin,
                          const int64_t end)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  int64_t first_error = OB_SUCCESS;
  bool all_success = true;
  common::ObSEArray<AiColumnTaskValue, AI_COLUMN_TASK_BATCH_SIZE> values;
  if (begin >= end) {
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ai_claim_task_group(tasks, begin, end, affected))) {
    LOG_WARN("ai column task, claim task group failed", KR(ret));
  } else if (affected != end - begin) {
    // another worker owns the group, retry in the next round
    LOG_INFO("ai column task, task group is claimed by another worker",
             K(affected), "expected", end - begin);
  } else {
    for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
      const AiColumnTaskRow &row = tasks.at(i);
      AiColumnTaskValue value;
      common::ObString prompt;
      if (!ai_result_column_needed(row)) {
      } else if (OB_FAIL(ai_eval_prompt(alloc, row.db_name_, row.table_name_,
                                 row.prompt_expr_, row.rowkey_predicate_, prompt))) {
        LOG_WARN("ai column task, evaluate prompt failed", KR(ret), "column_id", row.column_id_);
        all_success = false;
        if (OB_SUCCESS == first_error) {
          first_error = ret;
        }
        ret = OB_SUCCESS;
      } else if (OB_FAIL(ai_call_model(alloc, row.model_name_, row.system_hint_,
                                       prompt, value.value_))) {
        LOG_WARN("ai column task, call ai model failed", KR(ret), "column_id", row.column_id_);
        all_success = false;
        if (OB_SUCCESS == first_error) {
          first_error = ret;
        }
        ret = OB_SUCCESS;
      } else {
        value.column_id_ = row.column_id_;
        value.column_name_ = row.column_name_;
        value.data_type_ = row.result_type_;
        value.is_success_ = true;
        if (OB_FAIL(values.push_back(value))) {
          LOG_WARN("ai column task, push result value failed", KR(ret));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (!all_success || values.count() != end - begin) {
      ret = ai_fail_task_group(values, tasks, begin, end, first_error);
      if (OB_FAIL(ret)) {
        LOG_WARN("ai column task, mark task group failed", KR(ret));
      }
      ret = OB_SUCCESS;
    } else if (OB_FAIL(ai_publish_task_group(values, tasks, begin, end,
                                             tasks.at(begin).db_name_,
                                             tasks.at(begin).table_name_,
                                             tasks.at(begin).rowkey_predicate_))) {
      if (OB_EAGAIN == ret) {
        // stale result, dropped by design
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("ai column task, publish task group failed", KR(ret));
        int tmp_ret = ai_fail_task_group(values, tasks, begin, end, ret);
        UNUSED(tmp_ret);
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

void ai_process_tasks()
{
  int ret = OB_SUCCESS;
  common::ObArenaAllocator allocator(ObModIds::OB_DUTY_TASK);
  common::ObSEArray<AiColumnTaskRow, AI_COLUMN_TASK_BATCH_SIZE> tasks;
  common::ObSEArray<AiColumnTaskRow, AI_COLUMN_TASK_BATCH_SIZE> ready_tasks;
  if (OB_FAIL(ai_load_tasks(allocator, tasks))) {
    LOG_WARN("ai column task, load pending tasks failed", KR(ret));
  } else if (tasks.empty()) {
  } else {
    // The task rows of a transaction that has not been committed yet are not
    // ready, they are processed in a later round of the worker.
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      if (ai_task_is_ready(tasks.at(i))) {
        if (OB_FAIL(ready_tasks.push_back(tasks.at(i)))) {
          LOG_WARN("ai column task, push ready task row failed", KR(ret));
        }
      }
    }
    int64_t begin = 0;
    while (OB_SUCC(ret) && begin < ready_tasks.count()) {
      int64_t end = begin + 1;
      while (end < ready_tasks.count()
             && ready_tasks.at(end).table_id_ == ready_tasks.at(begin).table_id_
             && ready_tasks.at(end).request_id_ == ready_tasks.at(begin).request_id_) {
        ++end;
      }
      int tmp_ret = ai_process_task_group(allocator, ready_tasks, begin, end);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("ai column task, process task group failed", K(tmp_ret), K(begin), K(end));
      }
      begin = end;
    }
  }
}

}  // namespace

ObAIColumnTaskWorker::ObAIColumnTaskWorker()
{
}

int ObAIColumnTaskWorker::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObAIColumnTaskWorker::runTimerTask()
{
  int ret = OB_SUCCESS;
  {
    SERVER_MODULE_SCOPE {
      // The worker is scheduled during init_server_runtime(), i.e. before the
      // bootstrap has created the inner tables.  Running the task query while
      // the bootstrap transaction is still open makes bootstrap fail with
      // OB_ERR_PRIMARY_KEY_DUPLICATE.  SS_SERVING is set only after bootstrap
      // and all local services have completed.
      if (OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)
          || share::SS_SERVING != GCTX.status_) {
        // the server is not ready yet
      } else {
        ai_process_tasks();
      }
    }
  }
}

}  // observer
}  // oceanbase
