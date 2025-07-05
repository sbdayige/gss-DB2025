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

#include "execution_common.h"
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

        // 返回投影后的记录，但不推进游标（游标由nextTuple推进）
        auto &record = buffer_[tuple_idx_];
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
        bool end = tuple_idx_ >= buffer_.size();
        return end;
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

                // 1. 获取对当前事务可见的记录版本
                auto visible_record = GetVisibleVersion(rid, context_->txn_);
                if (!visible_record) {
                    // 没有可见版本（可能被删除或不可见），跳过
                    scan.next();
                    continue;
                }

                // 2. 检查是否满足WHERE条件
                if (EvaluateConditions(*visible_record, conds_, tab_)) {
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
        // 检查记录版本是否对当前事务可见
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            return true;  // 没有版本链，基础版本可见
        }

        // 检查版本链中的事务状态
        auto prev_txn_id = version_link->prev_.prev_txn_;
        if (prev_txn_id == INVALID_TXN_ID) {
            return true;  // 没有前一个事务，基础版本可见
        }

        if (prev_txn_id == txn->get_transaction_id()) {
            // 自己的修改，检查是否被删除
            auto undo_log = GetUndoLog(version_link->prev_);
            if (undo_log && undo_log->is_deleted_) {
                return false;  // 被自己删除了，不可见
            }
            return true;  // 自己的修改可见
        }

        // 检查其他事务的状态
        auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);
        if (!prev_txn) {
            // 事务不存在，在无法确定提交时间戳的情况下保守处理
            // 假设不可见，避免不可重复读问题
            return false;
        }

        if (prev_txn->get_state() == TransactionState::COMMITTED) {
            if (prev_txn->get_commit_ts() <= txn->get_read_ts()) {
                // 在快照时间内，检查是否是删除操作
                auto undo_log = GetUndoLog(version_link->prev_);
                if (undo_log && undo_log->is_deleted_) {
                    return false;  // 已被删除，不可见
                }
                return true;  // 已提交且在快照时间内，可见
            } else {
                // 提交时间戳大于读时间戳，不可见，需要查看更早版本
                return false;
            }
        } else if (prev_txn->get_state() == TransactionState::ABORTED) {
            // 已回滚的事务，不可见，需要查看更早版本
            return false;
        } else {
            // 未提交的事务（DEFAULT状态），不可见，需要查看更早版本
            return false;
        }
    }

    std::unique_ptr<RmRecord> GetVisibleVersion(const Rid &rid, Transaction *txn) {
        // 获取对当前事务可见的记录版本
        auto version_link = txn_mgr_->GetVersionLink(rid);

        if (!version_link.has_value()) {
            // 没有版本链，返回基础版本
            return fh_->get_record(rid, context_);
        }

        if (version_link->in_progress_) {
            // 这是一个正在进行中的操作
            auto prev_txn_id = version_link->prev_.prev_txn_;
            if (prev_txn_id == txn->get_transaction_id()) {
                // 是自己的事务，需要检查操作类型
                auto undo_log = GetUndoLog(version_link->prev_);
                if (undo_log) {
                    if (undo_log->is_deleted_) {
                        return nullptr;
                    } else {
                        return fh_->get_record(rid, context_);
                    }
                }
                return fh_->get_record(rid, context_);
            } else {
                auto undo_log = GetUndoLog(version_link->prev_);
                if (undo_log) {
                    if (!undo_log->is_deleted_ && undo_log->prev_version_.prev_txn_ == INVALID_TXN_ID) {
                        return nullptr;
                    } else {
                        // 这是未提交的UPDATE/DELETE操作，返回UndoLog中存储的旧值
                        return RestoreRecordFromUndoLog(*undo_log);
                    }
                }
                // 如果获取不到UndoLog，不可见
                return nullptr;
            }
        }

        auto current_link = version_link->prev_;

        while (current_link.prev_txn_ != INVALID_TXN_ID) {
            auto prev_txn_id = current_link.prev_txn_;

            if (prev_txn_id == txn->get_transaction_id()) {
                // 自己的修改，返回对应版本
                auto undo_log = GetUndoLog(current_link);
                if (undo_log) {
                    if (undo_log->is_deleted_) {
                        return nullptr;  // 被自己删除，不可见
                    } else {
                        // 自己的修改，返回当前版本
                        return fh_->get_record(rid, context_);
                    }
                }
                break;
            }

            // 检查其他事务
            auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);

            if (prev_txn) {
                if (prev_txn->get_state() == TransactionState::COMMITTED) {
                    if (prev_txn->get_commit_ts() <= txn->get_read_ts()) {
                        // 找到可见的已提交版本
                        auto undo_log = GetUndoLog(current_link);
                        if (undo_log) {
                            if (undo_log->is_deleted_) {
                                return nullptr;  // 被删除，不可见
                            } else {
                                            // 记录可见，返回对应版本
                                // 检查这是否是最新版本（版本链头部）
                                if (current_link.prev_txn_ == version_link->prev_.prev_txn_ &&
                                    current_link.prev_log_idx_ == version_link->prev_.prev_log_idx_) {
                                    // 这是最新版本，可以读取物理记录
                                    return fh_->get_record(rid, context_);
                                } else {
                                    // 这是历史版本，需要从UndoLog重构数据
                                    return RestoreRecordFromUndoLog(*undo_log);
                                }
                            }
                        }
                        // 如果没有UndoLog，读取物理记录
                        return fh_->get_record(rid, context_);
                    } else {
                        auto undo_log = GetUndoLog(current_link);
                        if (undo_log && undo_log->prev_version_.IsValid()) {
                            current_link = undo_log->prev_version_;
                            continue;
                        } else {
                            break;
                        }
                    }
                } else if (prev_txn->get_state() == TransactionState::ABORTED) {
                    // 已回滚的事务，忽略这个版本，继续查找更早版本
                    auto undo_log = GetUndoLog(current_link);
                    if (undo_log && undo_log->prev_version_.IsValid()) {
                        current_link = undo_log->prev_version_;
                        continue;
                    } else {
                        break;
                    }
                } else {
                    // 未提交的事务（DEFAULT状态），不可见
                    auto undo_log = GetUndoLog(current_link);
                    if (undo_log && undo_log->prev_version_.IsValid()) {
                        current_link = undo_log->prev_version_;
                        continue;
                    } else {
                        break;
                    }
                }
            } else {
                // 事务不存在（可能已经结束）
                auto undo_log = GetUndoLog(current_link);
                if (undo_log) {
                    if (undo_log->prev_version_.IsValid()) {
                        // 有更早版本，继续查找
                        current_link = undo_log->prev_version_;
                        continue;
                    } else {
                        // 没有更早版本，从当前UndoLog恢复修改前的版本
                        return RestoreRecordFromUndoLog(*undo_log);
                    }
                } else {
                    // 无法获取UndoLog，中断查找
                    break;
                }
            }
        }

        // 如果遍历完版本链都没找到可见版本，说明所有修改都不可见
        if (version_link.has_value()) {
            auto first_undo_log = GetUndoLog(version_link->prev_);
            if (first_undo_log) {
                return nullptr;
            }
        }
        return nullptr;
    }

    std::unique_ptr<UndoLog> GetUndoLog(const UndoLink &link) {
        auto undo_log_opt = txn_mgr_->GetUndoLogOptional(link);
        if (undo_log_opt.has_value()) {
            return std::make_unique<UndoLog>(undo_log_opt.value());
        }
        return nullptr;
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

    std::unique_ptr<RmRecord> RestoreRecordFromUndoLog(const UndoLog &undo_log) {
        // 从UndoLog恢复记录的原始版本
        size_t record_size = 0;
        for (const auto &col : tab_.cols) {
            record_size += col.len;
        }

        auto record = std::make_unique<RmRecord>(record_size);

        // 从UndoLog的tuple_数组中恢复数据
        for (size_t i = 0; i < tab_.cols.size() && i < undo_log.tuple_.size(); ++i) {
            const auto &col = tab_.cols[i];
            const auto &value = undo_log.tuple_[i];

            if (col.type == TYPE_INT) {
                *(int *)(record->data + col.offset) = value.int_val;
            } else if (col.type == TYPE_FLOAT) {
                *(float *)(record->data + col.offset) = value.float_val;
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
};
