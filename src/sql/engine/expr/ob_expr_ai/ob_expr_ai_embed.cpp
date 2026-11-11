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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // the null content is propagated as the null result,
    // it is not sent to the model service
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_,
                                                          temp_allocator,
                                                          *arg_content,
                                                          expr.args_[CONTENT_IDX]->datum_meta_,
                                                          expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(),
                                                          content))) {
      LOG_WARN("fail to read real string data of the content", K(ret));
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_batch_ai_embed(BATCH_EVAL_FUNC_ARG_DECL)
{
  INIT_SUCC(ret);
  ObDatumVector model_id_datum;
  ObDatumVector content_datum;
  ObDatumVector dim_datum;
  const bool has_dim = (3 == expr.arg_cnt_);
  // the constant arguments are evaluated only once, so their values are read from the first row
  const bool model_id_is_batch = expr.args_[MODEL_IDX]->is_batch_result();
  const bool content_is_batch = expr.args_[CONTENT_IDX]->is_batch_result();
  const bool dim_is_batch = has_dim ? expr.args_[DIM_IDX]->is_batch_result() : false;
  if (has_dim) {
    if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_id_datum, content_datum, dim_datum))) {
    }
  } else if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_id_datum, content_datum))) {
  }
  if (OB_SUCC(ret)) {
    ObDatum *res_datum = expr.locate_batch_datums(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObIAllocator &allocator = temp_allocator;
    // the rows are grouped by the model id and the dimension, so the rows of one group can be
    // merged into one request to the model service
    const int64_t max_group_count = 8;
    int64_t group_count = 0;
    ObString group_model_id[max_group_count];
    int64_t group_dim[max_group_count];
    int64_t group_row_begin[max_group_count];
    int64_t group_row_cnt[max_group_count] = {0};
    int64_t group_row_pos[max_group_count] = {0};
    int64_t *group_of_row = nullptr;
    int64_t *group_rows = nullptr;
    if (size > 0 && OB_ISNULL(group_of_row = static_cast<int64_t *>(allocator.alloc(sizeof(int64_t) * size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (size > 0 && OB_ISNULL(group_rows = static_cast<int64_t *>(allocator.alloc(sizeof(int64_t) * size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      // the rows which are skipped, null or invalid are marked with -1, they are not sent to
      // the model service and they do not take part in the mapping of the results
      for (int64_t i = 0; i < size; ++i) {
        group_of_row[i] = -1;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        if (skip.contain(i) || eval_flags.at(i)) {
          // the skipped rows and the rows which have been evaluated are not evaluated again
          continue;
        }
        const ObDatum *content = content_datum.at(content_is_batch ? i : 0);
        const ObDatum *model_id = model_id_datum.at(model_id_is_batch ? i : 0);
        if (OB_ISNULL(content) || OB_ISNULL(model_id)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (content->is_null()) {
          // the null input is not sent to the model service, and the result of the row is null
          res_datum[i].set_null();
          eval_flags.set(i);
          expr.get_eval_info(ctx).notnull_ = false;
        } else if (model_id->is_null()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
        } else {
          ObString row_model_id = model_id->get_string();
          int64_t row_dim = 0;
          if (has_dim) {
            const ObDatum *dim = dim_datum.at(dim_is_batch ? i : 0);
            if (OB_ISNULL(dim) || dim->is_null()) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
            } else {
              row_dim = dim->get_int();
              if (row_dim <= 0) {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
              }
            }
          }
          if (OB_SUCC(ret) && row_model_id.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          }
          if (OB_SUCC(ret)) {
            int64_t group_idx = -1;
            for (int64_t j = 0; j < group_count; ++j) {
              if (group_model_id[j] == row_model_id && group_dim[j] == row_dim) {
                group_idx = j;
                break;
              }
            }
            if (group_idx < 0) {
              if (group_count >= max_group_count) {
                ret = OB_ERR_UNEXPECTED;
                LOG_USER_ERROR(OB_ERR_UNEXPECTED, "ai_embed, too many models in one batch");
              } else {
                group_idx = group_count++;
                group_model_id[group_idx] = row_model_id;
                group_dim[group_idx] = row_dim;
              }
            }
            if (OB_SUCC(ret)) {
              group_of_row[i] = group_idx;
              group_row_cnt[group_idx]++;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      // the rows of one group are collected together, the order of the rows in the batch is kept
      int64_t row_pos = 0;
      for (int64_t g = 0; g < group_count; ++g) {
        group_row_begin[g] = row_pos;
        row_pos += group_row_cnt[g];
      }
      for (int64_t i = 0; i < size; ++i) {
        const int64_t group_idx = group_of_row[i];
        if (group_idx >= 0) {
          group_rows[group_row_begin[group_idx] + group_row_pos[group_idx]++] = i;
        }
      }
    }
    // send one merged request for every group, and map the results back to the rows
    for (int64_t g = 0; OB_SUCC(ret) && g < group_count; ++g) {
      ObArray<ObString> contents;
      ObArray<ObString> results;
      ObJsonObject *config = nullptr;
      ObJsonInt *dim_json = nullptr;
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      const int64_t dim = group_dim[g];
      if (dim > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < group_row_cnt[g]; ++j) {
        const int64_t row_idx = group_rows[group_row_begin[g] + j];
        const ObDatum *content = content_datum.at(content_is_batch ? row_idx : 0);
        if (OB_ISNULL(content) || content->is_null()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ObString content_str;
          if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_,
                                                                allocator,
                                                                *content,
                                                                expr.args_[CONTENT_IDX]->datum_meta_,
                                                                expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(),
                                                                content_str))) {
            LOG_WARN("fail to read real string data of the row", K(ret), K(row_idx));
          } else if (content_str.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          } else if (OB_FAIL(contents.push_back(content_str))) {
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, group_model_id[g], info))) {
        } else if (OB_ISNULL(endpoint_resolver)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(group_model_id[g], allocator, resolved_endpoint))) {
        } else {
          ObAIFuncModel model(allocator, *info, *endpoint_info);
          if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
          } else if (results.count() != group_row_cnt[g]) {
            ret = OB_ERR_UNEXPECTED;
            LOG_USER_ERROR(OB_ERR_UNEXPECTED, "ai_embed, the number of the results is not equal to the number of the inputs");
          } else {
            for (int64_t j = 0; OB_SUCC(ret) && j < group_row_cnt[g]; ++j) {
              const int64_t row_idx = group_rows[group_row_begin[g] + j];
              ObString &result = results[j];
              // the result of the row is set in the same way as the single row evaluation
              if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res_datum[row_idx], result))) {
                LOG_WARN("fail to set the string result of the row", K(ret), K(row_idx));
              } else {
                eval_flags.set(row_idx);
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    // the batch evaluation merges the rows into one request to the model service,
    // it is only an optimization, so the single row evaluation is still needed
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_batch_ai_embed;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
