#!/bin/bash

echo "=== 测试点4：ORDER BY 语句测试 ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf test_point4_db

# 检查是否有编译好的可执行文件
if [ ! -f "./build/bin/rmdb" ]; then
    echo "数据库服务器可执行文件不存在，尝试编译..."
    
    # 创建build目录并编译
    mkdir -p build
    cd build
    cmake ..
    make -j4
    cd ..
    
    if [ ! -f "./build/bin/rmdb" ]; then
        echo "❌ 编译失败，无法找到rmdb可执行文件"
        exit 1
    fi
fi

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb test_point4_db > server_test4.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行测试点4 - ORDER BY语句测试..."

# 运行测试SQL
cat test_point4.sql | ./rmdb_client/rmdb_client > test_point4_db/output.txt 2>&1

# 显示输出结果
echo "测试结果："
cat test_point4_db/output.txt

echo ""
echo "期待输出："
echo "| vendor | invoice_number | amount |"
echo "| nancy | 1001 | 89.750000 |"
echo "| sunny | 1001 | 95.250000 |"

# 检查输出结果是否包含期待的内容
if grep -q "nancy.*1001.*89.750000" test_point4_db/output.txt && grep -q "sunny.*1001.*95.250000" test_point4_db/output.txt; then
    echo ""
    echo "✅ 测试通过：ORDER BY语句正确返回了按invoice_number和amount排序的前2条记录"
else
    echo ""
    echo "❌ 测试失败：输出结果与期待不符"
fi

# 检查是否生成了output.txt
if [ -f "test_point4_db/output.txt" ]; then
    echo "✅ output.txt文件已生成在test_point4_db/目录下"
else
    echo "❌ output.txt文件生成失败"
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "测试点4完成" 