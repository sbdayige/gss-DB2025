/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file plan.h
 * @brief RMDB查询优化器执行计划定义
 *
 * 本文件定义了RMDB数据库系统查询优化器的所有执行计划类型。
 * 执行计划是查询优化的核心数据结构，描述了SQL语句的具体执行方式。
 *
 * 主要包含以下内容：
 * - PlanTag枚举：定义所有支持的执行计划类型
 * - Plan基类：所有执行计划的抽象基类
 * - 各种具体执行计划类：
 *   * ScanPlan：表扫描计划(顺序扫描/索引扫描)
 *   * JoinPlan：连接计划(嵌套循环/排序合并连接)
 *   * ProjectionPlan：投影计划(SELECT列选择)
 *   * SortPlan：排序计划(ORDER BY)
 *   * DMLPlan：数据操作计划(INSERT/UPDATE/DELETE/SELECT)
 *   * DDLPlan：数据定义计划(CREATE/DROP TABLE/INDEX)
 *   * OtherPlan：其他操作计划(HELP/SHOW/DESC/事务控制)
 *   * SetKnobPlan：系统参数设置计划
 * - plannerInfo：规划器信息类，用于规划过程中的信息传递
 *
 * 这些计划类构成了执行计划树的节点，通过组合可以表达复杂的查询执行逻辑。
 *
 * @author RMDB Team
 * @version 1.0
 */

#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"
#include "parser/parser.h"

/**
 * @brief 执行计划标签枚举
 * 定义了RMDB支持的所有执行计划类型，用于标识不同的操作和算法
 */
typedef enum PlanTag {
    T_Invalid = 1,  // 无效计划
    // DDL语句类型
    T_Help,         // 帮助命令
    T_ShowTable,    // 显示表命令
    T_DescTable,    // 描述表结构命令
    T_ShowIndex,    // 显示索引命令
    T_CreateTable,  // 创建表命令
    T_DropTable,    // 删除表命令
    T_CreateIndex,  // 创建索引命令
    T_DropIndex,    // 删除索引命令
    T_SetKnob,      // 设置系统参数命令
    // DML语句类型
    T_Insert,  // 插入数据命令
    T_Update,  // 更新数据命令
    T_Delete,  // 删除数据命令
    T_Select,  // 查询数据命令
    T_Explain,
    // 事务控制语句类型
    T_Transaction_begin,     // 开始事务
    T_Transaction_commit,    // 提交事务
    T_Transaction_abort,     // 中止事务
    T_Transaction_rollback,  // 回滚事务
    // 查询执行算子类型
    T_SeqScan,     // 顺序扫描
    T_IndexScan,   // 索引扫描
    T_NestLoop,    // 嵌套循环连接
    T_SortMerge,   // 排序合并连接
    T_Sort,        // 排序操作
    T_Projection,  // 投影操作
    T_Filter,      // 过滤操作
    T_Aggregate    // 聚合操作
} PlanTag;

/**
 * @brief 查询执行计划基类
 * 所有执行计划的抽象基类，提供统一的接口
 * 每个计划节点都有一个标签用于标识其类型
 */
class Plan {
   public:
    PlanTag tag;  // 计划类型标签
    virtual ~Plan() = default;
};

/**
 * @brief 扫描计划类
 * 用于表扫描操作的执行计划，支持顺序扫描和索引扫描两种方式
 * 包含扫描条件、表元信息等执行所需的所有参数
 */
class ScanPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_SeqScan或T_IndexScan)
     * @param sm_manager 系统管理器，用于获取表元信息
     * @param tab_name 表名
     * @param conds 扫描条件列表
     * @param index_col_names 索引列名列表(用于索引扫描)
     */
    ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names) {
        Plan::tag = tag;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager->db_.get_table(tab_name_);
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;
        fed_conds_ = conds_;
        index_col_names_ = index_col_names;
    }
    ~ScanPlan() {}

    // 以下变量与ScanExecutor中的变量对应，用于执行时的参数传递
    std::string tab_name_;                      // 表名
    std::vector<ColMeta> cols_;                 // 表的列元信息
    std::vector<Condition> conds_;              // 扫描条件
    size_t len_;                                // 记录长度
    std::vector<Condition> fed_conds_;          // 馈送条件(用于优化)
    std::vector<std::string> index_col_names_;  // 索引列名
};

/**
 * @brief 连接计划类
 * 用于两表连接操作的执行计划，支持多种连接算法
 * 包含左右子计划和连接条件
 */
class JoinPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_NestLoop或T_SortMerge)
     * @param left 左子计划
     * @param right 右子计划
     * @param conds 连接条件列表
     */
    JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds) {
        Plan::tag = tag;
        left_ = std::move(left);
        right_ = std::move(right);
        conds_ = std::move(conds);
        type = INNER_JOIN;
    }

    /**
     * @brief 构造函数（支持指定连接类型）
     * @param tag 计划类型标签(T_NestLoop或T_SortMerge)
     * @param left 左子计划
     * @param right 右子计划
     * @param conds 连接条件列表
     * @param join_type 连接类型
     */
    JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds, JoinType join_type) {
        Plan::tag = tag;
        left_ = std::move(left);
        right_ = std::move(right);
        conds_ = std::move(conds);
        type = join_type;
    }
    ~JoinPlan() {}

    std::shared_ptr<Plan> left_;    // 左子计划(通常是外表)
    std::shared_ptr<Plan> right_;   // 右子计划(通常是内表)
    std::vector<Condition> conds_;  // 连接条件列表
    JoinType type;                  // 连接类型(目前支持内连接，未来可扩展外连接等)
};

/**
 * @brief 投影计划类
 * 用于SELECT子句的列选择操作，从子计划的输出中选择指定的列
 * 相当于关系代数中的投影操作π
 */
class ProjectionPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_Projection)
     * @param subplan 子计划，提供数据源
     * @param sel_cols 要选择的列列表
     */
    ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols) {
        Plan::tag = tag;
        subplan_ = std::move(subplan);
        sel_cols_ = std::move(sel_cols);
    }
    ~ProjectionPlan() {}

    std::shared_ptr<Plan> subplan_;  // 子计划，数据来源
    std::vector<TabCol> sel_cols_;   // 要投影的列列表
};

/**
 * @brief 过滤计划类
 * 用于WHERE子句的条件过滤操作，从子计划的输出中筛选满足条件的记录
 * 相当于关系代数中的选择操作σ
 */
class FilterPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_Filter)
     * @param subplan 子计划，提供数据源
     * @param conds 过滤条件列表
     */
    FilterPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<Condition> conds) {
        Plan::tag = tag;
        subplan_ = std::move(subplan);
        conds_ = std::move(conds);
    }
    ~FilterPlan() {}

    std::shared_ptr<Plan> subplan_;  // 子计划，数据来源
    std::vector<Condition> conds_;   // 过滤条件列表
};

/**
 * @brief 排序列信息结构
 * 表示单个排序列的信息，包含列信息和排序方向
 */
struct SortColumn {
    TabCol col;         // 排序列
    bool is_desc;       // 排序方向：true=降序，false=升序
    
    SortColumn(TabCol col_, bool is_desc_) : col(col_), is_desc(is_desc_) {}
};

/**
 * @brief 排序计划类
 * 用于ORDER BY子句的排序操作，支持单列和多列排序
 * 对子计划的输出按指定列进行排序
 */
class SortPlan : public Plan {
   public:
    /**
     * @brief 构造函数（多列排序）
     * @param tag 计划类型标签(T_Sort)
     * @param subplan 子计划，提供要排序的数据
     * @param sort_cols 排序列列表，支持多列排序
     */
    SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<SortColumn> sort_cols) {
        Plan::tag = tag;
        subplan_ = std::move(subplan);
        sort_cols_ = std::move(sort_cols);
        
        // 为了向后兼容，如果只有一列，也设置单列字段
        if (!sort_cols_.empty()) {
            sel_col_ = sort_cols_[0].col;
            is_desc_ = sort_cols_[0].is_desc;
        }
    }

    /**
     * @brief 构造函数（单列排序，向后兼容）
     * @param tag 计划类型标签(T_Sort)
     * @param subplan 子计划，提供要排序的数据
     * @param sel_col 排序列
     * @param is_desc 是否降序排列(true为降序，false为升序)
     */
    SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, TabCol sel_col, bool is_desc) {
        Plan::tag = tag;
        subplan_ = std::move(subplan);
        sel_col_ = sel_col;
        is_desc_ = is_desc;
        
        // 创建对应的多列排序列表
        sort_cols_.push_back(SortColumn(sel_col_, is_desc));
    }
    ~SortPlan() {}

    // 检查是否为多列排序
    bool is_multi_column() const {
        return sort_cols_.size() > 1;
    }

    // 获取排序列数量
    size_t column_count() const {
        return sort_cols_.size();
    }

    std::shared_ptr<Plan> subplan_;             // 子计划，数据来源
    std::vector<SortColumn> sort_cols_;         // 多列排序列表
    TabCol sel_col_;                           // 排序的列（向后兼容）
    bool is_desc_;                             // 排序方向（向后兼容）
};

/**
 * @brief 数据操作语言(DML)计划类
 * 用于INSERT、DELETE、UPDATE、SELECT等数据操作语句的执行计划
 * 是DML语句的统一抽象，包含了所有DML操作所需的参数
 */
class DMLPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_Insert/T_Delete/T_Update/T_select)
     * @param subplan 子计划(对于某些操作如DELETE/UPDATE的WHERE条件)
     * @param tab_name 目标表名
     * @param values 插入的值列表(用于INSERT操作)
     * @param conds 条件列表(用于DELETE/UPDATE的WHERE条件)
     * @param set_clauses SET子句列表(用于UPDATE操作)
     */
    DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::string tab_name,
            std::vector<Value> values, std::vector<Condition> conds,
            std::vector<SetClause> set_clauses, int limit = 0) {
        Plan::tag = tag;
        subplan_ = std::move(subplan);
        tab_name_ = std::move(tab_name);
        values_ = std::move(values);
        conds_ = std::move(conds);
        set_clauses_ = std::move(set_clauses);
        is_select_star_ = false;  // 初始化新增的成员变量
        limit_ = limit;
    }

    /**
     * @brief 构造函数(支持表别名映射，用于EXPLAIN)
     * @param tag 计划类型标签
     * @param subplan 子计划
     * @param tab_name 目标表名
     * @param values 插入的值列表
     * @param conds 条件列表
     * @param set_clauses SET子句列表
     * @param table_alias_map 表别名映射
     * @param is_select_star 是否为SELECT *查询
     */
    DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::string tab_name,
            std::vector<Value> values, std::vector<Condition> conds,
            std::vector<SetClause> set_clauses, std::map<std::string, std::string> table_alias_map,
            bool is_select_star = false, int limit = 0) {
        Plan::tag = tag;
        subplan_ = std::move(subplan);
        tab_name_ = std::move(tab_name);
        values_ = std::move(values);
        conds_ = std::move(conds);
        set_clauses_ = std::move(set_clauses);
        table_alias_map_ = std::move(table_alias_map);
        is_select_star_ = is_select_star;
        limit_ = limit;
    }
    ~DMLPlan() {}

    std::shared_ptr<Plan> subplan_;                       // 子计划(通常用于条件过滤)
    std::string tab_name_;                                // 操作的目标表名
    std::vector<Value> values_;                           // 插入值列表(INSERT使用)
    std::vector<Condition> conds_;                        // 条件列表(WHERE子句)
    std::vector<SetClause> set_clauses_;                  // SET子句列表(UPDATE使用)
    std::map<std::string, std::string> table_alias_map_;  // 表别名映射(EXPLAIN使用)
    bool is_select_star_ = false;                         // 是否为SELECT *查询(EXPLAIN使用)
    int limit_;                                          // 限制条件
};

/**
 * @brief 数据定义语言(DDL)计划类
 * 用于CREATE/DROP TABLE、CREATE/DROP INDEX等数据定义语句的执行计划
 * 负责数据库模式的修改操作
 */
class DDLPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_CreateTable/T_DropTable/T_CreateIndex/T_DropIndex)
     * @param tab_name 表名或索引相关的表名
     * @param col_names 列名列表(用于索引创建)
     * @param cols 列定义列表(用于表创建)
     */
    DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names, std::vector<ColDef> cols) {
        Plan::tag = tag;
        tab_name_ = std::move(tab_name);
        cols_ = std::move(cols);
        tab_col_names_ = std::move(col_names);
    }
    ~DDLPlan() {}

    std::string tab_name_;                    // 表名
    std::vector<std::string> tab_col_names_;  // 列名列表(索引相关)
    std::vector<ColDef> cols_;                // 列定义列表(建表相关)
};

/**
 * @brief 其他操作计划类
 * 用于HELP、SHOW TABLES、DESC TABLE、BEGIN、COMMIT、ABORT、ROLLBACK等
 * 系统管理和事务控制语句的执行计划
 */
class OtherPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param tag 计划类型标签(T_Help/T_ShowTable/T_DescTable/T_Transaction_*)
     * @param tab_name 表名(某些操作如DESC TABLE需要)
     */
    OtherPlan(PlanTag tag, std::string tab_name) {
        Plan::tag = tag;
        tab_name_ = std::move(tab_name);
    }
    ~OtherPlan() {}

    std::string tab_name_;  // 表名(适用于需要表名的操作)
};

/**
 * @brief 聚合计划类
 * 用于聚合函数操作的执行计划，支持COUNT、MAX、MIN、SUM、AVG等聚合操作
 * 可以与GROUP BY子句结合使用进行分组聚合，支持HAVING子句过滤
 */
class AggregatePlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param subplan 子计划，提供数据源
     * @param agg_exprs 聚合表达式列表
     * @param group_by_cols GROUP BY列列表
     * @param having_conds HAVING条件列表
     */
    AggregatePlan(std::shared_ptr<Plan> subplan, 
                  std::vector<std::shared_ptr<ast::AggregateExpr>> agg_exprs,
                  std::vector<TabCol> group_by_cols = {},
                  std::vector<Condition> having_conds = {}) {
        Plan::tag = T_Aggregate;
        subplan_ = std::move(subplan);
        agg_exprs_ = std::move(agg_exprs);
        group_by_cols_ = std::move(group_by_cols);
        having_conds_ = std::move(having_conds);
    }
    ~AggregatePlan() {}

    std::shared_ptr<Plan> subplan_;                                     // 子计划，数据来源
    std::vector<std::shared_ptr<ast::AggregateExpr>> agg_exprs_;       // 聚合表达式列表
    std::vector<TabCol> group_by_cols_;                                // GROUP BY列列表
    std::vector<Condition> having_conds_;                             // HAVING条件列表
};

/**
 * @brief 设置系统参数计划类
 * 用于SET语句的执行计划，允许动态调整数据库系统的运行参数
 * 如启用/禁用某些优化选项、连接算法等
 */
class SetKnobPlan : public Plan {
   public:
    /**
     * @brief 构造函数
     * @param knob_type 参数类型(如连接算法开关等)
     * @param bool_value 布尔值(启用/禁用)
     */
    SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
        Plan::tag = T_SetKnob;
        set_knob_type_ = knob_type;
        bool_value_ = bool_value;
    }

    ast::SetKnobType set_knob_type_;  // 要设置的参数类型
    bool bool_value_;                 // 参数值(布尔类型)
};

/**
 * @brief 规划器信息类
 * 用于在查询规划过程中传递和存储中间信息
 * 包含解析结果、条件、列信息、执行计划等规划过程中的关键数据
 */
class plannerInfo {
   public:
    std::shared_ptr<ast::SelectStmt> parse;                   // 解析后的SELECT语句AST
    std::vector<Condition> where_conds;                       // WHERE条件列表
    std::vector<TabCol> sel_cols;                             // 选择的列列表
    std::shared_ptr<Plan> plan;                               // 生成的执行计划
    std::vector<std::shared_ptr<Plan>> table_scan_executors;  // 表扫描执行器列表
    std::vector<SetClause> set_clauses;                       // SET子句列表(UPDATE使用)

    /**
     * @brief 构造函数
     * @param parse_ 解析后的SELECT语句AST
     */
    plannerInfo(std::shared_ptr<ast::SelectStmt> parse_) : parse(std::move(parse_)) {}
};
