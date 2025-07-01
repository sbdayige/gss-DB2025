/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

#include <iostream>

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        // 处理表名和别名 - 使用新的table_refs结构
        query->tables = x->get_table_names();

        // 从JOIN表达式中添加额外的表名
        for (const auto &join_expr : x->jointree) {
            // 添加JOIN右表
            std::string right_table = join_expr->right_ref->tab_name;
            if (std::find(query->tables.begin(), query->tables.end(), right_table) == query->tables.end()) {
                query->tables.push_back(right_table);
            }
        }
        // 创建别名到实际表名的映射
        std::map<std::string, std::string> alias_map;
        for (const auto &table_ref : x->table_refs) {
            // 检查表是否存在
            if (!sm_manager_->db_.is_table(table_ref->tab_name)) {
                throw TableNotFoundError(table_ref->tab_name);
            }

            if (!table_ref->alias.empty()) {
                // 检查别名是否已经存在
                if (alias_map.find(table_ref->alias) != alias_map.end()) {
                    throw DuplicateAliasError(table_ref->alias);
                }
                alias_map[table_ref->alias] = table_ref->tab_name;
            }
            // 表名也映射到自己，支持完整表名引用
            // 但需要检查表名是否与已有别名冲突
            if (alias_map.find(table_ref->tab_name) != alias_map.end() &&
                alias_map[table_ref->tab_name] != table_ref->tab_name) {
                throw DuplicateAliasError(table_ref->tab_name);
            }
            alias_map[table_ref->tab_name] = table_ref->tab_name;
        }

        // 为JOIN表也建立别名映射（从语法解析器输出中提取）
        for (const auto &join_expr : x->jointree) {
            auto right_ref = join_expr->right_ref;
            // 检查JOIN右表是否存在
            if (!sm_manager_->db_.is_table(right_ref->tab_name)) {
                throw TableNotFoundError(right_ref->tab_name);
            }

            // JOIN右表的别名映射
            if (!right_ref->alias.empty()) {
                // 检查别名是否已经存在
                if (alias_map.find(right_ref->alias) != alias_map.end()) {
                    throw DuplicateAliasError(right_ref->alias);
                }
                alias_map[right_ref->alias] = right_ref->tab_name;
            }
            // JOIN右表的表名映射
            // 检查表名是否与已有别名冲突
            if (alias_map.find(right_ref->tab_name) != alias_map.end() &&
                alias_map[right_ref->tab_name] != right_ref->tab_name) {
                throw DuplicateAliasError(right_ref->tab_name);
            }
            alias_map[right_ref->tab_name] = right_ref->tab_name;
        }
        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &sv_sel_col : x->cols) {
            TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
            query->cols.push_back(sel_col);
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        if (query->cols.empty()) {
            // select all columns (SELECT *)
            query->is_select_star = true;

            // 检查是否有SEMI JOIN
            bool has_semi_join = false;
            std::string left_table_name;
            for (const auto &join_expr : x->jointree) {
                if (join_expr->type == SEMI_JOIN) {
                    has_semi_join = true;
                    left_table_name = join_expr->left.empty() ? query->tables[0] : join_expr->left;
                    break;
                }
            }

            for (auto &col : all_cols) {
                if (has_semi_join) {
                    // SEMI JOIN: 只添加左表的列
                    if (col.tab_name == left_table_name) {
                        TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                        query->cols.push_back(sel_col);
                    }
                } else {
                    // 普通查询: 添加所有表的列
                    TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                    query->cols.push_back(sel_col);
                }
            }
        } else {
            // infer table name from column name, 考虑别名
            // 对于SEMI JOIN，需要特殊处理模糊列名（优先选择左表列）
            bool has_semi_join = false;
            std::string left_table_name;
            for (const auto &join_expr : x->jointree) {
                if (join_expr->type == SEMI_JOIN) {
                    has_semi_join = true;
                    left_table_name = join_expr->left;
                    break;
                }
            }

            for (auto &sel_col : query->cols) {
                if (has_semi_join) {
                    sel_col = check_column_with_semi_join_preference(all_cols, sel_col, alias_map, left_table_name);
                } else {
                    sel_col = check_column_with_alias(all_cols, sel_col, alias_map);  // 列元数据校验（支持别名）
                }
            }
        }
        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause_with_alias(query->tables, query->conds, alias_map);

        // 处理JOIN ON条件
        for (const auto &join_expr : x->jointree) {
            // 设置左表名（语法解析器中留空，在这里确定）
            if (join_expr->left.empty() && !query->tables.empty()) {
                join_expr->left = query->tables[0];  // 假设第一个表是左表
            }

            std::vector<Condition> join_conds;
            get_clause(join_expr->conds, join_conds);
            check_clause_with_alias(query->tables, join_conds, alias_map);

            // 将JOIN ON条件添加到查询条件中
            query->conds.insert(query->conds.end(), join_conds.begin(), join_conds.end());
        }

        // SEMI JOIN语义检查：SELECT子句中只能选择左表的列
        for (const auto &join_expr : x->jointree) {
            if (join_expr->type == SEMI_JOIN) {
                std::string right_table_name = join_expr->right_ref->tab_name;
                std::string right_table_alias = join_expr->right_ref->alias;

                // 检查所有选择的列是否来自右表
                for (const auto &sel_col : query->cols) {
                    bool is_from_right_table = false;

                    // 检查是否通过表名引用右表
                    if (sel_col.tab_name == right_table_name) {
                        is_from_right_table = true;
                    }

                    // 检查是否通过别名引用右表
                    if (!right_table_alias.empty() && sel_col.tab_name == right_table_alias) {
                        is_from_right_table = true;
                    }

                    if (is_from_right_table) {
                        throw InternalError("SEMI JOIN cannot select columns from right table: " +
                                            sel_col.tab_name + "." + sel_col.col_name);
                    }
                }
            }
        }

        // 保存别名映射到Query对象（用于EXPLAIN显示）
        query->table_alias_map = alias_map;
        if (auto x = std::dynamic_pointer_cast<ast::ExplainStmt>(parse)) {
            query->parse = std::move(parse);
            return query;
        }
        // 仅SelectStmt继续往下走
        //  处理聚合函数 - 现在从SelectStmt中直接获取聚合表达式
        //  处理聚合函数 - 现在从SelectStmt中直接获取聚合表达式
        std::cerr << "DEBUG: analyze.cpp - agg_exprs.size() = " << x->agg_exprs.size() << std::endl;
        if (!x->agg_exprs.empty()) {
            // 有聚合函数，直接使用SelectStmt中的聚合表达式
            std::cerr << "DEBUG: analyze.cpp - Using agg_exprs from SelectStmt" << std::endl;
            query->agg_exprs = x->agg_exprs;
        } else {
            // 兼容性处理：检查列名中是否包含聚合函数（用于向后兼容）
            for (auto &col : x->cols) {
                // 如果列名包含聚合函数标识，说明这是从聚合表达式转换来的
                if (col->col_name.find("COUNT") != std::string::npos ||
                    col->col_name.find("MAX") != std::string::npos ||
                    col->col_name.find("MIN") != std::string::npos ||
                    col->col_name.find("SUM") != std::string::npos ||
                    col->col_name.find("AVG") != std::string::npos) {
                    // 根据列名重建聚合表达式（这是一个简化的方法）
                    // 在更完善的实现中，应该直接从语法解析器获取AggregateExpr节点
                    ast::AggType agg_type;
                    std::shared_ptr<ast::Col> agg_col = nullptr;

                    if (col->col_name.find("COUNT(*)") != std::string::npos) {
                        agg_type = ast::AGG_COUNT;
                        agg_col = nullptr;  // COUNT(*) 不需要列
                    } else if (col->col_name.find("COUNT") != std::string::npos) {
                        agg_type = ast::AGG_COUNT;
                        // 提取列名
                        size_t start = col->col_name.find('(') + 1;
                        size_t end = col->col_name.find(')', start);
                        if (start != std::string::npos && end != std::string::npos) {
                            std::string col_name = col->col_name.substr(start, end - start);
                            agg_col = std::make_shared<ast::Col>("", col_name);
                        }
                    } else if (col->col_name.find("MAX") != std::string::npos) {
                        agg_type = ast::AGG_MAX;
                        size_t start = col->col_name.find('(') + 1;
                        size_t end = col->col_name.find(')', start);
                        if (start != std::string::npos && end != std::string::npos) {
                            std::string col_name = col->col_name.substr(start, end - start);
                            agg_col = std::make_shared<ast::Col>("", col_name);
                        }
                        // 类似地提取列名
                    } else if (col->col_name.find("MIN") != std::string::npos) {
                        agg_type = ast::AGG_MIN;
                        size_t start = col->col_name.find('(') + 1;
                        size_t end = col->col_name.find(')', start);
                        if (start != std::string::npos && end != std::string::npos) {
                            std::string col_name = col->col_name.substr(start, end - start);
                            agg_col = std::make_shared<ast::Col>("", col_name);
                        }
                    } else if (col->col_name.find("SUM") != std::string::npos) {
                        agg_type = ast::AGG_SUM;
                        size_t start = col->col_name.find('(') + 1;
                        size_t end = col->col_name.find(')', start);
                        if (start != std::string::npos && end != std::string::npos) {
                            std::string col_name = col->col_name.substr(start, end - start);
                            agg_col = std::make_shared<ast::Col>("", col_name);
                        }
                    } else if (col->col_name.find("AVG") != std::string::npos) {
                        agg_type = ast::AGG_AVG;
                        size_t start = col->col_name.find('(') + 1;
                        size_t end = col->col_name.find(')', start);
                        if (start != std::string::npos && end != std::string::npos) {
                            std::string col_name = col->col_name.substr(start, end - start);
                            agg_col = std::make_shared<ast::Col>("", col_name);
                        }
                    }

                    auto agg_expr = std::make_shared<ast::AggregateExpr>(agg_type, agg_col, col->alias);
                    query->agg_exprs.push_back(agg_expr);
                }
            }
        }

        // 处理GROUP BY子句
        for (auto &group_col : x->group_by_cols) {
            TabCol gb_col = {.tab_name = group_col->tab_name, .col_name = group_col->col_name};
            gb_col = check_column_with_alias(all_cols, gb_col, alias_map);
            query->group_by_cols.push_back(gb_col);
        }

        // GROUP BY语义检查：SELECT列表中的非聚合列必须出现在GROUP BY子句中
        if (!query->group_by_cols.empty()) {
            for (const auto &sel_col : query->cols) {
                bool found_in_group_by = false;

                // 检查该列是否在GROUP BY子句中
                for (const auto &group_col : query->group_by_cols) {
                    if (sel_col.tab_name == group_col.tab_name &&
                        sel_col.col_name == group_col.col_name) {
                        found_in_group_by = true;
                        break;
                    }
                }

                if (!found_in_group_by) {
                    // 该列不在GROUP BY中，这在SQL语义上是错误的
                    throw InternalError("Column '" + sel_col.col_name +
                                        "' must appear in the GROUP BY clause or be used in an aggregate function");
                }
            }
        }

        // 处理HAVING子句
        get_clause(x->having_conds, query->having_conds);
        // 对于HAVING子句，我们需要特殊处理聚合函数，跳过列名验证
        // 因为聚合函数如COUNT(*)不是表的实际列
        for (auto &cond : query->having_conds) {
            // 检查左侧是否是聚合函数
            bool is_aggregate_function =
                cond.lhs_col.col_name.find("COUNT(") != std::string::npos ||
                cond.lhs_col.col_name.find("MAX(") != std::string::npos ||
                cond.lhs_col.col_name.find("MIN(") != std::string::npos ||
                cond.lhs_col.col_name.find("SUM(") != std::string::npos ||
                cond.lhs_col.col_name.find("AVG(") != std::string::npos;

            if (!is_aggregate_function) {
                // 只对非聚合函数的列进行验证
                cond.lhs_col = check_column_with_alias(all_cols, cond.lhs_col, alias_map);
                if (!cond.is_rhs_val) {
                    cond.rhs_col = check_column_with_alias(all_cols, cond.rhs_col, alias_map);
                }
            }
            // 聚合函数的列名验证将在执行阶段处理
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {  // 检查表是否存在
            throw TableNotFoundError(x->tab_name);
        }
        query->tables.push_back(x->tab_name);
        // 获取相关表的所有列，用于后续校验
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        // 准备SET子句
        query->set_clauses.reserve(x->set_clauses.size());
        // 处理每个SET赋值
        for (auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs.tab_name = x->tab_name;              // 设置左侧列的表名
            set_clause.lhs.col_name = sv_set_clause->col_name;  // 设置左侧列名

            // 分析右侧表达式
            if (auto val = std::dynamic_pointer_cast<ast::Value>(sv_set_clause->val)) {
                // 简单值赋值
                set_clause.rhs_type = SetRhsType::SET_RHS_VALUE;
                set_clause.rhs_val = convert_sv_value(val);
            } else if (auto col = std::dynamic_pointer_cast<ast::Col>(sv_set_clause->val)) {
                // 列引用赋值
                set_clause.rhs_type = SetRhsType::SET_RHS_COL;
                set_clause.rhs_col = {col->tab_name, col->col_name};
            } else if (auto arith_expr = std::dynamic_pointer_cast<ast::ArithExpr>(sv_set_clause->val)) {
                // 算术表达式赋值
                set_clause.rhs_type = SetRhsType::SET_RHS_EXPR;
                set_clause.rhs_expr = arith_expr;
            } else {
                throw RMDBError("Unsupported right-hand side type in UPDATE statement");
            }
            query->set_clauses.push_back(std::move(set_clause));  // 使用 move 提高效率
        }

        // 处理WHERE条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);  // 检查WHERE条件的有效性

        // 检查每个SET子句的有效性和类型兼容性
        for (auto &set_clause : query->set_clauses) {
            set_clause.lhs = check_column(all_cols, set_clause.lhs);  // 检查列是否存在

            if (set_clause.rhs_type == SetRhsType::SET_RHS_VALUE) {
                TabMeta &tab = sm_manager_->db_.get_table(set_clause.lhs.tab_name);
                auto col = tab.get_col(set_clause.lhs.col_name);
                // 允许数值类型之间的转换(INT与FLOAT)
                bool is_numeric =
                    (col->type == ColType::TYPE_INT || col->type == ColType::TYPE_FLOAT) &&
                    (set_clause.rhs_val.type == ColType::TYPE_INT || set_clause.rhs_val.type == ColType::TYPE_FLOAT);
                if (col->type != set_clause.rhs_val.type && !is_numeric) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs_val.type));
                }
                set_clause.rhs_val.init_raw(col->len);  // 初始化值的原始数据
            } else if (set_clause.rhs_type == SetRhsType::SET_RHS_COL) {
                // 检查右侧列是否存在
                set_clause.rhs_col = check_column(all_cols, set_clause.rhs_col);
                // 类型兼容校验：允许 INT↔FLOAT，其他必须完全一致
                TabMeta &lhs_tab = sm_manager_->db_.get_table(set_clause.lhs.tab_name);
                auto l_col = lhs_tab.get_col(set_clause.lhs.col_name);
                TabMeta &rhs_tab = sm_manager_->db_.get_table(set_clause.rhs_col.tab_name);
                auto r_col = rhs_tab.get_col(set_clause.rhs_col.col_name);

                bool is_numeric = (l_col->type == ColType::TYPE_INT || l_col->type == ColType::TYPE_FLOAT) &&
                                  (r_col->type == ColType::TYPE_INT || r_col->type == ColType::TYPE_FLOAT);
                if (l_col->type != r_col->type && !is_numeric) {
                    throw IncompatibleTypeError(coltype2str(l_col->type), coltype2str(r_col->type));
                }
            } else if (set_clause.rhs_type == SetRhsType::SET_RHS_EXPR) {
                // 验证算术表达式
                validate_arith_expr(set_clause.rhs_expr, {set_clause.lhs.tab_name});

                // 检查目标列类型是否支持算术表达式
                TabMeta &tab = sm_manager_->db_.get_table(set_clause.lhs.tab_name);
                auto col = tab.get_col(set_clause.lhs.col_name);
                if (col->type == ColType::TYPE_STRING) {
                    throw IncompatibleTypeError(coltype2str(col->type), "numeric type for arithmetic result");
                }
            }
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        query->tables.push_back(x->tab_name);

        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }

        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        /** TODO: Make sure target column exists */
        bool found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }
    }
    return target;
}
TabCol Analyze::check_column_with_alias(const std::vector<ColMeta> &all_cols, TabCol target, const std::map<std::string, std::string> &alias_map) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        // 检查是否使用了别名，如果是则转换为实际表名
        std::string actual_tab_name = target.tab_name;
        auto alias_it = alias_map.find(target.tab_name);
        if (alias_it != alias_map.end()) {
            actual_tab_name = alias_it->second;
        }

        // 使用实际表名检查列是否存在
        bool found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == actual_tab_name && col.name == target.col_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }

        // 返回的target仍然使用实际表名，这样后续处理就不需要再考虑别名
        target.tab_name = actual_tab_name;
    }
    return target;
}

// 支持SEMI JOIN的列检查函数：在模糊列名时优先选择左表的列
TabCol Analyze::check_column_with_semi_join_preference(const std::vector<ColMeta> &all_cols, TabCol target, const std::map<std::string, std::string> &alias_map, const std::string &preferred_table) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        std::string preferred_tab_name;

        // 先检查是否存在首选表（左表）中的列
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (col.tab_name == preferred_table) {
                    preferred_tab_name = col.tab_name;
                }
                if (tab_name.empty()) {
                    tab_name = col.tab_name;
                }
            }
        }

        if (preferred_tab_name.empty() && tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }

        // 如果找到首选表中的列，优先使用它
        target.tab_name = preferred_tab_name.empty() ? tab_name : preferred_tab_name;
    } else {
        // 检查是否使用了别名，如果是则转换为实际表名
        std::string actual_tab_name = target.tab_name;
        auto alias_it = alias_map.find(target.tab_name);
        if (alias_it != alias_map.end()) {
            actual_tab_name = alias_it->second;
        }

        // 使用实际表名检查列是否存在
        bool found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == actual_tab_name && col.name == target.col_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }

        // 返回的target仍然使用实际表名，这样后续处理就不需要再考虑别名
        target.tab_name = actual_tab_name;
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        } else if (auto rhs_arith = std::dynamic_pointer_cast<ast::ArithExpr>(expr->rhs)) {
            // 处理算术表达式（例如一元减号 -1.0）
            cond.is_rhs_val = true;
            cond.rhs_val = evaluate_arith_expr(rhs_arith);
        } else {
            throw InternalError("Unexpected expression type in WHERE clause");
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            // 不在这里进行init_raw，让执行器来处理
            // cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        // 检查类型兼容性 - 使用can_cast_type函数
        if (lhs_type != rhs_type && !can_cast_type(rhs_type, lhs_type) && !can_cast_type(lhs_type, rhs_type)) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}

// 支持别名的条件检查函数
void Analyze::check_clause_with_alias(const std::vector<std::string> &tab_names, std::vector<Condition> &conds, const std::map<std::string, std::string> &alias_map) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name, 考虑别名
        cond.lhs_col = check_column_with_alias(all_cols, cond.lhs_col, alias_map);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column_with_alias(all_cols, cond.rhs_col, alias_map);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            // 不在这里进行init_raw，让执行器来处理
            // cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            if (cond.is_rhs_val) {
                if (!can_cast_type(rhs_type, lhs_type)) {
                    throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
                }
                // 不进行实际的类型转换，只检查兼容性
                // 保持原始值和类型不变，由执行器在运行时处理类型转换
            } else {
                // 对于列与列的比较，也检查类型兼容性
                if (!can_cast_type(rhs_type, lhs_type) && !can_cast_type(lhs_type, rhs_type)) {
                    throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
                }
            }
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    // Assert to trigger segmentation fault when != or <> operators are used
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ},
        {ast::SV_OP_NE, OP_NE},
        {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT},
        {ast::SV_OP_LE, OP_LE},
        {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}

bool Analyze::can_cast_type(ColType from, ColType to) {
    // Add logic to determine if a type can be cast to another type
    if (from == to)
        return true;
    if (from == TYPE_INT && to == TYPE_FLOAT)
        return true;
    if (from == TYPE_FLOAT && to == TYPE_INT)
        return true;
    return false;
}

void Analyze::cast_value(Value &val, ColType to) {
    // Add logic to cast val to the target type
    if (val.type == TYPE_INT && to == TYPE_FLOAT) {
        int int_val = val.int_val;
        val.type = TYPE_FLOAT;
        val.float_val = static_cast<float>(int_val);
    } else if (val.type == TYPE_FLOAT && to == TYPE_INT) {
        // do not things
    } else {
        throw IncompatibleTypeError(coltype2str(val.type), coltype2str(to));
    }
}

void Analyze::validate_arith_expr(std::shared_ptr<ast::ArithExpr> arith_expr, const std::vector<std::string> &tab_names) {
    // 获取所有表的列信息
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);

    // 验证左操作数
    if (auto lhs_col = std::dynamic_pointer_cast<ast::Col>(arith_expr->lhs)) {
        TabCol lhs_tab_col = {lhs_col->tab_name, lhs_col->col_name};
        lhs_tab_col = check_column(all_cols, lhs_tab_col);

        // 检查列类型是否支持算术运算
        TabMeta &tab = sm_manager_->db_.get_table(lhs_tab_col.tab_name);
        auto col = tab.get_col(lhs_tab_col.col_name);
        if (col->type != TYPE_INT && col->type != TYPE_FLOAT) {
            throw IncompatibleTypeError(coltype2str(col->type), "numeric type for arithmetic operation");
        }
    } else if (auto lhs_val = std::dynamic_pointer_cast<ast::Value>(arith_expr->lhs)) {
        // 验证值类型是否支持算术运算
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(lhs_val)) {
            // INT类型支持算术运算
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(lhs_val)) {
            // FLOAT类型支持算术运算
        } else {
            throw IncompatibleTypeError("non-numeric literal", "numeric type for arithmetic operation");
        }
    } else if (auto nested_arith = std::dynamic_pointer_cast<ast::ArithExpr>(arith_expr->lhs)) {
        // 递归验证嵌套的算术表达式
        validate_arith_expr(nested_arith, tab_names);
    }

    // 验证右操作数（逻辑与左操作数相同）
    if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(arith_expr->rhs)) {
        TabCol rhs_tab_col = {rhs_col->tab_name, rhs_col->col_name};
        rhs_tab_col = check_column(all_cols, rhs_tab_col);

        TabMeta &tab = sm_manager_->db_.get_table(rhs_tab_col.tab_name);
        auto col = tab.get_col(rhs_tab_col.col_name);
        if (col->type != TYPE_INT && col->type != TYPE_FLOAT) {
            throw IncompatibleTypeError(coltype2str(col->type), "numeric type for arithmetic operation");
        }
    } else if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(arith_expr->rhs)) {
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(rhs_val)) {
            // INT类型支持算术运算
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(rhs_val)) {
            // FLOAT类型支持算术运算
        } else {
            throw IncompatibleTypeError("non-numeric literal", "numeric type for arithmetic operation");
        }
    } else if (auto nested_arith = std::dynamic_pointer_cast<ast::ArithExpr>(arith_expr->rhs)) {
        // 递归验证嵌套的算术表达式
        validate_arith_expr(nested_arith, tab_names);
    }
}

Value Analyze::evaluate_arith_expr(const std::shared_ptr<ast::ArithExpr> &arith_expr) {
    Value result;

    // 获取左操作数的值
    Value lhs_val;
    if (auto lhs_lit = std::dynamic_pointer_cast<ast::Value>(arith_expr->lhs)) {
        lhs_val = convert_sv_value(lhs_lit);
    } else if (auto lhs_arith = std::dynamic_pointer_cast<ast::ArithExpr>(arith_expr->lhs)) {
        // 递归计算嵌套的算术表达式
        lhs_val = evaluate_arith_expr(lhs_arith);
    } else {
        throw InternalError("Cannot evaluate non-constant arithmetic expression in WHERE clause");
    }

    // 获取右操作数的值
    Value rhs_val;
    if (auto rhs_lit = std::dynamic_pointer_cast<ast::Value>(arith_expr->rhs)) {
        rhs_val = convert_sv_value(rhs_lit);
    } else if (auto rhs_arith = std::dynamic_pointer_cast<ast::ArithExpr>(arith_expr->rhs)) {
        // 递归计算嵌套的算术表达式
        rhs_val = evaluate_arith_expr(rhs_arith);
    } else {
        throw InternalError("Cannot evaluate non-constant arithmetic expression in WHERE clause");
    }

    // 执行算术运算
    switch (arith_expr->op) {
        case ast::ARITH_OP_ADD:
            if (lhs_val.type == TYPE_INT && rhs_val.type == TYPE_INT) {
                result.set_int(lhs_val.int_val + rhs_val.int_val);
            } else {
                // 转换为浮点数运算
                float lhs_float = (lhs_val.type == TYPE_INT) ? lhs_val.int_val : lhs_val.float_val;
                float rhs_float = (rhs_val.type == TYPE_INT) ? rhs_val.int_val : rhs_val.float_val;
                result.set_float(lhs_float + rhs_float);
            }
            break;

        case ast::ARITH_OP_SUB:
            if (lhs_val.type == TYPE_INT && rhs_val.type == TYPE_INT) {
                result.set_int(lhs_val.int_val - rhs_val.int_val);
            } else {
                // 转换为浮点数运算
                float lhs_float = (lhs_val.type == TYPE_INT) ? lhs_val.int_val : lhs_val.float_val;
                float rhs_float = (rhs_val.type == TYPE_INT) ? rhs_val.int_val : rhs_val.float_val;
                result.set_float(lhs_float - rhs_float);
            }
            break;

        case ast::ARITH_OP_MUL:
            if (lhs_val.type == TYPE_INT && rhs_val.type == TYPE_INT) {
                result.set_int(lhs_val.int_val * rhs_val.int_val);
            } else {
                // 转换为浮点数运算
                float lhs_float = (lhs_val.type == TYPE_INT) ? lhs_val.int_val : lhs_val.float_val;
                float rhs_float = (rhs_val.type == TYPE_INT) ? rhs_val.int_val : rhs_val.float_val;
                result.set_float(lhs_float * rhs_float);
            }
            break;

        case ast::ARITH_OP_DIV:
            // 除法总是返回浮点数
            {
                float lhs_float = (lhs_val.type == TYPE_INT) ? lhs_val.int_val : lhs_val.float_val;
                float rhs_float = (rhs_val.type == TYPE_INT) ? rhs_val.int_val : rhs_val.float_val;
                if (rhs_float == 0.0) {
                    throw InternalError("Division by zero in arithmetic expression");
                }
                result.set_float(lhs_float / rhs_float);
            }
            break;

        default:
            throw InternalError("Unknown arithmetic operator");
    }

    return result;
}