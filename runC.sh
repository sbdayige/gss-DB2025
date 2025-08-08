#!/bin/bash

# 检查客户端程序是否存在
if [ ! -f "rmdb_client/build/rmdb_client" ]; then
    echo "错误: rmdb_client 不存在，请先编译项目"
    exit 1
fi

# 创建测试SQL文件


echo "正在启动数据库客户端并执行测试命令..."
echo "=========================================="

# 运行客户端并传入测试命令
rmdb_client/build/rmdb_client < stress_test_multi_index.sql

echo "=========================================="
echo "测试完成"

# 清理临时文件
rm -f test_commands.sql

# cd /home/guo/code/copy/gss-DB2025 && chmod +x runC.sh && ./runC.sh

# python3 TPCC-Tester/db/load.py -w 1 -o src/test/performance_test/table_data

#python3 TPCC-Tester/runner.py --prepare --thread 8 --rw 120 --analyze --w 1

#python3 TPCC-Tester/runner.py --prepare --analyze --w 1

#python main.py -w 1 -p zucc20252025 -t /home/ac/Desktop/workspace/database/TPCC-Tester -f /home/ac/Desktop/workspace/database/DB2025/FlameGraph -m normal