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

#include <cinttypes>
#include <cstring>

#include "common/common.h"
#include "parser/ast.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"

/**
 * @brief 表达式项结构，用于包装算术表达式
 */
struct ExprTerm {
    std::shared_ptr<ast::ArithExpr> expr;

    ExprTerm(std::shared_ptr<ast::ArithExpr> expr_) : expr(std::move(expr_)) {}
};

/**
 * @brief 从记录中获取列的值
 * @param record 记录
 * @param col_meta 列元数据
 * @return 列的值
 */
Value GetColumnValue(const RmRecord& record, const ColMeta& col_meta);

/**
 * @brief 递归计算算术表达式的值
 * @param expr 算术表达式
 * @param record 当前记录
 * @param cols 所有列的元数据
 * @return 计算结果的值
 */
Value EvaluateArithExpr(std::shared_ptr<ast::ArithExpr> expr, const RmRecord& record, const std::vector<ColMeta>& cols);

/**
 * @brief 计算表达式项的值
 * @param expr_term 表达式项
 * @param record 当前记录
 * @param cols 所有列的元数据
 * @return 计算结果的值
 */
Value EvaluateExpr(const ExprTerm& expr_term, const RmRecord& record, const std::vector<ColMeta>& cols);
