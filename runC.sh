#!/bin/bash

# 检查客户端程序是否存在
if [ ! -f "rmdb_client/build/rmdb_client" ]; then
    echo "错误: rmdb_client 不存在，请先编译项目"
    exit 1
fi

# 创建测试SQL文件

cat > test_commands.sql << EOF
CREATE TABLE users (id INT, name CHAR(50), age INT);

BEGIN;
INSERT INTO users VALUES (1, 'Alice', 25);
INSERT INTO users VALUES (2, 'Bob', 30);
UPDATE users SET age = 26 WHERE id = 1;
COMMIT;

BEGIN;
DELETE FROM users WHERE id = 2;
INSERT INTO users VALUES (3, 'Charlie', 35);
UPDATE users SET name = 'Alice_Updated' WHERE id = 1;
COMMIT;

BEGIN;
INSERT INTO users VALUES (4, 'David', 28);
UPDATE users SET age = 36 WHERE id = 3;
EOF

echo "正在启动数据库客户端并执行测试命令..."
echo "=========================================="

# 运行客户端并传入测试命令
rmdb_client/build/rmdb_client < test_commands.sql

echo "=========================================="
echo "测试完成"

# 清理临时文件
rm -f test_commands.sql

# cd /home/sword1e1/code/gssdb && chmod +x runC.sh && ./runC.sh