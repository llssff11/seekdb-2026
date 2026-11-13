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
#include "config_bridge.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "lib/string/ob_sql_string.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "sql/engine/expr/ob_datum_cast.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/das/ob_das_insert_op.h"
#include "sql/das/ob_das_delete_op.h"
#include "sql/das/ob_das_update_op.h"
#include "sql/das/ob_das_lock_op.h"
#include "sql/das/ob_das_utils.h"
#include "sql/engine/dml/ob_trigger_handler.h"
#include "share/autoincrement/ob_i_tablet_autoincrement_service.h"
#include "data_plane/lob/ob_lob_read.h"
#include "data_plane/transaction/ob_i_transaction_service.h"
#include "data_plane/transaction/ob_tx_desc_access.h"
#include "sql/pl/ob_pl.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "share/geo/ob_geo_utils.h"
#include "sql/engine/ob_batch_rows.h"
#include "sql/engine/ob_physical_plan.h"
#include "query/vector/ob_vector_index_util.h"
#include "sql/engine/dml/ob_ai_column_task.h"
#include "lib/lock/ob_lock_guard.h"
#include "lib/lock/ob_mutex.h"
#include <vector>
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/expr/ob_raw_expr.h"
namespace oceanbase
{
using namespace common;
using namespace share;
using namespace transaction;
namespace sql
{

bool ObDMLService::check_cascaded_reference(const ObExpr *expr, const ObExprPtrIArray &row)
{
  bool bret = false;
  if (OB_ISNULL(expr) || expr->parent_cnt_ <= 0) {
    bret = false;
  } else {
    for (int i = 0; !bret && i < expr->parent_cnt_; ++i) {
      ObExpr *parent_expr = expr->parents_[i];
      if (parent_expr != nullptr) {
        if (parent_expr->type_ == T_FUN_COLUMN_CONV) {
          bret = has_exist_in_array(row, parent_expr);
        }
      }
      if (!bret) {
        bret = check_cascaded_reference(parent_expr, row);
      }
    }
  }
  return bret;
}

int ObDMLService::check_row_null(const ObExprPtrIArray &row,
                                 ObEvalCtx &eval_ctx,
                                 int64_t row_num,
                                 const ColContentIArray &column_infos,
                                 bool is_ignore,
                                 bool is_single_value,
                                 ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  CK(row.count() >= column_infos.count());
  if (OB_ISNULL(session = dml_op.get_exec_ctx().get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int i = 0; OB_SUCC(ret) && i < column_infos.count(); i++) {
    ObDatum *datum = NULL;
    const bool is_nullable = column_infos.at(i).is_nullable_;
    uint64_t col_idx = column_infos.at(i).projector_index_;
    if (OB_FAIL(row.at(col_idx)->eval(eval_ctx, datum))) {
      common::ObString column_name = column_infos.at(i).column_name_;
      ret = ObDMLService::log_user_error_inner(ret, row_num, column_name, dml_op.get_exec_ctx(),
                                               row.at(col_idx)->datum_meta_.type_);
    } else if (OB_ISNULL(datum)) {
      // impossible
    } else if (OB_FAIL(check_column_null(dml_op.get_eval_ctx(),
                                         row,
                                         column_infos.at(i),
                                         *datum,
                                         is_ignore,
                                         is_single_value,
                                         session->get_sql_mode()))) {
    }
  }
  return ret;
}

int ObDMLService::check_column_null(
    ObEvalCtx &eval_ctx,
    const ObExprPtrIArray &row,
    const ColumnContent &column_info,
    ObDatum &datum,
    const bool is_ignore,
    const bool is_single_value,
    const ObSQLMode sql_mode)
{
  int ret = OB_SUCCESS;
  const bool is_nullable = column_info.is_nullable_;
  uint64_t col_idx = column_info.projector_index_;
  ObExpr *expr = row.at(col_idx);
  if (!is_nullable && datum.is_null()) {
    if (is_ignore || (!is_single_value
                      && !is_strict_mode(sql_mode))) {
      ObObj zero_obj;
      ObExprStrResAlloc res_alloc(*expr, eval_ctx);
      ObDatum &row_datum = expr->locate_datum_for_write(eval_ctx);
      bool is_decimal_int = ob_is_decimal_int(expr->datum_meta_.type_);
      if (ob_is_geometry(expr->obj_meta_.get_type())) {
          ret = OB_BAD_NULL_ERROR;
          LOG_WARN("dml with ignore not supported in geometry type");
          LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_info.column_name_.length(), column_info.column_name_.ptr());
      } else if (ob_is_collection_sql_type(expr->obj_meta_.get_type())) {
          ret = OB_BAD_NULL_ERROR;
          LOG_WARN("dml with ignore not supported in collection type");
          LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_info.column_name_.length(), column_info.column_name_.ptr());
      } else if (check_cascaded_reference(expr, row)) {
        //This column is dependent on other columns and cannot be modified again;
        //otherwise, it will necessitate a cascading recalculation of the dependent expression results.
        ret = OB_BAD_NULL_ERROR;
        LOG_WARN("dml with ignore not supported with cascaded column", KPC(expr));
        LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_info.column_name_.length(), column_info.column_name_.ptr());
      } else if (OB_FAIL(sql::get_obj_zero_value(
          expr->obj_meta_.get_type(),
          expr->obj_meta_.get_collation_type(),
          zero_obj))) {
      } else if (is_decimal_int) {
        ObDecimalIntBuilder dec_val;
        dec_val.set_zero(wide::ObDecimalIntConstValue::get_int_bytes_by_precision(
            expr->datum_meta_.precision_));
        row_datum.set_decimal_int(dec_val.get_decimal_int(), dec_val.get_int_bytes());
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObDASUtils::padding_fixed_string_value(expr->max_length_,
                                                                res_alloc,
                                                                zero_obj))) {
      } else if (!is_decimal_int && OB_FAIL(row_datum.from_obj(zero_obj))) {
      } else if (zero_obj.is_lob_storage() && zero_obj.has_lob_header() != expr->obj_meta_.has_lob_header()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        //output warning msg
        const ObString &column_name = column_info.column_name_;
        LOG_USER_WARN(OB_BAD_NULL_ERROR, column_name.length(), column_name.ptr());
      }
    } else if (ob_is_user_defined_type(expr->obj_meta_.get_type())) {
      // do nothing for xml column
    } else {
      //output warning msg
      const ObString &column_name = column_info.column_name_;
      ret = OB_BAD_NULL_ERROR;
      LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_name.length(), column_name.ptr());
    }
  }
  return ret;
}

int ObDMLService::check_column_type(const ExprFixedArray &dml_row,
                                    int64_t row_num,
                                    const ObIArray<ColumnContent> &column_infos,
                                    ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  CK(dml_row.count() >= column_infos.count());
  ObArenaAllocator tmp_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObUserLoggingCtx::Guard logging_ctx_guard(*dml_op.get_exec_ctx().get_user_logging_ctx());
  dml_op.get_exec_ctx().set_cur_rownum(row_num);
  for (int64_t i = 0; OB_SUCC(ret) && i < column_infos.count(); ++i) {
    const ColumnContent &column_info = column_infos.at(i);
    common::ObString column_name = column_infos.at(i).column_name_;
    dml_op.get_exec_ctx().set_cur_column_name(&column_name);
    ObExpr *expr = dml_row.at(column_info.projector_index_);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(dml_op.get_eval_ctx(), datum))) {
      ret = ObDMLService::log_user_error_inner(ret, row_num, column_name, dml_op.get_exec_ctx(),
                                               expr->datum_meta_.type_);
    } else if (OB_ISNULL(datum)) {
      // impossible
    } else if (OB_FAIL(check_geometry_type(dml_op.get_eval_ctx(),
                                           dml_row,
                                           column_info,
                                           tmp_allocator,
                                           *datum))) {
    }
  }
  return ret;
}

int ObDMLService::check_geometry_type(
    ObEvalCtx &eval_ctx,
    const ExprFixedArray &dml_row,
    const ColumnContent &column_info,
    ObIAllocator &allocator,
    ObDatum &datum)
{
  int ret = OB_SUCCESS;
  ObExpr *expr = dml_row.at(column_info.projector_index_);
  if (!datum.is_null() && expr->obj_meta_.is_geometry()) {
    const uint32_t column_srid = column_info.srs_info_.srid_;
    const ObGeoType column_geo_type = static_cast<ObGeoType>(column_info.srs_info_.geo_type_);
    ObString wkb = datum.get_string();
    uint32_t input_srid = UINT32_MAX;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
        eval_ctx.exec_ctx_, allocator, datum,
        expr->datum_meta_, expr->obj_meta_.has_lob_header(), wkb))) {
    } else if (OB_FAIL(ObGeoTypeUtil::check_geo_type(column_geo_type, wkb))) {
      ret = OB_ERR_CANT_CREATE_GEOMETRY_OBJECT;
      LOG_USER_ERROR(OB_ERR_CANT_CREATE_GEOMETRY_OBJECT);
    } else if (OB_FAIL(ObGeoTypeUtil::get_srid_from_wkb(wkb, input_srid))) {
    } else if (OB_ISNULL(eval_ctx.exec_ctx_.get_srs_provider())) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(ObSqlGeoUtils::check_srid(
                   *eval_ctx.exec_ctx_.get_srs_provider(),
                   column_srid,
                   input_srid))) {
      ret = OB_ERR_WRONG_SRID_FOR_COLUMN;
      LOG_USER_ERROR(OB_ERR_WRONG_SRID_FOR_COLUMN, static_cast<uint64_t>(input_srid),
          static_cast<uint64_t>(column_srid));
    }
  }
  return ret;
}

int ObDMLService::check_rowkey_is_null(const ObExprPtrIArray &row,
                                       int64_t rowkey_cnt,
                                       ObEvalCtx &eval_ctx,
                                       bool &is_null)
{
  int ret = OB_SUCCESS;
  is_null = false;
  CK(row.count() >= rowkey_cnt);
  ObDatum *datum = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && !is_null && i < rowkey_cnt; ++i) {
    if (OB_FAIL(row.at(i)->eval(eval_ctx, datum))) {
    } else {
      is_null = datum->is_null();
    }
  }
  return ret;
}

int ObDMLService::check_table_cycle(const DASTableIdList* parent_table_set, const uint64_t table_id, bool &exist)
{
  int ret = OB_SUCCESS;
  exist = false;
  if (OB_NOT_NULL(parent_table_set)) {
    DASTableIdList::const_iterator it = parent_table_set->begin();
    for (; it != parent_table_set->end(); it++) {
      if (*it == table_id) {
        exist = true;
        break;
      }
    }
  }
  return ret;
}

int ObDMLService::delete_table_id_from_parent_table_set(ObDMLRtCtx &dml_rtctx, const ObDMLBaseCtDef &dml_ctdef)
{
  int ret = OB_SUCCESS;
  ObExecContext* root_ctx;
  if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_rtctx.get_exec_ctx(), root_ctx))) {
  } else if (OB_ISNULL(root_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    DASTableIdList& parent_table_set = root_ctx->get_das_ctx().get_parent_table_set();
    DASTableIdList::iterator it = parent_table_set.begin();
    bool found = false;
    for (; it != parent_table_set.end() && !found; it++) {
      if (*it == dml_ctdef.das_base_ctdef_.index_tid_) {
        found = true;
        parent_table_set.erase(it);
      }
    }
    if (!found) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObDMLService::check_rowkey_whether_distinct(const ObExprPtrIArray &row,
                                                DistinctType distinct_algo,
                                                ObEvalCtx &eval_ctx,
                                                ObExecContext &root_ctx,
                                                ObRowkey &tmp_table_rowkey,
                                                SeRowkeyDistCtx *rowkey_dist_ctx,
                                                bool &is_dist)
{
  int ret = OB_SUCCESS;
  is_dist = true;
  if (T_DISTINCT_NONE != distinct_algo) {
    if (T_HASH_DISTINCT == distinct_algo) {
      ObIAllocator &allocator = root_ctx.get_allocator();
      const int64_t rowkey_cnt = tmp_table_rowkey.get_obj_cnt();
      if (OB_ISNULL(rowkey_dist_ctx)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        //step1: Init ObObj of ObTableRowkey
        ObObj *tmp_obj_ptr = tmp_table_rowkey.get_obj_ptr();
        for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
          ObExpr *expr = row.at(i);
          ObDatum *col_datum = nullptr;
          if (OB_ISNULL(expr)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(expr->eval(eval_ctx, col_datum))) {
          } else if (OB_ISNULL(col_datum)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(col_datum->to_obj(tmp_obj_ptr[i], expr->obj_meta_, expr->obj_datum_map_))) {
          }
        }

        //step2: Perform distinct check use ObRowkey
        {
          ret = rowkey_dist_ctx->exist_refactored(tmp_table_rowkey);
          if (OB_HASH_EXIST == ret) {
            ret = OB_SUCCESS;
            is_dist = false;
          } else if (OB_HASH_NOT_EXIST == ret) {
            //step3: if not exist, deep copy data and add ObRowkey to hash set
            //step3.1: Init the buffer of ObObj Array
            ret = OB_SUCCESS;
            ObObj *obj_ptr = nullptr;
            void *buf = nullptr;
            if (OB_ISNULL(buf = allocator.alloc(sizeof(ObObj) * rowkey_cnt))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              obj_ptr = new(buf) ObObj[rowkey_cnt];
            }

            //step3.2: deep copy data to ObObj
            for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
              if (OB_FAIL(ob_write_obj(allocator, tmp_obj_ptr[i], obj_ptr[i]))) {
              }
            }
            ObRowkey table_rowkey(obj_ptr, rowkey_cnt);
            //step3.3: add ObRowkey to hash set
            if (OB_SUCC(ret) && OB_FAIL(rowkey_dist_ctx->set_refactored(table_rowkey))) {
            }
          } else {
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "Check Update/Delete row with merge distinct");
    }
  }
  return ret;
}

int ObDMLService::create_rowkey_check_hashset(int64_t estimate_row,
                                           ObExecContext *root_ctx,
                                           SeRowkeyDistCtx *&rowkey_dist_ctx)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = root_ctx->get_allocator();
  if (OB_ISNULL(rowkey_dist_ctx)) {
    //create rowkey distinct context
    void *buf = allocator.alloc(sizeof(SeRowkeyDistCtx));
    ObSQLSessionInfo *my_session = root_ctx->get_my_session();
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(my_session)) {
        ret = OB_ERR_UNEXPECTED;
    } else {
      rowkey_dist_ctx = new (buf) SeRowkeyDistCtx();
      int64_t match_rows = estimate_row > ObDMLBaseCtDef::MIN_ROWKEY_DISTINCT_BUCKET_NUM ?
                            estimate_row : ObDMLBaseCtDef::MIN_ROWKEY_DISTINCT_BUCKET_NUM;
      //
      // match_rows is the result of the optimizer's row estimation, if this value is very large,
      // Directly creating a hashmap with so many buckets will allocate
      // Not enough memory, here the limit is set to 64k to prevent out of memory errors
      const int64_t max_bucket_num = match_rows > ObDMLBaseCtDef::MAX_ROWKEY_DISTINCT_BUCKET_NUM ?
                              ObDMLBaseCtDef::MAX_ROWKEY_DISTINCT_BUCKET_NUM : match_rows;
      if (OB_FAIL(rowkey_dist_ctx->create(max_bucket_num,
                                      ObModIds::OB_DML_CHECK_ROWKEY_DISTINCT_BUCKET,
                                      ObModIds::OB_DML_CHECK_ROWKEY_DISTINCT_NODE))) {
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObDMLService::check_lob_column_changed(ObEvalCtx &eval_ctx,
            const ObExpr& old_expr, ObDatum& old_datum,
            const ObExpr& new_expr, ObDatum& new_datum,
            int64_t& result) {
  INIT_SUCC(ret);
  int64_t timeout = 0;
  int64_t query_st = eval_ctx.exec_ctx_.get_my_session()->get_query_start_time();
  if (OB_FAIL(eval_ctx.exec_ctx_.get_my_session()->get_query_timeout(timeout))) {
  } else {
    timeout += query_st;
    ObString old_str = old_datum.get_string();
    ObString new_str = new_datum.get_string();
    bool old_set_has_lob_header = old_expr.obj_meta_.has_lob_header() && old_str.length() > 0;
    bool new_set_has_lob_header = new_expr.obj_meta_.has_lob_header() && new_str.length() > 0;
    ObLobLocatorV2 old_lob(old_str, old_set_has_lob_header);
    ObLobLocatorV2 new_lob(new_str, new_set_has_lob_header);
    if (old_set_has_lob_header && new_set_has_lob_header) {
      bool is_equal = false;
      if (old_lob.is_persist_lob() && new_lob.is_delta_temp_lob()) {
        if (OB_FAIL(ObDeltaLob::has_diff(new_lob, result))) {
        }
      } else if (OB_FAIL(data_plane::lob_binary_equal(
                     old_lob, new_lob, timeout,
                     eval_ctx.exec_ctx_.get_my_session()->get_tx_desc(), is_equal))) {
      } else {
        result = is_equal ? 0 : 1;
      }
    } else {
      result = ObDatum::binary_equal(old_datum, new_datum) ? 0 : 1;
    }
  }
  return ret;
}

int ObDMLService::check_row_whether_changed(const ObUpdCtDef &upd_ctdef,
                                            ObUpdRtDef &upd_rtdef,
                                            ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  NG_TRACE_TIMES(2, update_start_check_row);
  if (!upd_ctdef.is_primary_index_) {
    // if in pdml, upd_rtdef.primary_rtdef_ is null
    // in normal dml stmt, upd_rtdef.primary_rtdef_ is not null
    upd_rtdef.is_row_changed_ = false;
    if (OB_NOT_NULL(upd_rtdef.primary_rtdef_)) {
      upd_rtdef.is_row_changed_ = upd_rtdef.primary_rtdef_->is_row_changed_;
    } else {
      // whether the global index row is updated is subject to the result of the primary index
      // pdml e.g.:
      // create table t22(a int primary key,
      //                 b int,
      //                 c int,
      //                 d timestamp DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
      // ) partition by hash(a) partitions 5;
      // create index idx on t22(c, d) global;
      // insert into t22 values(1, 1, 1, now());
      // set _force_parallel_dml_dop = 3;
      // update t22 set b=2, c=1 where a=1;
      // this case, we must update data_table and index_table
      const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
      const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
      FOREACH_CNT_X(info, upd_ctdef.assign_columns_, OB_SUCC(ret) && !upd_rtdef.is_row_changed_) {
        const uint64_t idx = info->projector_index_;
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
        } else {
          if(is_lob_storage(old_row.at(idx)->datum_meta_.type_)
              && is_lob_storage(new_row.at(idx)->datum_meta_.type_))
          {
            int64_t cmp_res = 0;
            if(OB_FAIL(check_lob_column_changed(eval_ctx, *old_row.at(idx), *old_datum, *new_row.at(idx), *new_datum, cmp_res))) {
            } else {
              upd_rtdef.is_row_changed_ = (cmp_res != 0);
            }
          } else {
            upd_rtdef.is_row_changed_ = !ObDatum::binary_equal(*old_datum, *new_datum);
          }
        }
      }
    }
  } else {
    upd_rtdef.is_row_changed_ = false;
    const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
    const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
    FOREACH_CNT_X(info, upd_ctdef.assign_columns_, OB_SUCC(ret) && !upd_rtdef.is_row_changed_) {
      const uint64_t idx = info->projector_index_;
      if (OB_LIKELY(!info->auto_filled_timestamp_)) {
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
        } else {
          if(is_lob_storage(old_row.at(idx)->datum_meta_.type_)
              && is_lob_storage(new_row.at(idx)->datum_meta_.type_))
          {
            int64_t cmp_res = 0;
            if(OB_FAIL(check_lob_column_changed(eval_ctx, *old_row.at(idx), *old_datum, *new_row.at(idx), *new_datum, cmp_res))) {
            } else {
              upd_rtdef.is_row_changed_ = (cmp_res != 0);
            }
          } else {
            upd_rtdef.is_row_changed_ = !ObDatum::binary_equal(*old_datum, *new_datum);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) &&
      upd_rtdef.is_row_changed_ &&
      upd_ctdef.is_primary_index_ &&
      upd_ctdef.dupd_ctdef_.is_batch_stmt_) {
    //check predicate column whether changed in batch stmt execution
    const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
    const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_ctdef.assign_columns_.count(); ++i) {
      const ColumnContent &info = upd_ctdef.assign_columns_.at(i);
      uint64_t idx = info.projector_index_;
      ObDatum *old_datum = NULL;
      ObDatum *new_datum = NULL;
      if (info.is_predicate_column_) {
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
        } else if (!ObDatum::binary_equal(*old_datum, *new_datum)) {
          //update the predicate column, will lead to the next stmt result change, need rollback
          ret = OB_BATCHED_MULTI_STMT_ROLLBACK;
        }
      }
    }
  }
  LOG_DEBUG("after check update row changed", K(ret), K(upd_rtdef.is_row_changed_),
              "old_row", ROWEXPR2STR(eval_ctx, upd_ctdef.old_row_),
              "new_row", ROWEXPR2STR(eval_ctx, upd_ctdef.new_row_));
  NG_TRACE_TIMES(2, update_end_check_row);
  return ret;
}

int ObDMLService::filter_row_for_check_cst(const ExprFixedArray &cst_exprs,
                                           ObEvalCtx &eval_ctx,
                                           bool &filtered)
{
  int ret = OB_SUCCESS;
  filtered = false;
  for (int64_t i = 0; OB_SUCC(ret) && !filtered && i < cst_exprs.count(); ++i) {
    ObExpr *expr = cst_exprs.at(i);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
      LOG_INFO("cover original errno while calulating check expr in mysql mode", K(ret));
      filtered = true;
      ret = OB_SUCCESS;
    } else {
      OB_ASSERT(ob_is_int_tc(expr->datum_meta_.type_));
      if (!datum->is_null() && 0 == datum->get_int()) {
        filtered = true;
      }
    }
  }
  return ret;
}

int ObDMLService::filter_row_for_view_check(const ExprFixedArray &cst_exprs,
                                           ObEvalCtx &eval_ctx,
                                           bool &filtered)
{
  int ret = OB_SUCCESS;
  filtered = false;
  for (int64_t i = 0; OB_SUCC(ret) && !filtered && i < cst_exprs.count(); ++i) {
    ObExpr *expr = cst_exprs.at(i);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
    } else {
      OB_ASSERT(ob_is_int_tc(expr->datum_meta_.type_));
      if (datum->is_null() || 0 == datum->get_int()) {
        filtered = true;
      }
    }
  }
  return ret;
}

int ObDMLService::init_heap_table_pk_for_ins(const ObInsCtDef &ins_ctdef, ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  if (ins_ctdef.is_primary_index_ && ins_ctdef.is_table_without_pk_) {
    ObExpr *auto_inc_expr = ins_ctdef.new_row_.at(0);
    if (OB_ISNULL(auto_inc_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
      datum.set_null();
      auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
    }
  }
  return ret;
}

int ObDMLService::process_insert_row(const ObInsCtDef &ins_ctdef,
                                     ObInsRtDef &ins_rtdef,
                                     ObTableModifyOp &dml_op,
                                     bool &is_skipped)
{
  int ret = OB_SUCCESS;
  if (ins_ctdef.is_primary_index_) {
    bool is_filtered = false;
    is_skipped = false;
    ObEvalCtx &eval_ctx = dml_op.get_eval_ctx();
    uint64_t ref_table_id = ins_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    //first, check insert value whether matched column type
    if (OB_FAIL(check_column_type(ins_ctdef.new_row_,
                                  ins_rtdef.cur_row_num_,
                                  ins_ctdef.column_infos_,
                                  dml_op))) {
    } else if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), ins_ctdef.das_ctdef_.index_tid_))) {
    } else if (OB_FAIL(TriggerHandle::init_param_new_row(
        eval_ctx, ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
    } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
        dml_op, ins_ctdef.das_base_ctdef_, ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(check_row_null(ins_ctdef.new_row_,
                                      dml_op.get_eval_ctx(),
                                      ins_rtdef.cur_row_num_,
                                      ins_ctdef.column_infos_,
                                      ins_ctdef.das_ctdef_.is_ignore_,
                                      ins_ctdef.is_single_value_,
                                      dml_op))) {
    } else if (OB_FAIL(check_filter_row(ins_ctdef, dml_op.get_eval_ctx(), is_skipped))) {
    }

  }
  return ret;
}

int ObDMLService::check_filter_row(
    const ObInsCtDef &ins_ctdef,
    ObEvalCtx &eval_ctx,
    bool &is_skipped)
{
  int ret = OB_SUCCESS;
  bool is_filtered = false;
  if (OB_FAIL(filter_row_for_view_check(ins_ctdef.view_check_exprs_,
                                        eval_ctx, is_filtered))) {
  } else if (OB_UNLIKELY(is_filtered)) {
    ret = OB_ERR_CHECK_OPTION_VIOLATED;
  } else if (OB_FAIL(filter_row_for_check_cst(ins_ctdef.check_cst_exprs_,
                                              eval_ctx, is_filtered))) {
  } else if (OB_UNLIKELY(is_filtered)) {
    if (ins_ctdef.das_ctdef_.is_ignore_) {
      is_skipped = true;
      LOG_USER_WARN(OB_ERR_CHECK_CONSTRAINT_VIOLATED);
    } else {
      ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED;
    }
  }
  return ret;
}

int ObDMLService::process_lock_row(const ObLockCtDef &lock_ctdef,
                                   ObLockRtDef &lock_rtdef,
                                   bool &is_skipped,
                                   ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  bool is_null = false;
  UNUSED(dml_op);
  if (lock_ctdef.need_check_filter_null_ &&
      OB_FAIL(check_rowkey_is_null(lock_ctdef.old_row_,
                                   lock_ctdef.das_ctdef_.rowkey_cnt_,
                                   dml_op.get_eval_ctx(),
                                   is_null))) {
  } else if (is_null) {
    // no need to lock
    is_skipped = true;
  }
  return ret;
}

int ObDMLService::process_delete_row(const ObDelCtDef &del_ctdef,
                                     ObDelRtDef &del_rtdef,
                                     bool &is_skipped,
                                     ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  if (del_ctdef.is_primary_index_) {
    uint64_t ref_table_id = del_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), del_ctdef.das_ctdef_.index_tid_))) {
    }
    if (OB_SUCC(ret) && del_ctdef.need_check_filter_null_) {
      bool is_null = false;
      if (OB_FAIL(check_rowkey_is_null(del_ctdef.old_row_,
                                       del_ctdef.das_ctdef_.rowkey_cnt_,
                                       dml_op.get_eval_ctx(),
                                       is_null))) {
      } else if (is_null) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped && OB_NOT_NULL(del_rtdef.se_rowkey_dist_ctx_)) {
      bool is_distinct = false;
      ObExecContext *root_ctx = nullptr;
      if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
      } else if (OB_ISNULL(root_ctx)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(check_rowkey_whether_distinct(del_ctdef.distinct_key_,
                                                      T_HASH_DISTINCT,
                                                      dml_op.get_eval_ctx(),
                                                      *root_ctx,
                                                      del_rtdef.table_rowkey_,
                                                      del_rtdef.se_rowkey_dist_ctx_,
                                                      is_distinct))) {
      } else if (!is_distinct) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      if (OB_FAIL(TriggerHandle::init_param_old_row(
        dml_op.get_eval_ctx(), del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_))) {
      } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
          dml_op, del_ctdef.das_base_ctdef_, del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_))) {
      }
    }
    LOG_DEBUG("process delete row", K(ret), K(is_skipped),
               "old_row", ROWEXPR2STR(dml_op.get_eval_ctx(), del_ctdef.old_row_));
  }

  return ret;
}

int ObDMLService::process_update_row(const ObUpdCtDef &upd_ctdef,
                                     ObUpdRtDef &upd_rtdef,
                                     bool &is_skipped,
                                     ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  if (upd_ctdef.is_primary_index_) {
    uint64_t ref_table_id = upd_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    if (OB_SUCC(ret) && upd_ctdef.need_check_filter_null_) {
      bool is_null = false;
      if (OB_FAIL(check_rowkey_is_null(upd_ctdef.old_row_,
                                       upd_ctdef.dupd_ctdef_.rowkey_cnt_,
                                       dml_op.get_eval_ctx(),
                                       is_null))) {
      } else if (is_null) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      if (upd_ctdef.is_table_without_pk_ &&
          OB_FAIL(copy_heap_table_hidden_pk(dml_op.get_eval_ctx(), upd_ctdef))) {
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      bool is_distinct = false;
      if (OB_FAIL(check_rowkey_whether_distinct(upd_ctdef.distinct_key_,
                                                upd_ctdef.distinct_algo_,
                                                dml_op.get_eval_ctx(),
                                                dml_op.get_exec_ctx(),
                                                upd_rtdef.table_rowkey_,
                                                upd_rtdef.se_rowkey_dist_ctx_,
                                                is_distinct))) {
      } else if (!is_distinct) {
        is_skipped = true;
      }
    }
    if (OB_SUCC(ret) && !is_skipped) {
      bool is_filtered = false;
      //first, check assignment column whether matched column type
      if (OB_FAIL(check_column_type(upd_ctdef.new_row_,
                                    upd_rtdef.cur_row_num_,
                                    upd_ctdef.assign_columns_,
                                    dml_op))) {
      } else if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), upd_ctdef.dupd_ctdef_.index_tid_))) {
      } else if (OB_FAIL(TriggerHandle::init_param_rows(
          dml_op.get_eval_ctx(), upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
      } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
          dml_op, upd_ctdef.das_base_ctdef_, upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(check_row_null(upd_ctdef.new_row_,
                                        dml_op.get_eval_ctx(),
                                        upd_rtdef.cur_row_num_,
                                        upd_ctdef.assign_columns_,
                                        upd_ctdef.dupd_ctdef_.is_ignore_,
                                        false,
                                        dml_op))) {
      } else if (OB_FAIL(check_row_whether_changed(upd_ctdef, upd_rtdef, dml_op.get_eval_ctx()))) {
      } else if (OB_UNLIKELY(!upd_rtdef.is_row_changed_)) {
        //do nothing
      } else if (OB_FAIL(filter_row_for_view_check(upd_ctdef.view_check_exprs_, dml_op.get_eval_ctx(), is_filtered))) {
      } else if (OB_UNLIKELY(is_filtered)) {
        ret = OB_ERR_CHECK_OPTION_VIOLATED;
      } else if (OB_FAIL(filter_row_for_check_cst(upd_ctdef.check_cst_exprs_, dml_op.get_eval_ctx(), is_filtered))) {
      } else if (OB_UNLIKELY(is_filtered)) {
        if (upd_ctdef.dupd_ctdef_.is_ignore_) {
          is_skipped = true;
          LOG_USER_WARN(OB_ERR_CHECK_CONSTRAINT_VIOLATED);
        } else {
          ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED;
        }
      }
    }
  } else {
    //for global index, only check whether the updated row is changed
    if (OB_FAIL(check_row_whether_changed(upd_ctdef, upd_rtdef, dml_op.get_eval_ctx()))) {
    }
  }

  if (OB_FAIL(ret)) {
  } else if (!is_skipped && upd_rtdef.has_table_cycle_) {
    ret = OB_ERR_ROW_IS_REFERENCED;
  }

  if (OB_SUCC(ret) && !is_skipped) {
    LOG_DEBUG("process update row", K(ret), K(is_skipped), K(upd_ctdef), K(upd_rtdef),
                "old_row", ROWEXPR2STR(dml_op.get_eval_ctx(), upd_ctdef.old_row_),
                "new_row", ROWEXPR2STR(dml_op.get_eval_ctx(), upd_ctdef.new_row_));
  }
  return ret;
}

namespace {

// ---------------------------------------------------------------------------
// AI column DML integration.
// ---------------------------------------------------------------------------
// A row change of a table with AI columns materializes the asynchronous
// computation as one task row per AI column in __all_ai_column_task.  The task
// rows are written with nested inner SQL bound to the session that performs the
// change, so they belong to the very same transaction: a rollback drops them
// again and the background worker never observes them.
// ---------------------------------------------------------------------------

const int64_t AI_TASK_STATE_PENDING = 0;
// A task of a transaction that has not committed yet.  The worker only claims
// PENDING/RETRYING/RUNNING tasks, so this state keeps the model call strictly
// after the commit of the user transaction.
const int64_t AI_TASK_STATE_UNCOMMITTED = 4;
const int64_t AI_TASK_STATE_RETRY = 3;
const int64_t AI_TASK_PREDICATE_BUF_LEN = 4096;
const char *const AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";
const char *const AI_META_TABLE_NAME = "oceanbase.__all_ai_column";

struct AiColumnTarget
{
  uint64_t column_id_;
  int64_t max_retries_;
  int64_t row_index_;
  AiColumnTarget()
    : column_id_(OB_INVALID_ID),
      max_retries_(0),
      row_index_(-1)
  {}
  TO_STRING_KV(K_(column_id), K_(max_retries), K_(row_index));
};

int ai_append_char(ObSqlString &sql, const char c)
{
  return sql.append(&c, 1);
}

int ai_append_sql_string(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ai_append_char(sql, '\''))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if ('\'' == c || '\\' == c) {
        ret = ai_append_char(sql, '\\');
      }
      if (OB_SUCC(ret)) {
        ret = ai_append_char(sql, c);
      }
    }
    if (OB_SUCC(ret)) {
      ret = ai_append_char(sql, '\'');
    }
  }
  return ret;
}

bool ai_dep_list_contains(const ObString &deps, const ObString &token)
{
  bool found = false;
  if (!token.empty()) {
    int64_t start = 0;
    for (int64_t i = 0; !found && i <= deps.length(); ++i) {
      if (i == deps.length() || ',' == deps.ptr()[i]) {
        const int64_t len = i - start;
        if (len == token.length()
            && 0 == strncasecmp(deps.ptr() + start, token.ptr(), static_cast<size_t>(len))) {
          found = true;
        }
        start = i + 1;
      }
    }
  }
  return found;
}

int ai_find_column_index(const ObDMLBaseCtDef &base_ctdef,
                         const uint64_t column_id,
                         int64_t &index)
{
  int ret = OB_SUCCESS;
  index = -1;
  for (int64_t i = 0; OB_SUCC(ret) && -1 == index && i < base_ctdef.column_ids_.count(); ++i) {
    if (base_ctdef.column_ids_.at(i) == column_id) {
      index = i;
    }
  }
  return ret;
}

// Force one row expression to NULL without evaluating its expression tree, the
// storage layer copies the datum that is already materialized on the expression.
int ai_force_null_datum(ObExpr *expr, ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column dml, null row expr", K(ret));
  } else {
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx);
    datum.set_null();
    expr->get_eval_info(eval_ctx).evaluated_ = true;
    if (expr->is_batch_result()) {
      // A batch expression is evaluated row by row, the evaluated flag of the
      // current row has to be raised as well.  Otherwise the storage layer
      // recomputes the old value of the column and overwrites the reset result.
      expr->get_evaluated_flags(eval_ctx).set(eval_ctx.get_batch_idx());
    }
  }
  return ret;
}

// Build the SQL predicate that addresses exactly this row.  The row expressions
// are aligned with the column id array of the DML ctdef, the primary key
// columns are resolved through the rowkey info of the table so that the layout
// of the row array does not matter.
int ai_build_rowkey_predicate(const ObDMLBaseCtDef &base_ctdef,
                              const ObTableSchema &table_schema,
                              const ExprFixedArray &row,
                              ObEvalCtx &eval_ctx,
                              ObIAllocator &allocator,
                              bool &usable,
                              ObString &predicate)
{
  int ret = OB_SUCCESS;
  usable = false;
  predicate.reset();
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  const int64_t key_cnt = rowkey_info.get_size();
  char *buf = NULL;
  int64_t pos = 0;
  if (key_cnt <= 0 || key_cnt > base_ctdef.column_ids_.count()) {
    LOG_DEBUG("ai column dml, row cannot be addressed by a predicate",
              K(key_cnt), K(base_ctdef.column_ids_.count()));
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(AI_TASK_PREDICATE_BUF_LEN)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("ai column dml, allocate predicate buffer failed", K(ret));
  } else {
    bool null_key = false;
    bool missing_key = false;
    for (int64_t i = 0; OB_SUCC(ret) && !null_key && !missing_key && i < key_cnt; ++i) {
      uint64_t column_id = OB_INVALID_ID;
      int64_t row_index = -1;
      const ObColumnSchemaV2 *column = NULL;
      ObDatum *datum = NULL;
      if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
        LOG_WARN("ai column dml, get rowkey column id failed", K(ret), K(i));
      } else if (OB_ISNULL(column = table_schema.get_column_schema(column_id))) {
        // the rowkey may contain a generated column that is not part of the row
        missing_key = true;
      } else if (OB_FAIL(ai_find_column_index(base_ctdef, column_id, row_index))) {
      } else if (-1 == row_index) {
        missing_key = true;
      } else if (OB_ISNULL(row.at(row_index))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column dml, null rowkey expr", K(ret), K(row_index));
      } else if (OB_FAIL(row.at(row_index)->eval(eval_ctx, datum))) {
        LOG_WARN("ai column dml, evaluate rowkey failed", K(ret), K(row_index));
      } else if (OB_ISNULL(datum) || datum->is_null()) {
        null_key = true;
      } else {
        ObObj obj;
        const ObString &column_name = column->get_column_name_str();
        int n = 0;
        datum->to_obj(obj, column->get_meta_type());
        n = snprintf(buf + pos, AI_TASK_PREDICATE_BUF_LEN - pos, "`%.*s` = ",
                     static_cast<int32_t>(column_name.length()), column_name.ptr());
        if (n < 0 || pos + n >= AI_TASK_PREDICATE_BUF_LEN) {
          ret = OB_SIZE_OVERFLOW;
          LOG_WARN("ai column dml, print column name failed", K(ret), K(column_name));
        } else {
          pos += n;
          if (OB_FAIL(obj.print_sql_literal(buf, AI_TASK_PREDICATE_BUF_LEN, pos,
                                            ObObjPrintParams()))) {
            LOG_WARN("ai column dml, print rowkey literal failed", K(ret), K(obj));
          } else if (i + 1 < key_cnt) {
            n = snprintf(buf + pos, AI_TASK_PREDICATE_BUF_LEN - pos, " and ");
            if (n < 0 || pos + n >= AI_TASK_PREDICATE_BUF_LEN) {
              ret = OB_SIZE_OVERFLOW;
              LOG_WARN("ai column dml, print predicate separator failed", K(ret));
            } else {
              pos += n;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && !null_key && !missing_key) {
      if (OB_UNLIKELY(pos >= AI_TASK_PREDICATE_BUF_LEN)) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("ai column dml, rowkey predicate too long", K(ret), K(pos));
      } else {
        buf[pos] = '\0';
        predicate.assign_ptr(buf, static_cast<int32_t>(pos));
        usable = true;
      }
    }
  }
  return ret;
}

int ai_execute_inner_write(ObExecContext &exec_ctx,
                           ObSQLSessionInfo *session,
                           const ObString &stmt,
                           int64_t &affected_rows)
{
  UNUSED(exec_ctx);
  UNUSED(session);
  return ai_execute_inner_write_raw(stmt, affected_rows);
}

int ai_delete_tasks(ObExecContext &exec_ctx,
                    ObSQLSessionInfo *session,
                    const uint64_t table_id,
                    const ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (predicate.empty()) {
  } else if (OB_FAIL(sql.append_fmt("delete from %s where table_id = %ld and rowkey_predicate = ",
                                    AI_TASK_TABLE_NAME,
                                    static_cast<int64_t>(table_id)))) {
    LOG_WARN("ai column dml, build task delete sql failed", K(ret));
  } else if (OB_FAIL(ai_append_sql_string(sql, predicate))) {
  } else if (OB_FAIL(ai_execute_inner_write(exec_ctx, session, sql.string(), affected_rows))) {
    LOG_WARN("ai column dml, delete ai column task failed", K(ret), "sql", sql.ptr());
  }
  return ret;
}

// The rows of the AI column task table are written by an autonomous
// transaction, they are therefore not covered by the rollback of the
// transaction of the user statement.  The registry remembers the task rows
// that were created by the current session so that a rollback of the
// transaction of the user statement removes them again.
struct AiColumnTaskKey
{
  uint64_t session_id_;
  uint64_t table_id_;
  int64_t request_id_;
  int64_t tx_id_;
};

class AiColumnTaskRegistry
{
public:
  static AiColumnTaskRegistry &instance()
  {
    static AiColumnTaskRegistry registry;
    return registry;
  }

  void register_task(const uint64_t session_id,
                     const uint64_t table_id,
                     const int64_t request_id,
                     const int64_t tx_id)
  {
    lib::ObMutexGuard guard(mutex_);
    // The task rows of a former transaction of the session are not needed
    // any more, that transaction has ended and its task rows have either
    // been published or removed by the rollback.
    std::vector<AiColumnTaskKey> rest;
    rest.reserve(tasks_.size());
    for (size_t i = 0; i < tasks_.size(); ++i) {
      if (tasks_[i].session_id_ != session_id || tasks_[i].tx_id_ == tx_id) {
        rest.push_back(tasks_[i]);
      }
    }
    tasks_.swap(rest);
    AiColumnTaskKey key;
    key.session_id_ = session_id;
    key.table_id_ = table_id;
    key.request_id_ = request_id;
    key.tx_id_ = tx_id;
    tasks_.push_back(key);
  }

  // Move the task rows of the transaction out of the registry.
  void take_tasks(const uint64_t session_id,
                  const int64_t tx_id,
                  std::vector<AiColumnTaskKey> &tasks)
  {
    lib::ObMutexGuard guard(mutex_);
    std::vector<AiColumnTaskKey> rest;
    rest.reserve(tasks_.size());
    for (size_t i = 0; i < tasks_.size(); ++i) {
      if (tasks_[i].session_id_ == session_id && tasks_[i].tx_id_ == tx_id) {
        tasks.push_back(tasks_[i]);
      } else {
        rest.push_back(tasks_[i]);
      }
    }
    tasks_.swap(rest);
  }

private:
  lib::ObMutex mutex_;
  std::vector<AiColumnTaskKey> tasks_;
};

int ai_insert_tasks(ObExecContext &exec_ctx,
                    ObSQLSessionInfo *session,
                    const uint64_t table_id,
                    const int64_t request_id,
                    const ObString &predicate,
                    const ObIArray<AiColumnTarget> &targets)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  const int64_t eligible_ts = ObTimeUtility::current_time() / 1000000;
  if (targets.empty()) {
  } else if (OB_FAIL(sql.append_fmt(
                 "insert into %s(table_id, request_id, column_id, rowkey_predicate, retry_count, "
                 "state, eligible_ts, definition_version, error_msg) values ",
                 AI_TASK_TABLE_NAME))) {
    LOG_WARN("ai column dml, build task insert sql failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < targets.count(); ++i) {
      const AiColumnTarget &target = targets.at(i);
      if (i > 0 && OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(sql.append_fmt("(%ld, %ld, %ld, ",
                                        static_cast<int64_t>(table_id),
                                        request_id,
                                        static_cast<int64_t>(target.column_id_)))) {
        LOG_WARN("ai column dml, append task row failed", K(ret));
      } else if (OB_FAIL(ai_append_sql_string(sql, predicate))) {
      } else if (OB_FAIL(sql.append_fmt(", 0, %ld, %ld, 0, '')",
                                        AI_TASK_STATE_UNCOMMITTED,
                                        eligible_ts))) {
        LOG_WARN("ai column dml, append task row tail failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(ai_execute_inner_write(exec_ctx, session, sql.string(), affected_rows))) {
      LOG_WARN("ai column dml, insert ai column task failed", K(ret), "sql", sql.ptr());
    } else if (OB_SUCC(ret) && OB_NOT_NULL(session)) {
      // Remember the task rows, the rollback of the transaction of the user
      // statement has to remove them again.
      int64_t tx_id = 0;
      if (OB_NOT_NULL(session->get_tx_desc())) {
        tx_id = data_plane::tx_desc_id(session->get_tx_desc()).get_id();
      }
      AiColumnTaskRegistry::instance().register_task(session->get_server_sid(),
                                                      table_id,
                                                      request_id,
                                                      tx_id);
    }
  }
  return ret;
}

int64_t ai_next_request_id()
{
  static std::atomic<int64_t> seq(0);
  const int64_t now_us = ObTimeUtility::current_time();
  const int64_t id = ((now_us % 1000000000) * 1000 + (seq.fetch_add(1) % 1000)) & 0x7FFFFFFF;
  return id;
}

// Return whether the result of an AI column has to be recomputed for this
// update, i.e. whether one of the columns it depends on is assigned.
struct AiColumnMeta
{
  int64_t column_id_;
  int64_t max_retries_;
  ObString dep_names_;
};

int ai_execute_inner_read(ObExecContext &exec_ctx,
                          ObSQLSessionInfo *session,
                          const ObString &stmt,
                          ObMySQLProxy::MySQLResult &res)
{
  int ret = OB_SUCCESS;
  UNUSED(exec_ctx);
  UNUSED(session);
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column dml, null sql proxy", K(ret));
  } else if (OB_FAIL(GCTX.sql_proxy_->read(res, stmt.ptr()))) {
    LOG_WARN("ai column dml, read failed", K(ret), "sql", stmt);
  }
  return ret;
}

// Resolve the AI column definitions of a table from __all_ai_column. The
// definition is persisted there (and not in __all_column), so the in memory
// column schema does not necessarily carry it.
bool ai_table_has_ai_columns(const ObTableSchema &table_schema)
{
  bool has = false;
  const ObString prefix = ObString::make_string("__ai_state_");
  for (ObTableSchema::const_column_iterator iter = table_schema.column_begin();
       !has && iter != table_schema.column_end(); ++iter) {
    const ObColumnSchemaV2 *column = *iter;
    if (NULL != column && column->is_hidden()) {
      const ObString name = column->get_column_name_str();
      if (name.length() > prefix.length()
          && 0 == strncasecmp(name.ptr(), prefix.ptr(), prefix.length())) {
        has = true;
      }
    }
  }
  return has;
}

int ai_load_ai_column_metas(ObExecContext &exec_ctx,
                            ObSQLSessionInfo *session,
                            const ObTableSchema &table_schema,
                            ObIAllocator &allocator,
                            ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  metas.reuse();
  if (OB_FAIL(sql.append_fmt("select column_id, dep_column_ids, max_retries from %s where table_id = %ld",
                             AI_META_TABLE_NAME,
                             static_cast<int64_t>(table_schema.get_table_id())))) {
    LOG_WARN("ai column dml, build ai column meta sql failed", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(ai_execute_inner_read(exec_ctx, session, sql.string(), res))) {
        LOG_WARN("ai column dml, read ai column meta failed", K(ret), "sql", sql.ptr());
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column dml, null ai column meta result", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          int64_t column_id = 0;
          int64_t max_retries = 0;
          ObString dep_ids;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("ai column dml, iterate ai column meta failed", K(ret));
            }
          } else if (OB_FAIL(result->get_int("column_id", column_id))) {
          } else if (OB_FAIL(result->get_int("max_retries", max_retries))) {
          } else if (OB_FAIL(result->get_varchar("dep_column_ids", dep_ids))) {
          } else {
            AiColumnMeta meta;
            ObSqlString dep_names;
            meta.column_id_ = column_id;
            meta.max_retries_ = max_retries;
            const char *ptr = dep_ids.ptr();
            const int64_t len = dep_ids.length();
            int64_t pos = 0;
            while (OB_SUCC(ret) && pos < len) {
              int64_t end = pos;
              while (end < len && ',' != ptr[end]) {
                ++end;
              }
              if (end > pos) {
                char buf[32];
                const int64_t buf_len = std::min(end - pos, static_cast<int64_t>(sizeof(buf) - 1));
                MEMCPY(buf, ptr + pos, buf_len);
                buf[buf_len] = '\0';
                const uint64_t dep_id = strtoull(buf, NULL, 10);
                const ObColumnSchemaV2 *dep_column = table_schema.get_column_schema(dep_id);
                if (NULL != dep_column) {
                  if (0 != dep_names.length() && OB_FAIL(dep_names.append(","))) {
                  } else if (OB_FAIL(dep_names.append(dep_column->get_column_name_str()))) {
                  }
                }
              }
              pos = end + 1;
            }
            if (OB_SUCC(ret)
                && OB_FAIL(ob_write_string(allocator, dep_names.string(), meta.dep_names_))) {
              LOG_WARN("ai column dml, copy dep names failed", K(ret));
            } else if (OB_SUCC(ret) && OB_FAIL(metas.push_back(meta))) {
              LOG_WARN("ai column dml, push back ai column meta failed", K(ret));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

// The value that is written to the AI column itself only invalidates the
// result when it is NULL.  The worker publishes the computed result with a non
// NULL value and that value has to survive the DML, while RETRY AI COLUMN
// writes NULL and has to invalidate the result and to create a new task.
static bool ai_assigned_value_is_null(ObEvalCtx &eval_ctx,
                                      const ObUpdCtDef &upd_ctdef,
                                      const ColumnContent &content)
{
  bool bret = false;
  int ret = OB_SUCCESS;
  if (content.projector_index_ >= 0
      && content.projector_index_ < upd_ctdef.new_row_.count()) {
    ObExpr *expr = upd_ctdef.new_row_.at(content.projector_index_);
    if (OB_NOT_NULL(expr)) {
      ObDatum *datum = NULL;
      if (OB_SUCC(expr->eval(eval_ctx, datum)) && OB_NOT_NULL(datum)) {
        bret = datum->is_null();
      }
    }
  }
  return bret;
}

// The result column of an AI column is written with NULL: the result is
// invalidated and has to be computed again.  This is the semantics of
// RETRY AI COLUMN.
static bool ai_column_assigned_null(const ObUpdCtDef &upd_ctdef,
                                    const ObString &ai_column_name,
                                    ObEvalCtx &eval_ctx)
{
  bool bret = false;
  int ret = OB_SUCCESS;
  if (!ai_column_name.empty()) {
    for (int64_t i = 0; !bret && i < upd_ctdef.assign_columns_.count(); ++i) {
      const ColumnContent &content = upd_ctdef.assign_columns_.at(i);
      if (content.column_name_.length() == ai_column_name.length()
          && 0 == strncasecmp(content.column_name_.ptr(),
                              ai_column_name.ptr(),
                              static_cast<size_t>(ai_column_name.length()))) {
        bret = ai_assigned_value_is_null(eval_ctx, upd_ctdef, content);
      }
    }
  }
  return bret;
}

bool ai_column_needs_refresh(const ObString &dep_names,
                             const ObString &ai_column_name,
                             const ObUpdCtDef &upd_ctdef,
                             const bool refresh_all,
                             ObEvalCtx &eval_ctx)
{
  bool refresh = refresh_all;
  for (int64_t i = 0; !refresh && i < upd_ctdef.assign_columns_.count(); ++i) {
    const ColumnContent &content = upd_ctdef.assign_columns_.at(i);
    if (ai_dep_list_contains(dep_names, content.column_name_)) {
      refresh = true;
    } else if (!ai_column_name.empty()
               && content.column_name_.length() == ai_column_name.length()
               && 0 == strncasecmp(content.column_name_.ptr(),
                                  ai_column_name.ptr(),
                                  static_cast<size_t>(ai_column_name.length()))
               && ai_assigned_value_is_null(eval_ctx, upd_ctdef, content)) {
      // the result column itself is reset to NULL, the result has to be
      // computed again, this is the semantics of RETRY AI COLUMN
      refresh = true;
    }
  }
  return refresh;
}

int ai_column_handle_insert(const ObInsCtDef &ins_ctdef, ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  bool usable = false;
  const uint64_t ai_tid = ins_ctdef.das_ctdef_.index_tid_;
  if (!ins_ctdef.is_primary_index_ || OB_INVALID_ID == ai_tid || is_inner_table(ai_tid)) {
  } else {
    ObExecContext &exec_ctx = dml_rtctx.get_exec_ctx();
    ObSQLSessionInfo *session = exec_ctx.get_my_session();
    ObSqlCtx *sql_ctx = exec_ctx.get_sql_ctx();
    ObSchemaGetterGuard *schema_guard = NULL == sql_ctx ? NULL : sql_ctx->schema_guard_;
    const ObTableSchema *table_schema = NULL;
    ObSEArray<AiColumnMeta, 4> ai_metas;
    ObSEArray<AiColumnTarget, 4> targets;
    ObString predicate;
    if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
      // no session level schema guard (e.g. during bootstrap), nothing to do
      LOG_INFO("ai column dml, skip insert without exec context", KP(schema_guard));
    } else if (OB_FAIL(schema_guard->get_table_schema(ins_ctdef.das_ctdef_.index_tid_,
                                                      table_schema))) {
      ret = OB_SUCCESS;
      LOG_INFO("ai column dml, skip insert without table schema");
    } else if (OB_ISNULL(table_schema)) {
      LOG_INFO("ai column dml, skip insert with null table schema");
    } else if (!ai_table_has_ai_columns(*table_schema)) {
      // not an ai column table, keep the dml path untouched
    } else if (OB_FAIL(ai_load_ai_column_metas(dml_rtctx.get_exec_ctx(), session, *table_schema, exec_ctx.get_allocator(), ai_metas))) {
      ret = OB_SUCCESS;
      LOG_DEBUG("ai column dml, load ai column metadata failed, skip");
    } else if (ai_metas.empty()) {
      // nothing to do for tables without AI columns
      LOG_DEBUG("ai column dml, insert skipped no ai columns",
               K(table_schema->get_table_name_str()), K(table_schema->get_column_count()));
    } else if (OB_FAIL(ai_build_rowkey_predicate(ins_ctdef,
                                                 *table_schema,
                                                 ins_ctdef.new_row_,
                                                 dml_rtctx.get_eval_ctx(),
                                                 exec_ctx.get_allocator(),
                                                 usable,
                                                 predicate))) {
    } else if (!usable) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < ai_metas.count(); ++i) {
        const AiColumnMeta &meta = ai_metas.at(i);
        int64_t row_index = -1;
        bool need_task = true;
        if (OB_FAIL(ai_find_column_index(ins_ctdef, meta.column_id_, row_index))) {
        } else if (-1 == row_index) {
          // the inserted statement does not carry this AI column, the stored value is
          // NULL anyway and the task still has to be created for the new row
        } else {
          ObDatum *datum = NULL;
          if (OB_ISNULL(ins_ctdef.new_row_.at(row_index))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai column dml, null ai column expr", K(ret), K(row_index));
          } else if (OB_FAIL(ins_ctdef.new_row_.at(row_index)->eval(dml_rtctx.get_eval_ctx(),
                                                                    datum))) {
            LOG_WARN("ai column dml, evaluate ai column failed", K(ret), K(row_index));
          } else if (OB_ISNULL(datum)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai column dml, null ai column datum", K(ret));
          } else if (!datum->is_null()) {
            // an explicitly provided result wins, no task is needed
            need_task = false;
          }
        }
        if (OB_SUCC(ret) && need_task) {
          AiColumnTarget target;
          target.column_id_ = meta.column_id_;
          target.max_retries_ = meta.max_retries_;
          target.row_index_ = row_index;
          if (OB_FAIL(targets.push_back(target))) {
            LOG_WARN("ai column dml, push back task target failed", K(ret));
          }
        }
      }
      LOG_INFO("ai column dml insert target", K(table_schema->get_table_name_str()),
               K(ins_ctdef.column_ids_.count()), K(ai_metas.count()),
               K(targets.count()), K(usable), K(predicate));
      if (OB_SUCC(ret) && !targets.empty()) {
        const int64_t request_id = ai_next_request_id();
        if (OB_FAIL(ai_delete_tasks(dml_rtctx.get_exec_ctx(), session, ins_ctdef.das_ctdef_.index_tid_, predicate))) {
        } else if (OB_FAIL(ai_insert_tasks(dml_rtctx.get_exec_ctx(), session,
                                           ins_ctdef.das_ctdef_.index_tid_,
                                           request_id,
                                           predicate,
                                           targets))) {
        }
      }
    }
  }
  LOG_INFO("ai column dml, insert handler done", K(ret));
  return ret;
}



// ---------------------------------------------------------------------------
// Strict read for AI columns.
//
// A result column is READY only when the worker has published a value.  While a
// task is PENDING/RETRYING, and after it has reached its terminal FAILED state,
// the result stays NULL.  Strict mode therefore checks the whole result column
// of every AI column referenced by the statement, without applying the query
// predicate: the spec requires column-level completeness, not row-level.
// Inner SQL runs on an inner session (is_inner()), so the check is not applied
// recursively to its own probes.
// ---------------------------------------------------------------------------
static int ai_collect_column_refs(ObDMLStmt *stmt,
                                  ObIArray<ObColumnRefRawExpr *> &cols)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(stmt)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(stmt->get_column_exprs(cols))) {
    LOG_WARN("ai column strict read, collect column refs failed", K(ret));
  } else {
    ObSEArray<ObSelectStmt *, 8> children;
    if (OB_FAIL(stmt->get_subquery_stmts(children))) {
      LOG_WARN("ai column strict read, collect subqueries failed", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < children.count(); ++i) {
      if (OB_FAIL(ai_collect_column_refs(children.at(i), cols))) {
        LOG_WARN("ai column strict read, collect subquery columns failed", K(ret));
      }
    }
  }
  return ret;
}

static int ai_read_single_count(ObExecContext &exec_ctx,
                                ObSQLSessionInfo *session,
                                const ObString &sql,
                                int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(ai_execute_inner_read(exec_ctx, session, sql, res))) {
      LOG_WARN("ai column strict read, inner read failed", K(ret), "sql", sql);
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column strict read, null inner result", K(ret));
    } else if (OB_FAIL(result->next())) {
      LOG_WARN("ai column strict read, fetch inner result failed", K(ret));
    } else if (OB_FAIL(result->get_int("c", count))) {
      LOG_WARN("ai column strict read, read count failed", K(ret));
    }
  }
  return ret;
}

// The table id and the column id of an AI column are read from the metadata of
// the AI column: the table id of a column reference is not always filled by
// the resolver.
static int ai_read_ai_column_meta(ObExecContext &exec_ctx,
                                   ObSQLSessionInfo *session,
                                   const ObString &db_name,
                                   const ObString &table_name,
                                   const ObString &column_name,
                                   int64_t &table_id,
                                   int64_t &column_id,
                                   bool &is_ai)
{
  int ret = OB_SUCCESS;
  table_id = OB_INVALID_ID;
  column_id = OB_INVALID_ID;
  is_ai = false;
  ObSqlString meta_sql;
  if (OB_FAIL(meta_sql.append_fmt(
          "select t.table_id as ai_tid, a.column_id as ai_cid "
          "from oceanbase.__all_ai_column a "
          "join oceanbase.__all_table t on t.table_id = a.table_id "
          "join oceanbase.__all_database d on d.database_id = t.database_id "
          "where d.database_name = '%.*s' and t.table_name = '%.*s' "
          "and a.column_name = '%.*s'",
          db_name.length(), db_name.ptr(),
          table_name.length(), table_name.ptr(),
          column_name.length(), column_name.ptr()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column strict read, build meta sql failed", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(ai_execute_inner_read(exec_ctx, session, meta_sql.string(), res))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column strict read, inner read failed", K(ret), "sql", meta_sql.string());
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column strict read, null inner result", K(ret));
      } else {
        const int64_t next_ret = result->next();
        if (OB_ITER_END == next_ret) {
          // the column is not an AI column
        } else if (OB_FAIL(next_ret)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column strict read, fetch inner result failed", K(ret));
        } else if (OB_FAIL(result->get_int("ai_tid", table_id))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column strict read, read table id failed", K(ret));
        } else if (OB_FAIL(result->get_int("ai_cid", column_id))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column strict read, read column id failed", K(ret));
        } else {
          is_ai = true;
        }
      }
    }
  }
  return ret;
}


int ai_column_handle_update(const ObUpdCtDef &upd_ctdef,
                             ObDMLRtCtx &dml_rtctx,
                             const bool row_changed)
{
  int ret = OB_SUCCESS;
  bool old_usable = false;
  bool new_usable = false;
  const uint64_t ai_tid = upd_ctdef.das_base_ctdef_.index_tid_;
  if (!upd_ctdef.is_primary_index_ || OB_INVALID_ID == ai_tid || is_inner_table(ai_tid)) {
  } else {
    ObExecContext &exec_ctx = dml_rtctx.get_exec_ctx();
    ObSQLSessionInfo *session = exec_ctx.get_my_session();
    ObSqlCtx *sql_ctx = exec_ctx.get_sql_ctx();
    ObSchemaGetterGuard *schema_guard = NULL == sql_ctx ? NULL : sql_ctx->schema_guard_;
    const ObTableSchema *table_schema = NULL;
    ObSEArray<AiColumnMeta, 4> ai_metas;
    ObSEArray<AiColumnTarget, 4> targets;
    ObString old_predicate;
    ObString new_predicate;
    if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
      // no session level schema guard (e.g. during bootstrap), nothing to do
      LOG_DEBUG("ai column dml, skip update without exec context", KP(schema_guard));
    } else if (OB_FAIL(schema_guard->get_table_schema(upd_ctdef.das_base_ctdef_.index_tid_,
                                                      table_schema))) {
      ret = OB_SUCCESS;
      LOG_DEBUG("ai column dml, skip update without table schema");
    } else if (OB_ISNULL(table_schema)) {
      LOG_DEBUG("ai column dml, skip update with null table schema");
    } else if (!ai_table_has_ai_columns(*table_schema)) {
      // not an ai column table, keep the dml path untouched
    } else if (OB_FAIL(ai_load_ai_column_metas(dml_rtctx.get_exec_ctx(), session, *table_schema, exec_ctx.get_allocator(), ai_metas))) {
      ret = OB_SUCCESS;
      LOG_DEBUG("ai column dml, load ai column metadata failed, skip");
    } else if (ai_metas.empty()) {
    } else if (OB_FAIL(ai_build_rowkey_predicate(upd_ctdef,
                                                 *table_schema,
                                                 upd_ctdef.old_row_,
                                                 dml_rtctx.get_eval_ctx(),
                                                 exec_ctx.get_allocator(),
                                                 old_usable,
                                                 old_predicate))) {
    } else if (OB_FAIL(ai_build_rowkey_predicate(upd_ctdef,
                                                 *table_schema,
                                                 upd_ctdef.new_row_,
                                                 dml_rtctx.get_eval_ctx(),
                                                 exec_ctx.get_allocator(),
                                                 new_usable,
                                                 new_predicate))) {
    } else if (!old_usable || !new_usable) {
    } else {
      const bool refresh_all = upd_ctdef.das_base_ctdef_.is_update_pk_
                               || upd_ctdef.das_base_ctdef_.is_update_partition_key_;
      for (int64_t i = 0; OB_SUCC(ret) && i < ai_metas.count(); ++i) {
        const AiColumnMeta &meta = ai_metas.at(i);
        int64_t row_index = -1;
        ObString ai_column_name;
        auto *ai_column_schema = table_schema->get_column_schema(meta.column_id_);
        if (OB_NOT_NULL(ai_column_schema)) {
          ai_column_name = ai_column_schema->get_column_name_str();
        }
        const bool assigned_null = ai_column_assigned_null(upd_ctdef, ai_column_name,
                                                            dml_rtctx.get_eval_ctx());
        // A row that does not change is only refreshed when the result of the
        // AI column itself is reset to NULL: RETRY AI COLUMN resets the
        // result of a row whose result is already NULL.
        const bool refresh = row_changed
                             ? ai_column_needs_refresh(meta.dep_names_, ai_column_name,
                                                       upd_ctdef,
                                                       refresh_all,
                                                       dml_rtctx.get_eval_ctx())
                             : assigned_null;
        if (!refresh) {
          LOG_DEBUG("ai column dml, skip refresh", K(meta.column_id_), K(meta.dep_names_),
                    K(refresh_all), K(upd_ctdef.assign_columns_.count()));
        } else if (OB_FAIL(ai_find_column_index(upd_ctdef,
                                                meta.column_id_,
                                                row_index))) {
        } else if (-1 == row_index) {
          LOG_DEBUG("ai column dml, ai column missing from the write set",
                    K(meta.column_id_), K(upd_ctdef.column_ids_.count()));
        } else if (row_changed
                   && OB_FAIL(ai_force_null_datum(upd_ctdef.new_row_.at(row_index),
                                                  dml_rtctx.get_eval_ctx()))) {
          LOG_WARN("ai column dml, reset ai column result failed", K(ret), K(row_index));
        } else if (row_changed) {
          // full_row_ is the storage row: its order and length differ from
          // column_ids_.  The code generator reuses the same ObExpr object in
          // both rows, so locate the write-row expression by pointer identity.
          ObExpr *ai_expr = upd_ctdef.new_row_.at(row_index);
          int64_t full_row_index = -1;
          for (int64_t j = 0; j < upd_ctdef.full_row_.count(); ++j) {
            if (upd_ctdef.full_row_.at(j) == ai_expr) {
              full_row_index = j;
              break;
            }
          }
          LOG_DEBUG("ai column dml, reset the result of the ai column",
                    K(meta.column_id_), K(row_index),
                    K(full_row_index), K(upd_ctdef.full_row_.count()));
          if (full_row_index < 0) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai column dml, ai expression missing from storage row", K(ret),
                     K(meta.column_id_), K(row_index));
          } else if (OB_FAIL(ai_force_null_datum(upd_ctdef.full_row_.at(full_row_index),
                                                 dml_rtctx.get_eval_ctx()))) {
            LOG_WARN("ai column dml, reset ai column write row failed", K(ret),
                     K(full_row_index));
          }
          if (OB_SUCC(ret)) {
            AiColumnTarget target;
            target.column_id_ = meta.column_id_;
            target.max_retries_ = meta.max_retries_;
            target.row_index_ = row_index;
            if (OB_FAIL(targets.push_back(target))) {
              LOG_WARN("ai column dml, push back task target failed", K(ret));
            }
          }
        } else {
          // The row is not changed, so the result of the AI column was already
          // NULL.  RETRY AI COLUMN resets the result of such a row and the
          // result has to be computed again, so the task is created.  The
          // value of the column is already NULL, nothing is written.
          AiColumnTarget target;
          target.column_id_ = meta.column_id_;
          target.max_retries_ = meta.max_retries_;
          target.row_index_ = row_index;
          if (OB_FAIL(targets.push_back(target))) {
            LOG_WARN("ai column dml, push back task target failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret) && !targets.empty()) {
        const int64_t request_id = ai_next_request_id();
        if (OB_FAIL(ai_delete_tasks(dml_rtctx.get_exec_ctx(), session,
                                    upd_ctdef.das_base_ctdef_.index_tid_,
                                    old_predicate))) {
        } else if (0 != new_predicate.compare(old_predicate)
                   && OB_FAIL(ai_delete_tasks(dml_rtctx.get_exec_ctx(), session,
                                              upd_ctdef.das_base_ctdef_.index_tid_,
                                              new_predicate))) {
        } else if (OB_FAIL(ai_insert_tasks(dml_rtctx.get_exec_ctx(), session,
                                           upd_ctdef.das_base_ctdef_.index_tid_,
                                           request_id,
                                           new_predicate,
                                           targets))) {
        }
      }
    }
  }
  return ret;
}

int ai_column_handle_delete(const ObDelCtDef &del_ctdef, ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  bool usable = false;
  const uint64_t ai_tid = del_ctdef.das_ctdef_.index_tid_;
  if (!del_ctdef.is_primary_index_ || OB_INVALID_ID == ai_tid || is_inner_table(ai_tid)) {
  } else {
    ObExecContext &exec_ctx = dml_rtctx.get_exec_ctx();
    ObSQLSessionInfo *session = exec_ctx.get_my_session();
    ObSqlCtx *sql_ctx = exec_ctx.get_sql_ctx();
    ObSchemaGetterGuard *schema_guard = NULL == sql_ctx ? NULL : sql_ctx->schema_guard_;
    const ObTableSchema *table_schema = NULL;
    ObSEArray<AiColumnMeta, 4> ai_metas;
    ObString predicate;
    if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
      // no session level schema guard (e.g. during bootstrap), nothing to do
      LOG_DEBUG("ai column dml, skip delete without exec context", KP(schema_guard));
    } else if (OB_FAIL(schema_guard->get_table_schema(del_ctdef.das_ctdef_.index_tid_,
                                                      table_schema))) {
      ret = OB_SUCCESS;
      LOG_DEBUG("ai column dml, skip delete without table schema");
    } else if (OB_ISNULL(table_schema)) {
      LOG_DEBUG("ai column dml, skip delete with null table schema");
    } else if (!ai_table_has_ai_columns(*table_schema)) {
      // not an ai column table, keep the dml path untouched
    } else if (OB_FAIL(ai_load_ai_column_metas(dml_rtctx.get_exec_ctx(), session, *table_schema, exec_ctx.get_allocator(), ai_metas))) {
      ret = OB_SUCCESS;
      LOG_DEBUG("ai column dml, load ai column metadata failed, skip");
    } else if (ai_metas.empty()) {
    } else if (OB_FAIL(ai_build_rowkey_predicate(del_ctdef,
                                                 *table_schema,
                                                 del_ctdef.old_row_,
                                                 dml_rtctx.get_eval_ctx(),
                                                 exec_ctx.get_allocator(),
                                                 usable,
                                                 predicate))) {
    } else if (!usable) {
    } else if (OB_FAIL(ai_delete_tasks(dml_rtctx.get_exec_ctx(), session,
                                       del_ctdef.das_ctdef_.index_tid_,
                                       predicate))) {
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Text helpers of the RETRY AI COLUMN statement.
//
// The retry statement is not a schema change, it invalidates the result of an
// AI column for the rows that match the predicate.  The predicate is taken
// from the text of the statement, the status pseudo column of the retry is
// replaced by the state of the result column.
// ---------------------------------------------------------------------------
bool ai_is_ident_char(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
         || '_' == c || '$' == c;
}

// Find a keyword in the text of a statement.  The keyword must not be part of
// a longer identifier.
int ai_find_keyword(const ObString &text,
                    const int64_t from,
                    const char *keyword,
                    int64_t &pos)
{
  int ret = OB_SUCCESS;
  pos = -1;
  const int64_t kw_len = static_cast<int64_t>(strlen(keyword));
  const int64_t len = text.length();
  for (int64_t i = from; OB_SUCC(ret) && i + kw_len <= len; ++i) {
    if (0 == strncasecmp(text.ptr() + i, keyword, static_cast<size_t>(kw_len))
        && (i == 0 || !ai_is_ident_char(text.ptr()[i - 1]))
        && (i + kw_len == len || !ai_is_ident_char(text.ptr()[i + kw_len]))) {
      pos = i;
      break;
    }
  }
  return ret;
}

// Read the identifier at the position, the identifier may be quoted.
int ai_read_identifier(const ObString &text, const int64_t from, ObString &name)
{
  int ret = OB_SUCCESS;
  int64_t i = from;
  const int64_t len = text.length();
  while (i < len && (' ' == text.ptr()[i] || '\t' == text.ptr()[i]
                     || '\n' == text.ptr()[i] || '\r' == text.ptr()[i])) {
    ++i;
  }
  if (i >= len) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column retry, the column name is missing", K(ret));
  } else if ('`' == text.ptr()[i]) {
    const int64_t start = ++i;
    while (i < len && '`' != text.ptr()[i]) {
      ++i;
    }
    if (i >= len) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column retry, the column name is not terminated", K(ret));
    } else {
      name.assign_ptr(text.ptr() + start, static_cast<int32_t>(i - start));
    }
  } else {
    const int64_t start = i;
    while (i < len && ai_is_ident_char(text.ptr()[i])) {
      ++i;
    }
    if (i == start) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column retry, invalid column name", K(ret));
    } else {
      name.assign_ptr(text.ptr() + start, static_cast<int32_t>(i - start));
    }
  }
  return ret;
}

// Check whether the text of a statement references the identifier.  The
// identifier must not be part of a longer identifier, an alias like
// ready_label_is_readable does not reference the column label.
bool ai_text_contains_identifier(const ObString &text, const ObString &name)
{
  bool found = false;
  if (name.empty()) {
  } else {
    const int64_t len = text.length();
    const int64_t name_len = name.length();
    for (int64_t i = 0; !found && i + name_len <= len; ++i) {
      if (0 == strncasecmp(text.ptr() + i, name.ptr(), static_cast<size_t>(name_len))
          && (i == 0 || !ai_is_ident_char(text.ptr()[i - 1]))
          && (i + name_len == len || !ai_is_ident_char(text.ptr()[i + name_len]))) {
        found = true;
      }
    }
  }
  return found;
}

// Replace the status pseudo column of the retry predicate by the state of the
// result column.  A quoted identifier addresses the physical column, the
// pseudo column is only available for the retry.
int ai_replace_status_pseudo_column(const ObString &predicate,
                                    const ObString &column_name,
                                    ObSqlString &result)
{
  int ret = OB_SUCCESS;
  const int64_t len = predicate.length();
  int64_t i = 0;
  while (OB_SUCC(ret) && i < len) {
    const char c = predicate.ptr()[i];
    if ('\'' == c || '"' == c) {
      // a literal is copied as it is
      ret = result.append(&c, 1);
      ++i;
      while (OB_SUCC(ret) && i < len) {
        const char cur = predicate.ptr()[i];
        ret = result.append(&cur, 1);
        ++i;
        if (cur == c) {
          break;
        }
      }
    } else if ('`' == c) {
      // a quoted identifier addresses the physical column
      const int64_t start = i;
      ++i;
      while (i < len && '`' != predicate.ptr()[i]) {
        ++i;
      }
      if (i < len) {
        ++i;
      }
      ret = result.append(predicate.ptr() + start, i - start);
    } else if (ai_is_ident_char(c)) {
      const int64_t start = i;
      while (i < len && ai_is_ident_char(predicate.ptr()[i])) {
        ++i;
      }
      const int64_t id_len = i - start;
      if (6 == id_len && 0 == strncasecmp(predicate.ptr() + start, "status", 6)) {
        ret = result.append_fmt("(case when `%.*s` is not null then 'READY' else 'FAILED' end)",
                                column_name.length(), column_name.ptr());
      } else {
        ret = result.append(predicate.ptr() + start, id_len);
      }
    } else {
      ret = result.append(&c, 1);
      ++i;
    }
  }
  return ret;
}

// Strict read of the AI columns that are referenced by the text of the
// statement.  A select statement does not register its columns in the column
// items of the statement, the text is scanned for the AI columns of the
// referenced tables instead.
int ai_check_strict_read_by_text(ObExecContext &exec_ctx,
                                 ObSQLSessionInfo *session,
                                 ObDMLStmt *stmt,
                                 const ObString &sql_text)
{
  int ret = OB_SUCCESS;
  const auto &table_items = stmt->get_table_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); ++i) {
    auto *item = table_items.at(i);
    if (OB_ISNULL(item)) {
      continue;
    }
    const ObString &table_name = item->table_name_;
    if (table_name.empty()) {
      continue;
    }
    ObString db_name = item->database_name_;
    if (db_name.empty()) {
      db_name = session->get_database_name();
    }
    if (db_name.empty()) {
      continue;
    }
    ObSqlString meta_sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(meta_sql.append_fmt(
              "select t.table_id as ai_tid, a.column_id as ai_cid, a.column_name as ai_cname "
              "from oceanbase.__all_ai_column a "
              "join oceanbase.__all_table t on t.table_id = a.table_id "
              "join oceanbase.__all_database d on d.database_id = t.database_id "
              "where d.database_name = '%.*s' and t.table_name = '%.*s'",
              db_name.length(), db_name.ptr(),
              table_name.length(), table_name.ptr()))) {
        LOG_WARN("ai column strict read, build meta sql failed", K(ret));
      } else if (OB_FAIL(ai_execute_inner_read(exec_ctx, session, meta_sql.string(), res))) {
        LOG_WARN("ai column strict read, read ai columns failed", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column strict read, null inner result", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          const int rc = result->next();
          if (OB_ITER_END == rc) {
            break;
          } else if (OB_FAIL(rc)) {
            ret = rc;
            LOG_WARN("ai column strict read, fetch inner result failed", K(ret));
            break;
          }
          int64_t table_id = 0;
          int64_t column_id = 0;
          ObString column_name;
          if (OB_FAIL(result->get_int("ai_tid", table_id))
              || OB_FAIL(result->get_int("ai_cid", column_id))
              || OB_FAIL(result->get_varchar("ai_cname", column_name))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai column strict read, read ai column failed", K(ret));
          } else if (!ai_text_contains_identifier(sql_text, column_name)) {
            // the statement does not reference the AI column
          } else {
            ObSqlString data_sql;
            int64_t unfinished = 0;
            if (OB_FAIL(data_sql.append_fmt(
                    "select count(*) as c from `%.*s`.`%.*s` where `%.*s` is null",
                    db_name.length(), db_name.ptr(),
                    table_name.length(), table_name.ptr(),
                    column_name.length(), column_name.ptr()))) {
              LOG_WARN("ai column strict read, build data sql failed", K(ret));
            } else if (OB_FAIL(ai_read_single_count(exec_ctx, session,
                                                    data_sql.string(), unfinished))) {
              LOG_WARN("ai column strict read, check unfinished rows failed", K(ret));
            } else if (unfinished > 0) {
              ret = OB_AI_COLUMN_UNFINISHED;
              LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED,
                             table_id,
                             column_id);
              LOG_WARN("ai column strict read, reject unfinished ai column", K(ret),
                       K(db_name), K(table_name), K(column_name), K(unfinished));
            }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace

// Run a statement on an autonomous transaction, the rows of the AI column task
// table are not part of the transaction of the user statement.
int ai_execute_inner_write_raw(const ObString &stmt, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_, true))) {
    LOG_WARN("ai column dml, start task transaction failed", K(ret));
  } else {
    if (OB_FAIL(trans.write(stmt.ptr(), affected_rows))) {
      LOG_WARN("ai column dml, write task row failed", K(ret), "sql", stmt);
    }
    if (OB_FAIL(trans.end(OB_SUCCESS == ret))) {
      LOG_WARN("ai column dml, end task transaction failed", K(ret));
    }
  }
  return ret;
}

int ai_build_ai_column_retry_sql(const ObString &db_name,
                                 const ObString &table_name,
                                 const ObString &column_name,
                                 const ObString &sql_text,
                                 ObIAllocator &allocator,
                                 ObString &retry_sql)
{
  int ret = OB_SUCCESS;
  ObString text_column_name;
  ObString predicate;
  int64_t retry_pos = -1;
  int64_t ai_pos = -1;
  int64_t column_pos = -1;
  int64_t where_pos = -1;
  if (OB_FAIL(ai_find_keyword(sql_text, 0, "retry", retry_pos))) {
  } else if (retry_pos < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column retry, the retry keyword is missing", K(ret));
  } else if (OB_FAIL(ai_find_keyword(sql_text, retry_pos + 5, "ai", ai_pos))) {
  } else if (ai_pos < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column retry, the ai keyword is missing", K(ret));
  } else if (OB_FAIL(ai_find_keyword(sql_text, ai_pos + 2, "column", column_pos))) {
  } else if (column_pos < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column retry, the column keyword is missing", K(ret));
  } else if (OB_FAIL(ai_read_identifier(sql_text, column_pos + 6, text_column_name))) {
  } else if (OB_FAIL(ai_find_keyword(sql_text, column_pos + 6, "where", where_pos))) {
  } else if (where_pos >= 0) {
    int64_t start = where_pos + 5;
    int64_t end = sql_text.length();
    while (end > start && (' ' == sql_text.ptr()[end - 1] || '\t' == sql_text.ptr()[end - 1]
                           || '\n' == sql_text.ptr()[end - 1] || '\r' == sql_text.ptr()[end - 1]
                           || ';' == sql_text.ptr()[end - 1])) {
      --end;
    }
    if (end > start) {
      predicate.assign_ptr(sql_text.ptr() + start, static_cast<int32_t>(end - start));
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString pred_sql;
    if (!predicate.empty()
        && OB_FAIL(ai_replace_status_pseudo_column(predicate, column_name, pred_sql))) {
    } else {
      ObSqlString stmt_sql;
      ret = stmt_sql.append_fmt("update `%.*s`.`%.*s` set `%.*s` = null",
                                db_name.length(), db_name.ptr(),
                                table_name.length(), table_name.ptr(),
                                column_name.length(), column_name.ptr());
      if (OB_SUCC(ret) && !predicate.empty()) {
        ret = stmt_sql.append(" where ");
        if (OB_SUCC(ret)) {
          ret = stmt_sql.append(pred_sql.string().ptr(), pred_sql.string().length());
        }
      }
      if (OB_SUCC(ret)) {
        const ObString &text = stmt_sql.string();
        char *buf = static_cast<char *>(allocator.alloc(text.length() + 1));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("ai column retry, allocate the retry statement failed", K(ret));
        } else {
          MEMCPY(buf, text.ptr(), text.length());
          buf[text.length()] = '\0';
          retry_sql.assign_ptr(buf, text.length());
        }
      }
    }
  }
  return ret;
}

int ai_column_check_strict_read(ObExecContext &exec_ctx,
                                ObDMLStmt *stmt,
                                const ObString &sql_text)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = exec_ctx.get_my_session();
  if (OB_ISNULL(session) || OB_ISNULL(stmt) || session->is_inner()) {
    // inner SQL must never be blocked by its own probes
  } else {
    bool strict = false;
    if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict))) {
      LOG_WARN("ai column strict read, read session variable failed", K(ret));
      ret = OB_SUCCESS;
    } else if (!strict) {
      // default behaviour: unfinished results read as NULL
    } else {
      ObSEArray<ObColumnRefRawExpr *, 16> cols;
      if (OB_FAIL(ai_collect_column_refs(stmt, cols))) {
        LOG_WARN("ai column strict read, collect referenced columns failed", K(ret));
        ret = OB_SUCCESS;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
        ObColumnRefRawExpr *col = cols.at(i);
        if (OB_ISNULL(col)) {
          continue;
        }
        const ObString &db_name = col->get_database_name();
        const ObString &table_name = col->get_table_name();
        const ObString &column_name = col->get_column_name();
        if (db_name.empty() || table_name.empty() || column_name.empty()) {
          continue;
        }
        int64_t ai_table_id = OB_INVALID_ID;
        int64_t ai_column_id = OB_INVALID_ID;
        bool is_ai = false;
        if (OB_FAIL(ai_read_ai_column_meta(exec_ctx, session,
                                           db_name, table_name, column_name,
                                           ai_table_id, ai_column_id,
                                           is_ai))) {
          LOG_WARN("ai column strict read, check ai column metadata failed", K(ret));
          ret = OB_SUCCESS;
        } else if (!is_ai) {
          // not an AI result column, plain columns are never blocked
        } else {
          ObSqlString data_sql;
          int64_t unfinished = 0;
          if (OB_FAIL(data_sql.append_fmt(
                  "select count(*) as c from `%.*s`.`%.*s` where `%.*s` is null",
                  db_name.length(), db_name.ptr(),
                  table_name.length(), table_name.ptr(),
                  column_name.length(), column_name.ptr()))) {
            LOG_WARN("ai column strict read, build data sql failed", K(ret));
          } else if (OB_FAIL(ai_read_single_count(exec_ctx, session,
                                                  data_sql.string(), unfinished))) {
            LOG_WARN("ai column strict read, check unfinished rows failed", K(ret));
          } else if (unfinished > 0) {
            ret = OB_AI_COLUMN_UNFINISHED;
            LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED, ai_table_id, ai_column_id);
            LOG_WARN("ai column strict read, reject unfinished ai column", K(ret),
                     K(db_name), K(table_name), K(column_name), K(unfinished));
          }
        }
      }
      if (OB_SUCC(ret) && !sql_text.empty()) {
        // A select statement does not register its columns in the column items
        // of the statement, the text of the statement is scanned for the AI
        // columns of the referenced tables as well.
        ret = ai_check_strict_read_by_text(exec_ctx, session, stmt, sql_text);
      }
    }
  }
  return ret;
}

int ai_column_task_on_end_trans(const uint64_t session_id,
                                 const int64_t tx_id,
                                 const bool is_rollback)
{
  int ret = OB_SUCCESS;
  if (!is_rollback) {
    // The task rows of a committed transaction stay in the task table, the
    // worker publishes them once the transaction is visible.
    return ret;
  }
  std::vector<AiColumnTaskKey> tasks;
  AiColumnTaskRegistry::instance().take_tasks(session_id, tx_id, tasks);
  if (tasks.empty()) {
    LOG_INFO("ai column task, end trans", K(session_id), K(tx_id), K(is_rollback),
             K(tasks.size()), "no_task", 0);
    return ret;
  }
  ObSqlString sql;
  int64_t affected_rows = 0;
  ret = sql.append_fmt("delete from %s where ", AI_TASK_TABLE_NAME);
  for (size_t i = 0; OB_SUCC(ret) && i < tasks.size(); ++i) {
    if (i > 0 && OB_FAIL(sql.append(" or "))) {
    } else if (OB_FAIL(sql.append_fmt("(table_id = %ld and request_id = %ld)",
                                      static_cast<int64_t>(tasks.at(i).table_id_),
                                      tasks.at(i).request_id_))) {
    }
  }
  if (OB_SUCC(ret)) {
    const int tmp_ret = ai_execute_inner_write_raw(sql.string(), affected_rows);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("ai column task, batch end-trans action failed", K(tmp_ret),
               K(is_rollback), K(tasks.size()), "sql", sql.ptr());
    }
  }
  LOG_INFO("ai column task, end trans", K(session_id), K(tx_id), K(is_rollback),
           K(tasks.size()), "sql", sql.ptr());
  return ret;
}

int ObDMLService::insert_row(const ObInsCtDef &ins_ctdef,
                             ObInsRtDef &ins_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        ins_ctdef.new_row_,
                                        ins_ctdef,
                                        ins_rtdef))) {
  } else if (OB_FAIL(ai_column_handle_insert(ins_ctdef, dml_rtctx))) {
  } else if (OB_FAIL(insert_row(ins_ctdef.das_ctdef_,
                                ins_rtdef.das_rtdef_,
                                tablet_loc,
                                dml_rtctx,
                                ins_ctdef.new_row_,
                                stored_row))) {
  }
  return ret;
}

int ObDMLService::insert_row(const ObDASInsCtDef &ins_ctdef,
                             ObDASInsRtDef &ins_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             const ExprFixedArray &new_row,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  ret = write_row_to_das_op<DAS_OP_TABLE_INSERT>(ins_ctdef,
                                                 ins_rtdef,
                                                 tablet_loc,
                                                 dml_rtctx,
                                                 new_row,
                                                 nullptr,
                                                 stored_row);
  return ret;
}

int ObDMLService::delete_row(const ObDelCtDef &del_ctdef,
                             ObDelRtDef &del_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        del_ctdef.old_row_,
                                        del_ctdef,
                                        del_rtdef))) {
  } else if (OB_FAIL(ai_column_handle_delete(del_ctdef, dml_rtctx))) {
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_DELETE>(del_ctdef.das_ctdef_,
                                                              del_rtdef.das_rtdef_,
                                                              tablet_loc,
                                                              dml_rtctx,
                                                              del_ctdef.old_row_,
                                                              del_ctdef.trans_info_expr_,
                                                              stored_row))) {
  }
  return ret;
}
int ObDMLService::lock_row(const ObLockCtDef &lock_ctdef,
                           ObLockRtDef &lock_rtdef,
                           const ObDASTabletLoc *tablet_loc,
                           ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  ObChunkDatumStore::StoredRow* stored_row = nullptr;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        lock_ctdef.old_row_,
                                        lock_ctdef,
                                        lock_rtdef))) {
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_LOCK>(lock_ctdef.das_ctdef_,
                                                lock_rtdef.das_rtdef_,
                                                tablet_loc,
                                                dml_rtctx,
                                                lock_ctdef.old_row_,
                                                nullptr,
                                                stored_row))) {
  }
  return ret;
}

int ObDMLService::lock_row(const ObDASLockCtDef &dlock_ctdef,
                           ObDASLockRtDef &dlock_rtdef,
                           const ObDASTabletLoc *tablet_loc,
                           ObDMLRtCtx &das_rtctx,
                           const ExprFixedArray &old_row)
{
  ObChunkDatumStore::StoredRow* stored_row = nullptr;
  return write_row_to_das_op<DAS_OP_TABLE_LOCK>(dlock_ctdef,
                                                dlock_rtdef,
                                                tablet_loc,
                                                das_rtctx,
                                                old_row,
                                                nullptr,
                                                stored_row);
}

int ObDMLService::split_upd_to_del_and_ins(const ObUpdCtDef &upd_ctdef,
                                           ObUpdRtDef &upd_rtdef,
                                           const ObDASTabletLoc *old_tablet_loc,
                                           const ObDASTabletLoc *new_tablet_loc,
                                           ObDMLRtCtx &dml_rtctx,
                                           ObChunkDatumStore::StoredRow *&old_row,
                                           ObChunkDatumStore::StoredRow *&new_row)
{
  int ret = OB_SUCCESS;
  if (dml_rtctx.das_ref_.get_parallel_type() == DAS_STREAMING_PARALLEL) {
    dml_rtctx.das_ref_.get_das_parallel_ctx().set_das_parallel_type(DAS_BLOCKING_PARALLEL);
  }
  if (OB_LIKELY(old_tablet_loc != new_tablet_loc && !upd_ctdef.multi_ctdef_->is_enable_row_movement_)) {
    ret = OB_ERR_UPD_CAUSE_PART_CHANGE;
  } else if (OB_ISNULL(upd_rtdef.ddel_rtdef_)) {
    if (OB_FAIL(init_das_del_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
    } else if (OB_FAIL(init_das_ins_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
    }
  }

  if (OB_SUCC(ret)) {
    //because of this bug:
    //if the updated row is moved across partitions, we must delete old row at first
    //and then store new row to a temporary buffer,
    //only when all old rows have been deleted, new rows can be inserted
    if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_DELETE>(*upd_ctdef.ddel_ctdef_,
                                                         *upd_rtdef.ddel_rtdef_,
                                                         old_tablet_loc,
                                                         dml_rtctx,
                                                         upd_ctdef.old_row_,
                                                         upd_ctdef.trans_info_expr_,
                                                         old_row))) {
    } else if (((upd_ctdef.is_table_without_pk_ && old_tablet_loc != new_tablet_loc) || (upd_ctdef.is_vec_hnsw_index_vid_opt_)) &&
        OB_FAIL(set_update_hidden_pk(dml_rtctx.get_eval_ctx(),
                                     upd_ctdef,
                                     new_tablet_loc->tablet_id_))) {
    } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_INSERT>(*upd_ctdef.dins_ctdef_,
                                                                *upd_rtdef.dins_rtdef_,
                                                                new_tablet_loc,
                                                                dml_rtctx,
                                                                upd_ctdef.new_row_,
                                                                nullptr,
                                                                new_row))) {
    } else {
      LOG_DEBUG("update pkey changed", K(ret), KPC(old_tablet_loc), KPC(new_tablet_loc),
                "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
    }
  }
  return ret;
}


/*
 * Note: During the update process,
 * ObDMLService::check_row_whether_changed() and ObDMLService::update_row() must be executed together
 * within a single iteration,
 * because the update_row process relies on check_row_whether_changed to determine
 * whether the new and old values of the row being updated have changed.
 **/
int ObDMLService::update_row(const ObUpdCtDef &upd_ctdef,
                             ObUpdRtDef &upd_rtdef,
                             const ObDASTabletLoc *old_tablet_loc,
                             const ObDASTabletLoc *new_tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             ObChunkDatumStore::StoredRow *&old_row,
                             ObChunkDatumStore::StoredRow *&new_row,
                             ObChunkDatumStore::StoredRow *&full_row)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(dml_rtctx.get_exec_ctx());
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *old_tablet_loc,
                                        upd_ctdef.old_row_,
                                        upd_ctdef,
                                        upd_rtdef))) {
  } else if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                               *new_tablet_loc,
                                               upd_ctdef.new_row_,
                                               upd_ctdef,
                                               upd_rtdef))) {
  } else if (OB_UNLIKELY(!upd_rtdef.is_row_changed_)) {
    //old row is equal to new row, only need to lock row
    // RETRY AI COLUMN resets the result of an AI column to NULL.  When the
    // result was already NULL the row does not change, but the result has to
    // be computed again, so the task is created nevertheless.
    if (OB_FAIL(ai_column_handle_update(upd_ctdef, dml_rtctx, false))) {
    } else if (OB_ISNULL(upd_rtdef.dlock_rtdef_)) {
      ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
      if (OB_FAIL(init_das_lock_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
      } else {
        upd_rtdef.dlock_rtdef_->for_upd_wait_time_ = plan_ctx->get_ps_timeout_timestamp();
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_LOCK>(*upd_ctdef.dlock_ctdef_,
                                                         *upd_rtdef.dlock_rtdef_,
                                                         old_tablet_loc,
                                                         dml_rtctx,
                                                         upd_ctdef.old_row_,
                                                         nullptr,
                                                         old_row))) {
      } else {
        new_row = old_row;
      }
    }
  } else if (OB_FAIL(ai_column_handle_update(upd_ctdef, dml_rtctx, true))) {
  } else if (upd_ctdef.das_base_ctdef_.is_update_pk_) {
    if (!upd_rtdef.dupd_rtdef_.is_immediate_row_conflict_check_ && !upd_ctdef.dupd_ctdef_.is_ignore_) {
      if (OB_FAIL(split_upd_to_del_and_ins(upd_ctdef,
                                           upd_rtdef,
                                           old_tablet_loc,
                                           new_tablet_loc,
                                           dml_rtctx,
                                           old_row,
                                           new_row))) {
      }
    } else {
      if (new_tablet_loc != old_tablet_loc) {
        if (upd_ctdef.dupd_ctdef_.is_ignore_) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "Cross-partition update ignore");
        } else if (OB_FAIL(split_upd_to_del_and_ins(upd_ctdef,
                                                    upd_rtdef,
                                                    old_tablet_loc,
                                                    new_tablet_loc,
                                                    dml_rtctx,
                                                    old_row,
                                                    new_row))) {
        }
      } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_UPDATE>(upd_ctdef.dupd_ctdef_,
                                                                upd_rtdef.dupd_rtdef_,
                                                                old_tablet_loc,
                                                                dml_rtctx,
                                                                upd_ctdef.full_row_,
                                                                upd_ctdef.trans_info_expr_,
                                                                full_row))) {
      } else {
        LOG_DEBUG("update pkey", K(ret), KPC(old_tablet_loc),
                  "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                  "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
      }
    }
  } else {
    // do update
    if (new_tablet_loc != old_tablet_loc) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_UPDATE>(upd_ctdef.dupd_ctdef_,
                                                                upd_rtdef.dupd_rtdef_,
                                                                old_tablet_loc,
                                                                dml_rtctx,
                                                                upd_ctdef.full_row_,
                                                                upd_ctdef.trans_info_expr_,
                                                                full_row))) {
    } else {
      LOG_DEBUG("not update pkey", K(ret), KPC(old_tablet_loc),
                "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
    }
  }
  return ret;
}

int ObDMLService::update_row(const ObDASUpdCtDef &ctdef,
                             ObDASUpdRtDef &rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             const ExprFixedArray &full_row)
{
  ObChunkDatumStore::StoredRow* stored_row = nullptr;
  return write_row_to_das_op<DAS_OP_TABLE_UPDATE>(ctdef,
                                                  rtdef,
                                                  tablet_loc,
                                                  dml_rtctx,
                                                  full_row,
                                                  nullptr,
                                                  stored_row);
}

int ObDMLService::delete_row(const ObDASDelCtDef &das_del_ctdef,
                             ObDASDelRtDef &das_del_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &das_rtctx,
                             const ExprFixedArray &old_row,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  return write_row_to_das_op<DAS_OP_TABLE_DELETE>(das_del_ctdef,
                                                  das_del_rtdef,
                                                  tablet_loc,
                                                  das_rtctx,
                                                  old_row,
                                                  nullptr,
                                                  stored_row);
}

int ObDMLService::prepare_dml_execution(
    const ObDASDMLBaseCtDef &base_ctdef,
    ObDASDMLBaseRtDef &base_rtdef,
    transaction::ObTxReadSnapshot &snapshot,
    const int16_t write_branch_id,
    ObIAllocator &das_alloc,
    data_plane::ObWriteContext &write_context,
    data_plane::ObDmlExecution &execution,
    bool use_snapshot_opt)
{
  int ret = OB_SUCCESS;
  data_plane::ObDmlWriteSpec write_spec;
  concurrent_control::ObWriteFlag write_flag;
  write_spec.timeout_ = base_rtdef.timeout_ts_;
  write_spec.schema_version_ = base_ctdef.schema_version_;
  write_spec.sql_mode_ = base_rtdef.sql_mode_;
  write_spec.tz_info_ = &base_ctdef.tz_info_;
  write_spec.branch_id_ = write_branch_id;
  write_spec.is_total_quantity_log_ = base_ctdef.is_total_quantity_log_;
  write_spec.prelock_ = base_rtdef.prelock_;
  write_spec.is_batch_stmt_ = base_ctdef.is_batch_stmt_;
  write_spec.is_main_table_in_fts_ddl_ =
      base_ctdef.is_main_table_in_fts_ddl_;
  write_spec.check_schema_version_ =
      !base_ctdef.skip_check_schema_version_;
  write_spec.access_vector_id_as_master_table_ =
      base_ctdef.is_access_vidx_as_master_table_;
  init_dml_write_flag(
      base_ctdef, base_rtdef, write_flag, use_snapshot_opt);
  data_plane::ObIDmlService *dml_service =
      ::oceanbase::share::server_service<::oceanbase::data_plane::ObIDmlService>();
  if (OB_ISNULL(dml_service)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(dml_service->prepare_execution(
                 write_spec,
                 base_ctdef.table_param_,
                 snapshot,
                 das_alloc,
                 write_context,
                 write_flag,
                 execution))) {
  }
  return ret;
}

void ObDMLService::init_dml_write_flag(const ObDASDMLBaseCtDef &base_ctdef,
                                       ObDASDMLBaseRtDef &base_rtdef,
                                       concurrent_control::ObWriteFlag &write_flag,
                                       bool use_snapshot_opt)
{
  if (base_ctdef.is_batch_stmt_) {
    write_flag.set_is_dml_batch_opt();
  }
  if (base_ctdef.is_insert_up_) {
    write_flag.set_is_insert_up();
  }
  if (base_ctdef.table_param_.get_data_table().is_storage_index_table()
      && !base_ctdef.table_param_.get_data_table().can_read_index()) {
    write_flag.set_is_write_only_index();
  }
  if (base_rtdef.is_for_foreign_key_check_) {
    write_flag.set_check_row_locked();
  }
  if (base_ctdef.is_update_uk_) {
    write_flag.set_update_uk();
  }
  if (base_ctdef.is_update_pk_with_dop_) {
    write_flag.set_update_pk_dop();
  }
  if (base_rtdef.is_immediate_row_conflict_check_ && base_ctdef.is_update_pk_) {
    write_flag.set_immediate_row_check();
  }
  if (use_snapshot_opt) {
    write_flag.set_snapshot_opt();
  }
}

int ObDMLService::init_das_dml_rtdef(ObDMLRtCtx &dml_rtctx,
                                     const ObDASDMLBaseCtDef &das_ctdef,
                                     ObDASDMLBaseRtDef &das_rtdef,
                                     const ObDASTableLocMeta *loc_meta)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(dml_rtctx.get_exec_ctx());
  ObSQLSessionInfo *my_session = GET_MY_SESSION(dml_rtctx.get_exec_ctx());
  ObDASCtx &das_ctx = dml_rtctx.get_exec_ctx().get_das_ctx();
  uint64_t table_loc_id = das_ctdef.table_id_;
  uint64_t ref_table_id = das_ctdef.index_tid_;
  das_rtdef.ctdef_ = &das_ctdef;
  das_rtdef.timeout_ts_ = plan_ctx->get_ps_timeout_timestamp();
  das_rtdef.prelock_ = my_session->get_prelock();
  das_rtdef.runtime_schema_version_ = plan_ctx->get_runtime_schema_version();
  das_rtdef.sql_mode_ = my_session->get_sql_mode();
  das_rtdef.is_immediate_row_conflict_check_ =  my_session->enable_immediate_row_conflict_check();
  if (OB_ISNULL(das_rtdef.table_loc_ = dml_rtctx.op_.get_input()->get_table_loc())) {
    das_rtdef.table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id);
    if (OB_ISNULL(das_rtdef.table_loc_)) {
      if (OB_ISNULL(loc_meta)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(das_ctx.extended_table_loc(*loc_meta, das_rtdef.table_loc_))) {
      }
    }
  }
  if (ObSQLUtils::is_fk_nested_sql(&dml_rtctx.get_exec_ctx())) {
    das_rtdef.is_for_foreign_key_check_ = true;
  }
  return ret;
}

int ObDMLService::init_trigger_for_insert(
  ObDMLRtCtx &dml_rtctx,
  const ObInsCtDef &ins_ctdef,
  ObInsRtDef &ins_rtdef,
  ObIArray<ObExpr*> &clear_exprs)
{
  int ret = OB_SUCCESS;
  if (!ins_ctdef.is_primary_index_ || 0 >= ins_ctdef.trig_ctdef_.tg_args_.count()) {
    // nothing
  } else if (OB_FAIL(TriggerHandle::init_trigger_params(dml_rtctx, ins_ctdef.trig_ctdef_.tg_event_,
                          ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
  } else {
    append(clear_exprs, ins_ctdef.trig_ctdef_.new_row_exprs_);
  }
  return ret;
}

int ObDMLService::init_fk_checker_array(ObDMLRtCtx &dml_rtctx,
                                        const ObDMLBaseCtDef &dml_ctdef,
                                        FkCheckerArray &fk_checker_array)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObForeignKeyArgArray &fk_args = dml_ctdef.fk_args_;
  if (!fk_args.empty()) {
    if (OB_FAIL(fk_checker_array.allocate_array(allocator, fk_args.count()))) {
    } else {
      for (int i = 0; OB_SUCC(ret) && i < fk_args.count(); ++i) {
        fk_checker_array.at(i) = nullptr;
      }
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < fk_args.count(); ++i) {
    const ObForeignKeyArg &fk_arg = fk_args.at(i);
    ObForeignKeyChecker *fk_checker = nullptr;
    if (fk_arg.use_das_scan_) {
      if (OB_ISNULL(fk_arg.fk_ctdef_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        // create fk_checker here
        void *checker_buf = allocator.alloc(sizeof(ObForeignKeyChecker));
        if (OB_ISNULL(checker_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          fk_checker = new(checker_buf) ObForeignKeyChecker(dml_rtctx.get_eval_ctx(), *fk_arg.fk_ctdef_);
          fk_checker_array.at(i) = fk_checker;
          const ObExprFrameInfo *expr_frame_info = NULL;
          expr_frame_info = nullptr != dml_rtctx.op_.get_spec().expr_frame_info_
                               ? dml_rtctx.op_.get_spec().expr_frame_info_
                               : &(dml_rtctx.op_.get_spec().plan_->get_expr_frame_info());
          int64_t estimate_row = dml_rtctx.op_.get_spec().rows_;
          ObIAllocator *allocator = &dml_rtctx.op_.get_exec_ctx().get_allocator();
          if (OB_FAIL(fk_checker->init_foreign_key_checker(estimate_row, expr_frame_info, *fk_arg.fk_ctdef_,
                                                           dml_ctdef.new_row_, allocator))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObDMLService::init_ins_rtdef(
  ObDMLRtCtx &dml_rtctx,
  ObInsRtDef &ins_rtdef,
  const ObInsCtDef &ins_ctdef,
  ObIArray<ObExpr*> &clear_exprs,
  ObIArray<ObForeignKeyChecker*> &fk_checkers)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_INSERTING);
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(ins_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(ins_ctdef.das_ctdef_.index_tid_, dml_rtctx.get_exec_ctx()))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                       ins_ctdef.das_ctdef_,
                                       ins_rtdef.das_rtdef_,
                                       loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, ins_ctdef.related_ctdefs_, ins_rtdef.related_rtdefs_))) {
  } else if (OB_FAIL(init_trigger_for_insert(dml_rtctx, ins_ctdef, ins_rtdef, clear_exprs))) {
  } else if (OB_FAIL(init_fk_checker_array(dml_rtctx, ins_ctdef, ins_rtdef.fk_checker_array_))) {
  } else {
    ins_rtdef.das_rtdef_.related_ctdefs_ = &ins_ctdef.related_ctdefs_;
    ins_rtdef.das_rtdef_.related_rtdefs_ = &ins_rtdef.related_rtdefs_;
    for (int64_t i = 0; OB_SUCC(ret) && i < ins_rtdef.fk_checker_array_.count(); ++i) {
      if (OB_NOT_NULL(ins_rtdef.fk_checker_array_.at(i))) {
        fk_checkers.push_back(ins_rtdef.fk_checker_array_.at(i));
      }
    }
  }
  return ret;
}

int ObDMLService::init_trigger_for_delete(
  ObDMLRtCtx &dml_rtctx, const ObDelCtDef &del_ctdef, ObDelRtDef &del_rtdef)
{
  int ret = OB_SUCCESS;
  if (del_ctdef.is_primary_index_ && 0 < del_ctdef.trig_ctdef_.tg_args_.count()) {
    OZ(TriggerHandle::init_trigger_params(dml_rtctx, del_ctdef.trig_ctdef_.tg_event_,
                                del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_));
    LOG_DEBUG("debug delete trigger init", K(ret), K(del_ctdef.is_primary_index_),
      K(del_ctdef.trig_ctdef_.tg_args_.count()));
  }
  return ret;
}

int ObDMLService::init_del_rtdef(ObDMLRtCtx &dml_rtctx,
                                 ObDelRtDef &del_rtdef,
                                 const ObDelCtDef &del_ctdef)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_DELETING);
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(del_ctdef.multi_ctdef_);
    if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(del_ctdef.das_ctdef_.index_tid_, dml_rtctx.get_exec_ctx()))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                       del_ctdef.das_ctdef_,
                                       del_rtdef.das_rtdef_,
                                       loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, del_ctdef.related_ctdefs_, del_rtdef.related_rtdefs_))) {
  } else if (OB_FAIL(init_trigger_for_delete(dml_rtctx, del_ctdef, del_rtdef))) {
  } else if (OB_FAIL(init_fk_checker_array(dml_rtctx, del_ctdef, del_rtdef.fk_checker_array_))) {
  } else {
    del_rtdef.das_rtdef_.related_ctdefs_ = &del_ctdef.related_ctdefs_;
    del_rtdef.das_rtdef_.related_rtdefs_ = &del_rtdef.related_rtdefs_;
  }

  if (OB_SUCC(ret)) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    const uint64_t del_table_id = del_ctdef.das_base_ctdef_.index_tid_;
    ObExecContext *root_ctx = nullptr;
    if (T_DISTINCT_NONE != del_ctdef.distinct_algo_) {
      if (dml_op.is_fk_nested_session()) {
        // for delete distinct check that has foreign key, perform global distinct check between nested session,
        // to avoid delete same row mutiple times between different nested sqls
        if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
        } else if (OB_ISNULL(root_ctx)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          DASDelCtxList& del_ctx_list = root_ctx->get_das_ctx().get_das_del_ctx_list();
          if (ObDMLService::has_nested_delete_ctx(del_table_id, del_ctx_list)) {
            // for table deleted at parent session too, no need to create a new hash set
            if (OB_FAIL(ObDMLService::get_nested_delete_ctx(del_table_id, del_ctx_list, del_rtdef.se_rowkey_dist_ctx_))) {
            }
          } else {
            // for table not deleted at parent session, create a new hash set and add to the list at root ctx
            DmlRowkeyDistCtx del_ctx;
            del_ctx.table_id_ = del_table_id;
            if (OB_FAIL(ObDMLService::create_rowkey_check_hashset(dml_op.get_spec().rows_, root_ctx, del_ctx.deleted_rows_))) {
            } else if (OB_FAIL(del_ctx_list.push_back(del_ctx))) {
              if (nullptr != del_ctx.deleted_rows_) {
                del_ctx.deleted_rows_->destroy();
                del_ctx.deleted_rows_ = nullptr;
              }
            } else {
              del_rtdef.se_rowkey_dist_ctx_ = del_ctx.deleted_rows_;
            }
          }
        }
      } else {
        // for delete distinct check without foreign key, perform distinct check at current sql
        DASDelCtxList& del_ctx_list = dml_op.get_exec_ctx().get_das_ctx().get_das_del_ctx_list();
        DmlRowkeyDistCtx del_ctx;
        del_ctx.table_id_ = del_table_id;
        if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
        } else if (OB_ISNULL(root_ctx) || root_ctx != &dml_op.get_exec_ctx()) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ObDMLService::create_rowkey_check_hashset(dml_op.get_spec().rows_, root_ctx, del_ctx.deleted_rows_))) {
        } else if (OB_FAIL(del_ctx_list.push_back(del_ctx))) {
          if (nullptr != del_ctx.deleted_rows_) {
            del_ctx.deleted_rows_->destroy();
            del_ctx.deleted_rows_ = nullptr;
          }
        } else {
          del_rtdef.se_rowkey_dist_ctx_ = del_ctx.deleted_rows_;
        }
      }
    } else { //T_DISTINCT_NONE == del_ctdef.distinct_algo_, means optimizer think don't need to create a new hash set for distinct check
      if (dml_op.is_fk_nested_session()) { //for delete triggered by delete cascade, need to check whether upper nested sqls will delete the same table
        if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
        } else if (OB_ISNULL(root_ctx)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          DASDelCtxList& del_ctx_list = root_ctx->get_das_ctx().get_das_del_ctx_list();
          if (ObDMLService::has_nested_delete_ctx(del_table_id, del_ctx_list)) {
            // Reuse the root context when an outer cascade already deletes this table.
            if (OB_FAIL(ObDMLService::get_nested_delete_ctx(del_table_id, del_ctx_list, del_rtdef.se_rowkey_dist_ctx_))) {
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(del_rtdef.se_rowkey_dist_ctx_)) {
    const int64_t rowkey_cnt = del_ctdef.distinct_key_.count();
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(init_ob_rowkey(allocator, del_ctdef.distinct_key_.count(), del_rtdef.table_rowkey_))) {
    }
  }
  return ret;
}

int ObDMLService::init_trigger_for_update(
  ObDMLRtCtx &dml_rtctx,
  const ObUpdCtDef &upd_ctdef,
  ObUpdRtDef &upd_rtdef,
  ObTableModifyOp &dml_op,
  ObIArray<ObExpr*> &clear_exprs)
{
  UNUSED(dml_op);
  int ret = OB_SUCCESS;
  if (!upd_ctdef.is_primary_index_ || 0 >= upd_ctdef.trig_ctdef_.tg_args_.count()) {
    // nothing
  } else if (OB_FAIL(TriggerHandle::init_trigger_params(dml_rtctx, upd_ctdef.trig_ctdef_.tg_event_,
                          upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
  } else {
    ObSEArray<ObExpr *, 4> new_exprs;
    for (int64_t i = 0; i < upd_ctdef.trig_ctdef_.new_row_exprs_.count() && OB_SUCC(ret); ++i) {
      if (upd_ctdef.trig_ctdef_.new_row_exprs_.at(i) != upd_ctdef.trig_ctdef_.old_row_exprs_.at(i)) {
        OZ(new_exprs.push_back(upd_ctdef.trig_ctdef_.new_row_exprs_.at(i)));
      }
    }
    if (OB_SUCC(ret)) {
      append(clear_exprs, new_exprs);
    }
  }
  return ret;
}

int ObDMLService::init_upd_rtdef(
  ObDMLRtCtx &dml_rtctx,
  ObUpdRtDef &upd_rtdef,
  const ObUpdCtDef &upd_ctdef,
  ObIArray<ObExpr*> &clear_exprs,
  ObIArray<ObForeignKeyChecker*> &fk_checkers)
{
  int ret = OB_SUCCESS;
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_UPDATING);
  if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(upd_ctdef.dupd_ctdef_.index_tid_, dml_rtctx.get_exec_ctx()))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                       upd_ctdef.dupd_ctdef_,
                                       upd_rtdef.dupd_rtdef_,
                                       loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_upd_ctdefs_, upd_rtdef.related_upd_rtdefs_))) {
  } else if (OB_FAIL(init_trigger_for_update(dml_rtctx, upd_ctdef, upd_rtdef, dml_rtctx.op_, clear_exprs))) {
  } else if (OB_FAIL(init_fk_checker_array(dml_rtctx, upd_ctdef, upd_rtdef.fk_checker_array_))) {
  } else {
    upd_rtdef.dupd_rtdef_.related_ctdefs_ = &upd_ctdef.related_upd_ctdefs_;
    upd_rtdef.dupd_rtdef_.related_rtdefs_ = &upd_rtdef.related_upd_rtdefs_;
    dml_rtctx.get_exec_ctx().set_update_columns(&upd_ctdef.assign_columns_);
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_rtdef.fk_checker_array_.count(); ++i) {
      if (OB_NOT_NULL(upd_rtdef.fk_checker_array_.at(i))) {
        fk_checkers.push_back(upd_rtdef.fk_checker_array_.at(i));
      }
    }
  }

  if (OB_SUCC(ret) && T_DISTINCT_NONE != upd_ctdef.distinct_algo_) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    if (OB_FAIL(create_rowkey_check_hashset(dml_op.get_spec().rows_,
                                            &dml_op.get_exec_ctx(),
                                            upd_rtdef.se_rowkey_dist_ctx_))) {
    }
  }

  if (OB_SUCC(ret) && OB_NOT_NULL(upd_rtdef.se_rowkey_dist_ctx_)) {
    const int64_t rowkey_cnt = upd_ctdef.distinct_key_.count();
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(init_ob_rowkey(allocator, upd_ctdef.distinct_key_.count(), upd_rtdef.table_rowkey_))) {
    }
  }

  // Calculate if there exists table cycle.
  // A repeated table in the parent set means the cascade contains a cycle.
  // Otherwise it is false.
  if (OB_SUCC(ret) && upd_ctdef.is_primary_index_) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    const uint64_t upd_table_id = upd_ctdef.das_base_ctdef_.index_tid_;
    ObExecContext *root_ctx = nullptr;
    if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
    } else if (OB_ISNULL(root_ctx)) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret)) {
      DASTableIdList* parent_table_set = &root_ctx->get_das_ctx().get_parent_table_set();
      // check table cycle even if upd_ctdef.need_check_table_cycle_ = false.
      // For example, create table A(a int primary key, a2 int, foreign key(a2) references A(a) on update cascade)
      // execute 'update A set a = a * 10;', it will first update A(a), then update A(a2)
      // When it updates A(a), upd_ctdef.need_check_table_cycle_ = true. 'A' is added to the parent table set.
      // When it updates A(a2) through the inner sql, upd_ctdef.need_check_table_cycle_ = false.
      // because there is no cycle if we only update A(a2).
      // but we need to check the cycle, because it may be from A(a).
      if (OB_FAIL(check_table_cycle(parent_table_set, upd_table_id, upd_rtdef.has_table_cycle_))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!upd_rtdef.has_table_cycle_ && upd_ctdef.need_check_table_cycle_) {
        if (OB_FAIL(parent_table_set->push_front(upd_table_id))) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
  }
  return ret;
}

int ObDMLService::init_ob_rowkey( ObIAllocator &allocator, const int64_t rowkey_cnt, ObRowkey &table_rowkey)
{
  int ret = OB_SUCCESS;
  ObObj *obj_ptr = nullptr;
  void *buf = nullptr;
  if (OB_ISNULL(buf = allocator.alloc(sizeof(ObObj) * rowkey_cnt))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    obj_ptr = new(buf) ObObj[rowkey_cnt];
  }
  table_rowkey.assign(obj_ptr, rowkey_cnt);
  return ret;
}

int ObDMLService::add_trans_info_datum(ObExpr *trans_info_expr,
                                       ObEvalCtx &eval_ctx,
                                       ObChunkDatumStore::StoredRow *stored_row)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;

  if (OB_ISNULL(trans_info_expr) || OB_ISNULL(stored_row)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(trans_info_expr->eval(eval_ctx, datum))) {
  } else if (OB_ISNULL(datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    char *buf = static_cast<char *>(stored_row->get_extra_payload());
    *static_cast<int32_t *>(stored_row->get_extra_payload()) = datum->len_;
    int64_t pos = sizeof(int32_t);
    MEMCPY(buf + pos, datum->ptr_, datum->len_);
  }

  return ret;
}

int ObDMLService::init_das_ins_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                const ObUpdCtDef &upd_ctdef,
                                                ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_INSERT,
                                                allocator,
                                                upd_rtdef.dins_rtdef_))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.dins_ctdef_,
                                        *upd_rtdef.dins_rtdef_,
                                        loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_ins_ctdefs_, upd_rtdef.related_ins_rtdefs_))) {
  } else {
    upd_rtdef.dins_rtdef_->related_ctdefs_ = &upd_ctdef.related_ins_ctdefs_;
    upd_rtdef.dins_rtdef_->related_rtdefs_ = &upd_rtdef.related_ins_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_das_del_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                const ObUpdCtDef &upd_ctdef,
                                                ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_DELETE,
                                                allocator,
                                                upd_rtdef.ddel_rtdef_))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.ddel_ctdef_,
                                        *upd_rtdef.ddel_rtdef_,
                                        loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_del_ctdefs_, upd_rtdef.related_del_rtdefs_))) {
  } else {
    upd_rtdef.ddel_rtdef_->related_ctdefs_ = &upd_ctdef.related_del_ctdefs_;
    upd_rtdef.ddel_rtdef_->related_rtdefs_ = &upd_rtdef.related_del_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_das_lock_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                 const ObUpdCtDef &upd_ctdef,
                                                 ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_LOCK,
                                                allocator,
                                                upd_rtdef.dlock_rtdef_))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.dlock_ctdef_,
                                        *upd_rtdef.dlock_rtdef_,
                                        loc_meta))) {
  }
  return ret;
}
int ObDMLService::init_lock_rtdef(ObDMLRtCtx &dml_rtctx,
                                  const ObLockCtDef &lock_ctdef,
                                  ObLockRtDef &lock_rtdef,
                                  int64_t wait_ts)
{
  lock_rtdef.das_rtdef_.for_upd_wait_time_ = wait_ts;
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(lock_ctdef.multi_ctdef_);
  return init_das_dml_rtdef(dml_rtctx, lock_ctdef.das_ctdef_, lock_rtdef.das_rtdef_, loc_meta);
}

int ObDMLService::check_agg_task_state(ObDMLRtCtx &dml_rtctx, ObIDASTaskOp *das_op, int64_t row_size, bool &reach_mem_limit)
{
  int ret = OB_SUCCESS;
  ObDasAggregatedTask *agg_task = nullptr;
  reach_mem_limit = false;
  int64_t simulate_buffer_size = - EVENT_CALL(EventTable::EN_DAS_SIMULATE_AGG_TASK_BUFF_LIMIT);
  int64_t buffer_size_limit = das::OB_DAS_TASK_BUFFER_SIZE;
  if (OB_UNLIKELY(simulate_buffer_size > 0)) {
    buffer_size_limit = simulate_buffer_size;
  }
  if (dml_rtctx.das_ref_.get_parallel_type() != DAS_SERIALIZATION) {
    if (OB_ISNULL(agg_task = das_op->get_agg_task())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(agg_task->add_mem_used(row_size))) {
      // do nothing
    } else if (agg_task->get_mem_used() >= buffer_size_limit) {
      agg_task->start_status_ = DAS_AGG_TASK_REACH_MEM_LIMIT;
      reach_mem_limit = true;
      LOG_TRACE("this agg_task buff is reach memory limit", K(agg_task->get_mem_used()), K(buffer_size_limit));
    }
  }
  return ret;
}

int ObDMLService::parallel_submit_das_task(ObDMLRtCtx &dml_rtctx, ObDasAggregatedTask *agg_task)
{
  int ret = OB_SUCCESS;
  if (dml_rtctx.das_ref_.get_parallel_type() == DAS_SERIALIZATION) {
    // not support parallel_submit
    LOG_TRACE("can't parallel submit", K(dml_rtctx.das_ref_.get_parallel_type()));
  } else if (dml_rtctx.das_ref_.get_parallel_type() == DAS_STREAMING_PARALLEL) {
    if (dml_rtctx.das_ref_.get_submitted_task_count() >= dml_rtctx.das_ref_.get_das_dop()) {
      LOG_INFO("submitted tasks reach dop", "submitted_cnt", dml_rtctx.das_ref_.get_submitted_task_count(),
          "das_dop", dml_rtctx.das_ref_.get_das_dop());
    } else if (OB_FAIL(dml_rtctx.das_ref_.parallel_submit_agg_task(agg_task))) {
    } else {
      dml_rtctx.add_das_parallel_task_size(agg_task->get_mem_used());
      dml_rtctx.das_ref_.get_das_parallel_ctx().add_submitted_task_count(1);
      LOG_TRACE("succeed parallel submit one agg_task", K(agg_task), K(agg_task->get_start_status()),
              K(dml_rtctx.get_das_task_memory_size()), K(dml_rtctx.get_das_parallel_task_size()));
    }
  } else if (dml_rtctx.das_ref_.get_parallel_type() == DAS_BLOCKING_PARALLEL) {
    // Blocking submission, temporarily not committing, but will wait until there are enough parallel task numbers to truly submit all at once
    if (dml_rtctx.das_ref_.get_submitted_task_count() >= dml_rtctx.das_ref_.get_das_dop()) {
      LOG_INFO("submitted tasks reach dop", "submitted_cnt", dml_rtctx.das_ref_.get_submitted_task_count(),
          "das_dop", dml_rtctx.das_ref_.get_das_dop());
    } else {
      dml_rtctx.add_das_parallel_task_size(agg_task->get_mem_used());
      dml_rtctx.das_ref_.get_das_parallel_ctx().add_submitted_task_count(1);
      LOG_TRACE("block parallel submit one agg_task", K(agg_task), K(agg_task->get_start_status()),
              K(dml_rtctx.get_das_task_memory_size()), K(dml_rtctx.get_das_parallel_task_size()));
    }
  }
  return ret;
}


template <int N>
int ObDMLService::write_row_to_das_op(const ObDASDMLBaseCtDef &ctdef,
                                      ObDASDMLBaseRtDef &rtdef,
                                      const ObDASTabletLoc *tablet_loc,
                                      ObDMLRtCtx &dml_rtctx,
                                      const ExprFixedArray &row,
                                      ObExpr *trans_info_expr,
                                      ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  bool need_retry = false;
  bool reach_agg_mem_limit = false;
  bool is_strict_defensive_check = trans_info_expr == nullptr ? false : true;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASCtDef CtDefType;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASRtDef RtDefType;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASOp OpType;
  OB_ASSERT(typeid(ctdef) == typeid(CtDefType));
  OB_ASSERT(typeid(rtdef) == typeid(RtDefType));
  int64_t extend_size = is_strict_defensive_check ?
      ObDASWriteBuffer::DAS_WITH_TRANS_INFO_EXTEND_SIZE : ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE;
  // 1. find das dml op
  OpType *dml_op = nullptr;
  if (OB_UNLIKELY(!dml_rtctx.das_ref_.has_das_op(tablet_loc, dml_op))) {
    if (OB_FAIL(dml_rtctx.das_ref_.prepare_das_task(tablet_loc, dml_op))) {
    } else if (OB_FAIL(dml_op->init_task_info(extend_size))) {
    } else {
      dml_op->set_das_ctdef(static_cast<const CtDefType*>(&ctdef));
      dml_op->set_das_rtdef(static_cast<RtDefType*>(&rtdef));
    }
    bool has_domain_index = ctdef.table_param_.get_data_table().is_domain_index();
    if (!has_domain_index && OB_NOT_NULL(rtdef.related_ctdefs_)) {
      for (int64_t i = 0; !has_domain_index && i < rtdef.related_ctdefs_->count(); ++i) {
        const ObDASDMLBaseCtDef *related_ctdef = rtdef.related_ctdefs_->at(i);
        has_domain_index = OB_NOT_NULL(related_ctdef)
            && related_ctdef->table_param_.get_data_table().is_domain_index();
      }
    }
    if (OB_SUCC(ret) && has_domain_index) {
      const common::ObLobReadOptions *lob_read_options = nullptr;
      if (OB_FAIL(dml_rtctx.get_exec_ctx().get_lob_read_options(lob_read_options))) {
      } else {
        dml_op->set_srs_provider(dml_rtctx.get_exec_ctx().get_srs_provider());
        dml_op->set_lob_read_options(lob_read_options);
      }
    }
    if (OB_SUCC(ret) &&
        rtdef.related_ctdefs_ != nullptr && !rtdef.related_ctdefs_->empty()) {
      if (OB_FAIL(add_related_index_info(*tablet_loc,
                                         *rtdef.related_ctdefs_,
                                         *rtdef.related_rtdefs_,
                                         *dml_op))) {
      }
    }
  }

  // 2. add row to das dml buffer
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_FAIL(dml_op->write_row(row, dml_rtctx.get_eval_ctx(), stored_row))) {
  } else if (OB_ISNULL(stored_row)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(dml_rtctx.add_das_task_memory_size(stored_row->row_size_))) {
    // do nothing
  } else if (OB_NOT_NULL(trans_info_expr) &&
      OB_FAIL(ObDMLService::add_trans_info_datum(trans_info_expr, dml_rtctx.get_eval_ctx(), stored_row))) {
  } else if (dml_rtctx.das_ref_.get_parallel_type() != DAS_SERIALIZATION) {
    // parallel submit
    if (OB_FAIL(check_agg_task_state(dml_rtctx, dml_op, stored_row->row_size_, reach_agg_mem_limit))) {
    } else if (!reach_agg_mem_limit) {
    } else if (OB_FAIL(parallel_submit_das_task(dml_rtctx, dml_op->get_agg_task()))) {
    } else {
      dml_rtctx.das_ref_.clear_task_map();
    }
  } else {
    // serialize submit
  }
  return ret;
}

int ObDMLService::add_related_index_info(const ObDASTabletLoc &tablet_loc,
                                         const DASDMLCtDefArray &related_ctdefs,
                                         DASDMLRtDefArray &related_rtdefs,
                                         ObIDASTaskOp &das_op)
{
  int ret = OB_SUCCESS;
  das_op.get_related_ctdefs().set_capacity(related_ctdefs.count());
  das_op.get_related_rtdefs().set_capacity(related_rtdefs.count());
  das_op.get_related_tablet_ids().set_capacity(related_ctdefs.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < related_ctdefs.count(); ++i) {
    ObDASTabletLoc *index_tablet_loc = ObDASUtils::get_related_tablet_loc(
        const_cast<ObDASTabletLoc&>(tablet_loc), related_ctdefs.at(i)->index_tid_);
    if (OB_ISNULL(index_tablet_loc)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(das_op.get_related_ctdefs().push_back(related_ctdefs.at(i)))) {
    } else if (OB_FAIL(das_op.get_related_rtdefs().push_back(related_rtdefs.at(i)))) {
    } else if (OB_FAIL(das_op.get_related_tablet_ids().push_back(index_tablet_loc->tablet_id_))) {
    }
  }
  return ret;
}


int ObDMLService::copy_heap_table_hidden_pk(ObEvalCtx &eval_ctx,
                                            const ObUpdCtDef &upd_ctdef)
{
  int ret = OB_SUCCESS;
  ObExpr *old_hidden_pk = upd_ctdef.old_row_.at(0);
  ObExpr *new_hidden_pk = upd_ctdef.new_row_.at(0);
  ObDatum *hidden_pk_datum = NULL;
  if (OB_ISNULL(old_hidden_pk) || OB_ISNULL(new_hidden_pk)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!upd_ctdef.is_table_without_pk_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (new_hidden_pk->type_ != T_TABLET_AUTOINC_NEXTVAL) {
  } else if (OB_FAIL(old_hidden_pk->eval(eval_ctx, hidden_pk_datum))) {
  } else if (OB_ISNULL(hidden_pk_datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!old_hidden_pk->obj_meta_.is_uint64()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObDatum &datum = new_hidden_pk->locate_datum_for_write(eval_ctx);
    datum.set_uint(hidden_pk_datum->get_uint());
    new_hidden_pk->get_eval_info(eval_ctx).evaluated_ = true;
  }
  return ret;
}

int ObDMLService::set_update_hidden_pk(ObEvalCtx &eval_ctx,
                                       const ObUpdCtDef &upd_ctdef,
                                       const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  if (upd_ctdef.is_table_without_pk_ && upd_ctdef.is_primary_index_) {
    ObExpr *auto_inc_expr = upd_ctdef.new_row_.at(0);
    ObSQLSessionInfo *my_session = eval_ctx.exec_ctx_.get_my_session();
    
    if (OB_FAIL(get_heap_table_hidden_pk(tablet_id, autoinc_seq))) {
    } else if (OB_ISNULL(auto_inc_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
      datum.set_uint(autoinc_seq);
      auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
    }
  }
  return ret;
}

int ObDMLService::get_heap_table_hidden_pk(const ObTabletID &tablet_id,
                                           uint64_t &pk)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  share::ObITabletAutoincrementService *auto_inc =
      ::oceanbase::share::server_service<::oceanbase::share::ObITabletAutoincrementService>();
  if (OB_ISNULL(auto_inc)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(auto_inc->next_value(tablet_id, autoinc_seq))) {
  } else {
    pk = autoinc_seq;
  }
  return ret;
}

int ObDMLService::set_heap_table_hidden_pk(const ObInsCtDef &ins_ctdef,
                                           const ObTabletID &tablet_id,
                                           ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  if (ins_ctdef.is_table_without_pk_ && ins_ctdef.is_primary_index_) {
    ObSQLSessionInfo *my_session = eval_ctx.exec_ctx_.get_my_session();
    
    if (OB_FAIL(ObDMLService::get_heap_table_hidden_pk(tablet_id,
                                                       autoinc_seq))) {
    } else {
      ObExpr *auto_inc_expr = ins_ctdef.new_row_.at(0);
      if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
        datum.set_uint(autoinc_seq);
        auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
      }
    }
  }
  return ret;
}

int ObDMLService::check_nested_sql_legality(ObExecContext &ctx, common::ObTableID ref_table_id)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (session->get_is_deserialized() && ctx.get_parent_ctx() != nullptr) {
    // Nested SQL in a distributed worker lacks transaction scheduler control.
    pl::ObPLContext *pl_ctx = ctx.get_parent_ctx()->get_pl_stack_ctx();
    //this nested sql require transaction scheduler control
    //but the session is remote, means this sql executing without transaction scheduler control
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "Perform a DML operation inside a query or remote/distributed sql");
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(ref_table_id, ctx))) {
    }
  }
  return ret;
}

int ObDMLService::create_anonymous_savepoint(ObTxDesc &tx_desc, transaction::ObTxSEQ &savepoint)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObSqlTransControl::create_anonymous_savepoint(tx_desc, savepoint))) {
  }
  return ret;
}

int ObDMLService::rollback_local_savepoint(ObTxDesc &tx_desc, const transaction::ObTxSEQ savepoint, int64_t expire_ts)
{
  int ret = OB_SUCCESS;
  data_plane::ObITransactionService *tx =
      data_plane::query_transaction_service();
  if (OB_ISNULL(tx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(tx->rollback_to_implicit_savepoint(
                 tx_desc, savepoint, expire_ts, false))) {
  }
  return ret;
}

int ObDMLService::check_local_index_affected_rows(int64_t table_affected_rows,
                                                  int64_t index_affected_rows,
                                                  const ObDASDMLBaseCtDef &ctdef,
                                                  ObDASDMLBaseRtDef &rtdef,
                                                  const ObDASDMLBaseCtDef &related_ctdef,
                                                  ObDASDMLBaseRtDef &related_rtdef)
{
  int ret = OB_SUCCESS;
  if ((config::_enable_defensive_check() > 0)) {
    if (table_affected_rows != index_affected_rows
        && !related_ctdef.table_param_.get_data_table().is_domain_index()) {
      ret = OB_ERR_DEFENSIVE_CHECK;
      ObString func_name = ObString::make_string("check_local_index_affected_rows");
      LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
      SQL_DAS_LOG(ERROR, "Fatal Error!!! data table affected row is not match with index table", K(ret),
                  "table_affected_rows", table_affected_rows,
                  "index_affected_rows", index_affected_rows,
                  "table_ctdef", ctdef,
                  "index_ctdef", related_ctdef,
                  "table_rtdef", rtdef,
                  "index_rtdef", related_rtdef);
    }
  }
  return ret;
}

template <typename T>
const ObDASTableLocMeta *ObDMLService::get_table_loc_meta(const T *multi_ctdef)
{
  const ObDASTableLocMeta *loc_meta = nullptr;
  if (multi_ctdef != nullptr) {
    loc_meta = &(multi_ctdef->loc_meta_);
  }
  return loc_meta;
}

int ObDMLService::init_related_das_rtdef(ObDMLRtCtx &dml_rtctx,
                                         const DASDMLCtDefArray &das_ctdefs,
                                         DASDMLRtDefArray &das_rtdefs)
{
  int ret = OB_SUCCESS;
  if (!das_ctdefs.empty()) {
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(das_rtdefs.allocate_array(allocator, das_ctdefs.count()))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < das_ctdefs.count(); ++i) {
    ObDASTaskFactory &das_factory = dml_rtctx.get_exec_ctx().get_das_ctx().get_das_factory();
    ObDASBaseRtDef *das_rtdef = nullptr;
    if (OB_FAIL(das_factory.create_das_rtdef(das_ctdefs.at(i)->op_type_, das_rtdef))) {
    } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                          *das_ctdefs.at(i),
                                          static_cast<ObDASDMLBaseRtDef&>(*das_rtdef),
                                          nullptr))) {
    } else {
      das_rtdefs.at(i) = static_cast<ObDASDMLBaseRtDef*>(das_rtdef);
    }
  }
  return ret;
}

int ObDMLService::check_dml_tablet_validity(ObDMLRtCtx &dml_rtctx,
                                            const ObDASTabletLoc &tablet_loc,
                                            const ExprFixedArray &row,
                                            const ObDMLBaseCtDef &dml_ctdef,
                                            ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if ((config::_enable_defensive_check() == 2)) {
    //only strict defensive check need to check tablet validity, otherwise ignore it
    ObTableLocation *tmp_location = nullptr;
    ObSEArray<ObTabletID, 1> tablet_ids;
    ObSEArray<ObObjectID, 1> partition_ids;
    uint64_t table_id = tablet_loc.loc_meta_->ref_table_id_;
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_ISNULL(dml_rtdef.check_location_)) {
      void *location_buf = allocator.alloc(sizeof(ObTableLocation));
      if (OB_ISNULL(location_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        dml_rtdef.check_location_ = new(location_buf) ObTableLocation(allocator);
        tmp_location = dml_rtdef.check_location_;
        ObSchemaGetterGuard *schema_guard = dml_rtctx.get_exec_ctx().get_sql_ctx()->schema_guard_;
        ObSQLSessionInfo *session = dml_rtctx.get_exec_ctx().get_my_session();
        ObSqlSchemaGuard sql_schema_guard;
        sql_schema_guard.set_schema_guard(schema_guard);
        // Here, judge the schema version at the check table level.
        // If the table-level schema_version is not equal, directly report an error schema_again
        if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(tmp_location->init_table_location_with_column_ids(
            sql_schema_guard, table_id, dml_ctdef.column_ids_, dml_rtctx.get_exec_ctx()))) {
        }
      }
    } else {
      tmp_location = dml_rtdef.check_location_;
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(convert_exprs_to_row(row,
                                       dml_rtctx.get_eval_ctx(),
                                       dml_rtdef,
                                       dml_rtctx.get_exec_ctx().get_allocator()))) {
      } else if (OB_FAIL(tmp_location->calculate_tablet_id_by_row(dml_rtctx.get_exec_ctx(),
                                                                  table_id,
                                                                  dml_ctdef.column_ids_,
                                                                  *dml_rtdef.check_row_,
                                                                  tablet_ids,
                                                                  partition_ids))) {
      } else if (tablet_ids.count() != 1 || tablet_loc.tablet_id_ != tablet_ids.at(0)) {
        ObSchemaGetterGuard *schema_guard = dml_rtctx.get_exec_ctx().get_sql_ctx()->schema_guard_;
        ObSQLSessionInfo *session = dml_rtctx.get_exec_ctx().get_my_session();
        const ObTableSchema *table_schema = nullptr;
        // Here, judge the schema version at the check table level.
        // If the table-level schema_version is not equal, directly report an error schema_again
        if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(schema_guard->get_table_schema( table_id, table_schema))) {
        } else if (OB_ISNULL(table_schema)) {
          ret = OB_SCHEMA_ERROR;
        } else if (table_schema->get_schema_version() != dml_ctdef.das_base_ctdef_.schema_version_) {
          ret = OB_SCHEMA_EAGAIN;
          LOG_WARN("table version mismatch", K(ret), K(table_id),
              K(table_schema->get_schema_version()), K(dml_ctdef.das_base_ctdef_.schema_version_));
        } else {
          ret = OB_ERR_DEFENSIVE_CHECK;
          ObString func_name = ObString::make_string("check_dml_tablet_validity");
          LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
          LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                    K(tablet_loc), K(tablet_ids),
                    KPC(dml_rtdef.check_row_), KPC(dml_rtdef.check_location_));
          LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret), K(table_schema->get_schema_version()), K(dml_ctdef.das_base_ctdef_.schema_version_),
                    KPC(tmp_location), KPC(table_schema));
        }
      }
    }
  }
  return ret;
}

int ObDMLService::convert_exprs_to_row(const ExprFixedArray &exprs,
                                       ObEvalCtx &eval_ctx,
                                       ObDMLBaseRtDef &dml_rtdef,
                                       ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dml_rtdef.check_row_)) {
    if (OB_FAIL(ob_create_row(allocator, exprs.count(), dml_rtdef.check_row_))) {
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    ObDatum *datum = nullptr;
    if (OB_FAIL(exprs.at(i)->eval(eval_ctx, datum))) {
    } else if (OB_FAIL(datum->to_obj(dml_rtdef.check_row_->cells_[i], exprs.at(i)->obj_meta_))) {
    }
  }
  return ret;
}

bool ObDMLService::has_nested_delete_ctx(const uint64_t table_id, DASDelCtxList &del_ctx_list)
{
  bool ret = false;
  DASDelCtxList::iterator iter = del_ctx_list.begin();
  for (; !ret && iter != del_ctx_list.end(); iter++) {
    DmlRowkeyDistCtx del_ctx = *iter;
    if (del_ctx.table_id_ == table_id) {
      ret = true;
    }
  }
  return ret;
}

int ObDMLService::get_nested_delete_ctx(const uint64_t table_id,
                                        DASDelCtxList &del_ctx_list,
                                        SeRowkeyDistCtx *&rowkey_dist_ctx)
{
  int ret = OB_SUCCESS;
  bool find = false;
  DASDelCtxList::iterator iter = del_ctx_list.begin();
  for (; !find && iter != del_ctx_list.end(); iter++) {
    DmlRowkeyDistCtx del_ctx = *iter;
    if (del_ctx.table_id_ == table_id) {
      find = true;
      rowkey_dist_ctx = del_ctx.deleted_rows_;
    }
  }
  return ret;
}

int ObDMLService::handle_after_processing_multi_row(ObDMLModifyRowsList *dml_modify_rows, ObTableModifyOp *dml_op)
{
  int ret = OB_SUCCESS;
  bool is_iter_end = true;
  const ObDmlEventType t_insert = ObDmlEventType::DE_INSERTING;
  const ObDmlEventType t_update = ObDmlEventType::DE_UPDATING;
  const ObDmlEventType t_delete = ObDmlEventType::DE_DELETING;
  if (OB_ISNULL(dml_modify_rows) || OB_ISNULL(dml_op) || OB_ISNULL(dml_op->get_child())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(is_iter_end = dml_op->iter_end_)) {
  } else if (!is_iter_end && OB_ISNULL(dml_op->last_store_row_.get_store_row()) &&
    OB_FAIL(dml_op->last_store_row_.init(dml_op->get_exec_ctx().get_allocator(), dml_op->get_child()->get_spec().output_.count()))) {
  } else if (!is_iter_end && OB_FAIL(dml_op->last_store_row_.shadow_copy(dml_op->get_child()->get_spec().output_, dml_op->get_eval_ctx()))) {
    LOG_ERROR("failed to backup the datum ptr of child operator", K(ret));
  } else {
    ObDMLModifyRowsList::iterator row_iter = dml_modify_rows->begin();
    for (; OB_SUCC(ret) && row_iter != dml_modify_rows->end(); row_iter++) {
      ObDMLModifyRowNode &modify_row = *row_iter;
      if (OB_ISNULL(modify_row.full_row_) && OB_ISNULL(modify_row.new_row_) && OB_ISNULL(modify_row.old_row_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(modify_row.dml_op_) || OB_ISNULL(modify_row.dml_ctdef_) || OB_ISNULL(modify_row.dml_rtdef_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObTableModifyOp &op = *modify_row.dml_op_;
        const ObDMLBaseCtDef &dml_ctdef = *modify_row.dml_ctdef_;
        ObDMLBaseRtDef &dml_rtdef = *modify_row.dml_rtdef_;
        const ObDmlEventType dml_event = modify_row.dml_event_;
        // process foreign key
        if (OB_NOT_NULL(modify_row.full_row_) && OB_FAIL(modify_row.full_row_->to_expr(modify_row.dml_ctdef_->full_row_, op.get_eval_ctx()))) {
        } else if (OB_NOT_NULL(modify_row.old_row_) && OB_FAIL(modify_row.old_row_->to_expr(dml_ctdef.old_row_, op.get_eval_ctx()))) {
        } else if (OB_NOT_NULL(modify_row.new_row_) && OB_FAIL(modify_row.new_row_->to_expr(dml_ctdef.new_row_, op.get_eval_ctx()))) {
        } else if (op.need_foreign_key_checks()) {
          if (t_update == dml_event && !reinterpret_cast<ObUpdRtDef &>(dml_rtdef).is_row_changed_) {
          } else {
            // if check foreign key in batch, build fk check tasks here
            if (dml_op->get_spec().check_fk_batch_) {
              if (OB_FAIL(build_batch_fk_check_tasks(dml_ctdef, dml_rtdef))) {
              }
            } else if (OB_FAIL(ForeignKeyHandle::do_handle(op, dml_ctdef, dml_rtdef))) {
            }
          }
        }
        // process after row trigger
        if (OB_SUCC(ret) && dml_ctdef.trig_ctdef_.all_tm_points_.has_after_row()) {
          ObEvalCtx &eval_ctx = op.get_eval_ctx();
          if (dml_event != t_insert && dml_event != t_update && dml_event != t_delete) {
            ret = OB_ERR_UNEXPECTED;
          } else if (t_insert == dml_event && OB_FAIL(TriggerHandle::init_param_new_row(
              eval_ctx, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_))) {
          } else if (t_delete == dml_event && OB_FAIL(TriggerHandle::init_param_old_row(
              eval_ctx, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_))) {
          } else if (t_update == dml_event && OB_FAIL(TriggerHandle::init_param_rows(
              eval_ctx, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_))) {
          } else if (OB_FAIL(TriggerHandle::do_handle_after_row(op, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_, dml_event))) {
          }
        }
      }
    }

    // check the result of batch foreign key check results
    if (OB_SUCC(ret)) {
      if (dml_op->get_spec().check_fk_batch_ && OB_FAIL(dml_op->perform_batch_fk_check())) {
      } else if (OB_FAIL(!is_iter_end && dml_op->last_store_row_.restore(dml_op->get_child()->get_spec().output_, dml_op->get_eval_ctx()))) {
      }
    }
  }
  return ret;
}

int ObDMLService::handle_after_processing_single_row(ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(modify_row.full_row_) && OB_ISNULL(modify_row.new_row_) && OB_ISNULL(modify_row.old_row_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(modify_row.dml_op_) || OB_ISNULL(modify_row.dml_ctdef_) || OB_ISNULL(modify_row.dml_rtdef_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObTableModifyOp &op = *modify_row.dml_op_;
    const ObDMLBaseCtDef &dml_ctdef = *modify_row.dml_ctdef_;
    ObDMLBaseRtDef &dml_rtdef = *modify_row.dml_rtdef_;
    const ObDmlEventType t_insert = ObDmlEventType::DE_INSERTING;
    const ObDmlEventType t_update = ObDmlEventType::DE_UPDATING;
    const ObDmlEventType t_delete = ObDmlEventType::DE_DELETING;
    const ObDmlEventType dml_event = modify_row.dml_event_;
    if (dml_event != t_insert && dml_event != t_update && dml_event != t_delete) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      if (t_update == dml_event) {
        // for update op, Foreign key checks need to be performed only if the value has changed
        if (reinterpret_cast<ObUpdRtDef &>(dml_rtdef).is_row_changed_ && OB_FAIL(ForeignKeyHandle::do_handle(op, dml_ctdef, dml_rtdef))) {
        }
      } else if (OB_FAIL(ForeignKeyHandle::do_handle(op, dml_ctdef, dml_rtdef))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(TriggerHandle::do_handle_after_row(op, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_, dml_event))) {
    }
  }
  return ret;
}

int ObDMLService::handle_after_processing_single_row(ObDMLModifyRowsList *dml_modify_rows)
{
  int ret = OB_SUCCESS;
  // for single-row processing, the expr defined in ctdef and trig parameters haven't been refreshed
  // case1: only one row added to the list of dml_modify_rows;
  // case2: for multi table dml stmt, each table adds a row to the list;
  // In these cases, there is no need to re-convert the rows to be modified into an expression and init trig parameters;
  ObDMLModifyRowsList::iterator row_iter = dml_modify_rows->begin();
  for (; OB_SUCC(ret) && row_iter != dml_modify_rows->end(); row_iter++) {
    ObDMLModifyRowNode &modify_row = *row_iter;
    if (OB_FAIL(handle_after_processing_single_row(modify_row))) {
    }
  }
  return ret;
}

int ObDMLService::handle_after_row_processing(ObTableModifyOp *op,
                                              ObDMLModifyRowsList *dml_modify_rows)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(op) || OB_ISNULL(dml_modify_rows)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (1 > dml_modify_rows->size()) {
    // after row processing list is empty, nothing to do
    #ifndef NDEBUG
      LOG_INFO("No row need to perform foreign key check or after row trigger");
    #endif
  } else if (op->execute_single_row_) {
    ret = handle_after_processing_single_row(dml_modify_rows);
  } else {
    ret = handle_after_processing_multi_row(dml_modify_rows, op);
  }
  return ret;
}

int ObDMLService::build_batch_fk_check_tasks(const ObDMLBaseCtDef &dml_ctdef,
                                             ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (dml_rtdef.fk_checker_array_.count() != dml_ctdef.fk_args_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int i = 0; OB_SUCC(ret) && i < dml_rtdef.fk_checker_array_.count(); ++i) {
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      bool need_check = true;
      if (OB_ISNULL(fk_checker)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!fk_arg.use_das_scan_) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(fk_checker->build_fk_check_das_task(fk_arg.columns_, dml_ctdef.new_row_, need_check))) {
      }
    }
  }
  return ret;
}

int ObDMLService::log_user_error_inner(
    int ret,
    int64_t row_num,
    common::ObString &column_name,
    ObExecContext &ctx,
    const ObObjType column_type)
{
  if (OB_DATA_OUT_OF_RANGE == ret) {
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_DATA_OUT_OF_RANGE, column_name.length(), column_name.ptr(), row_num);
  } else if (OB_ERR_DATA_TOO_LONG == ret) {
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_DATA_TOO_LONG, column_name.length(), column_name.ptr(), row_num);
  } else if (OB_ERR_VALUE_LARGER_THAN_ALLOWED == ret) {
    LOG_USER_ERROR(OB_ERR_VALUE_LARGER_THAN_ALLOWED);
  } else if (OB_INVALID_DATE_VALUE == ret || OB_INVALID_DATE_FORMAT == ret) {
    ret = OB_INVALID_DATE_VALUE;
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(
        OB_ERR_INVALID_DATE_MSG_FMT_V2,
        column_name.length(),
        column_name.ptr(),
        row_num);
  } else if (OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD == ret || OB_INVALID_NUMERIC == ret) {
    ret = OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD;
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    ObString decimal_type_str("decimal");
    ObString default_type_str("integer");
    ObString &type_str = ob_is_number_or_decimal_int_tc(column_type) ?
      decimal_type_str : default_type_str;
    LOG_USER_ERROR(
        OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD,
        type_str.length(),
        type_str.ptr(),
        column_name.length(),
        column_name.ptr(),
        row_num);
  } else if (OB_ERR_WARN_DATA_OUT_OF_RANGE == ret) {
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_WARN_DATA_OUT_OF_RANGE, column_name.length(), column_name.ptr(), row_num);
  } else if (OB_ERR_DATA_TRUNCATED == ret || OB_ERR_DOUBLE_TRUNCATED == ret) {
    ret = OB_ERR_DATA_TRUNCATED;
    ob_reset_tsi_warning_buffer();
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_DATA_TRUNCATED, column_name.length(), column_name.ptr(), row_num);
  } else {
  }
  return ret;
}
// Find the first ancestor exec context that initiates fk cascading.
// The exec_ctx can be used to create hash set to perform duplicate rowkey check,
// which is used to avoid delete or update same row mutiple times
int ObDMLService::get_root_exec_ctx_for_fk_cascading(ObExecContext *ctx, ObExecContext* &needed_ctx)
{
  int ret = OB_SUCCESS;
  ObExecContext *parent_ctx = nullptr;
  needed_ctx = nullptr;
  if (OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(parent_ctx = ctx->get_parent_ctx())) {
    // case1: current ctx is the root ctx, means current stmt is the root stmt,
    // create hash set at current ctx to perform duplicate rowkey check
    needed_ctx = ctx;
  } else if (!(parent_ctx->get_das_ctx().is_fk_cascading_)) {
    // case2: current stmt is not the root stmt, is not triggered by foreign key cascade operations
    // may be trigger or PL instead, create hash set at current ctx to perform duplicate rowkey check
    needed_ctx = ctx;
  } else if (OB_FAIL(SMART_CALL(get_root_exec_ctx_for_fk_cascading(parent_ctx, needed_ctx)))) {
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
