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

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "recovery/log_manager.h"
#include "system/sm.h"

extern SmManager sm_manager;
extern TransactionManager txn_manager;

class MvccInsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                // 表的元数据
    std::vector<Value> values_;  // 需要插入的数据
    RmFileHandle *fh_;           // 表的数据文件句柄
    std::string tab_name_;       // 表名称
    Rid rid_;                    // 插入的位置，在字段不为空时赋值

   public:
    MvccInsertExecutor(const std::string &tab_name, std::vector<Value> values, Context *context) {
        tab_ = sm_manager.db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        context_ = context;
        fh_ = sm_manager.get_file_handle(tab_name);
    };

    std::unique_ptr<RmRecord> Next() override {
        if (values_.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }

        // 间隙锁冲突检测：检查插入的记录是否与现有的间隙锁冲突
        // 为插入的记录构造一个临时记录用于条件检查
        RmRecord temp_record(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); ++i) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (!val.raw) {
                val.init_raw(col.len);
            }
            memcpy(temp_record.data + col.offset, val.raw->data, col.len);
        }
        // CheckGapLockConflict(temp_record, tab_, fh_, context_);

        for (size_t i = 0; i < values_.size(); ++i) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            // 类型兼容：float->int、int->float自动转换
            if (col.type == TYPE_INT && val.type == TYPE_FLOAT) {
                int v = (int)val.float_val;
                val.set_int(v);
            } else if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                float v = (float)val.int_val;
                val.set_float(v);
            } else if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            if (col.type == TYPE_STRING && val.str_val.size() > static_cast<size_t>(col.len)) {
                throw StringOverflowError();
            }
        }
        std::unique_ptr<RmRecord> rec = std::make_unique<RmRecord>(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); ++i) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (!val.raw) {  // 只在未初始化时才调用init_raw
                val.init_raw(col.len);
            }
            memcpy(rec->data + col.offset, val.raw->data, col.len);
        }

        rid_ = fh_->insert_record(rec->data, context_);

        if (!mvcc_insert_index(tab_, rec, rid_, context_, &txn_manager, &sm_manager)) {
            fh_->delete_record(rid_, context_);
            txn_manager.abort(context_, context_->log_mgr_);
            throw RMDBError("Failed to insert into index, rolled back record insertion at " + getType());
        }

        // WAL: 记录INSERT日志
        // if (context_ && context_->txn_ && context_->log_mgr_) {
        //     InsertLogRecord log_record(context_->txn_->get_transaction_id(), *rec, rid_, tab_name_);
        //     log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
        //     lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
        //     context_->txn_->set_prev_lsn(lsn);
        // }

        WriteRecord *write_record = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, *rec);
        context_->txn_->append_write_record(write_record);

        // MVCC: 创建版本链
        UndoLink insert_link;
        insert_link.prev_txn_ = context_->txn_->get_transaction_id();
        insert_link.prev_log_idx_ = context_->txn_->GetUndoLogNum();  // 将要添加的UndoLog的索引

        // 创建INSERT操作的UndoLog
        UndoLog undo_log = CreateInsertUndoLog(*rec, rid_, tab_, &txn_manager, context_);
        context_->txn_->AppendUndoLog(undo_log);

        // 创建版本链
        VersionUndoLink version_link;
        version_link.prev_ = insert_link;
        version_link.in_progress_ = true;

        txn_manager.UpdateVersionLink(fh_->GetFd(), rid_, version_link, [](std::optional<VersionUndoLink>) { return true; });

        return nullptr;
    }

    Rid &rid() override { return rid_; }
};
