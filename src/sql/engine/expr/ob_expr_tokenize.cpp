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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"
#include "lib/utility/serialization.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace
{

// Default parser properties for the built-in parsers.  They only depend on the
// parser type, so they are resolved once instead of being serialized to a JSON
// document and parsed back on every TOKENIZE evaluation.
struct ObFTDefaultParserProperties
{
  storage::ObFTParserProperty ik_;
  storage::ObFTParserProperty beng_;
  storage::ObFTParserProperty space_;
  storage::ObFTParserProperty ngram_;
  storage::ObFTParserProperty ngram2_;
};

const ObFTDefaultParserProperties &get_default_parser_properties()
{
  static const ObFTDefaultParserProperties props = []() {
    ObFTDefaultParserProperties result;
    // IK keeps its dictionary tables and defaults to the smart segmentation mode.
    result.ik_.dict_table_ = ObString(storage::ObFTSLiteral::CONFIG_NAME_DICT_TABLE);
    result.ik_.stopword_table_ = ObString(storage::ObFTSLiteral::CONFIG_NAME_STOPWORD_TABLE);
    result.ik_.quantifier_table_ =
        ObString(storage::ObFTSLiteral::CONFIG_NAME_QUANTIFIER_TABLE);
    return result;
  }();
  return props;
}

int get_default_parser_property(const storage::ObFTParser &parser,
                                const storage::ObFTParserProperty *&property)
{
  int ret = OB_SUCCESS;
  const ObFTDefaultParserProperties &props = get_default_parser_properties();
  property = nullptr;
  if (parser.is_ik()) {
    property = &props.ik_;
  } else if (parser.is_beng()) {
    property = &props.beng_;
  } else if (parser.is_space()) {
    property = &props.space_;
  } else if (parser.is_ngram()) {
    property = &props.ngram_;
  } else if (parser.is_ngram2()) {
    property = &props.ngram2_;
  } else {
    ret = OB_FUNCTION_NOT_DEFINED;
  }
  return ret;
}

int init_parse_helper_with_default_property(storage::ObFTParseHelper &helper,
                                            ObIAllocator &allocator,
                                            const ObString &parser_name)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;
  const storage::ObFTParserProperty *property = nullptr;
  if (OB_FAIL(parser.parse_from_str(parser_name.ptr(), parser_name.length()))) {
  } else if (OB_FAIL(get_default_parser_property(parser, property))) {
  } else if (OB_ISNULL(property)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(helper.init(&allocator, parser_name, *property))) {
  }
  return ret;
}

// Encode the DEFAULT-mode TOKENIZE result (a JSON array of token strings)
// straight into its binary JSON representation.  The output is byte-identical
// to what ObFTParseHelper::make_token_array_json + ObJsonBinSerializer produce
// (document header v0, array node header, var-width count/size, value entries
// and string payloads), but it skips materializing the ObJsonTree nodes and
// re-walking them during serialization.
static int make_token_array_bin(const storage::ObFTWordMap &words,
                                common::ObIAllocator &allocator,
                                common::ObString &result)
{
  using namespace common;
  int ret = OB_SUCCESS;
  const uint64_t cnt = words.size();

  // Pass 1: per-string serialize size (ObJsonString::get_serialize_size =
  // vertype byte + vi64 length + payload).
  uint64_t node_total_size = 0;
  uint64_t last_node_size = 0;
  for (storage::ObFTWordMap::const_iterator it = words.begin(); it != words.end(); ++it) {
    const int64_t len = it->first.get_word().get_string().length();
    last_node_size = sizeof(uint8_t) + serialization::encoded_length_vi64(len) + len;
    node_total_size += last_node_size;
  }

  // Estimated node size, replicating ObJsonArray::update_serialize_size():
  // it decides the obj-size/entry var-width recorded in the array header.
  const uint8_t count_var_type = ObJsonVar::get_var_type(cnt);
  const uint64_t count_size = ObJsonVar::get_var_size(count_var_type);
  const uint64_t est_total = OB_JSON_BIN_OBJ_HEADER_LEN + count_size + sizeof(uint32_t)
      + (sizeof(uint32_t) + sizeof(uint8_t)) * cnt + node_total_size;
  const uint64_t last_offset = est_total - last_node_size;
  const uint8_t off_var_type = ObJsonVar::get_var_type(last_offset);
  const uint64_t total_off = (ObJsonVar::get_var_size(off_var_type) + sizeof(uint8_t)) * cnt;
  const uint64_t est_total2 = OB_JSON_BIN_OBJ_HEADER_LEN + count_size + sizeof(uint32_t)
      + total_off + node_total_size;
  const uint8_t size_var_type = ObJsonVar::get_var_type(est_total2);
  const uint64_t obj_size_est = OB_JSON_BIN_OBJ_HEADER_LEN + count_size
      + ObJsonVar::get_var_size(size_var_type) + total_off + node_total_size;

  // Resolve the header var-widths to a fixed point: widen them if the real
  // size ever needs more bits than the estimate reserved (mirrors the
  // reserialize step of the tree path).
  uint8_t obj_size_var_type = ObJsonVar::get_var_type(obj_size_est);
  uint64_t entries_off = 0;
  uint64_t data_off = 0;
  uint64_t obj_size_real = 0;
  bool resolved = false;
  for (int round = 0; OB_SUCC(ret) && !resolved && round < 5; ++round) {
    const uint64_t entry_size = ObJsonVar::get_var_size(obj_size_var_type);
    entries_off = OB_JSON_BIN_OBJ_HEADER_LEN + count_size
        + ObJsonVar::get_var_size(obj_size_var_type);
    data_off = entries_off + cnt * (entry_size + OB_JSON_BIN_VALUE_TYPE_LEN);
    obj_size_real = data_off + node_total_size;
    if (ObJsonVar::get_var_type(obj_size_real) <= obj_size_var_type) {
      resolved = true;
    } else {
      obj_size_var_type = ObJsonVar::get_var_type(obj_size_real);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(!resolved)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tokenize bin encode cannot settle entry var type",
             K(cnt), K(obj_size_est), K(obj_size_real));
  } else {
    const uint64_t entry_size = ObJsonVar::get_var_size(obj_size_var_type);
    const int64_t node_start = static_cast<int64_t>(sizeof(ObJsonBinDocHeader));
    const int64_t total_len = node_start + static_cast<int64_t>(obj_size_real);
    char *buf = static_cast<char *>(allocator.alloc(total_len));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(buf, 0, total_len);
      // document header (v0): extend_seg_offset is back-filled with the total
      // length, exactly like ObJsonBinSerializer::serialize does at the end.
      ObJsonBinDocHeader *doc = reinterpret_cast<ObJsonBinDocHeader *>(buf);
      doc->type_ = J_DOC_HEADER_V0;
      doc->use_lexicographical_order_ = 0;
      doc->reserved_ = 0;
      doc->extend_seg_offset_ = static_cast<uint64_t>(total_len);
      // array node header
      ObJsonBinHeader *hdr = reinterpret_cast<ObJsonBinHeader *>(buf + node_start);
      hdr->type_ = J_ARRAY_V0;
      hdr->entry_size_ = obj_size_var_type;
      hdr->count_size_ = count_var_type;
      hdr->obj_size_size_ = obj_size_var_type;
      hdr->is_continuous_ = 1;
      hdr->reserved_ = 0;
      // element count and object size
      ObJsonVar::set_var(cnt, count_var_type, buf + node_start + OB_JSON_BIN_OBJ_HEADER_LEN);
      ObJsonVar::set_var(obj_size_real, obj_size_var_type,
                         buf + node_start + OB_JSON_BIN_OBJ_HEADER_LEN + count_size);
      // value entries followed by the string payloads
      uint64_t cur = data_off;
      uint64_t idx = 0;
      for (storage::ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end();
           ++it, ++idx) {
        const ObString &word = it->first.get_word().get_string();
        const int64_t entry_pos = node_start
            + static_cast<int64_t>(entries_off) + static_cast<int64_t>(idx) * (entry_size + 1);
        ObJsonVar::set_var(cur, obj_size_var_type, buf + entry_pos);
        *(reinterpret_cast<uint8_t *>(buf + entry_pos + entry_size)) = J_STRING_V0;
        *(reinterpret_cast<uint8_t *>(buf + node_start + cur)) = J_STRING_V0;
        int64_t pos = node_start + static_cast<int64_t>(cur) + 1;
        if (OB_FAIL(serialization::encode_vi64(buf, total_len, pos, word.length()))) {
        } else {
          MEMCPY(buf + pos, word.ptr(), word.length());
          pos += word.length();
          cur += static_cast<uint64_t>(pos - (node_start + static_cast<int64_t>(cur)));
        }
      }
      if (OB_SUCC(ret)) {
        result.assign_ptr(buf, static_cast<int32_t>(total_len));
      }
    }
  }
  return ret;
}

}  // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (TokenizeParam::OUTPUT_MODE::DEFAULT == param.output_mode_) {
    // Fast path: the default output is a plain array of token strings, so the
    // binary JSON result is encoded directly from the word map, skipping the
    // ObJsonTree build and its re-serialization.
    storage::ObFTParseHelper tokenize_helper;
    const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
    int64_t doc_len = 0;
    storage::ObFTWordMap token_map;
    if (OB_FAIL(param.properties_.empty()
                    ? init_parse_helper_with_default_property(tokenize_helper,
                                                              temp_allocator,
                                                              param.parser_name_)
                    : tokenize_helper.init(&temp_allocator,
                                           param.parser_name_,
                                           param.properties_))) {
    } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
    } else if ((0 != param.fulltext_.length())
               && OB_FAIL(tokenize_helper.segment(param.meta_,
                                                  param.fulltext_.ptr(),
                                                  param.fulltext_.length(),
                                                  doc_len,
                                                  token_map))) {
    } else {
      common::ObString bin_str;
      if (OB_FAIL(make_token_array_bin(token_map, temp_allocator, bin_str))) {
      } else {
        if (OB_SUCC(ret)
            && OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, bin_str))) {
        }
      }
    }
  } else {
    ObIJsonBase *json_result = nullptr;
    if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                       ctx,
                                                       temp_allocator,
                                                       json_result,
                                                       expr_datum))) {
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(param.properties_.empty()
                         ? init_parse_helper_with_default_property(tokenize_helper,
                                                                   allocator,
                                                                   param.parser_name_)
                         : tokenize_helper.init(&allocator,
                                                param.parser_name_,
                                                param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (!param.properties_.empty()
             && OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  // A dictionary is loaded once per server process.  Cache the loader of this
  // thread so that repeated TOKENIZE calls skip the global loader lookup as
  // soon as the dictionary has been loaded.
  static thread_local storage::ObDicLoader *cached_loader = nullptr;
  static thread_local ObCharsetType cached_charset = CHARSET_INVALID;
  const ObCharsetType charset = ObCharset::charset_type_by_coll(meta_.get_collation_type());
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic
             && !(nullptr != cached_loader && cached_charset == charset
                  && cached_loader->is_loaded())) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    charset,
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    } else {
      cached_loader = dic_loader_handle.get_loader();
      cached_charset = charset;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
