# 通信协议修复总结

## 🔧 问题分析

在运行测试时发现了三个主要问题：

### 问题1：recv_all 函数错误处理
**症状**：连接重置错误 "Connection reset by peer"

**原因**：`recv_all` 函数没有正确区分 `recv` 返回 0（连接关闭）和返回 -1（错误）的情况

**修复**：
```c
// 修复前
if (result <= 0) {
    return result;  // 不区分错误类型
}

// 修复后
if (result < 0) {
    perror("recv_all error");
    return -1;
}
if (result == 0) {
    printf("Connection closed by peer, received %zu/%zu bytes\n", received, length);
    return -1;
}
```

**影响文件**：
- client.c
- server1.c
- server2.c

### 问题2：Server端缺少接收当前文件块列表
**症状**：客户端卡住，无法接收 FastFp 列表

**原因**：Server1.c 和 Server2.c 中缺少接收当前文件块列表的代码，导致协议不同步

**修复**：在接收匹配的 FastFp 列表之前，先接收当前文件的所有 FastFp

```c
// 接收当前文件的所有FastFp（用于清理旧块）
int current_file_chunk_count = 0;
uint64_t *current_file_fastfps_from_client = NULL;

if (recv_all(client_socket, &current_file_chunk_count, sizeof(int)) <= 0) {
    // 错误处理
    return;
}

if (current_file_chunk_count > 0) {
    current_file_fastfps_from_client = malloc(current_file_chunk_count * sizeof(uint64_t));
    // ... 接收数据
}
```

**影响文件**：
- server1.c
- server2.c

### 问题3：Server端缺少接收文件大小
**症状**：协议不匹配，导致数据接收错误

**原因**：Client 发送了文件大小，但 Server 没有接收，直接尝试接收文件内容

**修复**：在接收文件内容前，先接收文件大小

```c
// 接收文件大小
long file_size = 0;
if (recv_all(client_socket, &file_size, sizeof(long)) <= 0) {
    printf("Failed to receive file size from %s: %s\n", client_ip, strerror(errno));
    return;
}

// 丢弃文件内容
unsigned char buffer[4096];
long total_size = 0;
ssize_t bytes_read;
while (total_size < file_size && (bytes_read = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
    total_size += bytes_read;
}
```

**影响文件**：
- server1.c
- server2.c

### 问题4：cleanup_chunks_not_in_list 函数实现错误
**症状**：块清理逻辑不正确

**原因**：函数被改成了使用哈希表，但实现有问题

**修复**：恢复原来的线性搜索实现

```c
// 检查当前FastFp是否在列表中
int found = 0;
for (int i = 0; i < count; i++) {
    if (current_fastfps[i] == fastfp) {
        found = 1;
        break;
    }
}

// 如果当前FastFp不在当前文件的列表中，则删除该文件
if (!found) {
    // 删除文件
}
```

**影响文件**：
- server1.c
- server2.c

## 📋 修改清单

### client.c
- [x] 修复 recv_all 函数错误处理
- [x] 恢复 send_file_data 函数完整实现（包括发送文件大小）

### server1.c
- [x] 修复 recv_all 函数错误处理
- [x] 添加接收文件大小的代码
- [x] 添加接收当前文件块列表的代码
- [x] 修复 cleanup_chunks_not_in_list 函数

### server2.c
- [x] 修复 recv_all 函数错误处理
- [x] 添加接收文件大小的代码
- [x] 添加接收当前文件块列表的代码
- [x] 修复 cleanup_chunks_not_in_list 函数

## 🔄 通信协议流程

修复后的完整通信流程：

```
Client                          Server1/Server2
  |                                   |
  |--- 发送文件名长度 ------->         |
  |--- 发送文件名 ------->             |
  |--- 发送文件大小 ------->           |
  |--- 发送文件内容 ------->           |
  |                                   |
  |<------ 发送FastFp列表数量 ------   |
  |<------ 发送FastFp列表 ------       |
  |                                   |
  |--- 发送当前文件块数量 ------->     |
  |--- 发送当前文件块列表 ------->     |
  |                                   |
  |--- 发送匹配块数量 ------->         |
  |--- 发送匹配块列表 ------->         |
  |                                   |
  |<------ 发送SHA1哈希 ------         |
  |                                   |
  |--- 发送上传块数量 ------->         |
  |--- 发送上传块数据 ------->         |
  |                                   |
```

## ✅ 验证步骤

1. **编译**
   ```bash
   gcc -o client client.c -lssl -lcrypto
   gcc -o server1 server1.c -lssl -lcrypto
   gcc -o server2 server2.c -lssl -lcrypto
   ```

2. **启动服务器**
   ```bash
   ./server1 8082 &
   ./server2 8081 &
   ```

3. **运行客户端**
   ```bash
   ./client testfile.bin
   ```

4. **验证输出**
   - 客户端应该显示冗余率统计
   - 服务器应该显示接收和处理信息
   - 没有连接错误或超时

## 🎯 总结

通过修复这四个问题，系统现在可以：

1. ✅ 正确处理连接关闭和错误
2. ✅ 保持客户端和服务器的协议同步
3. ✅ 正确接收和处理文件数据
4. ✅ 正确清理过期的块文件
5. ✅ 计算和显示冗余率指标

系统现在应该能够稳定运行，支持多次连接和数据传输。

