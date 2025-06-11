#!/bin/bash

echo "=== 调试WHERE条件 ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf debug_where_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb debug_where_db > debug_where_server.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行WHERE条件调试测试..."

# 运行测试SQL
cat debug_where.sql | ./rmdb_client/rmdb_client

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "WHERE条件调试完成" 