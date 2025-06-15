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

            // 建立别名映射
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
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else {
            // infer table name from column name, 考虑别名
            for (auto &sel_col : query->cols) {
                sel_col = check_column_with_alias(all_cols, sel_col, alias_map);  // 列元数据校验（支持别名）
            }
        }
        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause_with_alias(query->tables, query->conds, alias_map);

        // 处理JOIN ON条件
        for (const auto &join_expr : x->jointree) {
            std::vector<Condition> join_conds;
            get_clause(join_expr->conds, join_conds);
            check_clause_with_alias(query->tables, join_conds, alias_map);

            // 将JOIN ON条件添加到查询条件中
            query->conds.insert(query->conds.end(), join_conds.begin(), join_conds.end());
        }

        // 保存别名映射到Query对象（用于EXPLAIN显示）
        query->table_alias_map = alias_map;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        /** TODO: */
        query->tables.push_back(x->tab_name);

        // 检查表是否存在
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }

        // 处理需要更新的列和值
        for (const auto &set_clause : x->set_clauses) {
            SetClause update_clause = {.lhs = {x->tab_name, set_clause->col_name}, .rhs = convert_sv_value(set_clause->val)};

            // 类型转换
            TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
            auto col = tab.get_col(set_clause->col_name);
            if (col->type != update_clause.rhs.type) {
                if (!can_cast_type(update_clause.rhs.type, col->type)) {
                    throw IncompatibleTypeError(coltype2str(update_clause.rhs.type), coltype2str(col->type));
                }
            }

            query->set_clauses.push_back(update_clause);
        }

        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
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

// 支持别名的列检查函数
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