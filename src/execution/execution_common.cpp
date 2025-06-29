/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_common.h"

Value GetColumnValue(const RmRecord& record, const ColMeta& col_meta) {
    Value value;
    char* data_ptr = record.data + col_meta.offset;

    switch (col_meta.type) {
        case TYPE_INT:
            value.set_int(*reinterpret_cast<int*>(data_ptr));
            break;
        case TYPE_FLOAT:
            value.set_float(*reinterpret_cast<float*>(data_ptr));
            break;
        case TYPE_STRING:
            value.set_str(std::string(data_ptr, col_meta.len));
            break;
        default:
            throw InternalError("Unsupported column type in GetColumnValue");
    }

    return value;
}

Value EvaluateArithExpr(std::shared_ptr<ast::ArithExpr> expr, const RmRecord& record, const std::vector<ColMeta>& cols) {
    Value left_val, right_val;

    // 计算左操作数
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr->lhs)) {
        // 列引用
        const ColMeta* col_meta = nullptr;
        for (const auto& meta : cols) {
            if ((col->tab_name.empty() || meta.tab_name == col->tab_name) && meta.name == col->col_name) {
                col_meta = &meta;
                break;
            }
        }
        if (!col_meta) {
            throw RMDBError("Column not found: " + col->col_name);
        }
        left_val = GetColumnValue(record, *col_meta);
    } else if (auto val = std::dynamic_pointer_cast<ast::Value>(expr->lhs)) {
        // 字面值
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(val)) {
            left_val.set_int(int_lit->val);
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(val)) {
            left_val.set_float(float_lit->val);
        } else {
            throw InternalError("Unsupported literal type in arithmetic expression");
        }
    } else if (auto nested_expr = std::dynamic_pointer_cast<ast::ArithExpr>(expr->lhs)) {
        // 嵌套算术表达式
        left_val = EvaluateArithExpr(nested_expr, record, cols);
    }

    // 计算右操作数
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
        // 列引用
        const ColMeta* col_meta = nullptr;
        for (const auto& meta : cols) {
            if ((col->tab_name.empty() || meta.tab_name == col->tab_name) && meta.name == col->col_name) {
                col_meta = &meta;
                break;
            }
        }
        if (!col_meta) {
            throw RMDBError("Column not found: " + col->col_name);
        }
        right_val = GetColumnValue(record, *col_meta);
    } else if (auto val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
        // 字面值
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(val)) {
            right_val.set_int(int_lit->val);
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(val)) {
            right_val.set_float(float_lit->val);
        } else {
            throw InternalError("Unsupported literal type in arithmetic expression");
        }
    } else if (auto nested_expr = std::dynamic_pointer_cast<ast::ArithExpr>(expr->rhs)) {
        // 嵌套算术表达式
        right_val = EvaluateArithExpr(nested_expr, record, cols);
    }

    // 执行算术运算
    Value result;

    // 确保操作数是数值类型
    if ((left_val.type != TYPE_INT && left_val.type != TYPE_FLOAT) ||
        (right_val.type != TYPE_INT && right_val.type != TYPE_FLOAT)) {
        throw IncompatibleTypeError("Non-numeric types", "arithmetic operation");
    }

    // 类型提升：如果任一操作数是 FLOAT，结果就是 FLOAT
    bool result_is_float = (left_val.type == TYPE_FLOAT || right_val.type == TYPE_FLOAT);

    float left_float = (left_val.type == TYPE_FLOAT) ? left_val.float_val : static_cast<float>(left_val.int_val);
    float right_float = (right_val.type == TYPE_FLOAT) ? right_val.float_val : static_cast<float>(right_val.int_val);

    switch (expr->op) {
        case ast::ARITH_OP_ADD:
            if (result_is_float) {
                result.set_float(left_float + right_float);
            } else {
                result.set_int(left_val.int_val + right_val.int_val);
            }
            break;
        case ast::ARITH_OP_SUB:
            if (result_is_float) {
                result.set_float(left_float - right_float);
            } else {
                result.set_int(left_val.int_val - right_val.int_val);
            }
            break;
        case ast::ARITH_OP_MUL:
            if (result_is_float) {
                result.set_float(left_float * right_float);
            } else {
                result.set_int(left_val.int_val * right_val.int_val);
            }
            break;
        case ast::ARITH_OP_DIV:
            // 除法总是返回浮点数
            if (right_float == 0.0f) {
                throw RMDBError("Division by zero");
            }
            result.set_float(left_float / right_float);
            break;
        default:
            throw InternalError("Unsupported arithmetic operator");
    }

    return result;
}

Value EvaluateExpr(const ExprTerm& expr_term, const RmRecord& record, const std::vector<ColMeta>& cols) {
    return EvaluateArithExpr(expr_term.expr, record, cols);
}
