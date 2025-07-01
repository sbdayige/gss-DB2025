/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/common.h"
#include "parser/parser.h"
#include "system/sm.h"

class Query {
   public:
    std::shared_ptr<ast::TreeNode> parse;
    // TODO jointree
    // where条件
    std::vector<Condition> conds;
    // 投影列
    std::vector<TabCol> cols;
    // 表名
    std::vector<std::string> tables;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    // insert 的values值
    std::vector<Value> values;
    // 表别名映射（用于EXPLAIN显示）
    std::map<std::string, std::string> table_alias_map;
    // 标记原始查询是否是SELECT *
    bool is_select_star = false;
    // 聚合函数表达式列表
    std::vector<std::shared_ptr<ast::AggregateExpr>> agg_exprs;
    // GROUP BY列列表
    std::vector<TabCol> group_by_cols;
    // HAVING条件列表
    std::vector<Condition> having_conds;

    Query() {}
};

class Analyze {
   private:
    SmManager *sm_manager_;

   public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager) {}
    ~Analyze() {}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

   private:
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target);
    TabCol check_column_with_alias(const std::vector<ColMeta> &all_cols, TabCol target, const std::map<std::string, std::string> &alias_map);
    TabCol check_column_with_semi_join_preference(const std::vector<ColMeta> &all_cols, TabCol target, const std::map<std::string, std::string> &alias_map, const std::string &preferred_table);
    void get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols);
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    void check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds);
    void check_clause_with_alias(const std::vector<std::string> &tab_names, std::vector<Condition> &conds, const std::map<std::string, std::string> &alias_map);
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    Value evaluate_arith_expr(const std::shared_ptr<ast::ArithExpr> &arith_expr);
    CompOp convert_sv_comp_op(ast::SvCompOp op);
    bool can_cast_type(ColType from, ColType to);
    void cast_value(Value &val, ColType to);
    void validate_arith_expr(std::shared_ptr<ast::ArithExpr> arith_expr, const std::vector<std::string> &tab_names);
};
