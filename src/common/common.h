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

#include <cassert>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "defs.h"
#include "record/rm_defs.h"

// 前向声明
namespace ast {
struct ArithExpr;
}

// 为了避免循环依赖，我们使用前向声明
// 在使用时需要包含完整的 ast.h 头文件

struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }

    std::string toString() const {
        switch (type) {
            case TYPE_INT:
                return std::to_string(int_val);
            case TYPE_FLOAT: {
                // 检查是否为整数值（没有小数部分）
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6) << float_val;
                return oss.str();
            }
            case TYPE_STRING:
                return "'" + str_val + "'";
            default:
                return "unknown";
        }
    }
};

enum CompOp { OP_EQ,
              OP_NE,
              OP_LT,
              OP_GT,
              OP_LE,
              OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;                                // 要更新的列
    SetRhsType rhs_type;                       // 右侧操作数类型
    Value rhs_val;                             // 右侧值 (当 rhs_type == SET_RHS_VALUE 时有效)
    TabCol rhs_col;                            // 右侧列 (当 rhs_type == SET_RHS_COL 时有效)
    std::shared_ptr<ast::ArithExpr> rhs_expr;  // 右侧算术表达式 (当 rhs_type == SET_RHS_EXPR 时有效)
};