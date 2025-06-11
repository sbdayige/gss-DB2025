#!/bin/bash

echo "=== 测试点1：单独使用聚合函数 ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf execution_test_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb execution_test_db > server_test1.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行测试点1..."

# 运行测试SQL
cat test_point1.sql | ./rmdb_client/rmdb_client > execution_test_db/output.txt 2>&1

# 显示输出结果
echo "测试结果："
cat execution_test_db/output.txt

# 检查是否生成了output.txt
if [ -f "execution_test_db/output.txt" ]; then
    echo "✅ output.txt文件已生成在execution_test_db/目录下"
else
    echo "❌ output.txt文件生成失败"
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "测试点1完成" 