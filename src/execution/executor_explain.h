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
#include <map>
#include <set>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "optimizer/plan.h"
#include "system/sm.h"

/**
 * @brief EXPLAIN操作的执行器类
 *
 * ExplainExecutor是RMDB数据库系统中负责执行EXPLAIN语句的核心组件。
 * 它能够将查询计划树格式化输出为可读的文本形式，帮助用户理解查询的执行方式。
 *
 * 主要功能：
 * 1. 递归遍历查询计划树
 * 2. 格式化输出每个计划节点的信息
 * 3. 按照特定的格式要求生成树状结构
 * 4. 支持四种基本的计划节点类型：Scan、Filter、Project、Join
 */
class ExplainExecutor : public AbstractExecutor {
   private:
    std::shared_ptr<Plan> plan_;                          ///< 要解释的查询计划
    Context *context_;                                    ///< 查询执行上下文
    std::string plan_output_;                             ///< 格式化后的计划输出
    bool has_executed_;                                   ///< 是否已经执行过
    mutable std::vector<ColMeta> explain_cols_;           ///< EXPLAIN输出的列元数据
    std::map<std::string, std::string> table_alias_map_;  ///< 实际表名到别名的映射（用于显示）
    bool is_select_star_;                                 ///< 是否为SELECT *查询

    /**
     * @brief 递归格式化查询计划树
     * @param plan 当前计划节点
     * @param indent 当前缩进级别
     * @return 格式化后的计划字符串
     */
    std::string format_plan_tree(std::shared_ptr<Plan> plan, int indent = 0);

    /**
     * @brief 获取计划节点的名称
     * @param plan 计划节点
     * @return 节点名称字符串
     */
    std::string get_plan_name(std::shared_ptr<Plan> plan);

    /**
     * @brief 生成缩进字符串
     * @param level 缩进级别
     * @return 缩进字符串
     */
    std::string generate_indent(int level);

    /**
     * @brief 收集计划树中涉及的所有表名
     * @param plan 计划节点
     * @param table_set 表名集合（输出参数）
     */
    void collectTableNames(std::shared_ptr<Plan> plan, std::set<std::string> &table_set);

    /**
     * @brief 构建计划树字符串
     * @param plan 计划节点
     * @param indent 缩进级别
     */
    void build_plan_tree_string(std::shared_ptr<Plan> plan, int indent = 0);

    /**
     * @brief 将实际表名转换为显示用的别名
     * @param table_name 实际表名
     * @return 显示用的表名（别名或实际表名）
     */
    std::string get_display_table_name(const std::string &table_name);

   public:
    /**
     * @brief 构造函数
     * @param plan 要解释的查询计划
     * @param context 查询执行上下文
     * @param table_alias_map 实际表名到别名的映射（用于EXPLAIN输出中显示原始别名）
     * @param is_select_star 是否为SELECT *查询
     */
    ExplainExecutor(std::shared_ptr<Plan> plan, Context *context,
                    const std::map<std::string, std::string> &table_alias_map = {},
                    bool is_select_star = false)
        : plan_(plan), context_(context), has_executed_(false), table_alias_map_(table_alias_map), is_select_star_(is_select_star) {}

    /**
     * @brief 开始执行EXPLAIN操作
     */
    void beginTuple() override;

    /**
     * @brief 获取下一个结果记录
     * @return 包含EXPLAIN输出的记录
     */
    std::unique_ptr<RmRecord> Next() override;

    /**
     * @brief 检查是否已经执行完毕
     * @return true表示已执行完毕，false表示还有结果
     */
    bool is_end() const override;

    /**
     * @brief 移动到下一个记录（EXPLAIN只有一条输出记录）
     */
    void nextTuple() override;

    /**
     * @brief 获取当前记录的RID（EXPLAIN不需要）
     * @return 空的RID
     */
    Rid &rid() override;

    /**
     * @brief 获取输出模式（EXPLAIN输出格式固定）
     * @return 包含EXPLAIN输出列信息的模式
     */
    const std::vector<ColMeta> &cols() const override;

    /**
     * @brief 析构函数
     */
    ~ExplainExecutor() override = default;
};
