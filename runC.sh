#!/bin/bash

# 检查客户端程序是否存在
if [ ! -f "rmdb_client/build/rmdb_client" ]; then
    echo "错误: rmdb_client 不存在，请先编译项目"
    exit 1
fi

# 创建测试SQL文件

cat > test_commands.sql << EOF
create table warehouse (id int, name char(8));
create index warehouse (id);
show index from warehouse;
create index warehouse (id,name);
show index from warehouse;
drop index warehouse (id);
drop index warehouse (id,name);
show index from warehouse;
drop table warehouse;
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