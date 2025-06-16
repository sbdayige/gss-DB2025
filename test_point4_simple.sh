#!/bin/bash

echo "=== 测试点4：ORDER BY 语句测试（简化版本） ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf test_point4_simple_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb test_point4_simple_db > server_test4_simple.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行测试点4 - ORDER BY语句测试（单列排序）..."

# 运行测试SQL
cat test_point4_simple.sql | ./rmdb_client/rmdb_client > test_point4_simple_db/output.txt 2>&1

# 显示输出结果
echo "测试结果："
cat test_point4_simple_db/output.txt

echo ""
echo "期待输出（按invoice_number升序排列的前2条）："
echo "应该是invoice_number为1001的记录"

# 检查输出结果是否包含1001的记录
if grep -q "1001" test_point4_simple_db/output.txt; then
    echo ""
    echo "✅ 测试部分通过：找到了1001的记录"
else
    echo ""
    echo "❌ 测试失败：输出结果中没有找到1001的记录"
fi

# 检查是否生成了output.txt
if [ -f "test_point4_simple_db/output.txt" ]; then
    echo "✅ output.txt文件已生成在test_point4_simple_db/目录下"
else
    echo "❌ output.txt文件生成失败"
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "简化版测试点4完成" 