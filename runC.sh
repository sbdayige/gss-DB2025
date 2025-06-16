#!/bin/bash

# 检查客户端程序是否存在
if [ ! -f "rmdb_client/build/rmdb_client" ]; then
    echo "错误: rmdb_client 不存在，请先编译项目"
    exit 1
fi

# 创建测试SQL文件

cat > test_commands.sql << EOF
create table teams (team_id int, team_name char(20), city char(20));
create table players (player_id int, team_id int, player_name char(20), points int);
insert into teams values (1, 'Rockets', 'Houston');
insert into teams values (2, 'Lakers', 'LA');
insert into players values (101, 1, 'john', 2300);
insert into players values (102, 1, 'mike', 1800);
insert into players values (103, 2, 'tony', 2100);
select t.team_name, p.player_name, p.points from teams t join players p on t.team_id = p.team_id;
drop table teams;
drop table players;
EOF

echo "正在启动数据库客户端并执行测试命令..."
echo "=========================================="

# 运行客户端并传入测试命令
rmdb_client/build/rmdb_client < test_commands.sql

echo "=========================================="
echo "测试完成"

# 清理临时文件
rm -f test_commands.sql

# cd /home/guo/code/copy/gss-DB2025 && chmod +x runC.sh && ./runC.sh