# 分布式FastCDC系统修复总结

## 问题分析

你的流程设计思路是完全正确的，但代码实现中存在以下关键问题：

### 1. **通信协议问题** ❌
**原问题**：客户端发送文件时没有发送文件大小，导致服务器无法知道何时停止接收文件数据
- 客户端发送：文件名长度 → 文件名 → 文件数据（无结束标记）
- 服务器接收：文件名长度 → 文件名 → 文件数据（一直等待，导致阻塞）

**修复**：在发送文件内容前先发送文件大小
```c
// 发送文件大小
fseek(file, 0, SEEK_END);
long file_size = ftell(file);
fseek(file, 0, SEEK_SET);
send_all(sock, &file_size, sizeof(long));

// 服务器接收文件大小
long file_size = 0;
recv_all(client_socket, &file_size, sizeof(long));
// 按照file_size接收数据
while (total_size < file_size && (bytes_read = recv(...)) > 0) {
    total_size += bytes_read;
}
```

### 2. **FastFp匹配逻辑问题** ❌
**原问题**：对server1和server2的匹配检查是独立的，导致同一个块可能被标记为与两个服务器都匹配

**修复**：使用标记数组来避免重复匹配
```c
int *matched_chunks = calloc(chunk_num, sizeof(int));

// 先检查server1的匹配
for (int i = 0; i < chunk_num; i++) {
    if (server1匹配) {
        matched_chunks[i] = 1;  // 标记为已匹配
    }
}

// 再检查server2的匹配（跳过已匹配的块）
for (int i = 0; i < chunk_num; i++) {
    if (matched_chunks[i]) continue;  // 跳过已匹配的块
    if (server2匹配) {
        matched_chunks[i] = 2;
    }
}
```

### 3. **SHA1验证逻辑不完整** ❌
**原问题**：计算了SHA1但没有正确使用验证结果来决定是否真正匹配

**修复**：添加验证标记数组，只有SHA1验证成功的块才算真正匹配
```c
int *server1_verified = calloc(chunk_num, sizeof(int));
int *server2_verified = calloc(chunk_num, sizeof(int));

// 验证SHA1
if (match) {
    server1_verified[chunk_idx] = 1;
    server1_verified_matches++;
}

// 上传时只上传未验证的块
for (int i = 0; i < chunk_num; i++) {
    if (server1_verified[i] || server2_verified[i]) {
        continue;  // 已验证，不需要上传
    }
    // 需要上传
}
```

### 4. **块查找逻辑错误** ❌
**原问题**：在`send_new_chunks`函数中，查找块索引时只搜索`upload_count`个块，而不是所有`chunk_num`个块

**修复**：搜索所有块来找到对应的FastFp
```c
// 修复前：只搜索upload_count个块
for (int j = 0; j < upload_count; j++) {
    if (local_fastfps[j] == fastfp) {
        chunk_idx = j;
        break;
    }
}

// 修复后：搜索所有chunk_num个块
for (int j = 0; j < chunk_num; j++) {
    if (local_fastfps[j] == fastfp) {
        chunk_idx = j;
        break;
    }
}
```

### 5. **清理逻辑有哈希冲突风险** ⚠️
**原问题**：使用简单的哈希表（`% 10000`）来标记FastFp，可能导致哈希冲突

**修复**：使用直接比较而不是哈希
```c
// 修复前：使用哈希表
int *fastfp_exists = calloc(10000, sizeof(int));
int index = (fastfp % 10000);
fastfp_exists[index] = 1;

// 修复后：直接比较
int found = 0;
for (int i = 0; i < count; i++) {
    if (current_fastfps[i] == fastfp) {
        found = 1;
        break;
    }
}
```

## 完整流程验证

### 第一次运行（初始上传）
```
客户端分块：10个块 (Chunk 0-9)
Server1接收：5个块 (0, 2, 4, 6, 8) - 偶数索引
Server2接收：5个块 (1, 3, 5, 7, 9) - 奇数索引
```

### 第二次运行（相同文件，测试匹配）
```
FastFp匹配：
  - Server1: 5个块全部匹配 (0, 2, 4, 6, 8)
  - Server2: 5个块全部匹配 (1, 3, 5, 7, 9)

SHA1验证：
  - Server1: 5个块全部验证成功
  - Server2: 5个块全部验证成功

上传新块：0个（所有块都已验证）
```

## 测试结果

✅ **通信协议正常**：FastFp列表成功接收，无"Failed to receive FastFp list size"错误
✅ **匹配功能正常**：第二次运行时正确识别所有匹配的块
✅ **SHA1验证正常**：所有匹配的块都通过SHA1验证
✅ **上传功能正常**：新块成功上传到对应的服务器
✅ **清理功能正常**：旧块被正确删除（当上传新文件时）

## 文件修改

修改的文件：
1. `client.c` - 修复通信协议、匹配逻辑、验证逻辑、块查找逻辑
2. `server1.c` - 修复文件接收逻辑、清理逻辑
3. `server2.c` - 修复文件接收逻辑、清理逻辑

## 关键改进

| 问题 | 修复前 | 修复后 |
|------|-------|-------|
| 文件接收 | 阻塞，无法继续 | 正常接收，继续处理 |
| FastFp匹配 | 可能重复匹配 | 避免重复匹配 |
| SHA1验证 | 未使用验证结果 | 正确使用验证结果 |
| 块查找 | 搜索范围错误 | 搜索范围正确 |
| 清理逻辑 | 有哈希冲突风险 | 直接比较，无风险 |

你的设计思路完全正确，现在代码实现也完全正确了！[object Object]



