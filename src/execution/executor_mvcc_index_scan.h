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
#include "system/sm.h"

class MvccIndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                           // 表名称
    TabMeta tab_;                                    // 表的元数据
    std::vector<Condition> conds_;                   // scan的条件
    RmFileHandle *fh_;                               // 表的数据文件句柄
    std::vector<ColMeta> cols_;                      // 需要读取的字段
    size_t tuple_idx_;                               // 当前遍历到的tuple位置
    std::vector<std::unique_ptr<RmRecord>> buffer_;  // 符合条件的记录缓冲区
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;  // 事务管理器指针

    // 索引相关
    std::string index_name_;           // 使用的索引名称
    IxIndexHandle *ih_;                // 索引句柄
    std::vector<ColMeta> index_cols_;  // 索引列
    bool fed_to_upper_;                // 是否已经被上层执行器消费

   public:
    MvccIndexScanExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name, std::vector<Condition> conds,
                          std::vector<ColMeta> cols, const std::string &index_name, Context *context) {
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
        index_name_ = index_name;

        // 获取索引句柄
        ih_ = sm_manager_->ihs_.at(index_name).get();

        // 获取索引列信息
        for (auto &index : tab_.indexes) {
            if (sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols) == index_name_) {
                for (int i = 0; i < index.col_num; ++i) {
                    index_cols_.push_back(index.cols[i]);
                }
                break;
            }
        }

        // 使用索引扫描并过滤符合条件且可见的记录
        IndexScanAndFilter();
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
        static Rid dummy_rid;
        return dummy_rid;
    }

    bool is_end() const {
        return tuple_idx_ >= buffer_.size();
    }

    std::string getType() { return "MvccIndexScanExecutor"; }

   private:
    void IndexScanAndFilter() {
        // 1. 分析查询条件，构造索引扫描范围
        char *lower_key = nullptr;
        char *upper_key = nullptr;
        bool has_lower = false;
        bool has_upper = false;

        ConstructScanRange(lower_key, upper_key, has_lower, has_upper);
        // 2. 执行索引扫描
        std::vector<Rid> candidate_rids;
        if (has_lower && has_upper) {
            // 范围扫描：从lower_key到upper_key
            auto lower_iid = ih_->lower_bound(lower_key);
            auto upper_iid = ih_->upper_bound(upper_key);
            for (IxScan scan(ih_, lower_iid, upper_iid, sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
            }
        } else if (has_lower) {
            // 从lower_key到结尾
            auto lower_iid = ih_->lower_bound(lower_key);
            auto upper_iid = ih_->leaf_end();
            for (IxScan scan(ih_, lower_iid, upper_iid, sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
            }
        } else if (has_upper) {
            // 从开始到upper_key
            auto lower_iid = ih_->leaf_begin();
            auto upper_iid = ih_->upper_bound(upper_key);
            for (IxScan scan(ih_, lower_iid, upper_iid, sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
            }
        } else {
            // 全表索引扫描
            for (IxScan scan(ih_, ih_->leaf_begin(), ih_->leaf_end(), sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
            }
        }

        // 3. 对每个候选RID进行可见性检查和条件过滤
        for (auto &rid : candidate_rids) {
            // 检查记录版本可见性
            if (!IsRecordVisible(rid, context_->txn_, txn_mgr_, fh_)) {
                continue;
            }

            // 获取记录数据
            auto record = GetVisibleRecord(rid, context_->txn_, txn_mgr_, fh_, context_, tab_);
            if (!record) {
                continue;
            }

            // 检查所有WHERE条件（包括非索引条件）
            if (EvaluateConditions(*record, conds_, tab_)) {
                buffer_.push_back(std::make_unique<RmRecord>(*record));
            }
        }

        // 清理内存
        if (lower_key) delete[] lower_key;
        if (upper_key) delete[] upper_key;
    }

    void ConstructScanRange(char *&lower_key, char *&upper_key, bool &has_lower, bool &has_upper) {
        // 根据查询条件构造索引扫描范围
        size_t key_size = 0;
        for (auto &col : index_cols_) {
            key_size += col.len;
        }

        has_lower = false;
        has_upper = false;

        // 分析条件，构造扫描键值
        for (auto &cond : conds_) {
            if (!cond.is_rhs_val) continue;  // 只处理与常量比较的条件

            // 检查条件是否涉及索引列
            bool is_index_col = false;
            size_t col_offset = 0;
            ColMeta *matched_index_col = nullptr;

            for (auto &index_col : index_cols_) {
                if (cond.lhs_col.col_name == index_col.name) {
                    is_index_col = true;
                    matched_index_col = &index_col;
                    break;
                }
                col_offset += index_col.len;
            }

            if (!is_index_col || !matched_index_col) continue;

            // 根据操作符构造扫描范围
            if (cond.op == OP_EQ) {
                // 等值查询
                if (!has_lower) {
                    lower_key = new char[key_size];
                    memset(lower_key, 0, key_size);
                    has_lower = true;
                }
                if (!has_upper) {
                    upper_key = new char[key_size];
                    memset(upper_key, 0, key_size);
                    has_upper = true;
                }

                // 设置键值 - 使用正确的列长度
                cond.rhs_val.init_raw(matched_index_col->len);
                memcpy(lower_key + col_offset, cond.rhs_val.raw->data, matched_index_col->len);
                memcpy(upper_key + col_offset, cond.rhs_val.raw->data, matched_index_col->len);

            } else if (cond.op == OP_GE || cond.op == OP_GT) {
                // 范围查询下界
                if (!has_lower) {
                    lower_key = new char[key_size];
                    memset(lower_key, 0, key_size);
                    has_lower = true;
                }

                cond.rhs_val.init_raw(matched_index_col->len);
                memcpy(lower_key + col_offset, cond.rhs_val.raw->data, matched_index_col->len);

            } else if (cond.op == OP_LE || cond.op == OP_LT) {
                // 范围查询上界
                if (!has_upper) {
                    upper_key = new char[key_size];
                    memset(upper_key, 0, key_size);
                    has_upper = true;
                }

                cond.rhs_val.init_raw(matched_index_col->len);
                memcpy(upper_key + col_offset, cond.rhs_val.raw->data, matched_index_col->len);
            }
        }
    }

    std::unique_ptr<RmRecord> ProjectRecord(const RmRecord &record) {
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
