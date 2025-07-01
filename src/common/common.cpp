/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common.h"

#include <iomanip>
#include <sstream>

std::string Value::toString() const {
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
