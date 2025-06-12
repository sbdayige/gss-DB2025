/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "executor_explain.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include "optimizer/plan.h"

void ExplainExecutor::beginTuple() {
    if (!has_executed_) {
        try {
            // 安全地构建查询计划树字符串，不进行复杂优化以避免段错误
            plan_output_.clear();
            build_plan_tree_string(plan_);
            has_executed_ = true;
        } catch (const std::exception &e) {
            // 如果出现异常，返回基本的计划信息
            plan_output_ = "EXPLAIN failed: " + std::string(e.what()) + "\n";
            has_executed_ = true;
        } catch (...) {
            // 捕获所有其他异常
            plan_output_ = "EXPLAIN failed: Unknown error occurred\n";
            has_executed_ = true;
        }
    }
}

std::unique_ptr<RmRecord> ExplainExecutor::Next() {
    // 为EXPLAIN创建包含计划输出的记录
    if (!plan_output_.empty()) {
        auto record = std::make_unique<RmRecord>(plan_output_.size() + 1);
        memcpy(record->data, plan_output_.c_str(), plan_output_.size());
        record->data[plan_output_.size()] = '\0';
        plan_output_.clear();  // 清空以避免重复输出
        return record;
    }
    return nullptr;
}

bool ExplainExecutor::is_end() const {
    return has_executed_;
}

void ExplainExecutor::nextTuple() {
    has_executed_ = true;
}

Rid &ExplainExecutor::rid() {
    static Rid dummy_rid;
    return dummy_rid;
}

const std::vector<ColMeta> &ExplainExecutor::cols() const {
    static std::vector<ColMeta> empty_cols;
    return empty_cols;
}

// 安全的计划树构建和显示功能 - 避免复杂优化导致的段错误

void ExplainExecutor::build_plan_tree_string(std::shared_ptr<Plan> plan, int indent) {
    if (!plan) {
        return;
    }

    try {
        // 生成缩进
        for (int i = 0; i < indent; ++i) {
            plan_output_ += "\t";
        }

        // 添加当前节点
        plan_output_ += get_plan_name(plan) + "\n";

        // 收集子计划
        std::vector<std::shared_ptr<Plan>> child_plans;

        if (auto projection_plan = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            if (projection_plan->subplan_)
                child_plans.push_back(projection_plan->subplan_);
        } else if (auto filter_plan = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            if (filter_plan->subplan_)
                child_plans.push_back(filter_plan->subplan_);
        } else if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            if (join_plan->left_)
                child_plans.push_back(join_plan->left_);
            if (join_plan->right_)
                child_plans.push_back(join_plan->right_);
        } else if (auto sort_plan = std::dynamic_pointer_cast<SortPlan>(plan)) {
            if (sort_plan->subplan_)
                child_plans.push_back(sort_plan->subplan_);
        }

        // 按节点名称排序
        std::sort(child_plans.begin(), child_plans.end(),
                  [this](const std::shared_ptr<Plan> &a, const std::shared_ptr<Plan> &b) {
                      try {
                          return get_plan_name(a) < get_plan_name(b);
                      } catch (...) {
                          return false;  // 如果比较出错，保持原顺序
                      }
                  });

        // 递归构建子计划字符串
        for (auto &child : child_plans) {
            build_plan_tree_string(child, indent + 1);
        }
    } catch (const std::exception &e) {
        plan_output_ += "Error processing plan node: " + std::string(e.what()) + "\n";
    } catch (...) {
        plan_output_ += "Unknown error processing plan node\n";
    }
}

std::string ExplainExecutor::get_plan_name(std::shared_ptr<Plan> plan) {
    if (!plan) {
        return "Unknown";
    }

    try {
        switch (plan->tag) {
            case T_SeqScan:
            case T_IndexScan: {
                auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan);
                if (!scan_plan) {
                    return "Scan(table=Unknown)";
                }
                // Scan节点显示真实表名，不使用别名
                return "Scan(table=" + scan_plan->tab_name_ + ")";
            }
            case T_Filter: {
                auto filter_plan = std::dynamic_pointer_cast<FilterPlan>(plan);
                if (!filter_plan) {
                    return "Filter(condition=[Unknown])";
                }

                std::vector<std::string> condition_strs;
                for (const auto &cond : filter_plan->conds_) {
                    try {
                        std::string condition_str = get_display_table_name(cond.lhs_col.tab_name) + "." + cond.lhs_col.col_name;

                        switch (cond.op) {
                            case OP_EQ:
                                condition_str += "=";
                                break;
                            case OP_NE:
                                condition_str += "<>";
                                break;
                            case OP_LT:
                                condition_str += "<";
                                break;
                            case OP_GT:
                                condition_str += ">";
                                break;
                            case OP_LE:
                                condition_str += "<=";
                                break;
                            case OP_GE:
                                condition_str += ">=";
                                break;
                            default:
                                condition_str += "?";
                                break;
                        }

                        if (cond.is_rhs_val) {
                            condition_str += cond.rhs_val.toString();
                        } else {
                            condition_str += get_display_table_name(cond.rhs_col.tab_name) + "." + cond.rhs_col.col_name;
                        }
                        condition_strs.push_back(condition_str);
                    } catch (...) {
                        condition_strs.push_back("condition_error");
                    }
                }

                std::sort(condition_strs.begin(), condition_strs.end());

                std::string conditions = "";
                for (size_t i = 0; i < condition_strs.size(); ++i) {
                    if (i > 0)
                        conditions += ",";
                    conditions += condition_strs[i];
                }
                return "Filter(condition=[" + conditions + "])";
            }
            case T_Projection: {
                auto proj_plan = std::dynamic_pointer_cast<ProjectionPlan>(plan);
                if (!proj_plan) {
                    return "Project(columns=[Unknown])";
                }

                // 检查是否为SELECT *查询
                if (is_select_star_) {
                    return "Project(columns=[*])";
                }

                if (proj_plan->sel_cols_.empty()) {
                    return "Project(columns=[*])";
                }

                std::vector<std::string> col_names;
                for (const auto &col : proj_plan->sel_cols_) {
                    try {
                        col_names.push_back(get_display_table_name(col.tab_name) + "." + col.col_name);
                    } catch (...) {
                        col_names.push_back("column_error");
                    }
                }

                std::sort(col_names.begin(), col_names.end());

                std::string cols_str = "";
                for (size_t i = 0; i < col_names.size(); ++i) {
                    if (i > 0)
                        cols_str += ",";
                    cols_str += col_names[i];
                }
                return "Project(columns=[" + cols_str + "])";
            }
            case T_NestLoop:
            case T_SortMerge: {
                auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan);
                if (!join_plan) {
                    return "Join(tables=[Unknown],condition=[Unknown])";
                }

                // 收集表名（Join中显示完整表名，不使用别名）
                std::set<std::string> table_set;
                collectTableNames(join_plan, table_set);

                std::vector<std::string> tables(table_set.begin(), table_set.end());
                std::sort(tables.begin(), tables.end());

                std::string tables_str = "";
                for (size_t i = 0; i < tables.size(); ++i) {
                    if (i > 0)
                        tables_str += ",";
                    tables_str += tables[i];
                }

                // 构建连接条件
                std::vector<std::string> condition_strs;
                for (const auto &cond : join_plan->conds_) {
                    try {
                        std::string lhs_table = get_display_table_name(cond.lhs_col.tab_name);
                        std::string rhs_table = get_display_table_name(cond.rhs_col.tab_name);
                        std::string lhs_col = cond.lhs_col.col_name;
                        std::string rhs_col = cond.rhs_col.col_name;
                        CompOp op = cond.op;
                        bool should_swap = false;
                        if (op == OP_EQ && lhs_table > rhs_table) {
                            should_swap = true;
                        }

                        std::string condition_str;
                        if (should_swap) {
                            condition_str = rhs_table + "." + rhs_col;
                        } else {
                            condition_str = lhs_table + "." + lhs_col;
                        }

                        switch (op) {
                            case OP_EQ:
                                condition_str += "=";
                                break;
                            case OP_NE:
                                condition_str += "<>";
                                break;
                            case OP_LT:
                                condition_str += should_swap ? ">" : "<";
                                break;
                            case OP_GT:
                                condition_str += should_swap ? "<" : ">";
                                break;
                            case OP_LE:
                                condition_str += should_swap ? ">=" : "<=";
                                break;
                            case OP_GE:
                                condition_str += should_swap ? "<=" : ">=";
                                break;
                            default:
                                condition_str += "?";
                                break;
                        }

                        if (should_swap) {
                            condition_str += lhs_table + "." + lhs_col;
                        } else {
                            condition_str += rhs_table + "." + rhs_col;
                        }
                        condition_strs.push_back(condition_str);
                    } catch (...) {
                        condition_strs.push_back("join_condition_error");
                    }
                }

                std::sort(condition_strs.begin(), condition_strs.end());

                std::string join_conds = "";
                for (size_t i = 0; i < condition_strs.size(); ++i) {
                    if (i > 0)
                        join_conds += ",";
                    join_conds += condition_strs[i];
                }

                return "Join(tables=[" + tables_str + "],condition=[" + join_conds + "])";
            }
            default:
                return "Unknown";
        }
    } catch (const std::exception &e) {
        return "Error: " + std::string(e.what());
    } catch (...) {
        return "Unknown_Error";
    }
}

void ExplainExecutor::collectTableNames(std::shared_ptr<Plan> plan, std::set<std::string> &table_set) {
    if (!plan) {
        return;
    }

    try {
        switch (plan->tag) {
            case T_SeqScan:
            case T_IndexScan: {
                auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan);
                if (scan_plan) {
                    table_set.insert(scan_plan->tab_name_);
                }
                break;
            }
            case T_NestLoop:
            case T_SortMerge: {
                auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan);
                if (join_plan) {
                    collectTableNames(join_plan->left_, table_set);
                    collectTableNames(join_plan->right_, table_set);
                }
                break;
            }
            case T_Projection: {
                auto proj_plan = std::dynamic_pointer_cast<ProjectionPlan>(plan);
                if (proj_plan) {
                    collectTableNames(proj_plan->subplan_, table_set);
                }
                break;
            }
            case T_Sort: {
                auto sort_plan = std::dynamic_pointer_cast<SortPlan>(plan);
                if (sort_plan) {
                    collectTableNames(sort_plan->subplan_, table_set);
                }
                break;
            }
            case T_Filter: {
                auto filter_plan = std::dynamic_pointer_cast<FilterPlan>(plan);
                if (filter_plan) {
                    collectTableNames(filter_plan->subplan_, table_set);
                }
                break;
            }
            default:
                break;
        }
    } catch (...) {
        // 忽略错误，继续处理其他节点
    }
}

std::string ExplainExecutor::get_display_table_name(const std::string &table_name) {
    try {
        // table_alias_map_: alias -> table_name
        // 需要通过table_name找alias
        for (const auto &pair : table_alias_map_) {
            if (pair.second == table_name) {
                return pair.first;  // 返回别名
            }
        }
        // 如果没有找到别名，返回原表名
        return table_name;
    } catch (...) {
        return table_name;  // 如果出错，返回原表名
    }
}
