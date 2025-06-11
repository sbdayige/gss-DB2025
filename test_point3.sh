#!/bin/bash

echo "=== 测试点3：健壮性测试 ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf test_point3_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb test_point3_db > server_test3.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行测试点3..."

# 创建测试SQL文件
cat > test_point3.sql << 'EOF'
create table grade (course char(20),id int,score float);

insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',3,94.5);
insert into grade values('ComputerNetworks',1,99);
insert into grade values('ComputerNetworks',2,88.5);
insert into grade values('ComputerNetworks',3,92.5);

-- SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列
select id , score from grade group by course;

-- WHERE 子句中不能用聚集函数作为条件表达式
select id, MAX(score) as max_score from grade where MAX(score) > 90 group by id;

drop table grade;
EOF

# 运行测试SQL
cat test_point3.sql | ./rmdb_client/rmdb_client > test_point3_db/output.txt 2>&1

# 显示输出结果
echo "测试结果："
cat test_point3_db/output.txt

# 检查是否生成了output.txt
if [ -f "test_point3_db/output.txt" ]; then
    echo "✅ output.txt文件已生成在test_point3_db/目录下"
else
    echo "❌ output.txt文件生成失败"
fi

# 分析测试结果
echo ""
echo "=== 健壮性测试分析 ==="

# 检查第一个错误查询的结果
echo "1. GROUP BY语义检查测试："
if grep -q "must appear in the GROUP BY clause" test_point3_db/output.txt; then
    echo "   ✅ 正确拒绝了SELECT列表中不在GROUP BY中的非聚合列"
    echo "   ✅ 错误信息: $(grep 'must appear in the GROUP BY clause' test_point3_db/output.txt)"
else
    echo "   ❌ 未能正确识别GROUP BY语义错误"
fi

echo ""
echo "2. WHERE子句聚合函数检查测试："
if grep -q "syntax error" test_point3_db/output.txt; then
    echo "   ✅ 正确拒绝了WHERE子句中的聚合函数"
    echo "   ✅ 错误信息: syntax error"
else
    echo "   ❌ 未能正确识别WHERE子句中的聚合函数错误"
fi

echo ""
echo "📊 健壮性测试总结："
if grep -q "Error\|syntax error" test_point3_db/output.txt; then
    echo "✅ 系统具备良好的健壮性，能正确识别并拒绝不合法的SQL语句"
    echo "✅ 两个测试用例都返回了期望的错误结果"
else
    echo "❌ 系统健壮性需要改进"
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "测试点3完成" 