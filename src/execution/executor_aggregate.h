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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "parser/ast.h"
#include "errors.h"
#include <limits>

/**
 * @brief 聚合函数执行器
 * 
 * 支持以下聚合函数：
 * - COUNT(*) / COUNT(column)
 * - MAX(column)
 * - MIN(column) 
 * - SUM(column)
 * - AVG(column)
 * 
 * 支持GROUP BY分组聚合
 */
class AggregateExecutor : public AbstractExecutor {
private:
    /**
     * @brief 聚合值结构
     */
    struct AggValue {
        int count;
        union {
            int int_val;
            float float_val;
        };
        double sum;     // 用于AVG计算
        bool initialized;
        
        AggValue() : count(0), int_val(0), sum(0.0), initialized(false) {}
    };

    std::unique_ptr<AbstractExecutor> prev_;        // 子执行器
    std::vector<ColMeta> cols_;                     // 输出列元数据
    size_t len_;                                    // 输出记录长度
    bool executed_;                                 // 是否已执行聚合计算
    bool has_result_;                               // 是否有结果
    std::unique_ptr<RmRecord> result_;              // 聚合结果记录（无GROUP BY时使用）
    
    // GROUP BY支持
    bool has_group_by_;                             // 是否有GROUP BY
    std::vector<TabCol> group_by_cols_;            // GROUP BY列列表
    std::vector<ColMeta> group_by_col_metas_;      // GROUP BY列元数据
    
    // 分组聚合结果
    struct GroupKey {
        std::vector<Value> values;                  // 分组键值
        
        bool operator<(const GroupKey& other) const {
            if (values.size() != other.values.size()) {
                return values.size() < other.values.size();
            }
            for (size_t i = 0; i < values.size(); ++i) {
                // 使用自定义比较函数
                int cmp = compareValues(values[i], other.values[i]);
                if (cmp != 0) {
                    return cmp < 0;
                }
            }
            return false;
        }
        
        bool operator==(const GroupKey& other) const {
            if (values.size() != other.values.size()) {
                return false;
            }
            for (size_t i = 0; i < values.size(); ++i) {
                if (compareValues(values[i], other.values[i]) != 0) {
                    return false;
                }
            }
            return true;
        }
        
    private:
        // 比较两个Value的函数
        static int compareValues(const Value& a, const Value& b) {
            if (a.type != b.type) {
                return static_cast<int>(a.type) - static_cast<int>(b.type);
            }
            
            switch (a.type) {
                case TYPE_INT:
                    if (a.int_val < b.int_val) return -1;
                    if (a.int_val > b.int_val) return 1;
                    return 0;
                case TYPE_FLOAT:
                    if (a.float_val < b.float_val) return -1;
                    if (a.float_val > b.float_val) return 1;
                    return 0;
                case TYPE_STRING:
                    return a.str_val.compare(b.str_val);
                default:
                    return 0;
            }
        }
    };
    
    std::map<GroupKey, std::vector<AggValue>> group_results_;  // 分组聚合结果
    std::map<GroupKey, std::vector<AggValue>>::iterator current_group_;  // 当前分组迭代器
    
    // 聚合函数信息
    struct AggInfo {
        ast::AggType type;                          // 聚合函数类型
        std::string col_name;                       // 列名（COUNT(*)时为空）
        std::string alias;                          // 别名
        ColMeta col_meta;                           // 列元数据
        bool is_count_star;                         // 是否为COUNT(*)
    };
    std::vector<AggInfo> agg_infos_;               // 聚合函数信息列表

public:
    /**
     * @brief 构造聚合执行器
     * @param prev 子执行器
     * @param agg_exprs 聚合表达式列表
     * @param group_by_cols GROUP BY列列表
     */
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, 
                     const std::vector<std::shared_ptr<ast::AggregateExpr>>& agg_exprs,
                     const std::vector<TabCol>& group_by_cols = {}) 
        : prev_(std::move(prev)), executed_(false), has_result_(false), 
          has_group_by_(!group_by_cols.empty()), group_by_cols_(group_by_cols) {
        
        // 首先处理GROUP BY列的元数据
        if (has_group_by_) {
            auto& prev_cols = prev_->cols();
            
            // 查找GROUP BY列的元数据
            for (const auto& group_col : group_by_cols_) {
                bool found = false;
                for (const auto& col : prev_cols) {
                    if (col.name == group_col.col_name) {
                        group_by_col_metas_.push_back(col);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw ColumnNotFoundError(group_col.col_name);
                }
            }
        }
        
        // 构建输出列结构
        size_t curr_offset = 0;
        
        // 如果有GROUP BY，首先添加GROUP BY列到输出
        if (has_group_by_) {
            for (const auto& group_col_meta : group_by_col_metas_) {
                ColMeta output_col = group_col_meta;
                output_col.offset = curr_offset;
                cols_.push_back(output_col);
                curr_offset += output_col.len;
            }
        }
        
        // 解析聚合表达式
        for (size_t i = 0; i < agg_exprs.size(); ++i) {
            const auto& agg_expr = agg_exprs[i];
            AggInfo agg_info;
            agg_info.type = agg_expr->agg_type;
            agg_info.is_count_star = (agg_expr->agg_type == ast::AGG_COUNT && agg_expr->col == nullptr);
            agg_info.alias = agg_expr->alias;
            
            if (!agg_info.is_count_star) {
                agg_info.col_name = agg_expr->col->col_name;
                
                // 从子执行器中找到对应的列
                auto& prev_cols = prev_->cols();
                
                // 查找列信息
                std::vector<ColMeta>::const_iterator pos = prev_cols.end();
                for (auto it = prev_cols.begin(); it != prev_cols.end(); ++it) {
                    if (it->name == agg_info.col_name) {
                        pos = it;
                        break;
                    }
                }
                
                if (pos == prev_cols.end()) {
                    throw ColumnNotFoundError(agg_info.col_name);
                }
                
                agg_info.col_meta = *pos;
            } else {
                // 对于COUNT(*)，初始化一个空的col_meta以避免访问未初始化的内存
                agg_info.col_meta = ColMeta();
                agg_info.col_meta.offset = 0;
                agg_info.col_meta.type = TYPE_INT;
                agg_info.col_meta.len = sizeof(int);
            }
            
            // 设置输出列的元数据
            ColMeta output_col;
            output_col.tab_name = "";
            output_col.name = agg_info.alias.empty() ? getDefaultAggName(agg_info) : agg_info.alias;
            output_col.offset = curr_offset;
            
            // 根据聚合函数类型设置输出数据类型
            switch (agg_info.type) {
                case ast::AGG_COUNT:
                    output_col.type = TYPE_INT;
                    output_col.len = sizeof(int);
                    break;
                case ast::AGG_MAX:
                case ast::AGG_MIN:
                    if (!agg_info.is_count_star) {
                        output_col.type = agg_info.col_meta.type;
                        output_col.len = agg_info.col_meta.len;
                    } else {
                        // 这种情况不应该发生，COUNT(*)不应该在这里
                        output_col.type = TYPE_INT;
                        output_col.len = sizeof(int);
                    }
                    break;
                case ast::AGG_SUM:
                    if (!agg_info.is_count_star) {
                        // SUM的结果类型与输入类型相同，但可能需要更大的存储空间
                        output_col.type = agg_info.col_meta.type;
                        output_col.len = agg_info.col_meta.len;
                    } else {
                        // 这种情况不应该发生，COUNT(*)不应该在这里
                        output_col.type = TYPE_INT;
                        output_col.len = sizeof(int);
                    }
                    break;
                case ast::AGG_AVG:
                    // AVG的结果总是浮点数
                    output_col.type = TYPE_FLOAT;
                    output_col.len = sizeof(float);
                    break;
            }
            
            cols_.push_back(output_col);
            agg_infos_.push_back(agg_info);
            curr_offset += output_col.len;
        }
        
        len_ = curr_offset;
    }

    void beginTuple() override {
        if (!executed_) {
            executeAggregation();
            executed_ = true;
        }
        
        if (has_group_by_) {
            current_group_ = group_results_.begin();
            has_result_ = (current_group_ != group_results_.end());
        } else {
            has_result_ = true;
        }
    }

    void nextTuple() override {
        if (has_group_by_) {
            if (current_group_ != group_results_.end()) {
                ++current_group_;
                has_result_ = (current_group_ != group_results_.end());
            } else {
                has_result_ = false;
            }
        } else {
            has_result_ = false;
        }
    }

    bool is_end() const override {
        return !has_result_;
    }

    size_t tupleLen() const override {
        return len_;
    }

    const std::vector<ColMeta>& cols() const override {
        return cols_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!has_result_) {
            return nullptr;
        }
        
        if (has_group_by_) {
            return buildGroupByResult();
        } else {
            // 创建结果记录的副本
            auto result_copy = std::make_unique<RmRecord>(len_);
            memcpy(result_copy->data, result_->data, len_);
            return result_copy;
        }
    }
    
    /**
     * @brief 构建GROUP BY结果记录
     */
    std::unique_ptr<RmRecord> buildGroupByResult() {
        if (current_group_ == group_results_.end()) {
            return nullptr;
        }
        
        auto result = std::make_unique<RmRecord>(len_);
        size_t offset = 0;
        
        // 写入GROUP BY列的值
        const auto& group_key = current_group_->first;
        for (size_t i = 0; i < group_key.values.size(); ++i) {
            const auto& val = group_key.values[i];
            const auto& col_meta = group_by_col_metas_[i];
            char* col_data = result->data + offset;
            
            switch (val.type) {
                case TYPE_INT:
                    *(int*)col_data = val.int_val;
                    break;
                case TYPE_FLOAT:
                    *(float*)col_data = val.float_val;
                    break;
                case TYPE_STRING:
                    memset(col_data, 0, col_meta.len);
                    memcpy(col_data, val.str_val.c_str(), std::min(val.str_val.length(), (size_t)col_meta.len));
                    break;
            }
            offset += col_meta.len;
        }
        
        // 写入聚合结果
        const auto& agg_values = current_group_->second;
        for (size_t i = 0; i < agg_infos_.size(); ++i) {
            size_t col_idx = group_by_col_metas_.size() + i;
            writeAggResult(agg_values[i], agg_infos_[i], cols_[col_idx], result.get());
        }
        
        return result;
    }

    Rid& rid() override {
        return _abstract_rid;
    }

    std::string getType() override {
        return "AggregateExecutor";
    }

private:
    /**
     * @brief 执行聚合计算
     */
    void executeAggregation() {
        if (has_group_by_) {
            executeGroupByAggregation();
        } else {
            executeSimpleAggregation();
        }
    }
    
    /**
     * @brief 执行简单聚合（无GROUP BY）
     */
    void executeSimpleAggregation() {
        result_ = std::make_unique<RmRecord>(len_);
        
        // 初始化聚合值
        std::vector<AggValue> agg_values(agg_infos_.size());
        for (size_t i = 0; i < agg_infos_.size(); ++i) {
            initAggValue(agg_values[i], agg_infos_[i]);
        }
        
        // 遍历所有输入记录进行聚合计算
        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto record = prev_->Next();
            if (record) {
                for (size_t i = 0; i < agg_infos_.size(); ++i) {
                    updateAggValue(agg_values[i], agg_infos_[i], record.get());
                }
            }
            prev_->nextTuple();
        }
        
        // 将聚合结果写入输出记录
        size_t agg_offset = 0;
        for (size_t i = 0; i < agg_infos_.size(); ++i) {
            size_t col_idx = agg_offset + (has_group_by_ ? group_by_col_metas_.size() : 0);
            writeAggResult(agg_values[i], agg_infos_[i], cols_[col_idx], result_.get());
            agg_offset++;
        }
    }
    
    /**
     * @brief 执行分组聚合（有GROUP BY）
     */
    void executeGroupByAggregation() {
        // 遍历所有输入记录进行分组聚合
        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto record = prev_->Next();
            if (record) {
                // 生成分组键
                GroupKey group_key = generateGroupKey(record.get());
                
                // 如果该分组不存在，初始化
                if (group_results_.find(group_key) == group_results_.end()) {
                    std::vector<AggValue> agg_values(agg_infos_.size());
                    for (size_t i = 0; i < agg_infos_.size(); ++i) {
                        initAggValue(agg_values[i], agg_infos_[i]);
                    }
                    group_results_[group_key] = agg_values;
                }
                
                // 更新该分组的聚合值
                auto& agg_values = group_results_[group_key];
                for (size_t i = 0; i < agg_infos_.size(); ++i) {
                    updateAggValue(agg_values[i], agg_infos_[i], record.get());
                }
            }
            prev_->nextTuple();
        }
    }
    
    /**
     * @brief 生成分组键
     */
    GroupKey generateGroupKey(const RmRecord* record) {
        GroupKey key;
        for (const auto& col_meta : group_by_col_metas_) {
            Value val;
            char* col_data = record->data + col_meta.offset;
            
            switch (col_meta.type) {
                case TYPE_INT:
                    val.set_int(*(int*)col_data);
                    break;
                case TYPE_FLOAT:
                    val.set_float(*(float*)col_data);
                    break;
                case TYPE_STRING:
                    val.set_str(std::string(col_data, col_meta.len));
                    // 去除字符串末尾的空字符
                    val.str_val = val.str_val.substr(0, val.str_val.find('\0'));
                    break;
                default:
                    throw InternalError("Unsupported column type for GROUP BY");
            }
            key.values.push_back(val);
        }
        return key;
    }

    /**
     * @brief 初始化聚合值
     */
    void initAggValue(AggValue& agg_val, const AggInfo& agg_info) {
        agg_val.count = 0;
        agg_val.sum = 0.0;
        agg_val.initialized = false;
        
        // 只对非COUNT(*)的MIN/MAX初始化极值
        if (agg_info.type == ast::AGG_MIN && !agg_info.is_count_star) {
            if (agg_info.col_meta.type == TYPE_INT) {
                agg_val.int_val = std::numeric_limits<int>::max();
            } else if (agg_info.col_meta.type == TYPE_FLOAT) {
                agg_val.float_val = std::numeric_limits<float>::max();
            }
        } else if (agg_info.type == ast::AGG_MAX && !agg_info.is_count_star) {
            if (agg_info.col_meta.type == TYPE_INT) {
                agg_val.int_val = std::numeric_limits<int>::min();
            } else if (agg_info.col_meta.type == TYPE_FLOAT) {
                agg_val.float_val = std::numeric_limits<float>::lowest();
            }
        }
    }

    /**
     * @brief 更新聚合值
     */
    void updateAggValue(AggValue& agg_val, const AggInfo& agg_info, const RmRecord* record) {
        switch (agg_info.type) {
            case ast::AGG_COUNT:
                if (agg_info.is_count_star) {
                    agg_val.count++;
                } else {
                    // COUNT(column) - 检查列值是否为NULL（这里简化处理，认为所有值都非NULL）
                    agg_val.count++;
                }
                break;
                
            case ast::AGG_MAX:
            case ast::AGG_MIN:
            case ast::AGG_SUM:
            case ast::AGG_AVG: {
                char* col_data = record->data + agg_info.col_meta.offset;
                
                if (agg_info.col_meta.type == TYPE_INT) {
                    int val = *(int*)col_data;
                    updateNumericAgg(agg_val, agg_info, val);
                } else if (agg_info.col_meta.type == TYPE_FLOAT) {
                    float val = *(float*)col_data;
                    updateNumericAgg(agg_val, agg_info, val);
                }
                break;
            }
        }
    }

    /**
     * @brief 更新数值型聚合值
     */
    template<typename T>
    void updateNumericAgg(AggValue& agg_val, const AggInfo& agg_info, T val) {
        switch (agg_info.type) {
            case ast::AGG_COUNT:
                // COUNT should not be handled here, but add case to avoid warnings
                agg_val.count++;
                break;
                
            case ast::AGG_MAX:
                if (!agg_val.initialized || (agg_info.col_meta.type == TYPE_INT ? 
                    val > agg_val.int_val : val > agg_val.float_val)) {
                    if (agg_info.col_meta.type == TYPE_INT) {
                        agg_val.int_val = static_cast<int>(val);
                    } else {
                        agg_val.float_val = static_cast<float>(val);
                    }
                    agg_val.initialized = true;
                }
                break;
                
            case ast::AGG_MIN:
                if (!agg_val.initialized || (agg_info.col_meta.type == TYPE_INT ? 
                    val < agg_val.int_val : val < agg_val.float_val)) {
                    if (agg_info.col_meta.type == TYPE_INT) {
                        agg_val.int_val = static_cast<int>(val);
                    } else {
                        agg_val.float_val = static_cast<float>(val);
                    }
                    agg_val.initialized = true;
                }
                break;
                
            case ast::AGG_SUM:
                agg_val.sum += static_cast<double>(val);
                agg_val.count++;
                agg_val.initialized = true;
                break;
                
            case ast::AGG_AVG:
                agg_val.sum += static_cast<double>(val);
                agg_val.count++;
                agg_val.initialized = true;
                break;
        }
    }

    /**
     * @brief 将聚合结果写入输出记录
     */
    void writeAggResult(const AggValue& agg_val, const AggInfo& agg_info, 
                       const ColMeta& output_col, RmRecord* record) {
        char* output_data = record->data + output_col.offset;
        
        switch (agg_info.type) {
            case ast::AGG_COUNT:
                *(int*)output_data = agg_val.count;
                break;
                
            case ast::AGG_MAX:
            case ast::AGG_MIN:
                if (!agg_val.initialized) {
                    // 处理空结果集的情况
                    if (output_col.type == TYPE_INT) {
                        *(int*)output_data = 0;
                    } else if (output_col.type == TYPE_FLOAT) {
                        *(float*)output_data = 0.0f;
                    }
                } else {
                    if (output_col.type == TYPE_INT) {
                        *(int*)output_data = agg_val.int_val;
                    } else if (output_col.type == TYPE_FLOAT) {
                        *(float*)output_data = agg_val.float_val;
                    }
                }
                break;
                
            case ast::AGG_SUM:
                if (output_col.type == TYPE_INT) {
                    *(int*)output_data = static_cast<int>(agg_val.sum);
                } else if (output_col.type == TYPE_FLOAT) {
                    *(float*)output_data = static_cast<float>(agg_val.sum);
                }
                break;
                
            case ast::AGG_AVG:
                if (agg_val.count > 0) {
                    *(float*)output_data = static_cast<float>(agg_val.sum / agg_val.count);
                } else {
                    *(float*)output_data = 0.0f;
                }
                break;
        }
    }

    /**
     * @brief 获取聚合函数的默认名称
     */
    std::string getDefaultAggName(const AggInfo& agg_info) {
        switch (agg_info.type) {
            case ast::AGG_COUNT:
                return agg_info.is_count_star ? "COUNT(*)" : "COUNT(" + agg_info.col_name + ")";
            case ast::AGG_MAX:
                return "MAX(" + agg_info.col_name + ")";
            case ast::AGG_MIN:
                return "MIN(" + agg_info.col_name + ")";
            case ast::AGG_SUM:
                return "SUM(" + agg_info.col_name + ")";
            case ast::AGG_AVG:
                return "AVG(" + agg_info.col_name + ")";
        }
        return "AGG";
    }
}; 