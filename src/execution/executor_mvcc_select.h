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
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // select的where条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::string tab_name_;                      // 表名称
    std::vector<ColMeta> cols_;                 // 需要select的字段
    std::unique_ptr<RmScan> scan_;              // 表扫描迭代器（流式模式）
    std::unique_ptr<RmRecord> current_record_;  // 当前记录
    Rid current_rid_;                           // 当前记录的RID
    bool fed_to_upper_;                         // 是否已经被上层执行器消费

   public:
    MvccSelectExecutor(const std::string &tab_name, std::vector<Condition> conds,
                       std::vector<ColMeta> cols, Context *context) {
        tab_name_ = tab_name;
        tab_ = sm_manager.db_.get_table(tab_name);
        fh_ = sm_manager.get_file_handle(tab_name);
        conds_ = conds;
        cols_ = cols;
        context_ = context;
        fed_to_upper_ = false;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!current_record_) {
            return nullptr;
        }

        // 返回投影后的当前记录
        return ProjectRecord(*current_record_);
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
        return current_rid_;
    }

    void beginTuple() override {
        // 创建扫描迭代器
        scan_ = std::make_unique<RmScan>(fh_);

        // 查找第一个满足条件的记录
        FindNextValidRecord();
    }

    void nextTuple() override {
        if (!scan_) {
            return;
        }

        // 移动到下一个记录位置
        if (!scan_->is_end()) {
            scan_->next();
        }
        FindNextValidRecord();
    }

    bool is_end() const override {
        bool end = !scan_ || scan_->is_end() || !current_record_;
        return end;
    }

    std::string getType() { return "MvccSelectExecutor"; }

   private:
    void FindNextValidRecord() {
        // 清空当前记录
        current_record_.reset();

        // 扫描直到找到符合条件的记录
        int scanned_count = 0;
        while (!scan_->is_end()) {
            current_rid_ = scan_->rid();
            scanned_count++;
            // 1. 获取对当前事务可见的记录版本（保留原有的可见性分析）
            auto visible_record = GetVisibleRecord(current_rid_, context_->txn_, &txn_manager, fh_, context_, tab_);
            if (!visible_record) {
                scan_->next();
                continue;
            }
            if (EvaluateConditions(*visible_record, conds_, tab_)) {
                current_record_ = std::move(visible_record);
                return;  // 找到符合条件的记录，立即返回
            }

            scan_->next();
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
