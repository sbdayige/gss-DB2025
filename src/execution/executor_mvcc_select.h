/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to theions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <set>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class MvccSelectExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                                    // 表的元数据
    std::vector<Condition> conds_;                   // select的where条件
    RmFileHandle *fh_;                               // 表的数据文件句柄
    std::string tab_name_;                           // 表名称
    std::vector<ColMeta> cols_;                      // 需要select的字段
    size_t tuple_idx_;                               // 当前遍历到的tuple位置
    std::vector<std::unique_ptr<RmRecord>> buffer_;  // 符合条件的记录缓冲区
    std::vector<Rid> rids_;                          // 对应记录的rid
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;  // 事务管理器指针
    bool fed_to_upper_;            // 是否已经被上层执行器消费

   public:
    MvccSelectExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name, std::vector<Condition> conds,
                       std::vector<ColMeta> cols, Context *context) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_name_ = tab_name;

        tab_ = sm_manager_->db_.get_table(tab_name);

        fh_ = sm_manager_->fhs_.at(tab_name).get();

        conds_ = conds;
        cols_ = cols;
        context_ = context;
        tuple_idx_ = 0;
        fed_to_upper_ = false;

        // 扫描表并过滤符合条件且可见的记录
        ScanAndFilter();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (tuple_idx_ >= buffer_.size()) {
            return nullptr;
        }

        // 返回投影后的记录
        auto &record = buffer_[tuple_idx_++];
        return ProjectRecord(*record);
    }

    size_t tupleLen() const {
        size_t len = 0;
        for (auto &col : cols_) {
            len += col.len;
        }
        return len;
    }

    const std::vector<ColMeta> &cols() const {
        return cols_;
    }

    Rid &rid() override {
        if (tuple_idx_ < rids_.size()) {
            return rids_[tuple_idx_];
        }
        static Rid dummy_rid;
        return dummy_rid;
    }

    void beginTuple() override {
        tuple_idx_ = 0;
    }

    void nextTuple() override {
        if (tuple_idx_ < buffer_.size()) {
            tuple_idx_++;
        }
    }

    bool is_end() const override {
        return tuple_idx_ >= buffer_.size();
    }

    std::string getType() { return "MvccSelectExecutor"; }

   private:
    void ScanAndFilter() {
        // MVCC Select Logic: 扫描表中的所有记录，过滤出可见且符合条件的记录

        try {
            // 检查文件句柄状态
            auto file_hdr = fh_->get_file_hdr();
            RmScan scan(fh_);

            // 手动检查第一页的记录
            if (file_hdr.num_pages > 1) {  // 至少有数据页
                try {
                    auto page_handle = fh_->fetch_page_handle(1);
                    auto bitmap = page_handle.bitmap;
                    for (int i = 0; i < file_hdr.num_records_per_page; i++) {
                        std::cout << (Bitmap::is_set(bitmap, i) ? "1" : "0");
                    }
                    std::cout << std::endl;

                    int next_bit = Bitmap::next_bit(true, bitmap, file_hdr.num_records_per_page, 0);

                    // Note: We won't manually unpin as it should be handled automatically
                } catch (const std::exception &e) {
                }
            }

            int record_count = 0;
            const int MAX_RECORDS = 100;  // 严格限制最大记录数

            while (!scan.is_end() && record_count < MAX_RECORDS) {
                record_count++;

                auto rid = scan.rid();

                // 1. 检查记录的MVCC可见性
                if (!IsVisible(rid, context_->txn_)) {
                    scan.next();
                    continue;
                }
                auto visible_record = GetVisibleVersion(rid, context_->txn_);
                if (!visible_record) {
                    scan.next();
                    continue;
                }
                if (EvaluateConditions(*visible_record, conds_)) {
                    buffer_.push_back(std::move(visible_record));
                    rids_.push_back(rid);
                }

                scan.next();
            }
        } catch (const std::exception &e) {
            throw;
        }
    }

    bool IsVisible(const Rid &rid, Transaction *txn) {
        std::cout << "DEBUG: Checking visibility for rid: " << rid.page_no << "," << rid.slot_no << " in txn: " << txn->get_transaction_id() << std::endl;

        // 检查记录版本是否对当前事务可见
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            std::cout << "DEBUG: No version link, record is visible" << std::endl;
            // 没有版本链，说明是基础版本，可见
            return true;
        }

        // 检查版本链中的事务状态
        auto prev_txn_id = version_link->prev_.prev_txn_;
        std::cout << "DEBUG: Version link prev_txn_id: " << prev_txn_id << std::endl;

        if (prev_txn_id == INVALID_TXN_ID) {
            std::cout << "DEBUG: Invalid txn_id, record is visible" << std::endl;
            return true;
        }

        if (prev_txn_id == txn->get_transaction_id()) {
            std::cout << "DEBUG: Same transaction, checking delete status..." << std::endl;
            // 自己的修改，检查是否被删除
            auto undo_log = GetUndoLog(version_link->prev_);
            if (undo_log && undo_log->is_deleted_) {
                std::cout << "DEBUG: Record deleted by same transaction, not visible" << std::endl;
                return false;  // 被自己删除了，不可见
            }
            std::cout << "DEBUG: Record modified by same transaction, visible" << std::endl;
            return true;
        }

        // 检查其他事务的状态
        auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);
        if (!prev_txn) {
            std::cout << "DEBUG: Previous transaction not found, record is visible" << std::endl;
            // 事务不存在，可能已经提交或回滚
            return true;
        }

        std::cout << "DEBUG: Previous transaction state: " << (int)prev_txn->get_state() << std::endl;

        if (prev_txn->get_state() == TransactionState::COMMITTED) {
            // 已提交的事务，检查时间戳
            std::cout << "DEBUG: Previous transaction committed, checking timestamps..." << std::endl;
            std::cout << "DEBUG: prev_commit_ts: " << prev_txn->get_commit_ts() << ", current_read_ts: " << txn->get_read_ts() << std::endl;

            if (prev_txn->get_commit_ts() <= txn->get_read_ts()) {
                // 检查是否是删除操作
                auto undo_log = GetUndoLog(version_link->prev_);
                if (undo_log && undo_log->is_deleted_) {
                    std::cout << "DEBUG: Record was deleted by committed transaction, not visible" << std::endl;
                    return false;  // 已被删除，不可见
                }
                std::cout << "DEBUG: Record modified by committed transaction, visible" << std::endl;
                return true;
            }
            std::cout << "DEBUG: Timestamp conflict, record not visible" << std::endl;
            return false;  // 时间戳冲突，不可见
        }

        std::cout << "DEBUG: Previous transaction not committed, record not visible" << std::endl;
        // 未提交的事务，不可见
        return false;
    }

    std::unique_ptr<RmRecord> GetVisibleVersion(const Rid &rid, Transaction *txn) {
        // 获取对当前事务可见的记录版本
        auto version_link = txn_mgr_->GetVersionLink(rid);

        if (!version_link.has_value()) {
            // 没有版本链，返回基础版本
            return fh_->get_record(rid, context_);
        }

        // 遍历版本链，找到可见的版本
        auto current_link = version_link->prev_;

        while (current_link.prev_txn_ != INVALID_TXN_ID) {
            auto prev_txn_id = current_link.prev_txn_;

            if (prev_txn_id == txn->get_transaction_id()) {
                // 自己的修改，返回对应版本
                auto undo_log = GetUndoLog(current_link);
                if (undo_log) {
                    if (undo_log->is_deleted_) {
                        return nullptr;  // 被删除
                    } else {
                        // 返回更新前的版本（使用tuple_test_）
                        return std::make_unique<RmRecord>(*undo_log->tuple_test_);
                    }
                }
                break;
            }

            // 检查其他事务
            auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);

            if (prev_txn) {
                if (prev_txn->get_state() == TransactionState::COMMITTED &&
                    prev_txn->get_commit_ts() <= txn->get_read_ts()) {
                    // 找到可见的已提交版本
                    auto undo_log = GetUndoLog(current_link);
                    if (undo_log) {
                        if (undo_log->is_deleted_) {
                            return nullptr;  // 被删除
                        } else {
                            return std::make_unique<RmRecord>(*undo_log->tuple_test_);
                        }
                    }
                    break;
                }
            }

            // 继续查找更早的版本
            auto undo_log = GetUndoLog(current_link);
            if (undo_log && undo_log->prev_version_.prev_log_idx_ >= 0) {
                current_link = undo_log->prev_version_;
            } else {
                break;
            }
        }

        // 返回基础版本
        return fh_->get_record(rid, context_);
    }

    std::unique_ptr<UndoLog> GetUndoLog(const UndoLink &link) {
        auto undo_log_opt = txn_mgr_->GetUndoLogOptional(link);
        if (undo_log_opt.has_value()) {
            return std::make_unique<UndoLog>(undo_log_opt.value());
        }
        return nullptr;
    }

    bool EvaluateConditions(const RmRecord &record, const std::vector<Condition> &conditions) {
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (!EvaluateCondition(record, conditions[i])) {
                return false;
            }
        }
        return true;
    }

    bool EvaluateCondition(const RmRecord &record, const Condition &cond) {
        auto lhs_col = tab_.get_col(cond.lhs_col.col_name);

        char *lhs_data = record.data + lhs_col->offset;

        char *rhs_data;
        ColType rhs_type;

        if (cond.is_rhs_val) {
            if (!cond.rhs_val.raw) {
                // 初始化 raw 数据
                const_cast<Value &>(cond.rhs_val).init_raw(lhs_col->len);
            }
            rhs_data = cond.rhs_val.raw->data;
            rhs_type = cond.rhs_val.type;
        } else {
            auto rhs_col = tab_.get_col(cond.rhs_col.col_name);
            rhs_data = record.data + rhs_col->offset;
            rhs_type = rhs_col->type;
        }

        if (lhs_col->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
        }

        int cmp_result = 0;
        if (lhs_col->type == TYPE_INT) {
            int lhs_val = *(int *)lhs_data;
            int rhs_val = *(int *)rhs_data;
            cmp_result = (lhs_val < rhs_val) ? -1 : (lhs_val > rhs_val) ? 1
                                                                        : 0;
        } else if (lhs_col->type == TYPE_FLOAT) {
            float lhs_val = *(float *)lhs_data;
            float rhs_val = *(float *)rhs_data;
            cmp_result = (lhs_val < rhs_val) ? -1 : (lhs_val > rhs_val) ? 1
                                                                        : 0;
        } else if (lhs_col->type == TYPE_STRING) {
            cmp_result = memcmp(lhs_data, rhs_data, lhs_col->len);
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

    std::unique_ptr<RmRecord> ProjectRecord(const RmRecord &record) {
        // 投影：只返回需要的列
        size_t tuple_size = tupleLen();
        auto projected_record = std::make_unique<RmRecord>(tuple_size);

        size_t offset = 0;
        for (auto &col : cols_) {
            memcpy(projected_record->data + offset, record.data + col.offset, col.len);
            offset += col.len;
        }

        return projected_record;
    }
};
