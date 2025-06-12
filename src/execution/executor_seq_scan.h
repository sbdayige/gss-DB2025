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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;  // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    void beginTuple() override {
        // 初始化条件值的raw数据
        init_condition_values();

        scan_ = std::make_unique<RmScan>(fh_);
        // 移动到第一个满足条件的记录
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr) {
            throw InternalError("Scan not initialized at " + getType());
        }
        if (!scan_->is_end()) {
            scan_->next();
        }
        // 移动到下一个满足条件的记录
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                return;
            }
            scan_->next();
        }
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_ == nullptr) {
            beginTuple();  // 自动初始化扫描
        }
        if (is_end()) {
            return nullptr;
        }
        auto record = fh_->get_record(rid_, context_);
        return record;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return rid_; }

    std::string getType() override { return "SeqScanExecutor"; }

   private:
    /**
     * @brief 初始化条件值的raw数据
     * 处理类型不匹配的情况，确保条件值能够正确比较
     */
    void init_condition_values() {
        for (auto &cond : fed_conds_) {
            if (cond.is_rhs_val) {
                // 找到左侧列的元数据
                auto lhs_col = get_col(cols_, cond.lhs_col);

                // 如果类型不匹配但可以转换，进行类型转换
                if (cond.rhs_val.type != lhs_col->type) {
                    if (can_cast_type(cond.rhs_val.type, lhs_col->type)) {
                        cast_condition_value(cond.rhs_val, lhs_col->type);
                    }
                }

                // 初始化raw数据
                if (cond.rhs_val.raw == nullptr) {
                    cond.rhs_val.init_raw(lhs_col->len);
                }
            }
        }
    }

    /**
     * @brief 检查是否可以进行类型转换
     */
    bool can_cast_type(ColType from, ColType to) {
        if (from == to) return true;
        if (from == TYPE_INT && to == TYPE_FLOAT) return true;
        if (from == TYPE_FLOAT && to == TYPE_INT) return true;
        return false;
    }

    /**
     * @brief 转换条件值的类型
     */
    void cast_condition_value(Value &val, ColType to) {
        if (val.type == TYPE_INT && to == TYPE_FLOAT) {
            int int_val = val.int_val;
            val.type = TYPE_FLOAT;
            val.float_val = static_cast<float>(int_val);
        } else if (val.type == TYPE_FLOAT && to == TYPE_INT) {
            float float_val = val.float_val;
            val.type = TYPE_INT;
            val.int_val = static_cast<int>(float_val);
        }
        // 如果类型相同或无法转换，保持原样
    }
};