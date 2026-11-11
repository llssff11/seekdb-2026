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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

// DESCRIBE of a file table (`describe select * from read_csv(...)`) reports the
// inferred schema instead of an execution plan, which is the DuckDB semantic.
// The metadata rows are produced as a plain UNION ALL query so that all of the
// existing resolve/optimize/execute machinery can be reused unchanged.
static bool parse_tree_has_file_table(const ParseNode *node)
{
  bool found = false;
  if (NULL != node) {
    if (T_FILE_TABLE_EXPRESSION == node->type_) {
      found = true;
    } else {
      for (int64_t i = 0; !found && i < node->num_child_; ++i) {
        found = parse_tree_has_file_table(node->children_[i]);
      }
    }
  }
  return found;
}

static const char *file_describe_type_name(const int32_t file_type)
{
  const char *name = "VARCHAR";
  switch (file_type) {
    case OB_FILE_TYPE_BIGINT: {
      name = "BIGINT";
      break;
    }
    case OB_FILE_TYPE_BOOLEAN: {
      name = "BOOLEAN";
      break;
    }
    case OB_FILE_TYPE_DOUBLE: {
      name = "DOUBLE";
      break;
    }
    case OB_FILE_TYPE_DATE: {
      name = "DATE";
      break;
    }
    case OB_FILE_TYPE_DATETIME: {
      name = "DATETIME";
      break;
    }
    default: {
      name = "VARCHAR";
      break;
    }
  }
  return name;
}

static int append_sql_string_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  char escaped[2];
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c || '\\' == c) {
      escaped[0] = '\\';
      escaped[1] = c;
      ret = sql.append(escaped, 2);
    } else {
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

static int append_file_describe_row(ObSqlString &sql,
                                    const ObString &name,
                                    const char *type_name,
                                    const bool nullable,
                                    const int64_t row_idx)
{
  int ret = OB_SUCCESS;
  const char *branch_head = (0 == row_idx) ? "select " : " union all select ";
  const char *nullable_str = nullable ? "YES" : "NO";
  if (OB_FAIL(sql.append(branch_head))) {
  } else if (OB_FAIL(append_sql_string_literal(sql, name))) {
  } else if (0 == row_idx && OB_FAIL(sql.append(" as `column_name`"))) {
  } else if (OB_FAIL(sql.append(",'"))) {
  } else if (OB_FAIL(sql.append(type_name))) {
  } else if (OB_FAIL(sql.append("'"))) {
  } else if (0 == row_idx && OB_FAIL(sql.append(" as `column_type`"))) {
  } else if (OB_FAIL(sql.append(",'"))) {
  } else if (OB_FAIL(sql.append(nullable_str))) {
  } else if (OB_FAIL(sql.append("'"))) {
  } else if (0 == row_idx && OB_FAIL(sql.append(" as `null`"))) {
  } else if (OB_FAIL(sql.append(",NULL"))) {
  } else if (0 == row_idx && OB_FAIL(sql.append(" as `key`"))) {
  } else if (OB_FAIL(sql.append(",NULL"))) {
  } else if (0 == row_idx && OB_FAIL(sql.append(" as `default`"))) {
  } else if (OB_FAIL(sql.append(",NULL"))) {
  } else if (0 == row_idx && OB_FAIL(sql.append(" as `extra`"))) {
  }
  return ret;
}

static int build_file_describe_sql(const ObFileTableDef &def, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (def.col_names_.count() <= 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("file table has no column to describe", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "describe file table without column");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < def.col_names_.count(); ++i) {
    const int32_t type = i < def.col_types_.count() ? def.col_types_.at(i) : OB_FILE_TYPE_VARCHAR;
    const bool nullable = i < def.col_nullables_.count() ? (0 != def.col_nullables_.at(i)) : true;
    if (OB_FAIL(append_file_describe_row(sql, def.col_names_.at(i),
                                         file_describe_type_name(type), nullable, i))) {
    }
  }
  return ret;
}

static const ObFileTableDef *find_file_table_def(const ObDMLStmt &stmt)
{
  const ObFileTableDef *def = NULL;
  const ObIArray<TableItem*> &table_items = stmt.get_table_items();
  for (int64_t i = 0; NULL == def && i < table_items.count(); ++i) {
    const TableItem *table_item = table_items.at(i);
    if (NULL != table_item && table_item->is_file_table() && NULL != table_item->file_table_def_) {
      def = table_item->file_table_def_;
    }
  }
  return def;
}

int ObExplainResolver::try_resolve_file_describe(const ParseNode &child_node, bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  ObStmt *child_stmt = NULL;
  const ObFileTableDef *file_def = NULL;
  ObResolver resolver(params_);
  if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, child_node, child_stmt))) {
  } else if (OB_ISNULL(child_stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to resolve described statement", K(ret));
  } else if (NULL == (file_def = find_file_table_def(*static_cast<ObDMLStmt*>(child_stmt)))) {
    // not a file table, fall back to the normal EXPLAIN behavior
  } else {
    ObSqlString describe_sql;
    ParseResult parse_result;
    ObStmt *describe_stmt = NULL;
    if (OB_FAIL(build_file_describe_sql(*file_def, describe_sql))) {
    } else {
      // The generated statement is resolved with the session sql mode so that
      // the produced plan is identical to a hand written UNION ALL query.
      ObParser parser(*allocator_, session_info_->get_sql_mode());
      if (OB_FAIL(parser.parse(describe_sql.string(), parse_result))) {
        LOG_WARN("failed to parse generated describe statement", K(ret), K(describe_sql.ptr()));
      } else if (OB_ISNULL(parse_result.result_tree_) || parse_result.result_tree_->num_child_ < 1
                 || OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("empty parse tree for describe statement", K(ret));
      } else {
        ObResolver describe_resolver(params_);
        if (OB_FAIL(describe_resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                              *parse_result.result_tree_->children_[0],
                                              describe_stmt))) {
        } else if (OB_ISNULL(describe_stmt)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to resolve describe statement", K(ret));
        } else {
          set_basic_stmt(describe_stmt);
          handled = true;
          LOG_TRACE("describe file table", K(describe_sql.ptr()));
        }
      }
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (T_EXPLAIN == parse_tree.type_ && 5 == parse_tree.num_child_
      && NULL != parse_tree.children_[EXPLAIN_CHILD_STMT]
      && T_SELECT == parse_tree.children_[EXPLAIN_CHILD_STMT]->type_
      && parse_tree_has_file_table(parse_tree.children_[EXPLAIN_CHILD_STMT])) {
    bool handled = false;
    if (OB_FAIL(try_resolve_file_describe(*parse_tree.children_[EXPLAIN_CHILD_STMT], handled))) {
      LOG_WARN("failed to resolve file table describe", K(ret));
    } else if (OB_UNLIKELY(!handled)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("file table describe is not handled", K(ret));
    }
    return ret;
  }
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
