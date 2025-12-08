#!/bin/bash
cd /home/csf/cocs/test

# 清理旧进程和文件
pkill -9 server1 2>/dev/null
pkill -9 server2 2>/dev/null
sleep 1

rm -rf server1file/* server2file/*

# 启动服务器
./server1 > server1.log 2>&1 &
SERVER1_PID=$!
./server2 > server2.log 2>&1 &
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
ls -la server1file/

echo ""
echo "=== Server2 files ==="
ls -la server2file/

echo ""
echo "=== Server1 log ==="
cat server1.log

echo ""
echo "=== Server2 log ==="
cat server2.log

# 清理
kill $SERVER1_PID 2>/dev/null
kill $SERVER2_PID 2>/dev/null




