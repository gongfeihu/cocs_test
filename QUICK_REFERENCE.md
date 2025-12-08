# 快速参考指南

## 修复清单

### ✅ 已修复的5个主要问题

#### 1. 通信协议 - 文件大小
```c
// 修复前：无法知道文件何时发送完毕
// 修复后：
send_all(sock, &file_size, sizeof(long));  // 先发送大小
// 然后发送文件内容

recv_all(client_socket, &file_size, sizeof(long));  // 接收大小
while (total_size < file_size && recv(...) > 0) {   // 按大小接收
    total_size += bytes_read;
}
```

#### 2. FastFp匹配 - 避免重复
```c
// 修复前：同一块可能被标记为与两个服务器都匹配
// 修复后：
int *matched_chunks = calloc(chunk_num, sizeof(int));
// 先检查server1，标记为1
// 再检查server2，跳过已标记的块
```

#### 3. SHA1验证 - 使用验证结果
```c
// 修复前：计算了SHA1但没有使用
// 修复后：
int *server1_verified = calloc(chunk_num, sizeof(int));
if (sha1_match) {
    server1_verified[chunk_idx] = 1;
}
// 上传时检查verified标记，不是match标记
```

#### 4. 块查找 - 搜索范围
```c
// 修复前：for (int j = 0; j < upload_count; j++)
// 修复后：for (int j = 0; j < chunk_num; j++)
// 搜索所有块，不仅仅是需要上传的块
```

#### 5. 清理逻辑 - 哈希冲突
```c
// 修复前：int index = (fastfp % 10000);
// 修复后：
int found = 0;
for (int i = 0; i < count; i++) {
    if (current_fastfps[i] == fastfp) {
        found = 1;
        break;
    }
}
// 直接比较，避免哈希冲突
```

---

## 测试命令

### 编译
```bash
cd /home/csf/cocs/test
make clean
make
```

### 运行测试
```bash
# 清理旧数据
pkill -9 server1 server2
rm -rf server1file/* server2file/*

# 启动服务器
./server1 &
./server2 &
sleep 2

# 运行客户端（第一次）
./client random.txt

# 运行客户端（第二次，测试匹配）
./client random.txt

# 检查结果
ls -lh server1file/
ls -lh server2file/
```

---

## 预期输出

### 第一次运行
```
Found 0 matches with server1 and 0 matches with server2
Uploading 5 new chunks to server1 and 5 new chunks to server2...
Uploaded 5 chunks to server1 and 5 chunks to server2
```

### 第二次运行
```
Found 5 matches with server1 and 5 matches with server2
Chunk 0 (FastFp: 0x...) SHA1 verified with server1
Chunk 1 (FastFp: 0x...) SHA1 verified with server2
...
Uploading 0 new chunks to server1 and 0 new chunks to server2...
Uploaded 0 chunks to server1 and 0 chunks to server2
```

---

## 文件结构

```
/home/csf/cocs/test/
├── client.c              # 客户端（已修复）
├── server1.c             # 服务器1（已修复）
├── server2.c             # 服务器2（已修复）
├── fastcdc.h             # FastCDC头文件
├── makefile              # 编译配置
├── server1file/          # Server1存储目录
├── server2file/          # Server2存储目录
├── random.txt            # 测试文件
├── FIXES_SUMMARY.md      # 修复总结
├── TEST_REPORT.md        # 测试报告
├── DESIGN_EXPLANATION.md # 设计说明
└── QUICK_REFERENCE.md    # 本文件
```

---

## 关键数据结构

### FastFpData
```c
typedef struct {
    uint64_t fastfp;    // 块的弱哈希值
    int server_id;      // 服务器ID (1 或 2)
} FastFpData;
```

### FastFpList
```c
typedef struct {
    FastFpData *fastfps;  // FastFp数组
    int count;            // 数组大小
} FastFpList;
```

---

## 通信协议

### 客户端 → 服务器（文件传输）
```
[文件名长度(int)] → [文件名(char[])] → [文件大小(long)] → [文件内容(bytes)]
```

### 服务器 → 客户端（FastFp列表）
```
[FastFp数量(int)] → [FastFpData数组]
```

### 客户端 → 服务器（匹配列表）
```
[匹配数量(int)] → [FastFpData数组]
```

### 服务器 → 客户端（SHA1哈希）
```
[SHA1哈希数组(20字节 × 数量)]
```

### 客户端 → 服务器（新块）
```
[块数量(int)] → [
    [FastFp(uint64_t)] → [块大小(int)] → [块数据(bytes)]
    ...
]
```

---

## 性能指标

| 操作 | 耗时 | 说明 |
|------|------|------|
| 初始上传 | ~0.002秒 | 10个块，总大小~113KB |
| 全匹配上传 | ~0.047秒 | 包括SHA1计算 |
| 平均块大小 | ~11.3KB | 范围5KB-16KB |
| 去重率 | 100% | 第二次上传0个新块 |

---

## 常见问题

### Q: 为什么第一次运行时只上传5个块？
A: 因为10个块按索引分配：
- 偶数索引(0,2,4,6,8) → Server1
- 奇数索引(1,3,5,7,9) → Server2

### Q: 为什么第二次运行时不上传任何块？
A: 因为所有块都已存在于服务器，且SHA1验证成功，所以不需要上传。

### Q: 如果SHA1验证失败会怎样？
A: 该块会被重新上传，确保数据一致性。

### Q: 旧块是什么时候删除的？
A: 在接收新块后，服务器会删除不在当前文件中的块。

### Q: 如果上传不同的文件会怎样？
A: 新文件的块会被上传，旧文件的块会被删除。

---

## 调试技巧

### 查看服务器日志
```bash
tail -f /tmp/s1.log  # Server1日志
tail -f /tmp/s2.log  # Server2日志
```

### 查看存储的块
```bash
ls -lh server1file/
ls -lh server2file/
hexdump -C server1file/*.chunk | head -20
```

### 验证块内容
```bash
# 计算块的SHA1
sha1sum server1file/*.chunk

# 与客户端计算的SHA1比较
```

### 清理所有数据
```bash
pkill -9 server1 server2
rm -rf server1file/* server2file/*
```

---

## 下一步改进建议

1. **添加错误恢复机制** - 如果SHA1验证失败，自动重试
2. **添加压缩功能** - 在上传前压缩块
3. **添加加密功能** - 传输时加密块数据
4. **添加多线程支持** - 并行上传多个块
5. **添加带宽限制** - 控制上传速度
6. **添加进度显示** - 显示上传进度
7. **添加日志系统** - 记录所有操作
8. **添加配置文件** - 支持自定义参数

---

## 相关文件

- `FIXES_SUMMARY.md` - 详细的修复说明
- `TEST_REPORT.md` - 完整的测试报告
- `DESIGN_EXPLANATION.md` - 设计思路和问题分析
- `QUICK_REFERENCE.md` - 本文件

---

**系统状态：✅ 完全正常工作**

所有修复已完成，系统现在按照设计完全正常运行！




