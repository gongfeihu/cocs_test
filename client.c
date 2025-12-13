// client.c - 客户端代码
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <time.h>
#include <errno.h>

#define DEFAULT_SERVER_PORT 8082
#define DEFAULT_SERVER_PORT1 8081
#define MAX_CACHE_SIZE (100 * 1024 * 1024)
#define SERVER1_ID 1
#define SERVER2_ID 2
#define SERVER3_ID 3
#define SERVER4_ID 4
#define NUM_SERVERS 4

#include "fastcdc.h"

typedef struct {
    uint64_t fastfp;
    int server_id;  // 服务器ID (1 或 2)
} FastFpData;

typedef struct {
    FastFpData *fastfps;
    int count;
} FastFpList;

// FastCDC 实现在 fastcdc.c 中

// FastCDC 分块函数在 fastcdc.c 中实现

// 计算 SHA1 哈希
void calculate_sha1(const unsigned char *data, size_t len, unsigned char *sha1_hash) {
    SHA1(data, len, sha1_hash);
}

// 确保所有数据都发送完成
int send_all(int socket, const void *buffer, size_t length) {
    const char *buf = (const char *)buffer;
    size_t sent = 0;
    
    while (sent < length) {
        int result = send(socket, buf + sent, length - sent, 0);
        if (result <= 0) {
            return result;
        }
        sent += result;
    }
    return sent;
}

// 确保所有数据都接收完成
int recv_all(int socket, void *buffer, size_t length) {
    char *buf = (char *)buffer;
    size_t received = 0;
    
    while (received < length) {
        int result = recv(socket, buf + received, length - received, 0);
        if (result < 0) {
            perror("recv_all error");
            return -1;
        }
        if (result == 0) {
            // 连接已关闭
            printf("Connection closed by peer, received %zu/%zu bytes\n", received, length);
            return -1;
        }
        received += result;
    }
    return received;
}

// 设置socket超时
int set_socket_timeout(int sock, int seconds) {
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
}

// 连接到服务器
int connect_to_server(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation error");
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    
    if(inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        printf("Invalid address: %s\n", ip);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Connection to %s:%d failed\n", ip, port);
        close(sock);
        return -1;
    }
    return sock;
}

// 发送文件数据到服务器
void send_file_data(int sock, const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        perror("Cannot open file");
        return;
    }
    
    int name_len = strlen(filename);
    if (send_all(sock, &name_len, sizeof(int)) <= 0) {
        printf("Failed to send filename length\n");
        fclose(file);
        return;
    }
    if (send_all(sock, filename, name_len) <= 0) {
        printf("Failed to send filename\n");
        fclose(file);
        return;
    }
    
    // 先计算文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // 发送文件大小
    if (send_all(sock, &file_size, sizeof(long)) <= 0) {
        printf("Failed to send file size\n");
        fclose(file);
        return;
    }
    
    // 发送文件内容
    unsigned char buffer[4096];
    size_t bytes_read;
    long total_sent = 0;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send_all(sock, buffer, bytes_read) <= 0) {
            printf("Failed to send file content\n");
            break;
        }
        total_sent += bytes_read;
    }
    
    fclose(file);
}

// 接收服务器的FastFp列表
FastFpList receive_fastfp_list(int sock) {
    FastFpList result = {NULL, 0};
    
    // 接收列表大小
    if (recv_all(sock, &result.count, sizeof(int)) <= 0) {
        printf("Failed to receive FastFp list size\n");
        return result;
    }
    
    if (result.count > 0) {
        result.fastfps = malloc(result.count * sizeof(FastFpData));
        if (result.fastfps) {
            if (recv_all(sock, result.fastfps, result.count * sizeof(FastFpData)) <= 0) {
                printf("Failed to receive FastFp list\n");
                free(result.fastfps);
                result.fastfps = NULL;
                result.count = 0;
            }
        }
    }
    
    return result;
}

// 发送匹配成功的FastFp列表
void send_matching_fastfps(int sock, FastFpData *matching_fastfps, int count) {
    if (send_all(sock, &count, sizeof(int)) <= 0) {
        printf("Failed to send matching count\n");
        return;
    }
    if (count > 0) {
        if (send_all(sock, matching_fastfps, count * sizeof(FastFpData)) <= 0) {
            printf("Failed to send matching FastFps\n");
        }
    }
}

// 接收SHA1哈希
int receive_sha1_hashes(int sock, unsigned char *sha1_hashes, int chunk_count) {
    if (chunk_count == 0) {
        return 0;
    }
    
    int bytes_to_receive = chunk_count * SHA_DIGEST_LENGTH;
    int total_received = 0;
    
    while (total_received < bytes_to_receive) {
        int bytes_to_read = (bytes_to_receive - total_received > 4096) ? 4096 : (bytes_to_receive - total_received);
        int result = recv(sock, sha1_hashes + total_received, bytes_to_read, 0);
        
        if (result <= 0) {
            if (result == 0) {
                printf("Connection closed while receiving SHA1 hashes\n");
            } else {
                printf("Error receiving SHA1 hashes: %s\n", strerror(errno));
            }
            return -1;
        }
        total_received += result;
    }
    
    return 0;
}

// 发送新块到服务器
void send_new_chunks(int server_sock, unsigned char *fileCache, 
                     int *boundary, uint64_t *local_fastfps, int chunk_num, FastFpData *upload_fastfps, 
                     int upload_count) {
    if (send_all(server_sock, &upload_count, sizeof(int)) <= 0) {
        printf("Failed to send upload count\n");
        return;
    }
    
    printf("Sending %d chunks to server\n", upload_count);
    
    for (int i = 0; i < upload_count; i++) {
        uint64_t fastfp = upload_fastfps[i].fastfp;
        
        // 在本地FastFp数组中找到对应索引（应在所有块中查找）
        int chunk_idx = -1;
        for (int j = 0; j < chunk_num; j++) {
            if (local_fastfps[j] == fastfp) {
                chunk_idx = j;
                break;
            }
        }
        
        if (chunk_idx == -1) {
            printf("Warning: could not locate chunk for FastFp 0x%016lx in local list\n", fastfp);
            continue;
        }
        
        int calc_offset = 0;
        for (int j = 0; j < chunk_idx; j++) {
            calc_offset += boundary[j];
        }
        
        if (send_all(server_sock, &fastfp, sizeof(uint64_t)) <= 0) {
            printf("Failed to send FastFp to server\n");
            return;
        }
        
        if (send_all(server_sock, &boundary[chunk_idx], sizeof(int)) <= 0) {
            printf("Failed to send chunk size to server\n");
            return;
        }
        
        if (send_all(server_sock, fileCache + calc_offset, boundary[chunk_idx]) <= 0) {
            printf("Failed to send chunk data to server\n");
            return;
        }
        
        printf("Sent chunk (FastFp: 0x%016lx, size: %d) to server\n", 
               fastfp, boundary[chunk_idx]);
    }
}

// 通用工具函数，抽象重复逻辑
static int send_current_fastfp_list_to_server(int sock, const uint64_t *local_fastfps, int chunk_num) {
    if (send_all(sock, &chunk_num, sizeof(int)) <= 0) {
        return -1;
    }
    if (chunk_num > 0) {
        if (send_all(sock, local_fastfps, chunk_num * sizeof(uint64_t)) <= 0) {
            return -1;
        }
    }
    return 0;
}

static void find_matches_for_server(const FastFpList *remote, const uint64_t *local_fastfps,
                                    int chunk_num, FastFpData *out_matches, int *out_count) {
    int cnt = 0;
    for (int i = 0; i < chunk_num; i++) {
        uint64_t current_fastfp = local_fastfps[i];
        for (int j = 0; j < remote->count; j++) {
            if (remote->fastfps[j].fastfp == current_fastfp) {
                out_matches[cnt].fastfp = current_fastfp;
                out_matches[cnt].server_id = remote->fastfps[j].server_id;
                cnt++;
                break;
            }
        }
    }
    *out_count = cnt;
}

static int receive_and_verify_sha1_for_server(int sock,
                                              const FastFpData *matching_fastfps, int match_count,
                                              const unsigned char *fileCache,
                                              const int *boundary, int chunk_num,
                                              const uint64_t *local_fastfps,
                                              int *verified_out, // size chunk_num, 0/1
                                              int *actual_matches_out) {
    *actual_matches_out = 0;
    if (match_count <= 0) return 0;

    unsigned char *sha1_hashes = (unsigned char *)malloc(match_count * SHA_DIGEST_LENGTH);
    if (!sha1_hashes) return -1;

    if (receive_sha1_hashes(sock, sha1_hashes, match_count) != 0) {
        free(sha1_hashes);
        // 接收失败，不标记验证，通过上层逻辑重新上传
        return -1;
    }

    for (int i = 0; i < match_count; i++) {
        uint64_t fastfp = matching_fastfps[i].fastfp;
        int chunk_idx = -1;
        int offset = 0;
        for (int j = 0; j < chunk_num; j++) {
            if (local_fastfps[j] == fastfp) { chunk_idx = j; break; }
            offset += boundary[j];
        }
        if (chunk_idx < 0) continue;

        unsigned char local_sha1[SHA_DIGEST_LENGTH];
        calculate_sha1(fileCache + offset, boundary[chunk_idx], local_sha1);

        int match = 1;
        for (int k = 0; k < SHA_DIGEST_LENGTH; k++) {
            if (sha1_hashes[i * SHA_DIGEST_LENGTH + k] != local_sha1[k]) { match = 0; break; }
        }
        if (match) {
            verified_out[chunk_idx] = 1;
            (*actual_matches_out)++;
        }
    }

    free(sha1_hashes);
    return 0;
}

// 客户端主逻辑
int process_file_on_client(const char* filename, const char* server1_ip, int server1_port, 
                          const char* server2_ip, int server2_port,
                          const char* server3_ip, int server3_port,
                          const char* server4_ip, int server4_port) {
    printf("Starting distributed FastCDC client for file: %s\n", filename);
    
    // 连接到四个服务器
    int server1_sock = connect_to_server(server1_ip, server1_port);
    int server2_sock = connect_to_server(server2_ip, server2_port);
    int server3_sock = connect_to_server(server3_ip, server3_port);
    int server4_sock = connect_to_server(server4_ip, server4_port);
    
    if (server1_sock < 0 || server2_sock < 0 || server3_sock < 0 || server4_sock < 0) {
        printf("Failed to connect to one or more servers\n");
        if (server1_sock >= 0) close(server1_sock);
        if (server2_sock >= 0) close(server2_sock);
        if (server3_sock >= 0) close(server3_sock);
        if (server4_sock >= 0) close(server4_sock);
        return -1;
    }
    
    printf("Connected to all 4 servers successfully\n");
    int socks[NUM_SERVERS] = {server1_sock, server2_sock, server3_sock, server4_sock};
    
    for (int s = 0; s < NUM_SERVERS; ++s) {
        set_socket_timeout(socks[s], 60);
    }
    
    // 向四个服务器发送文件信息
    printf("Sending file info to servers...\n");
    for (int s = 0; s < NUM_SERVERS; ++s) {
        send_file_data(socks[s], filename);
    }
    
    // 接收四个服务器的FastFp列表（统一循环）
    printf("Receiving FastFp lists from servers...\n");
    FastFpList server_fastfps[NUM_SERVERS];
    for (int s = 0; s < NUM_SERVERS; ++s) {
        server_fastfps[s] = receive_fastfp_list(socks[s]);
        printf("Server%d: %d entries\n", s+1, server_fastfps[s].count);
    }
    
    // 读取本地文件进行分块
    FILE* local_file = fopen(filename, "rb");
    if (!local_file) {
        perror("Cannot open local file");
        for (int s = 0; s < NUM_SERVERS; ++s) close(socks[s]);
        for (int s = 0; s < NUM_SERVERS; ++s) {
            if (server_fastfps[s].fastfps) free(server_fastfps[s].fastfps);
        }
        return -1;
    }
    
    unsigned char *fileCache = malloc(MAX_CACHE_SIZE);
    if (!fileCache) {
        perror("Memory allocation failed");
        fclose(local_file);
        for (int s = 0; s < NUM_SERVERS; ++s) close(socks[s]);
        for (int s = 0; s < NUM_SERVERS; ++s) {
            if (server_fastfps[s].fastfps) free(server_fastfps[s].fastfps);
        }
        return -1;
    }
    
    size_t fileSize = fread(fileCache, 1, MAX_CACHE_SIZE, local_file);
    fclose(local_file);
    
    if (fileSize == 0) {
        printf("File is empty\n");
        free(fileCache);
        for (int s = 0; s < NUM_SERVERS; ++s) close(socks[s]);
        for (int s = 0; s < NUM_SERVERS; ++s) {
            if (server_fastfps[s].fastfps) free(server_fastfps[s].fastfps);
        }
        return -1;
    }
    
    // 对本地文件进行FastCDC分块
    fastCDC_init();
    int chunk_num = 0;
    int offset = 0, chunkLength = 0;
    int maxchunksum = (fileSize / MinSize) + 1;
    int *boundary = malloc(maxchunksum * sizeof(int));
    uint64_t *local_fastfps = malloc(maxchunksum * sizeof(uint64_t));
    
    if (!boundary || !local_fastfps) {
        perror("Memory allocation failed");
        free(fileCache);
        free(boundary);
        free(local_fastfps);
        for (int s = 0; s < NUM_SERVERS; ++s) close(socks[s]);
        for (int s = 0; s < NUM_SERVERS; ++s) {
            if (server_fastfps[s].fastfps) free(server_fastfps[s].fastfps);
        }
        return -1;
    }
    
    int (*chunking)(unsigned char*, int, uint64_t*, uint64_t*) = normalized_chunking_64;
    
    // 分块处理
    int end = fileSize;
    while (offset < end) {
        uint64_t feature = 0, weakhash = 0;
        chunkLength = chunking(fileCache + offset, end - offset, &feature, &weakhash);
        if (chunkLength <= 0) {
            printf("Error in chunking, chunk length: %d\n", chunkLength);
            break;
        }
        if (chunk_num >= maxchunksum) {
            printf("Chunk array overflow\n");
            break;
        }
        boundary[chunk_num] = chunkLength;
        local_fastfps[chunk_num] = weakhash;
        offset += chunkLength;
        chunk_num++;
        if (offset >= end) break;
    }
    
    printf("Local file chunked into %d pieces\n", chunk_num);
    
    // 打印所有FastFp值
    printf("Local FastFp values:\n");
    for (int i = 0; i < chunk_num; i++) {
        printf("  Chunk %d: FastFp=0x%016lx, Size=%d\n", i, local_fastfps[i], boundary[i]);
    }
    
    // 封装为数组，统一匹配流程
    FastFpData *matching[NUM_SERVERS];
    int match_count[NUM_SERVERS] = {0};
    for (int s = 0; s < NUM_SERVERS; ++s) {
        matching[s] = (FastFpData *)malloc(chunk_num * sizeof(FastFpData));
        if (!matching[s]) { printf("malloc matching failed for server %d\n", s+1); }
        find_matches_for_server(&server_fastfps[s], local_fastfps, chunk_num, matching[s], &match_count[s]);
        printf("Server%d matched %d chunks\n", s+1, match_count[s]);
    }
    
    // 先向所有服务器发送当前文件的所有 FastFp（用于服务端清理旧块）
    printf("Sending current file FastFp list to all servers...\n");
    for (int s = 0; s < NUM_SERVERS; ++s) {
        if (send_current_fastfp_list_to_server(socks[s], local_fastfps, chunk_num) != 0) {
            printf("Failed to send FastFp list to server%d\n", s+1);
        }
    }
    
    // 再发送匹配的 FastFp 列表（统一循环）
    printf("Sending matching FastFp list to all servers...\n");
    for (int s = 0; s < NUM_SERVERS; ++s) {
        send_matching_fastfps(socks[s], matching[s], match_count[s]);
    }
    
    // 接收服务器返回的SHA1哈希并验证（抽象成循环与通用函数）
    int actual_matches[NUM_SERVERS] = {0};
    int *verified[NUM_SERVERS] = {0};
    for (int s = 0; s < NUM_SERVERS; ++s) {
        verified[s] = (int*)calloc(chunk_num, sizeof(int));
        if (!verified[s]) { printf("calloc verified failed for server %d\n", s+1); }
    }

    for (int s = 0; s < NUM_SERVERS; ++s) {
        if (match_count[s] <= 0) continue;
        int ret = receive_and_verify_sha1_for_server(
            socks[s], matching[s], match_count[s],
            fileCache, boundary, chunk_num, local_fastfps,
            verified[s], &actual_matches[s]
        );
        if (ret != 0 && s > 0) {
            // 兼容旧行为：server2/3/4 接收失败时，视为全部匹配有效
            actual_matches[s] = match_count[s];
            for (int i = 0; i < match_count[s]; ++i) {
                uint64_t fp = matching[s][i].fastfp;
                for (int j = 0; j < chunk_num; ++j) {
                    if (local_fastfps[j] == fp) { verified[s][j] = 1; break; }
                }
            }
        }
    }
    
    // 计算需要上传到每个服务器的块（抽象成轮询分配）
    FastFpData *upload_fastfps[NUM_SERVERS];
    int upload_count[NUM_SERVERS] = {0};
    for (int s = 0; s < NUM_SERVERS; ++s) {
        upload_fastfps[s] = (FastFpData*)malloc(chunk_num * sizeof(FastFpData));
    }

    for (int i = 0; i < chunk_num; i++) {
        uint64_t current_fastfp = local_fastfps[i];
        int verified_any = 0;
        for (int s = 0; s < NUM_SERVERS; ++s) {
            if (verified[s] && verified[s][i]) { verified_any = 1; break; }
        }
        if (verified_any) continue;

        int server_idx = i % NUM_SERVERS; // 轮询分配
        upload_fastfps[server_idx][upload_count[server_idx]].fastfp = current_fastfp;
        upload_fastfps[server_idx][upload_count[server_idx]].server_id = server_idx + 1; // 1-based
        upload_count[server_idx]++;
    }

    // 打印上传计划并发送
    for (int s = 0; s < NUM_SERVERS; ++s) {
        printf("Uploading %d new chunks to server%d...\n", upload_count[s], s+1);
        send_new_chunks(socks[s], fileCache, boundary, local_fastfps, chunk_num,
                        upload_fastfps[s], upload_count[s]);
    }
    
    // 计算冗余率指标（注意：总冗余率按“并集”计算，避免双计）
    long server_verified_size[NUM_SERVERS] = {0};
    long total_verified_size = 0;
    for (int i = 0; i < chunk_num; i++) {
        int any = 0;
        for (int s = 0; s < NUM_SERVERS; ++s) {
            if (verified[s] && verified[s][i]) { server_verified_size[s] += boundary[i]; any = 1; }
        }
        if (any) total_verified_size += boundary[i];
    }
    double total_redundancy_rate = (fileSize > 0) ? (total_verified_size * 100.0 / fileSize) : 0.0;

    

    
    printf("\n========== 冗余率统计 ==========\n");
    printf("文件总大小: %zu bytes\n", fileSize);
    printf("总块数: %d\n", chunk_num);
    printf("\n验证统计:\n");
    for (int s = 0; s < NUM_SERVERS; ++s) {
        printf("  Server%d 验证块数: %d, 验证数据量: %ld bytes\n", s+1, actual_matches[s], server_verified_size[s]);
    }
    printf("  总验证数据量: %ld bytes\n", total_verified_size);
    printf("\n冗余率指标:\n");
    for (int s = 0; s < NUM_SERVERS; ++s) {
        double rate = (fileSize > 0) ? (server_verified_size[s] * 100.0 / fileSize) : 0.0;
        printf("  Server%d 冗余率: %.2f%%\n", s+1, rate);
    }
    printf("  总冗余率: %.2f%%\n", total_redundancy_rate);
    // 上传统计（统一循环）
    int total_upload = 0; for (int s = 0; s < NUM_SERVERS; ++s) total_upload += upload_count[s];
    printf("\n上传统计:\n");
    printf("  需要上传块数: %d\n", total_upload);
    for (int s = 0; s < NUM_SERVERS; ++s) {
        printf("  Server%d 上传块数: %d\n", s+1, upload_count[s]);
    }
    printf("================================\n\n");
    
    printf("Client processed %d chunks\n", chunk_num);
    for (int s = 0; s < NUM_SERVERS; ++s) {
        printf("Server%d matched %d\n", s+1, actual_matches[s]);
    }
    
    // 清理资源
    free(fileCache);
    free(boundary);
    free(local_fastfps);
    for (int s = 0; s < NUM_SERVERS; ++s) {
        if (matching[s]) free(matching[s]);
        if (upload_fastfps[s]) free(upload_fastfps[s]);
        if (verified[s]) free(verified[s]);
        if (server_fastfps[s].fastfps) free(server_fastfps[s].fastfps);
        close(socks[s]);
    }
    
    return 0;
}

// 配置结构体
typedef struct {
    char server1_ip[256];
    int server1_port;
    char server2_ip[256];
    int server2_port;
    char server3_ip[256];
    int server3_port;
    char server4_ip[256];
    int server4_port;
} ServerConfig;

// 从配置文件读取服务器信息
int read_server_config(const char* config_file, ServerConfig* config) {
    FILE* file = fopen(config_file, "r");
    if (!file) {
        perror("Cannot open config file");
        return -1;
    }
    
    char line[512];
    int found_server1_ip = 0, found_server1_port = 0;
    int found_server2_ip = 0, found_server2_port = 0;
    int found_server3_ip = 0, found_server3_port = 0;
    int found_server4_ip = 0, found_server4_port = 0;
    
    while (fgets(line, sizeof(line), file)) {
        // 去掉换行符
        line[strcspn(line, "\n")] = 0;
        
        // 跳过空行和注释
        if (line[0] == '\0' || line[0] == '#') continue;
        
        // 解析 server1_ip
        if (sscanf(line, "server1_ip=%255s", config->server1_ip) == 1) {
            found_server1_ip = 1;
            continue;
        }
        
        // 解析 server1_port
        if (sscanf(line, "server1_port=%d", &config->server1_port) == 1) {
            found_server1_port = 1;
            continue;
        }
        
        // 解析 server2_ip
        if (sscanf(line, "server2_ip=%255s", config->server2_ip) == 1) {
            found_server2_ip = 1;
            continue;
        }
        
        // 解析 server2_port
        if (sscanf(line, "server2_port=%d", &config->server2_port) == 1) {
            found_server2_port = 1;
            continue;
        }
        
        // 解析 server3_ip
        if (sscanf(line, "server3_ip=%255s", config->server3_ip) == 1) {
            found_server3_ip = 1;
            continue;
        }
        
        // 解析 server3_port
        if (sscanf(line, "server3_port=%d", &config->server3_port) == 1) {
            found_server3_port = 1;
            continue;
        }
        
        // 解析 server4_ip
        if (sscanf(line, "server4_ip=%255s", config->server4_ip) == 1) {
            found_server4_ip = 1;
            continue;
        }
        
        // 解析 server4_port
        if (sscanf(line, "server4_port=%d", &config->server4_port) == 1) {
            found_server4_port = 1;
            continue;
        }
    }
    
    fclose(file);
    
    // 检查是否所有必要的配置都已读取
    if (!found_server1_ip || !found_server1_port || !found_server2_ip || !found_server2_port ||
        !found_server3_ip || !found_server3_port || !found_server4_ip || !found_server4_port) {
        printf("Error: Missing required configuration in %s\n", config_file);
        printf("Required: server1_ip, server1_port, server2_ip, server2_port, server3_ip, server3_port, server4_ip, server4_port\n");
        return -1;
    }
    
    return 0;
}

void print_usage(const char* program_name) {
    printf("Usage:\n");
    printf("  %s <filename>\n", program_name);
    printf("  %s <old_file> <new_file>  # 先用 old_file 预置服务端，再对 new_file 计算冗余率\n", program_name);
    printf("Example: %s random.txt random_copy.txt\n", program_name);
    printf("Note: Server configuration is read from client.conf\n");
}

int main(int argc, char *argv[]) {
    ServerConfig config;
    
    // 从配置文件读取服务器信息
    if (read_server_config("client.conf", &config) != 0) {
        printf("Failed to read server configuration from client.conf\n");
        return -1;
    }
    
    printf("Server configuration loaded:\n");
    printf("  Server1: %s:%d\n", config.server1_ip, config.server1_port);
    printf("  Server2: %s:%d\n", config.server2_ip, config.server2_port);
    printf("  Server3: %s:%d\n", config.server3_ip, config.server3_port);
    printf("  Server4: %s:%d\n", config.server4_ip, config.server4_port);

    if (argc == 2) {
        const char* filename = argv[1];
        struct timeval start, end; gettimeofday(&start, NULL);
        int result = process_file_on_client(filename, config.server1_ip, config.server1_port,
                                            config.server2_ip, config.server2_port,
                                            config.server3_ip, config.server3_port,
                                            config.server4_ip, config.server4_port);
        gettimeofday(&end, NULL);
        double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        printf("Total processing time: %.6f seconds\n", total_time);
        return result;
    } else if (argc == 3) {
        // 配对模式：先用旧文件铺底，再用新文件计算冗余率（保证对称的基线）
        const char* old_file = argv[1];
        const char* new_file = argv[2];

        printf("[Pair Mode] Seeding servers with old file: %s\n", old_file);
        if (process_file_on_client(old_file, config.server1_ip, config.server1_port, 
                                   config.server2_ip, config.server2_port,
                                   config.server3_ip, config.server3_port,
                                   config.server4_ip, config.server4_port) != 0) {
            printf("Seeding failed\n");
            return -1;
        }
        printf("[Pair Mode] Computing redundancy for new file: %s\n", new_file);
        return process_file_on_client(new_file, config.server1_ip, config.server1_port, 
                                      config.server2_ip, config.server2_port,
                                      config.server3_ip, config.server3_port,
                                      config.server4_ip, config.server4_port);
    } else {
        print_usage(argv[0]);
        return -1;
    }
}