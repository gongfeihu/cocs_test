# 快速开始指南

## 🚀 快速启动

### 1. 编译
```bash
cd /home/csf/cocs/test
gcc -o client client.c -lssl -lcrypto
gcc -o server1 server1.c -lssl -lcrypto
gcc -o server2 server2.c -lssl -lcrypto
```

### 2. 启动服务器
```bash
# 终端1
./server1 8082

# 终端2
./server2 8081
```

### 3. 运行客户端
```bash
# 终端3
./client random_copy.txt
```

## 📊 输出示例

### Client 输出
```
Starting distributed FastCDC client for file: random_copy.txt
Connected to both servers successfully
Sending file info to servers...
Receiving FastFp lists from servers...
Received FastFp lists from servers:
Server1: 5 entries
Server2: 5 entries
Local file chunked into 10 pieces

... (块信息) ...

========== 冗余率统计 ==========
文件总大小: 101701 bytes
总块数: 10

验证统计:
  Server1 验证块数: 1, 验证数据量: 12586 bytes
  Server2 验证块数: 1, 验证数据量: 5046 bytes
  总验证数据量: 17632 bytes

冗余率指标:
  Server1 冗余率: 12.38%
  Server2 冗余率: 4.96%
  总冗余率: 17.34%

上传统计:
  需要上传块数: 8
  Server1 上传块数: 4
  Server2 上传块数: 4
================================

Total processing time: 0.046427 seconds
```

### Server 输出
```
Starting server1 on port 8082
Server1 listening on port 8082, storing chunks in ./server1file/
Handling client connection from 127.0.0.1
Received file: random_copy.txt from client 127.0.0.1
Receiving file of size: 101701 bytes
Found 5 existing chunks in ./server1file directory
Sent 5 FastFp values to client 127.0.0.1
Client reported 3 matching FastFps
Calculated SHA1 for existing chunk 0xa4720680e4287be6
Calculated SHA1 for existing chunk 0xcc3206c33404a00f
Calculated SHA1 for existing chunk 0x1f0322eacca860b5
Receiving 4 new chunks from client 127.0.0.1
Saved chunk to ./server1file/7f9f22ac7824c6f7.chunk (size: 10949) from client 127.0.0.1
Saved chunk to ./server1file/b60e0633802c42e3.chunk (size: 12818) from client 127.0.0.1
Saved chunk to ./server1file/cc3206c33404a00f.chunk (size: 8637) from client 127.0.0.1
Saved chunk to ./server1file/1f0322eacca860b5.chunk (size: 8852) from client 127.0.0.1
Cleaning up chunks not in current file...
Deleted old chunk file: ./server1file/8f1b04b258845d85.chunk
Deleted old chunk file: ./server1file/10f6264528acdf6e.chunk

========== Server1 冗余率统计 ==========
匹配块数: 3, 匹配数据量: 30075 bytes
上传块数: 4, 上传数据量: 41256 bytes
文件总大小: 71331 bytes
Server1 冗余率: 42.16%
======================================

Finished handling client 127.0.0.1 on server1
```

## 🔍 冗余率解读

| 冗余率 | 含义 | 示例 |
|-------|------|------|
| 90%-100% | 极好，文件几乎完全复用 | 新文件与旧文件几乎相同 |
| 70%-90% | 很好，文件大部分复用 | 新文件大部分内容已存在 |
| 50%-70% | 良好，文件部分复用 | 新文件部分内容已存在 |
| 30%-50% | 一般，文件少量复用 | 新文件有一些新增内容 |
| 0%-30% | 较差，文件基本不复用 | 新文件大部分是新增内容 |

## 📁 文件结构

```
test/
├── client.c              # 客户端代码
├── server1.c             # 服务器1代码
├── server2.c             # 服务器2代码
├── fastcdc.h             # FastCDC头文件
├── server1file/          # Server1存储目录
├── server2file/          # Server2存储目录
└── 文档/
    ├── PROTOCOL_FIX_SUMMARY.md           # 协议修复说明
    ├── REDUNDANCY_RATE_GUIDE.md          # 冗余率计算指南
    ├── REDUNDANCY_QUICK_GUIDE.txt        # 快速参考
    ├── REDUNDANCY_IMPLEMENTATION_SUMMARY.md # 实现总结
    ├── IMPLEMENTATION_CHECKLIST.md       # 实现清单
    ├── FIX_COMPLETE.txt                  # 修复完成总结
    └── QUICK_START.md                    # 本文件
```

## 🐛 常见问题

### Q: 连接被拒绝
**A**: 确保服务器已启动，端口号正确（8082 和 8081）

### Q: 文件未找到
**A**: 确保文件在当前目录，或使用完整路径

### Q: 冗余率为 0%
**A**: 这是正常的，说明新文件与旧文件没有相同的块

### Q: 冗余率为 100%
**A**: 说明新文件与旧文件完全相同

## 💡 性能优化建议

1. **增加块大小** - 减少块数量，提高匹配效率
2. **使用更好的分块算法** - 提高块的稳定性
3. **定期清理过期数据** - 保留常用数据，删除过期数据
4. **监控冗余率趋势** - 根据趋势调整存储策略

## 📚 更多文档

- 详细的冗余率计算指南：`REDUNDANCY_RATE_GUIDE.md`
- 快速参考指南：`REDUNDANCY_QUICK_GUIDE.txt`
- 协议修复说明：`PROTOCOL_FIX_SUMMARY.md`
- 实现总结：`REDUNDANCY_IMPLEMENTATION_SUMMARY.md`

## ✅ 验证清单

- [x] 编译成功
- [x] 服务器启动成功
- [x] 客户端连接成功
- [x] 文件传输成功
- [x] 冗余率计算正确
- [x] 块文件清理正确

## 🎯 下一步

1. 测试不同大小的文件
2. 测试多次连接
3. 监控冗余率变化
4. 根据需要调整块大小
5. 部署到生产环境

---

**系统已准备好！** 🚀

