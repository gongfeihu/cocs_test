#!/bin/bash
cd /home/csf/cocs/test

# 清理旧进程和文件
pkill -9 server1 2>/dev/null
pkill -9 server2 2>/dev/null
sleep 1

rm -rf server1file/* server2file/*

# 启动服务器
./server1 &
SERVER1_PID=$!
./server2 &
SERVER2_PID=$!

sleep 2

# 运行客户端
echo "=== Running client ==="
./client random.txt

# 等待一下
sleep 1

# 检查结果
echo ""
echo "=== Server1 files ==="
ls -lh server1file/

echo ""
echo "=== Server2 files ==="
ls -lh server2file/

# 清理
kill $SERVER1_PID 2>/dev/null
kill $SERVER2_PID 2>/dev/null




