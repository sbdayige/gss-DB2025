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

extern SmManager sm_manager;
extern TransactionManager txn_manager;

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
    bool fed_to_upper_;                              // 是否已经被上层执行器消费

   public:
    MvccSelectExecutor(const std::string &tab_name, std::vector<Condition> conds,
                       std::vector<ColMeta> cols, Context *context) {
        tab_name_ = tab_name;

        tab_ = sm_manager.db_.get_table(tab_name);

        fh_ = sm_manager.get_file_handle(tab_name);

        conds_ = conds;
        cols_ = cols;
        context_ = context;
        tuple_idx_ = 0;
        fed_to_upper_ = false;

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
                } catch (const std::exception &e) {
                }
            }

            while (!scan.is_end()) {
                auto rid = scan.rid();

                // 1. 获取对当前事务可见的记录版本
                auto visible_record = GetVisibleRecord(rid, context_->txn_, &txn_manager, fh_, context_, tab_);
                if (!visible_record) {
                    scan.next();
                    continue;
                }
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
