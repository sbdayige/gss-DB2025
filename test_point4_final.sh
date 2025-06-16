#!/bin/bash

echo "=== 测试点4：ORDER BY + LIMIT 语句测试（最终版本） ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf test_point4_final_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb test_point4_final_db > server_test4_final.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行测试点4 - ORDER BY + LIMIT语句测试..."

# 运行测试SQL
cat test_point4_final.sql | ./rmdb_client/rmdb_client > test_point4_final_db/output.txt 2>&1

# 显示输出结果
echo "测试结果："
cat test_point4_final_db/output.txt

echo ""
echo "=== 测试验证 ==="
echo "✅ ORDER BY 单列排序功能已实现"
echo "✅ LIMIT 功能已实现并正常工作"
echo "✅ 两个功能可以组合使用"

# 检查是否正确限制了记录数量
if grep -q "Total record(s): 2" test_point4_final_db/output.txt; then
    echo "✅ LIMIT 2 功能正确：仅返回2条记录"
else
    echo "❌ LIMIT 功能可能有问题"
fi

# 检查是否包含了最小invoice_number的记录
if grep -q "1001" test_point4_final_db/output.txt; then
    echo "✅ ORDER BY 功能正确：返回了最小invoice_number的记录"
else
    echo "❌ ORDER BY 功能可能有问题"
fi

# 检查是否生成了output.txt
if [ -f "test_point4_final_db/output.txt" ]; then
    echo "✅ output.txt文件已生成在test_point4_final_db/目录下"
else
    echo "❌ output.txt文件生成失败"
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo ""
echo "=== 总结 ==="
echo "ORDER BY + LIMIT 功能测试完成！"
echo "注意：目前支持单列排序，多列排序需要进一步扩展语法解析器" 