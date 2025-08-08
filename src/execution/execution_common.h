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
#include <queue>
#include <unordered_map>
#include <vector>

#include "common/common.h"
#include "common/context.h"
#include "index/ix_index_handle.h"
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
 */

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

bool mvcc_insert_index(const TabMeta& tab_, std::unique_ptr<RmRecord>& rec, Rid rid, Context* context_,
                       TransactionManager* txn_mgr, SmManager* sm_manager);

bool check_conflict(Transaction* txn, TransactionManager* txn_mgr, RmFileHandle* fh, const Rid& rid);

/**
 * @brief 检查记录对事务的可见性（MVCC）
 * @param rid 记录ID
 * @param txn 当前事务
 * @param txn_mgr 事务管理器
 * @param fh 文件句柄
 * @return 记录是否对当前事务可见
 */
bool IsRecordVisible(const Rid& rid, Transaction* txn, TransactionManager* txn_mgr, RmFileHandle* fh);

/**
 * @brief 获取记录对事务可见的版本（MVCC）
 * @param rid 记录ID
 * @param txn 当前事务
 * @param txn_mgr 事务管理器
 * @param fh 文件句柄
 * @param context 执行上下文
 * @param tab_meta 表元数据
 * @return 可见的记录版本，如果不可见则返回nullptr
 */
std::unique_ptr<RmRecord> GetVisibleRecord(const Rid& rid, Transaction* txn, TransactionManager* txn_mgr,
                                           RmFileHandle* fh, Context* context, const TabMeta& tab_meta);

/**
 * @brief 按页读取优化 - 从已锁定页面获取可见记录
 * @param rid 记录ID
 * @param page_data 页面数据指针
 * @param txn 当前事务
 * @param txn_mgr 事务管理器
 * @param fh 文件句柄
 * @param context 执行上下文
 * @param tab_meta 表元数据
 * @return 可见的记录版本，如果不可见则返回nullptr
 */
std::unique_ptr<RmRecord> GetVisibleRecordFromPage(
    const Rid& rid,
    const char* page_data,
    Transaction* txn,
    TransactionManager* txn_mgr,
    RmFileHandle* fh,
    Context* context,
    const TabMeta& tab_meta);

/**
 * @brief 从UndoLog恢复记录
 * @param undo_log 撤销日志
 * @param tab_meta 表元数据
 * @return 恢复的记录
 */
std::unique_ptr<RmRecord> RestoreRecordFromUndoLog(const UndoLog& undo_log, const TabMeta& tab_meta);

/**
 * @brief 创建DELETE操作的UndoLog
 * @param record 要删除的记录
 * @param rid 记录ID
 * @param tab_meta 表元数据
 * @param fh 文件句柄
 * @param txn_mgr 事务管理器
 * @param context 执行上下文
 * @return 创建的UndoLog
 */
UndoLog CreateDeleteUndoLog(const RmRecord& record, const Rid& rid, const TabMeta& tab_meta,
                            RmFileHandle* fh, TransactionManager* txn_mgr, Context* context);

/**
 * @brief 创建INSERT操作的UndoLog
 * @param record 要插入的记录
 * @param rid 记录ID
 * @param tab_meta 表元数据
 * @param txn_mgr 事务管理器
 * @param context 执行上下文
 * @return 创建的UndoLog
 */
UndoLog CreateInsertUndoLog(const RmRecord& record, const Rid& rid, const TabMeta& tab_meta,
                            TransactionManager* txn_mgr, Context* context);

/**
 * @brief 检查间隙锁冲突（基于RmRecord）
 * @param record 要检查的记录
 * @param tab_meta 表元数据
 * @param fh 文件句柄
 * @param context 执行上下文
 */
// void CheckGapLockConflict(const RmRecord& record, const TabMeta& tab_meta, RmFileHandle* fh, Context* context);

struct BatchRecordResult {
    std::vector<std::unique_ptr<RmRecord>> records;
    std::vector<Rid> rids;
    size_t processed_count;
    size_t valid_count;

    BatchRecordResult() : processed_count(0), valid_count(0) {}
};

struct VisibilityResult {
    std::unique_ptr<RmRecord> record;
    Rid rid;
    bool is_visible;

    VisibilityResult(std::unique_ptr<RmRecord> rec, const Rid& r, bool visible)
        : record(std::move(rec)), rid(r), is_visible(visible) {}
};

class BatchRecordProcessor {
   public:
    static std::unordered_map<int, std::vector<Rid>> groupRidsByPage(const std::vector<Rid>& rids);

    static BatchRecordResult batchGetRecords(
        const std::vector<Rid>& rids,
        RmFileHandle* fh,
        Context* context);

    static BatchRecordResult batchGetVisibleRecords(
        const std::vector<Rid>& rids,
        RmFileHandle* fh,
        const std::vector<Condition>& conds,
        const TabMeta& tab_meta,
        Context* context);

    static std::vector<std::unique_ptr<RmRecord>> processSinglePage(
        int page_no,
        const std::vector<Rid>& page_rids,
        RmFileHandle* fh,
        Context* context);
};

std::vector<VisibilityResult> GetVisibleRecordBatch(
    const std::vector<Rid>& rids,
    Transaction* txn,
    TransactionManager* txn_mgr,
    RmFileHandle* fh,
    Context* context,
    const TabMeta& tab_meta);

std::unique_ptr<RmRecord> GetVisibleRecordFromPage(
    const Rid& rid,
    const RmPageHandle& page_handle,
    RmFileHandle* fh,
    Transaction* txn,
    TransactionManager* txn_mgr,
    Context* context,
    const TabMeta& tab_meta);
