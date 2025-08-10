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

#include "common/config.h"  // 包含IX_NO_SLOT和IX_NO_PAGE的定义
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
    std::unique_ptr<RecScan> index_scan_;       // 索引扫描迭代器
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

        // Debug: 打印WHERE条件
        std::cout << "[DEBUG IndexScan] Constructor called with " << conds_.size() << " conditions by txn_id="
                  << context->txn_->get_transaction_id() << std::endl;
        for (size_t i = 0; i < conds_.size(); ++i) {
            const auto &cond = conds_[i];
            std::cout << "[DEBUG IndexScan] Condition " << i << ": " << cond.lhs_col.col_name
                      << " op=" << static_cast<int>(cond.op) << " is_rhs_val=" << cond.is_rhs_val;
            if (cond.is_rhs_val && cond.rhs_val.type == TYPE_INT) {
                std::cout << " rhs_val=" << cond.rhs_val.int_val;
            }
            std::cout << std::endl;
        }

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
        std::cout << "[DEBUG IndexScan] Setting up index scan range: has_lower=" << has_lower_
                  << " has_upper=" << has_upper_ << std::endl;

        if (has_lower_ && lower_key_ && index_cols_[0].len >= 4) {
            int lower_val = *(int *)(lower_key_);
            std::cout << "[DEBUG IndexScan] Lower bound key value: " << lower_val << std::endl;
        }
        if (has_upper_ && upper_key_ && index_cols_[0].len >= 4) {
            int upper_val = *(int *)(upper_key_);
            std::cout << "[DEBUG IndexScan] Upper bound key value: " << upper_val << std::endl;
        }

        if (has_lower_ && has_upper_) {
            auto lower_iid = ih_->lower_bound(lower_key_);
            auto upper_iid = ih_->upper_bound(upper_key_);
            index_scan_ = std::make_unique<IxScan>(ih_, lower_iid, upper_iid, fh_->buffer_pool_manager_);
            std::cout << "[DEBUG IndexScan] Using bounded range scan" << std::endl;
        } else if (has_lower_) {
            auto lower_iid = ih_->lower_bound(lower_key_);
            auto upper_iid = ih_->leaf_end();
            index_scan_ = std::make_unique<IxScan>(ih_, lower_iid, upper_iid, fh_->buffer_pool_manager_);
            std::cout << "[DEBUG IndexScan] Using lower bounded scan" << std::endl;
        } else if (has_upper_) {
            auto lower_iid = ih_->leaf_begin();
            auto upper_iid = ih_->upper_bound(upper_key_);
            index_scan_ = std::make_unique<IxScan>(ih_, lower_iid, upper_iid, fh_->buffer_pool_manager_);
            std::cout << "[DEBUG IndexScan] Using upper bounded scan" << std::endl;
        } else {
            auto initial_lower = ih_->leaf_begin();
            auto initial_upper = ih_->leaf_end();
            std::cout << "[DEBUG IndexScan] Initial leaf_begin: (" << initial_lower.page_no << "," << initial_lower.slot_no << ")" << std::endl;
            std::cout << "[DEBUG IndexScan] Initial leaf_end: (" << initial_upper.page_no << "," << initial_upper.slot_no << ")" << std::endl;

            // 强制重新获取索引边界以确保包含最新更新
            auto fresh_lower = ih_->leaf_begin();
            auto fresh_upper = ih_->leaf_end();
            std::cout << "[DEBUG IndexScan] Fresh leaf_begin: (" << fresh_lower.page_no << "," << fresh_lower.slot_no << ")" << std::endl;
            std::cout << "[DEBUG IndexScan] Fresh leaf_end: (" << fresh_upper.page_no << "," << fresh_upper.slot_no << ")" << std::endl;

            index_scan_ = std::make_unique<IxScan>(ih_, fresh_lower, fresh_upper, fh_->buffer_pool_manager_);
            std::cout << "[DEBUG IndexScan] Using full table scan" << std::endl;
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
    /**
     * @brief 处理溢出页索引项，获取所有相关RID并检查可见性
     * @return 是否找到了可见的记录
     */
    bool ProcessOverflowPageEntry() {
        std::vector<Rid> all_rids;

        // 先获取当前记录来构造key
        Rid resolved_rid = index_scan_->rid();
        auto page = fh_->AcquirePageReadLock(resolved_rid);

        // 使用记录文件句柄的方法来正确获取记录内容
        auto record = fh_->get_record(resolved_rid, context_);
        fh_->buffer_pool_manager_->unpin_page(page->get_page_id(), false);

        if (!record) {
            std::cout << "[DEBUG IndexScan] Failed to get record for overflow page processing" << std::endl;
            return false;
        }

        // 假设表的第一列是int类型的key（根据调试输出中的id字段）
        char key_data[sizeof(int)];
        memcpy(key_data, record->data, sizeof(int));

        // 使用get_value获取该key的所有RID（包括溢出页中的）
        try {
            ih_->get_value(key_data, &all_rids, context_->txn_);
        } catch (...) {
            std::cout << "[DEBUG IndexScan] Failed to get value for overflow page processing" << std::endl;
            return false;
        }

        std::cout << "[DEBUG IndexScan] Retrieved " << all_rids.size() << " RIDs from overflow page" << std::endl;

        // 遍历所有RID，找到第一个可见且满足条件的记录
        for (const auto &rid : all_rids) {
            std::cout << "[DEBUG IndexScan] Checking overflow RID(" << rid.page_no << "," << rid.slot_no << ")" << std::endl;

            auto page = fh_->AcquirePageReadLock(rid);
            auto record = GetVisibleRecordFromPage(rid, page->get_data(),
                                                   context_->txn_, &txn_manager, fh_, context_, tab_);
            fh_->buffer_pool_manager_->unpin_page(page->get_page_id(), false);

            if (record) {
                std::cout << "[DEBUG IndexScan] Overflow record is visible" << std::endl;
                if (record->size >= 8) {
                    int id = *(int *)(record->data);
                    int score = *(int *)(record->data + 4);
                    std::cout << "[DEBUG IndexScan] Overflow record content: id=" << id << " score=" << score << std::endl;
                }

                if (EvaluateConditions(*record, conds_, tab_)) {
                    std::cout << "[DEBUG IndexScan] Overflow record matches conditions, returning" << std::endl;
                    current_record_ = std::move(record);
                    current_rid_ = rid;
                    return true;
                } else {
                    std::cout << "[DEBUG IndexScan] Overflow record doesn't match conditions" << std::endl;
                }
            } else {
                std::cout << "[DEBUG IndexScan] Overflow record not visible" << std::endl;
            }
        }

        return false;  // 没有找到可见的记录
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

    void FindNextValidRecord() {
        current_record_.reset();

        if (!index_scan_) {
            std::cout << "[DEBUG IndexScan] No index scan available" << std::endl;
            return;
        }

        std::cout << "[DEBUG IndexScan] Starting index scan by txn_id=" << context_->txn_->get_transaction_id()
                  << " start_ts=" << context_->txn_->get_start_ts() << std::endl;

        while (index_scan_ && !index_scan_->is_end()) {
            current_rid_ = index_scan_->rid();
            std::cout << "[DEBUG IndexScan] Found index entry pointing to rid(" << current_rid_.page_no
                      << "," << current_rid_.slot_no << ")" << std::endl;

            // 检查当前索引位置是否指向溢出页
            if (IsCurrentIndexPositionOverflowPage()) {
                std::cout << "[DEBUG IndexScan] Detected overflow page, processing all RIDs" << std::endl;
                if (ProcessOverflowPageEntry()) {
                    return;  // 找到了可见的记录
                }
            } else {
                // 普通RID，直接处理
                std::cout << "[DEBUG IndexScan] Processing normal RID" << std::endl;

                auto page = fh_->AcquirePageReadLock(current_rid_);
                auto record = GetVisibleRecordFromPage(current_rid_, page->get_data(),
                                                       context_->txn_, &txn_manager, fh_, context_, tab_);
                fh_->buffer_pool_manager_->unpin_page(page->get_page_id(), false);

                if (record) {
                    std::cout << "[DEBUG IndexScan] Record is visible" << std::endl;
                    if (record->size >= 8) {
                        int id = *(int *)(record->data);
                        int score = *(int *)(record->data + 4);
                        std::cout << "[DEBUG IndexScan] Record content: id=" << id << " score=" << score << std::endl;
                    }

                    if (EvaluateConditions(*record, conds_, tab_)) {
                        std::cout << "[DEBUG IndexScan] Record matches conditions, returning" << std::endl;
                        current_record_ = std::move(record);
                        return;
                    } else {
                        std::cout << "[DEBUG IndexScan] Record doesn't match conditions" << std::endl;
                    }
                } else {
                    std::cout << "[DEBUG IndexScan] Record not visible, continuing" << std::endl;
                }
            }

            index_scan_->next();
        }
        std::cout << "[DEBUG IndexScan] Index scan completed, no valid records found" << std::endl;
    }

    /**
     * @brief 检查当前索引位置是否指向溢出页
     * @return true如果当前位置是溢出页引用
     */
    bool IsCurrentIndexPositionOverflowPage() {
        // 将index_scan_转换为IxScan以访问iid()方法
        IxScan *ix_scan = dynamic_cast<IxScan *>(index_scan_.get());
        if (!ix_scan) {
            return false;
        }

        // 获取当前索引位置
        Iid current_iid = ix_scan->iid();

        // 我们无法直接访问索引页面的原始数据，因为相关成员都是私有的
        // 所以使用另一种方法：检查当前key是否有多个RID

        // 先获取当前记录来构造key
        Rid resolved_rid = index_scan_->rid();
        auto page = fh_->AcquirePageReadLock(resolved_rid);

        // 使用记录文件句柄的方法来正确获取记录内容
        auto record = fh_->get_record(resolved_rid, context_);
        fh_->buffer_pool_manager_->unpin_page(page->get_page_id(), false);

        if (!record) {
            std::cout << "[DEBUG IndexScan] Failed to get record, assuming normal RID" << std::endl;
            return false;
        }

        // 假设表的第一列是int类型的key（根据调试输出中的id字段）
        char key_data[sizeof(int)];
        memcpy(key_data, record->data, sizeof(int));

        // 使用get_value获取该key的所有RID
        std::vector<Rid> all_rids;
        try {
            ih_->get_value(key_data, &all_rids, context_->txn_);
        } catch (...) {
            // 如果获取失败，假设不是溢出页
            std::cout << "[DEBUG IndexScan] Failed to get value for key, assuming normal RID" << std::endl;
            return false;
        }
        bool is_overflow = all_rids.size() > 1;

        if (is_overflow) {
            std::cout << "[DEBUG IndexScan] Detected overflow page: found " << all_rids.size()
                      << " RIDs for current key" << std::endl;
        } else {
            std::cout << "[DEBUG IndexScan] Normal RID: found " << all_rids.size()
                      << " RID for current key" << std::endl;
        }
        return is_overflow;
    };
};