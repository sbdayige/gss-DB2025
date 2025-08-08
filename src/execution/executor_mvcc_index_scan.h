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

#include <iostream>
#include <queue>
#include <unordered_set>

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

extern SmManager sm_manager;
extern TransactionManager txn_manager;

class MvccIndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;          // 表名称
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // scan的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<ColMeta> cols_;     // 需要读取的字段

    // 流式处理相关（替代批量缓冲区）
    std::unique_ptr<IxScan> index_scan_;        // 索引扫描迭代器
    std::unique_ptr<RmRecord> current_record_;  // 当前记录
    Rid current_rid_;                           // 当前记录的RID

    // 索引相关
    std::string index_name_;           // 使用的索引名称
    IxIndexHandle *ih_;                // 索引句柄
    std::vector<ColMeta> index_cols_;  // 索引列
    bool fed_to_upper_;                // 是否已经被上层执行器消费

    // 索引扫描范围
    char *lower_key_;  // 下界键值
    char *upper_key_;  // 上界键值
    bool has_lower_;   // 是否有下界
    bool has_upper_;   // 是否有上界

    // 简化的流式处理 - 移除不必要的缓冲
    bool use_simple_streaming_;  // 使用简单流式处理

   public:
    MvccIndexScanExecutor(const std::string &tab_name, std::vector<Condition> conds,
                          std::vector<ColMeta> cols, const std::string &index_name, Context *context) {
        tab_name_ = tab_name;
        tab_ = sm_manager.db_.get_table(tab_name);
        fh_ = sm_manager.fhs_.at(tab_name).get();
        conds_ = conds;
        cols_ = cols;
        context_ = context;
        fed_to_upper_ = false;
        index_name_ = index_name;
        lower_key_ = nullptr;
        upper_key_ = nullptr;
        has_lower_ = false;
        has_upper_ = false;
        use_simple_streaming_ = true;  // 使用简单流式处理

        ih_ = sm_manager.ihs_.at(index_name).get();

        // 获取索引列信息
        for (auto &index : tab_.indexes) {
            if (sm_manager.get_ix_manager()->get_index_name(tab_name_, index.cols) == index_name_) {
                for (int i = 0; i < index.col_num; ++i) {
                    index_cols_.push_back(index.cols[i]);
                }
                break;
            }
        }
        // 构造索引扫描范围（保留原有逻辑）
        ConstructScanRange(lower_key_, upper_key_, has_lower_, has_upper_);
    }

    ~MvccIndexScanExecutor() {
        if (lower_key_) delete[] lower_key_;
        if (upper_key_) delete[] upper_key_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!current_record_) {
            return nullptr;
        }

        // 返回投影后的当前记录
        auto result = ProjectRecord(*current_record_);
        return result;
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
        if (has_lower_ && has_upper_) {
            auto lower_iid = ih_->lower_bound(lower_key_);
            auto upper_iid = ih_->upper_bound(upper_key_);
            index_scan_ = std::make_unique<IxScan>(ih_, lower_iid, upper_iid, sm_manager.get_bpm());
        } else if (has_lower_) {
            auto lower_iid = ih_->lower_bound(lower_key_);
            auto upper_iid = ih_->leaf_end();
            index_scan_ = std::make_unique<IxScan>(ih_, lower_iid, upper_iid, sm_manager.get_bpm());
        } else if (has_upper_) {
            auto lower_iid = ih_->leaf_begin();
            auto upper_iid = ih_->upper_bound(upper_key_);
            index_scan_ = std::make_unique<IxScan>(ih_, lower_iid, upper_iid, sm_manager.get_bpm());
        } else {
            index_scan_ = std::make_unique<IxScan>(ih_, ih_->leaf_begin(), ih_->leaf_end(), sm_manager.get_bpm());
        }
        FindNextValidRecord();
    }

    void nextTuple() override {
        if (!index_scan_) {
            return;
        }

        // 先移动到下一个索引位置，然后查找下一个有效记录
        index_scan_->next();
        FindNextValidRecord();
    }

    bool is_end() const override {
        // 简化结束条件判断
        return current_record_ == nullptr &&
               (!index_scan_ || index_scan_->is_end());
    }

    std::string getType() { return "MvccIndexScanExecutor"; }

   private:
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

    void FindNextValidRecord() {
        current_record_.reset();

        if (!index_scan_) {
            return;
        }

        while (index_scan_ && !index_scan_->is_end()) {
            current_rid_ = index_scan_->rid();

            // 使用按页读取优化 - 避免重复的页面锁获取
            try {
                auto page = fh_->AcquirePageReadLock(current_rid_);
                auto record = GetVisibleRecordFromPage(current_rid_, page->get_data(),
                                                       context_->txn_, &txn_manager, fh_, context_, tab_);
                fh_->buffer_pool_manager_->unpin_page(page->get_page_id(), false);

                if (record && EvaluateConditions(*record, conds_, tab_)) {
                    current_record_ = std::move(record);
                    return;
                }
            } catch (const RecordNotFoundError &) {
                // 记录不存在，继续查找下一个
            } catch (const PageNotExistError &) {
                // 页面不存在，继续查找下一个
            }

            index_scan_->next();
        }
    }
};
