/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_common.h"

#include "../errors.h"
#include "index/ix_index_handle.h"

Value GetColumnValue(const RmRecord& record, const ColMeta& col_meta) {
    Value value;
    char* data_ptr = record.data + col_meta.offset;

    switch (col_meta.type) {
        case TYPE_INT:
            value.set_int(*reinterpret_cast<int*>(data_ptr));
            break;
        case TYPE_FLOAT:
            value.set_float(*reinterpret_cast<float*>(data_ptr));
            break;
        case TYPE_STRING:
            value.set_str(std::string(data_ptr, col_meta.len));
            break;
        default:
            throw InternalError("Unsupported column type in GetColumnValue");
    }

    return value;
}

Value EvaluateArithExpr(std::shared_ptr<ast::ArithExpr> expr, const RmRecord& record, const std::vector<ColMeta>& cols) {
    Value left_val, right_val;

    // 计算左操作数
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr->lhs)) {
        // 列引用
        const ColMeta* col_meta = nullptr;
        for (const auto& meta : cols) {
            if ((col->tab_name.empty() || meta.tab_name == col->tab_name) && meta.name == col->col_name) {
                col_meta = &meta;
                break;
            }
        }
        if (!col_meta) {
            throw RMDBError("Column not found: " + col->col_name);
        }
        left_val = GetColumnValue(record, *col_meta);
    } else if (auto val = std::dynamic_pointer_cast<ast::Value>(expr->lhs)) {
        // 字面值
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(val)) {
            left_val.set_int(int_lit->val);
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(val)) {
            left_val.set_float(float_lit->val);
        } else {
            throw InternalError("Unsupported literal type in arithmetic expression");
        }
    } else if (auto nested_expr = std::dynamic_pointer_cast<ast::ArithExpr>(expr->lhs)) {
        // 嵌套算术表达式
        left_val = EvaluateArithExpr(nested_expr, record, cols);
    }

    // 计算右操作数
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
        // 列引用
        const ColMeta* col_meta = nullptr;
        for (const auto& meta : cols) {
            if ((col->tab_name.empty() || meta.tab_name == col->tab_name) && meta.name == col->col_name) {
                col_meta = &meta;
                break;
            }
        }
        if (!col_meta) {
            throw RMDBError("Column not found: " + col->col_name);
        }
        right_val = GetColumnValue(record, *col_meta);
    } else if (auto val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
        // 字面值
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(val)) {
            right_val.set_int(int_lit->val);
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(val)) {
            right_val.set_float(float_lit->val);
        } else {
            throw InternalError("Unsupported literal type in arithmetic expression");
        }
    } else if (auto nested_expr = std::dynamic_pointer_cast<ast::ArithExpr>(expr->rhs)) {
        // 嵌套算术表达式
        right_val = EvaluateArithExpr(nested_expr, record, cols);
    }

    // 执行算术运算
    Value result;

    // 确保操作数是数值类型
    if ((left_val.type != TYPE_INT && left_val.type != TYPE_FLOAT) ||
        (right_val.type != TYPE_INT && right_val.type != TYPE_FLOAT)) {
        throw IncompatibleTypeError("Non-numeric types", "arithmetic operation");
    }

    // 类型提升：如果任一操作数是 FLOAT，结果就是 FLOAT
    bool result_is_float = (left_val.type == TYPE_FLOAT || right_val.type == TYPE_FLOAT);

    float left_float = (left_val.type == TYPE_FLOAT) ? left_val.float_val : static_cast<float>(left_val.int_val);
    float right_float = (right_val.type == TYPE_FLOAT) ? right_val.float_val : static_cast<float>(right_val.int_val);

    switch (expr->op) {
        case ast::ARITH_OP_ADD:
            if (result_is_float) {
                result.set_float(left_float + right_float);
            } else {
                result.set_int(left_val.int_val + right_val.int_val);
            }
            break;
        case ast::ARITH_OP_SUB:
            if (result_is_float) {
                result.set_float(left_float - right_float);
            } else {
                result.set_int(left_val.int_val - right_val.int_val);
            }
            break;
        case ast::ARITH_OP_MUL:
            if (result_is_float) {
                result.set_float(left_float * right_float);
            } else {
                result.set_int(left_val.int_val * right_val.int_val);
            }
            break;
        case ast::ARITH_OP_DIV:
            // 除法总是返回浮点数
            if (right_float == 0.0f) {
                throw RMDBError("Division by zero");
            }
            result.set_float(left_float / right_float);
            break;
        default:
            throw InternalError("Unsupported arithmetic operator");
    }

    return result;
}

Value EvaluateExpr(const ExprTerm& expr_term, const RmRecord& record, const std::vector<ColMeta>& cols) {
    return EvaluateArithExpr(expr_term.expr, record, cols);
}

bool EvaluateCondition(const RmRecord& record, const Condition& cond, TabMeta& tab_meta) {
    auto lhs_col = tab_meta.get_col(cond.lhs_col.col_name);
    char* lhs_data = record.data + lhs_col->offset;

    char* rhs_data;
    ColType rhs_type;

    if (cond.is_rhs_val) {
        if (!cond.rhs_val.raw) {
            // 初始化 raw 数据
            const_cast<Value&>(cond.rhs_val).init_raw(lhs_col->len);
        }
        rhs_data = cond.rhs_val.raw->data;
        rhs_type = cond.rhs_val.type;
    } else {
        auto rhs_col = tab_meta.get_col(cond.rhs_col.col_name);
        rhs_data = record.data + rhs_col->offset;
        rhs_type = rhs_col->type;
    }

    // 支持类型自动转换：int ↔ float
    int cmp_result = 0;
    if (lhs_col->type == TYPE_INT && rhs_type == TYPE_INT) {
        int lhs_val = *(int*)lhs_data;
        int rhs_val = *(int*)rhs_data;
        cmp_result = (lhs_val < rhs_val) ? -1 : (lhs_val > rhs_val) ? 1
                                                                    : 0;
    } else if (lhs_col->type == TYPE_FLOAT && rhs_type == TYPE_FLOAT) {
        float lhs_val = *(float*)lhs_data;
        float rhs_val = *(float*)rhs_data;
        cmp_result = (lhs_val < rhs_val) ? -1 : (lhs_val > rhs_val) ? 1
                                                                    : 0;
    } else if (lhs_col->type == TYPE_INT && rhs_type == TYPE_FLOAT) {
        // int → float 转换
        float lhs_val = static_cast<float>(*(int*)lhs_data);
        float rhs_val = *(float*)rhs_data;
        cmp_result = (lhs_val < rhs_val) ? -1 : (lhs_val > rhs_val) ? 1
                                                                    : 0;
    } else if (lhs_col->type == TYPE_FLOAT && rhs_type == TYPE_INT) {
        // float → int 转换 (float与int比较)
        float lhs_val = *(float*)lhs_data;
        float rhs_val = static_cast<float>(*(int*)rhs_data);
        cmp_result = (lhs_val < rhs_val) ? -1 : (lhs_val > rhs_val) ? 1
                                                                    : 0;
    } else if (lhs_col->type == TYPE_STRING && rhs_type == TYPE_STRING) {
        cmp_result = memcmp(lhs_data, rhs_data, lhs_col->len);
    } else {
        throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
    }

    switch (cond.op) {
        case OP_EQ:
            return cmp_result == 0;
        case OP_NE:
            return cmp_result != 0;
        case OP_LT:
            return cmp_result < 0;
        case OP_GT:
            return cmp_result > 0;
        case OP_LE:
            return cmp_result <= 0;
        case OP_GE:
            return cmp_result >= 0;
        default:
            return false;
    }
}

bool EvaluateConditions(const RmRecord& record, const std::vector<Condition>& conditions, TabMeta& tab_meta) {
    for (const auto& condition : conditions) {
        if (!EvaluateCondition(record, condition, tab_meta)) {
            return false;
        }
    }
    return true;
}

bool IsRecordVisible(const Rid& rid, RmFileHandle* fh, TransactionManager* txn_mgr, Context* context) {
    auto version_link = txn_mgr->GetVersionLink(fh->GetFd(), rid);
    if (!version_link.has_value()) {
        return true;  // 没有版本链，基础版本可见
    }

    UndoLink link = version_link->prev_;
    while (true) {
        if (link.prev_txn_ == INVALID_TXN_ID) {
            // 回溯到链尾，基础版本可见
            return true;
        }

        auto undo_log_opt = txn_mgr->GetUndoLogOptional(link);
        if (!undo_log_opt.has_value()) {
            break;
        }
        auto& undo_log = undo_log_opt.value();

        // 1. 本事务产生的版本
        if (link.prev_txn_ == context->txn_->get_transaction_id()) {
            if (undo_log.is_deleted_) {
                return false;  // 本事务DELETE，记录不可见
            } else {
                return true;  // 本事务INSERT/UPDATE，记录可见
            }
        }

        // 2. 其他事务产生的版本
        Transaction* log_txn = txn_mgr->find_transaction(link.prev_txn_);
        if (log_txn && log_txn->get_state() == TransactionState::COMMITTED) {
            // 已提交版本：只有提交时间早于当前事务start_ts才可见
            if (log_txn->get_commit_ts() < context->txn_->get_start_ts()) {
                if (undo_log.is_deleted_) {
                    return false;  // 已被删除，不可见
                } else {
                    return true;
                }
            }
        }
        // 回溯到前一个版本
        link = undo_log.prev_version_;
    }
    return true;
}

bool CheckWriteConflict(const Rid& rid, RmFileHandle* fh, TransactionManager* txn_mgr, Context* context) {
    auto version_link = txn_mgr->GetVersionLink(fh->GetFd(), rid);
    if (!version_link.has_value()) {
        return true;  // 没有版本链，可以操作
    }
    auto prev_txn_id = version_link->prev_.prev_txn_;
    if (prev_txn_id == INVALID_TXN_ID) {
        return true;  // 基础版本，可以操作
    }
    if (prev_txn_id == context->txn_->get_transaction_id()) {
        return true;  // 自己的修改，可以操作
    }
    // 检查其他事务的状态
    auto prev_txn = txn_mgr->find_transaction(prev_txn_id);
    if (!prev_txn) {
        return true;  // 事务不存在，可能已经提交
    }
    if (prev_txn->get_state() == TransactionState::COMMITTED) {
        // 已提交的事务，检查时间戳冲突
        if (prev_txn->get_commit_ts() > context->txn_->get_read_ts()) {
            return false;  // 写入冲突
        }
        return true;
    } else if (prev_txn->get_state() == TransactionState::ABORTED) {
        return true;  // ABORTED的事务不应该造成冲突
    }
    return false;
}

std::unique_ptr<UndoLog> GetUndoLog(TransactionManager* txn_mgr, const UndoLink& link) {
    auto undo_log_opt = txn_mgr->GetUndoLogOptional(link);
    if (undo_log_opt.has_value()) {
        return std::make_unique<UndoLog>(undo_log_opt.value());
    }
    return nullptr;
}

bool mvcc_insert_index(const TabMeta& tab_, std::unique_ptr<RmRecord>& rec, Rid rid, Context* context_,
                       TransactionManager* txn_mgr, SmManager* sm_manager) {
    RmFileHandle* fh_ = sm_manager->fhs_.at(tab_.name).get();
    std::vector<std::unique_ptr<char[]>> inserted_keys;  // 记录已插入的键值
    inserted_keys.reserve(tab_.indexes.size());          // 预分配空间以提高性能
    // 遍历表的所有索引
    for (size_t i = 0; i < tab_.indexes.size(); ++i) {
        auto& index = tab_.indexes[i];
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_.name, index.cols)).get();
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
            memcpy(key.get() + offset, rec->data + index.cols[j].offset, index.cols[j].len);
            offset += index.cols[j].len;
        }
        // 检查索引项是否已存在
        std::vector<Rid> result;
        bool is_exist = ih->get_value(key.get(), &result, context_->txn_);
        page_id_t res = INVALID_PAGE_ID;
        if (is_exist == false) {
            res = ih->insert_entry(key.get(), rid, context_->txn_);
        } else {
            bool ok = true;
            for (const auto& rid_ : result) {
                bool is_not_deleted = check_conflict(context_->txn_, txn_mgr, fh_, rid_);
                if (is_not_deleted == true) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                res = ih->insert_entry_force(key.get(), rid, context_->txn_);
            }
        }
        if (res == INVALID_PAGE_ID) {
            // 插入索引失败，可能是因为索引已存在
            for (size_t rollback_i = 0; rollback_i < i; ++rollback_i) {
                auto& rollback_index = tab_.indexes[rollback_i];
                auto rollback_ih =
                    sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_.name, rollback_index.cols))
                        .get();
                rollback_ih->delete_entry_with_rid(inserted_keys[rollback_i].get(), rid, context_->txn_);
            }
            return false;
        }
        inserted_keys.emplace_back(std::move(key));  // 保存已插入的键值
    }
    return true;
}

bool check_conflict(Transaction* txn, TransactionManager* txn_mgr, RmFileHandle* fh, const Rid& rid) {
    auto pre_undo_link = txn_mgr->GetUndoLink(fh->GetFd(), rid);
    if (pre_undo_link.has_value()) {
        auto pre_txn = pre_undo_link.value().prev_txn_;
        auto undo_log = txn_mgr->GetUndoLog(pre_undo_link.value());
        // 检查是否有事务更新它且提交时间大于当前事务读时间
        if (pre_txn != txn->get_transaction_id() && undo_log.ts_ > txn->get_read_ts()) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
        }

        if (undo_log.is_deleted_) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 检查记录对事务的可见性（MVCC）
 */
bool IsRecordVisible(const Rid& rid, Transaction* txn, TransactionManager* txn_mgr, RmFileHandle* fh) {
    auto version_link = txn_mgr->GetVersionLink(fh->GetFd(), rid);
    if (!version_link.has_value()) {
        return true;  // 没有版本链，基础版本可见
    }

    auto prev_txn_id = version_link->prev_.prev_txn_;
    if (prev_txn_id == INVALID_TXN_ID) {
        return true;  // 没有前一个事务，基础版本可见
    }

    if (prev_txn_id == txn->get_transaction_id()) {
        // 是当前事务的修改，检查是否被删除
        auto undo_log = GetUndoLog(txn_mgr, version_link->prev_);
        if (undo_log && undo_log->is_deleted_) {
            return false;  // 被当前事务删除，不可见
        }
        return true;  // 当前事务的修改，可见
    }

    // 其他事务的修改，检查事务状态和提交时间
    auto prev_txn = txn_mgr->find_transaction(prev_txn_id);
    if (!prev_txn) {
        return true;  // 事务已结束，假设已提交
    }

    if (prev_txn->get_state() == TransactionState::COMMITTED) {
        if (prev_txn->get_commit_ts() <= txn->get_read_ts()) {
            auto undo_log = GetUndoLog(txn_mgr, version_link->prev_);
            if (undo_log && undo_log->is_deleted_) {
                return false;  // 记录被删除，不可见
            }
            return true;  // 已提交且在读时间戳之前，可见
        }
        return false;  // 提交时间晚于读时间戳，不可见
    }

    return false;  // 未提交或已回滚，不可见
}

/**
 * @brief 获取记录对事务可见的版本（MVCC）
 */
std::unique_ptr<RmRecord> GetVisibleRecord(const Rid& rid, Transaction* txn, TransactionManager* txn_mgr,
                                           RmFileHandle* fh, Context* context, const TabMeta& tab_meta) {
    auto version_link = txn_mgr->GetVersionLink(fh->GetFd(), rid);
    if (!version_link.has_value()) {
        // 没有版本链，返回基础版本
        return fh->get_record(rid, context);
    }

    UndoLink link = version_link->prev_;
    while (true) {
        if (link.prev_txn_ == INVALID_TXN_ID) {
            // 回溯到链尾，检查INSERT操作的可见性
            auto insert_version_link = txn_mgr->GetVersionLink(fh->GetFd(), rid);
            if (insert_version_link.has_value()) {
                UndoLink insert_link = insert_version_link->prev_;
                auto insert_undo_log = GetUndoLog(txn_mgr, insert_link);
                if (insert_undo_log) {
                    Transaction* insert_txn = txn_mgr->find_transaction(insert_link.prev_txn_);
                    if (insert_txn && insert_txn->get_state() == TransactionState::COMMITTED &&
                        insert_txn->get_commit_ts() < txn->get_start_ts()) {
                        return fh->get_record(rid, context);
                    }
                }
            }
            return nullptr;  // 记录不可见
        }

        auto undo_log = GetUndoLog(txn_mgr, link);
        if (!undo_log) {
            break;
        }

        Transaction* log_txn = txn_mgr->find_transaction(link.prev_txn_);

        // 1. 本事务产生的版本：只对本事务可见
        if (link.prev_txn_ == txn->get_transaction_id()) {
            if (undo_log->is_deleted_) {
                return nullptr;  // 本事务的DELETE操作，记录不可见
            } else {
                return fh->get_record(rid, context);  // 返回物理记录
            }
        }

        // 2. 其他事务产生的未提交版本：对本事务不可见
        if (link.prev_txn_ != txn->get_transaction_id() && log_txn &&
            log_txn->get_state() != TransactionState::COMMITTED) {
            link = undo_log->prev_version_;
            continue;
        }

        // 3. 已提交版本：只有提交时间早于当前事务start_ts才可见
        if (log_txn && log_txn->get_state() == TransactionState::COMMITTED) {
            if (log_txn->get_commit_ts() < txn->get_start_ts()) {
                if (undo_log->is_deleted_) {
                    return nullptr;  // 记录被删除
                }
                return RestoreRecordFromUndoLog(*undo_log, tab_meta);  // 从UndoLog恢复
            } else {
                link = undo_log->prev_version_;
                continue;
            }
        }

        // 4. 其他情况，继续回溯
        link = undo_log->prev_version_;
    }

    return nullptr;  // 没有可见版本
}

/**
 * @brief 从UndoLog恢复记录
 */
std::unique_ptr<RmRecord> RestoreRecordFromUndoLog(const UndoLog& undo_log, const TabMeta& tab_meta) {
    // 计算记录大小
    size_t record_size = 0;
    for (const auto& col : tab_meta.cols) {
        record_size += col.len;
    }

    auto record = std::make_unique<RmRecord>(record_size);

    // 从UndoLog的tuple_数组中恢复数据
    for (size_t i = 0; i < tab_meta.cols.size() && i < undo_log.tuple_.size(); ++i) {
        const auto& col = tab_meta.cols[i];
        const auto& value = undo_log.tuple_[i];

        if (col.type == TYPE_INT) {
            *(int*)(record->data + col.offset) = value.int_val;
        } else if (col.type == TYPE_FLOAT) {
            *(float*)(record->data + col.offset) = value.float_val;
        } else if (col.type == TYPE_STRING) {
            memcpy(record->data + col.offset, value.str_val.c_str(),
                   std::min(col.len, static_cast<int>(value.str_val.length())));
            // 确保字符串以null结尾
            if (value.str_val.length() < col.len) {
                memset(record->data + col.offset + value.str_val.length(), 0,
                       col.len - value.str_val.length());
            }
        }
    }

    return record;
}