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
        int scan_cnt = 0;

        if (has_lower && has_upper) {
            // 计算键的总长度
            size_t key_total_len = 0;
            for (auto &col : index_cols_) {
                key_total_len += col.len;
            }

            // 对于等值查询，使用get_value方法更精确
            if (memcmp(lower_key, upper_key, key_total_len) == 0) {
                // 等值查询，直接使用get_value
                std::vector<Rid> exact_rids;
                if (ih_->get_value(lower_key, &exact_rids, context_->txn_)) {
                    for (auto &rid : exact_rids) {
                        candidate_rids.push_back(rid);
                        ++scan_cnt;
                    }
                }
            } else {
                // 范围查询，使用扫描
                auto lower_iid = ih_->lower_bound(lower_key);
                auto upper_iid = ih_->upper_bound(upper_key);
                for (IxScan scan(ih_, lower_iid, upper_iid, sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                    candidate_rids.push_back(scan.rid());
                    ++scan_cnt;
                }
            }
        } else if (has_lower) {
            auto lower_iid = ih_->lower_bound(lower_key);
            auto upper_iid = ih_->leaf_end();
            for (IxScan scan(ih_, lower_iid, upper_iid, sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
                ++scan_cnt;
            }
        } else if (has_upper) {
            auto lower_iid = ih_->leaf_begin();
            auto upper_iid = ih_->upper_bound(upper_key);
            for (IxScan scan(ih_, lower_iid, upper_iid, sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
                ++scan_cnt;
            }
        } else {
            // 全表扫描
            for (IxScan scan(ih_, ih_->leaf_begin(), ih_->leaf_end(), sm_manager_->get_bpm()); !scan.is_end(); scan.next()) {
                candidate_rids.push_back(scan.rid());
                ++scan_cnt;
            }
        }

        // 3. 对每个候选RID进行可见性检查和条件过滤
        int visible_cnt = 0, cond_pass_cnt = 0;
        for (auto &rid : candidate_rids) {
            if (!IsRecordVisible(rid, fh_, txn_mgr_, context_)) {
                continue;
            }
            ++visible_cnt;

            auto record = GetVisibleRecord(rid, context_->txn_, txn_mgr_, fh_, context_, tab_);
            if (!record) {
                continue;
            }

            if (EvaluateConditions(*record, conds_, tab_)) {
                buffer_.push_back(std::make_unique<RmRecord>(*record));
                ++cond_pass_cnt;
            }
        }

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

        // 对于单列索引的等值查询，直接使用get_value方法
        if (index_cols_.size() == 1) {
            for (auto &cond : conds_) {
                if (cond.is_rhs_val && cond.op == OP_EQ &&
                    cond.lhs_col.col_name == index_cols_[0].name) {
                    // 构造查询键
                    lower_key = new char[key_size];
                    upper_key = new char[key_size];

                    auto &val = cond.rhs_val;
                    if (val.raw == nullptr) {
                        val.init_raw(index_cols_[0].len);
                    }

                    memcpy(lower_key, val.raw->data, index_cols_[0].len);
                    memcpy(upper_key, val.raw->data, index_cols_[0].len);

                    has_lower = true;
                    has_upper = true;
                    return;
                }
            }
        }

        // 多列索引或复杂条件的处理
        std::vector<bool> has_eq_condition(index_cols_.size(), false);
        std::vector<Value *> eq_values(index_cols_.size(), nullptr);

        for (auto &cond : conds_) {
            if (!cond.is_rhs_val || cond.op != OP_EQ) continue;

            for (size_t i = 0; i < index_cols_.size(); ++i) {
                if (cond.lhs_col.col_name == index_cols_[i].name) {
                    has_eq_condition[i] = true;
                    eq_values[i] = &cond.rhs_val;
                    break;
                }
            }
        }

        // 检查是否有连续的等值条件从第一列开始
        size_t eq_prefix_len = 0;
        for (size_t i = 0; i < index_cols_.size(); ++i) {
            if (has_eq_condition[i]) {
                eq_prefix_len = i + 1;
            } else {
                break;
            }
        }

        if (eq_prefix_len > 0) {
            // 有等值条件，构造前缀扫描
            lower_key = new char[key_size];
            upper_key = new char[key_size];
            memset(lower_key, 0, key_size);
            memset(upper_key, 0, key_size);
            has_lower = true;
            has_upper = true;

            size_t offset = 0;
            for (size_t i = 0; i < eq_prefix_len; ++i) {
                auto &val = *eq_values[i];
                if (val.raw == nullptr) {
                    val.init_raw(index_cols_[i].len);
                }
                memcpy(lower_key + offset, val.raw->data, index_cols_[i].len);
                memcpy(upper_key + offset, val.raw->data, index_cols_[i].len);
                offset += index_cols_[i].len;
            }

            // 如果还有剩余的列，需要构造正确的前缀范围
            if (eq_prefix_len < index_cols_.size()) {
                // 对于前缀查询，下界的剩余部分设为0（最小值）
                memset(lower_key + offset, 0, key_size - offset);
                
                // 上界需要特殊处理：构造下一个前缀
                // 复制前缀部分到上界
                memcpy(upper_key, lower_key, offset);
                
                // 尝试递增前缀以得到下一个前缀的起始位置
                bool carry = true;
                for (int i = offset - 1; i >= 0 && carry; --i) {
                    if ((unsigned char)upper_key[i] < 0xFF) {
                        upper_key[i]++;
                        carry = false;
                    } else {
                        upper_key[i] = 0;
                    }
                }
                
                if (carry) {
                    // 如果前缀已经是最大值，则使用全局最大上界
                    memset(upper_key, 0xFF, key_size);
                } else {
                    // 将剩余部分设为0，这样就是下一个前缀的最小值
                    memset(upper_key + offset, 0, key_size - offset);
                }
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
