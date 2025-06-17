#!/bin/bash

# 检查客户端程序是否存在
if [ ! -f "rmdb_client/build/rmdb_client" ]; then
    echo "错误: rmdb_client 不存在，请先编译项目"
    exit 1
fi

# 创建测试SQL文件

cat > test_commands.sql << EOF

create table warehouse (w_id int, name char(8));

insert into warehouse values (10 , 'qweruiop');

insert into warehouse values (534, 'asdfhjkl');

insert into warehouse values (100,'qwerghjk');

insert into warehouse values (500,'bgtyhnmj');


EOF

echo "正在启动数据库客户端并执行测试命令..."
echo "=========================================="

# 运行客户端并传入测试命令
rmdb_client/build/rmdb_client < test_commands.sql

echo "=========================================="
echo "测试完成"

# 清理临时文件
rm -f test_commands.sql