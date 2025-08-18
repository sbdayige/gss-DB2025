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
#include <set>
#include <string>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;   // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;  // 右儿子节点（需要join的表）
    size_t len_;                               // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                // join后获得的记录的字段

    std::vector<Condition> fed_conds_;  // join条件
    bool is_end_;
    JoinType join_type_;  // 连接类型：内连接、半连接等

    // 用于SEMI JOIN的去重逻辑
    std::set<std::string> matched_left_keys_;  // 已匹配的左表记录键值（用于去重）
    int num = 0;
    // 存储当前匹配的记录
    std::unique_ptr<RmRecord> current_record_;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                           std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds,
                           JoinType join_type = INNER_JOIN)  // 默认为内连接
    {
        left_ = std::move(left);
        right_ = std::move(right);
        join_type_ = join_type;
        fed_conds_ = std::move(conds);

        // 根据连接类型设置输出结构
        if (join_type_ == SEMI_JOIN) {
            // SEMI JOIN只输出左表列
            len_ = left_->tupleLen();
            cols_ = left_->cols();
        } else if (join_type_ == ANTI_JOIN) {
            len_ = left_->tupleLen();
            cols_ = left_->cols();
        } else {
            // 其他JOIN类型输出两表列的组合
            len_ = left_->tupleLen() + right_->tupleLen();
            cols_ = left_->cols();
            auto right_cols = right_->cols();
            for (auto& col : right_cols) {
                col.offset += left_->tupleLen();
            }
            cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        }

        is_end_ = false;
        current_record_ = nullptr;
    }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();

        // 对于SEMI JOIN，清空之前的匹配记录集合
        if (join_type_ == SEMI_JOIN || join_type_ == ANTI_JOIN) {
            matched_left_keys_.clear();
        }

        if (left_->is_end()) {
            is_end_ = true;
            return;
        }

        is_end_ = false;
        current_record_ = nullptr;
        find_record();
    }

    void nextTuple() override {
        if (is_end()) {
            return;
        }

        if (join_type_ == SEMI_JOIN) {
            // SEMI JOIN：先移动到下一个左表记录，然后查找匹配
            left_->nextTuple();
            find_semi_join_record();
        } else if (join_type_ == ANTI_JOIN) {
            left_->nextTuple();
            find_anti_join_record();
        } else {
            // 传统JOIN：移动到下一个组合，然后查找满足条件的记录
            right_->nextTuple();
            if (right_->is_end()) {
                left_->nextTuple();
                if (left_->is_end()) {
                    is_end_ = true;
                    current_record_ = nullptr;
                    return;
                }
                right_->beginTuple();
                if (right_->is_end()) {
                    is_end_ = true;
                    current_record_ = nullptr;
                    return;
                }
            }
            // 查找下一个满足条件的记录并存储
            find_regular_join_record();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        // 检查是否已经结束
        if (is_end()) {
            return nullptr;
        }

        if (join_type_ == SEMI_JOIN || join_type_ == ANTI_JOIN) {
            // SEMI JOIN只返回左表记录
            auto left_record = left_->Next();
            if (!left_record) {
                is_end_ = true;
                return nullptr;
            }

            auto record = std::make_unique<RmRecord>(len_);
            memcpy(record->data, left_record->data, left_->tupleLen());
            return record;
        } else {
            // 对于常规JOIN，返回存储的匹配记录
            if (!current_record_) {
                is_end_ = true;
                return nullptr;
            }
            // 返回当前记录并清空，准备查找下一个
            auto result = std::make_unique<RmRecord>(len_);
            memcpy(result->data, current_record_->data, len_);
            current_record_ = nullptr;

            return result;
        }
    }

    bool is_end() const override { return is_end_; }

    Rid& rid() override { return _abstract_rid; }

    void find_record() {
        if (join_type_ == SEMI_JOIN) {
            // SEMI JOIN逻辑：只返回左表中有匹配右表记录的行，每个左表记录最多返回一次
            find_semi_join_record();
        } else if (join_type_ == ANTI_JOIN) {
            find_anti_join_record();
        } else {
            // 传统JOIN逻辑
            find_regular_join_record();
        }
    }

   private:
    void find_anti_join_record() {
        while (!left_->is_end()) {
            // 获取当前左表记录
            auto left_record = left_->Next();

            // 生成左表记录的唯一键值
            std::string record_key = generate_record_key(left_record.get(), left_->cols());

            // 检查是否已经输出过这个记录
            if (matched_left_keys_.find(record_key) != matched_left_keys_.end()) {
                // 已经输出过，跳过这个记录
                left_->nextTuple();
                continue;
            }

            // 检查是否在右表中有匹配
            bool has_match = true;
            right_->beginTuple();

            while (!right_->is_end()) {
                auto right_record = right_->Next();

                // 创建组合记录用于条件评估
                auto combined_record = std::make_unique<RmRecord>(left_->tupleLen() + right_->tupleLen());
                memcpy(combined_record->data, left_record->data, left_->tupleLen());
                memcpy(combined_record->data + left_->tupleLen(), right_record->data, right_->tupleLen());

                // 构建组合列元数据用于条件评估
                std::vector<ColMeta> combined_cols = left_->cols();
                auto right_cols = right_->cols();
                for (auto& col : right_cols) {
                    col.offset += left_->tupleLen();
                }
                combined_cols.insert(combined_cols.end(), right_cols.begin(), right_cols.end());

                // 检查连接条件
                if (fed_conds_.empty() || eval_conds(combined_cols, fed_conds_, combined_record.get())) {
                    has_match = false;
                    break;
                }

                right_->nextTuple();
            }

            if (has_match) {
                // 找到匹配，记录这个键值并停在当前记录
                matched_left_keys_.insert(record_key);
                return;
            }

            // 没有匹配，继续下一个左表记录
            left_->nextTuple();
        }

        // 所有左表记录都检查完毕
        is_end_ = true;
    }
    void find_regular_join_record() {
        int left_count = 0, right_count = 0, combination_count = 0, match_count = 0;

        while (!left_->is_end()) {
            left_count++;
            right_count = 0;
            while (!right_->is_end()) {
                right_count++;
                combination_count++;
                // 创建组合记录用于条件评估
                auto record = std::make_unique<RmRecord>(len_);
                auto left_record = left_->Next();
                auto right_record = right_->Next();

                if (!left_record) {
                    break;
                }
                if (!right_record) {
                    break;
                }

                memcpy(record->data, left_record->data, left_->tupleLen());
                memcpy(record->data + left_->tupleLen(), right_record->data, right_->tupleLen());

                bool cond_result = false;
                if (fed_conds_.empty()) {
                    cond_result = true;
                } else {
                    cond_result = eval_conds(cols_, fed_conds_, record.get());
                }

                if (cond_result) {
                    match_count++;
                    // 存储匹配的记录
                    current_record_ = std::move(record);
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            right_->beginTuple();
        }
        is_end_ = true;
    }
    void find_semi_join_record() {
        // SEMI JOIN逻辑：对每个左表记录，检查是否在右表中有匹配
        // 如果有匹配，停在该记录；如果没有，继续下一个

        while (!left_->is_end()) {
            // 获取当前左表记录
            auto left_record = left_->Next();

            // 生成左表记录的唯一键值
            std::string record_key = generate_record_key(left_record.get(), left_->cols());

            // 检查是否已经输出过这个记录
            if (matched_left_keys_.find(record_key) != matched_left_keys_.end()) {
                // 已经输出过，跳过这个记录
                left_->nextTuple();
                continue;
            }

            // 检查是否在右表中有匹配
            bool has_match = false;
            right_->beginTuple();

            while (!right_->is_end()) {
                auto right_record = right_->Next();

                // 创建组合记录用于条件评估
                auto combined_record = std::make_unique<RmRecord>(left_->tupleLen() + right_->tupleLen());
                memcpy(combined_record->data, left_record->data, left_->tupleLen());
                memcpy(combined_record->data + left_->tupleLen(), right_record->data, right_->tupleLen());

                // 构建组合列元数据用于条件评估
                std::vector<ColMeta> combined_cols = left_->cols();
                auto right_cols = right_->cols();
                for (auto& col : right_cols) {
                    col.offset += left_->tupleLen();
                }
                combined_cols.insert(combined_cols.end(), right_cols.begin(), right_cols.end());

                // 检查连接条件
                if (fed_conds_.empty() || eval_conds(combined_cols, fed_conds_, combined_record.get())) {
                    has_match = true;
                    break;
                }

                right_->nextTuple();
            }

            if (has_match) {
                // 找到匹配，记录这个键值并停在当前记录
                matched_left_keys_.insert(record_key);
                return;
            }

            // 没有匹配，继续下一个左表记录
            left_->nextTuple();
        }

        // 所有左表记录都检查完毕
        is_end_ = true;
    }

    // 生成记录的唯一键值用于去重
    std::string generate_record_key(RmRecord* record, const std::vector<ColMeta>& cols) {
        std::string key;
        for (const auto& col : cols) {
            char* data_ptr = record->data + col.offset;
            switch (col.type) {
                case TYPE_INT: {
                    int val = *(int*)data_ptr;
                    key += std::to_string(val) + "|";
                    break;
                }
                case TYPE_FLOAT: {
                    float val = *(float*)data_ptr;
                    key += std::to_string(val) + "|";
                    break;
                }
                case TYPE_STRING: {
                    std::string val(data_ptr, col.len);
                    // 正确处理字符串，去除尾部空字符
                    size_t null_pos = val.find('\0');
                    if (null_pos != std::string::npos) {
                        val = val.substr(0, null_pos);
                    }
                    key += val + "|";
                    break;
                }
                default:
                    key += "unknown|";
                    break;
            }
        }
        return key;
    }

   public:
    std::string getType() override { return "NestedLoopJoinExecutor"; }
};