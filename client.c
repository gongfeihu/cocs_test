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
#include "fastcdc.h"

#define SERVER_PORT1 8080
#define SERVER_PORT2 8081
#define MAX_CACHE_SIZE (100 * 1024 * 1024)

struct FastFpList {
    uint64_t *fastfps;
    int count;
};

// 计算 SHA1 哈希的辅助函数
void calculate_sha1(const unsigned char *data, size_t len, unsigned char *sha1_hash) {
    SHA1(data, len, sha1_hash);
}

// 打印 SHA1 哈希值的辅助函数
void print_sha1_hash(const unsigned char *hash, const char *label) {
    printf("  %s: ", label);
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        printf("%02x", hash[i]);
    }
    printf("\n");
}

// 连接到服务器
int connect_to_server(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation error");
        return -1;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    
    if(inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
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
    
    // 发送文件名长度
    int name_len = strlen(filename);
    send(sock, &name_len, sizeof(int), 0);
    send(sock, filename, name_len, 0);
    
    // 发送文件内容
    unsigned char buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        send(sock, buffer, bytes_read, 0);
    }
    
    fclose(file);
}

// 接收服务器的FastFp列表
struct FastFpList receive_fastfp_list(int sock) {
    struct FastFpList result = {NULL, 0};
    
    // 接收列表大小
    recv(sock, &result.count, sizeof(int), 0);
    
    if (result.count > 0) {
        result.fastfps = malloc(result.count * sizeof(uint64_t));
        recv(sock, result.fastfps, result.count * sizeof(uint64_t), 0);
    }
    
    return result;
}

// 发送匹配成功的FastFp列表
void send_matching_fastfps(int sock, uint64_t *matching_fastfps, int count) {
    send(sock, &count, sizeof(int), 0);
    if (count > 0) {
        send(sock, matching_fastfps, count * sizeof(uint64_t), 0);
    }
}

// 接收SHA1哈希
void receive_sha1_hashes(int sock, unsigned char *server1_sha1, unsigned char *server2_sha1, int chunk_count) {
    recv(sock, server1_sha1, chunk_count * SHA_DIGEST_LENGTH, 0);
    recv(sock, server2_sha1, chunk_count * SHA_DIGEST_LENGTH, 0);
}

// 客户端主逻辑
int process_file_on_client(const char* filename) {
    // 连接到两个服务器
    int server1_sock = connect_to_server("127.0.0.1", SERVER_PORT1);
    int server2_sock = connect_to_server("127.0.0.1", SERVER_PORT2);
    
    if (server1_sock < 0 || server2_sock < 0) {
        printf("Failed to connect to servers\n");
        return -1;
    }
    
    printf("Connected to both servers\n");
    
    // 向两个服务器发送文件
    send_file_data(server1_sock, filename);
    send_file_data(server2_sock, filename);
    
    // 接收两个服务器的FastFp列表
    struct FastFpList server1_fastfps = receive_fastfp_list(server1_sock);
    struct FastFpList server2_fastfps = receive_fastfp_list(server2_sock);
    
    printf("Received FastFp lists from servers:\n");
    printf("Server1: %d entries\n", server1_fastfps.count);
    printf("Server2: %d entries\n", server2_fastfps.count);
    
    // 读取本地文件进行分块
    FILE* local_file = fopen(filename, "rb");
    if (!local_file) {
        perror("Cannot open local file");
        close(server1_sock);
        close(server2_sock);
        return -1;
    }
    
    unsigned char *fileCache = malloc(MAX_CACHE_SIZE);
    if (!fileCache) {
        perror("Memory allocation failed");
        fclose(local_file);
        close(server1_sock);
        close(server2_sock);
        return -1;
    }
    
    size_t fileSize = fread(fileCache, 1, MAX_CACHE_SIZE, local_file);
    fclose(local_file);
    
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
        close(server1_sock);
        close(server2_sock);
        return -1;
    }
    
    int (*chunking)(unsigned char*, int, uint64_t*, uint64_t*) = normalized_chunking_64;
    
    // 分块处理
    int end = fileSize;
    while (offset < end) {
        uint64_t feature = 0, weakhash = 0;
        chunkLength = chunking(fileCache + offset, end - offset, &feature, &weakhash);
        boundary[chunk_num] = chunkLength;
        local_fastfps[chunk_num] = weakhash;
        offset += chunkLength;
        chunk_num++;
        if (offset >= end) break;
    }
    
    printf("Local file chunked into %d pieces\n", chunk_num);
    
    // 检查哪些FastFp匹配
    int *matched_indices = malloc(chunk_num * sizeof(int));
    uint64_t *matching_fastfps = malloc(chunk_num * sizeof(uint64_t));
    int match_count = 0;
    
    for (int i = 0; i < chunk_num; i++) {
        uint64_t current_fastfp = local_fastfps[i];
        int matched = 0;
        
        // 检查是否在server1的列表中
        for (int j = 0; j < server1_fastfps.count; j++) {
            if (server1_fastfps.fastfps[j] == current_fastfp) {
                matched_indices[match_count] = i;
                matching_fastfps[match_count] = current_fastfp;
                match_count++;
                matched = 1;
                printf("Chunk %d (FastFp: 0x%016lx) matches server1\n", i, current_fastfp);
                break;
            }
        }
        
        if (!matched) {
            // 检查是否在server2的列表中
            for (int j = 0; j < server2_fastfps.count; j++) {
                if (server2_fastfps.fastfps[j] == current_fastfp) {
                    matched_indices[match_count] = i;
                    matching_fastfps[match_count] = current_fastfp;
                    match_count++;
                    matched = 1;
                    printf("Chunk %d (FastFp: 0x%016lx) matches server2\n", i, current_fastfp);
                    break;
                }
            }
        }
    }
    
    printf("Found %d potential matches based on FastFp\n", match_count);
    
    // 向服务器发送匹配的FastFp列表
    send_matching_fastfps(server1_sock, matching_fastfps, match_count);
    send_matching_fastfps(server2_sock, matching_fastfps, match_count);
    
    // 接收服务器返回的SHA1哈希
    unsigned char *server1_sha1_hashes = malloc(match_count * SHA_DIGEST_LENGTH);
    unsigned char *server2_sha1_hashes = malloc(match_count * SHA_DIGEST_LENGTH);
    
    receive_sha1_hashes(server1_sock, server1_sha1_hashes, server2_sha1_hashes, match_count);
    
    // 本地计算SHA1并比较
    int actual_matches = 0;
    for (int i = 0; i < match_count; i++) {
        int chunk_idx = matched_indices[i];
        uint64_t fastfp = matching_fastfps[i];
        
        // 计算本地块的SHA1
        unsigned char local_sha1[SHA_DIGEST_LENGTH];
        unsigned char *chunk_data = fileCache + (boundary[chunk_idx] ? 0 : 0); // 简化处理
        
        // 计算正确的偏移量
        int calc_offset = 0;
        for (int j = 0; j < chunk_idx; j++) {
            calc_offset += boundary[j];
        }
        
        calculate_sha1(fileCache + calc_offset, boundary[chunk_idx], local_sha1);
        
        // 比较SHA1
        int server1_match = 1, server2_match = 1;
        for (int j = 0; j < SHA_DIGEST_LENGTH; j++) {
            if (server1_sha1_hashes[i * SHA_DIGEST_LENGTH + j] != local_sha1[j]) {
                server1_match = 0;
            }
            if (server2_sha1_hashes[i * SHA_DIGEST_LENGTH + j] != local_sha1[j]) {
                server2_match = 0;
            }
        }
        
        printf("Chunk %d SHA1 comparison:\n", chunk_idx);
        print_sha1_hash(local_sha1, "Local SHA1");
        
        if (server1_match) {
            print_sha1_hash(server1_sha1_hashes + i * SHA_DIGEST_LENGTH, "Server1 SHA1");
            printf("  -> Match with Server1, skipping upload\n");
            actual_matches++;
        } else if (server2_match) {
            print_sha1_hash(server2_sha1_hashes + i * SHA_DIGEST_LENGTH, "Server2 SHA1");
            printf("  -> Match with Server2, skipping upload\n");
            actual_matches++;
        } else {
            printf("  -> No match found, will upload chunk\n");
        }
    }
    
    // 发送实际需要上传的块
    int *upload_indices = malloc((chunk_num - actual_matches) * sizeof(int));
    int upload_count = 0;
    
    for (int i = 0; i < chunk_num; i++) {
        int is_match = 0;
        for (int j = 0; j < match_count; j++) {
            if (matched_indices[j] == i) {
                // 检查是否真的是匹配的（通过SHA1验证）
                int calc_offset = 0;
                for (int k = 0; k < i; k++) {
                    calc_offset += boundary[k];
                }
                
                unsigned char local_sha1[SHA_DIGEST_LENGTH];
                calculate_sha1(fileCache + calc_offset, boundary[i], local_sha1);
                
                int server1_match = 1, server2_match = 1;
                for (int k = 0; k < SHA_DIGEST_LENGTH; k++) {
                    if (server1_sha1_hashes[j * SHA_DIGEST_LENGTH + k] != local_sha1[k]) {
                        server1_match = 0;
                    }
                    if (server2_sha1_hashes[j * SHA_DIGEST_LENGTH + k] != local_sha1[k]) {
                        server2_match = 0;
                    }
                }
                
                if (server1_match || server2_match) {
                    is_match = 1;
                }
                break;
            }
        }
        
        if (!is_match) {
            upload_indices[upload_count] = i;
            upload_count++;
        }
    }
    
    // 发送需要上传的块数量
    send(server1_sock, &upload_count, sizeof(int), 0);
    send(server2_sock, &upload_count, sizeof(int), 0);
    
    // 发送需要上传的块
    for (int i = 0; i < upload_count; i++) {
        int chunk_idx = upload_indices[i];
        int calc_offset = 0;
        for (int j = 0; j < chunk_idx; j++) {
            calc_offset += boundary[j];
        }
        
        // 发送到server1 (偶数块) 或 server2 (奇数块)
        if (chunk_idx % 2 == 0) {
            // 发送FastFp
            send(server1_sock, &local_fastfps[chunk_idx], sizeof(uint64_t), 0);
            // 发送块大小
            int chunk_size = boundary[chunk_idx];
            send(server1_sock, &chunk_size, sizeof(int), 0);
            // 发送块数据
            send(server1_sock, fileCache + calc_offset, chunk_size, 0);
        } else {
            // 发送FastFp
            send(server2_sock, &local_fastfps[chunk_idx], sizeof(uint64_t), 0);
            // 发送块大小
            int chunk_size = boundary[chunk_idx];
            send(server2_sock, &chunk_size, sizeof(int), 0);
            // 发送块数据
            send(server2_sock, fileCache + calc_offset, chunk_size, 0);
        }
    }
    
    printf("Client processed %d chunks, %d matched, %d uploaded\n", 
           chunk_num, actual_matches, upload_count);
    
    // 清理资源
    free(fileCache);
    free(boundary);
    free(local_fastfps);
    free(matched_indices);
    free(matching_fastfps);
    free(upload_indices);
    free(server1_sha1_hashes);
    free(server2_sha1_hashes);
    
    if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
    if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
    
    close(server1_sock);
    close(server2_sock);
    
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return -1;
    }
    
    printf("Starting distributed FastCDC client for file: %s\n", argv[1]);
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    int result = process_file_on_client(argv[1]);
    
    gettimeofday(&end, NULL);
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Total processing time: %.6f seconds\n", total_time);
    
    return result;
}