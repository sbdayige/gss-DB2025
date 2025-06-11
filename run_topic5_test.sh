#!/bin/bash

# 题目五聚合函数测试脚本

echo "=== 题目五：聚合函数与分组统计 测试 ==="

# 停止之前的服务器
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf test_agg_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb test_agg_db > server.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "测试基本连接..."
echo "SHOW TABLES;" | ./rmdb_client/rmdb_client > /dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "✅ 数据库连接成功"
else
    echo "❌ 数据库连接失败"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi

echo "创建测试表..."
echo "CREATE TABLE grade (course char(20), id int, score float);" | ./rmdb_client/rmdb_client

echo "插入测试数据..."
cat << EOF | ./rmdb_client/rmdb_client
INSERT INTO grade VALUES('DataStructure', 1, 95);
INSERT INTO grade VALUES('DataStructure', 2, 93.5);
INSERT INTO grade VALUES('DataStructure', 4, 87);
INSERT INTO grade VALUES('DataStructure', 3, 85);
INSERT INTO grade VALUES('DB', 1, 94);
INSERT INTO grade VALUES('DB', 2, 74.5);
INSERT INTO grade VALUES('DB', 4, 83);
INSERT INTO grade VALUES('DB', 3, 87);
EOF

echo "验证数据插入..."
echo "SELECT * FROM grade;" | ./rmdb_client/rmdb_client

echo "测试聚合函数..."
echo "SELECT COUNT(*) AS row_num FROM grade;" | ./rmdb_client/rmdb_client

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "测试完成" 