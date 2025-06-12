#!/bin/bash

echo "=== 测试点2：聚合函数加分组统计 ==="

# 停止之前的服务器进程
killall rmdb 2>/dev/null

# 删除旧的数据库文件
rm -rf test_point2_db

# 启动服务器
echo "启动数据库服务器..."
./build/bin/rmdb test_point2_db > server_test2.log 2>&1 &
SERVER_PID=$!

# 等待服务器启动
sleep 3

echo "运行测试点2..."

# 创建测试SQL文件
cat > test_point2.sql << 'EOF'
create table grade (course char(20),id int,score float);

insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',3,94.5);
insert into grade values('ComputerNetworks',1,99);
insert into grade values('ComputerNetworks',2,88.5);
insert into grade values('ComputerNetworks',3,92.5);
insert into grade values('C++',1,92);
insert into grade values('C++',2,89);
insert into grade values('C++',3,89.5);

select id,max(score) as max_score,min(score) as min_score,sum(score) as sum_score from grade group by id;

select id,max(score) as max_score from grade group by id having count(*) > 3;

insert into grade values ('ParallelCompute',1,100);

select id,max(score) as max_score from grade group by id having count(*) > 3;

select id,max(score) as max_score,min(score) as min_score from grade group by id having count(*) > 1 and min(score) > 88;

select course ,COUNT(*) as row_num , COUNT(id) as student_num , MAX(score) as top_score, MIN(score) as lowest_score from grade group by course;

select course, id, score from grade order by score desc;

drop table grade;
EOF

# 运行测试SQL
cat test_point2.sql | ./rmdb_client/rmdb_client > test_point2_db/output.txt 2>&1

# 显示输出结果
echo "测试结果："
cat test_point2_db/output.txt

# 检查是否生成了output.txt
if [ -f "test_point2_db/output.txt" ]; then
    echo "✅ output.txt文件已生成在test_point2_db/目录下"
else
    echo "❌ output.txt文件生成失败"
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "测试点2完成" 