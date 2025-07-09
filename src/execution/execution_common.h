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
#include "common/context.h"
#include "parser/ast.h"
#include "record/rm_defs.h"
#include "record/rm_file_handle.h"
#include "system/sm_meta.h"
#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"
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

/**
 * @brief 评估单个WHERE条件
 * @param record 记录
 * @param cond 条件
/**
 * @brief 检查记录对当前事务的可见性（MVCC规则）
 */
bool IsRecordVisible(const Rid& rid, RmFileHandle* fh, TransactionManager* txn_mgr, Context* context);

bool CheckWriteConflict(const Rid& rid, RmFileHandle* fh, TransactionManager* txn_mgr, Context* context);

bool EvaluateCondition(const RmRecord& record, const Condition& cond, TabMeta& tab_meta);

/**
 * @brief 评估多个WHERE条件（AND关系）
 * @param record 记录
 * @param conditions 条件列表
 * @param tab_meta 表元数据
 * @return 所有条件是否都满足
 */
bool EvaluateConditions(const RmRecord& record, const std::vector<Condition>& conditions, TabMeta& tab_meta);

std::unique_ptr<UndoLog> GetUndoLog(TransactionManager* txn_mgr, const UndoLink& link);
