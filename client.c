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

#include "fastcdc.h"

typedef struct {
    uint64_t fastfp;
    int server_id;  // 服务器ID (1 或 2)
} FastFpData;

typedef struct {
    FastFpData *fastfps;
    int count;
} FastFpList;

// FastCDC初始化函数
void fastCDC_init(void) {
    unsigned char md5_digest[16];
    uint8_t seed[SeedLength];
    
    for (int i = 0; i < SymbolCount; i++) {
        for (int j = 0; j < SeedLength; j++) {
            seed[j] = i;
        }
        g_global_matrix[i] = 0;
        MD5(seed, SeedLength, md5_digest);
        memcpy(&(g_global_matrix[i]), md5_digest, 4);
        g_global_matrix_left[i] = g_global_matrix[i] << 1;
    }

    for (int i = 0; i < SymbolCount; i++) {
        LEARv2[i] = GEARv2[i] << 1;
    }

    MinSize = 8192 / 4;
    MaxSize = 8192 * 4;
    Mask_15 = 0xf9070353;
    Mask_11 = 0xd9000353;
    Mask_11_64 = 0x0000d90003530000;
    Mask_15_64 = 0x0000f90703530000;
    MinSize_divide_by_2 = MinSize / 2;
}

// FastCDC分块函数
int normalized_chunking_64(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash) {
    uint64_t fingerprint = 0;
    int originalMinSize = MinSize;
    MinSize = 6 * 1024;
    int i = MinSize, Mid = 8 * 1024;

    if (n <= MinSize) {
        for (int j = 0; j < n; j++) {
            fingerprint = (fingerprint << 1) + (GEARv2[p[j]]);
        }
        MinSize = originalMinSize;
        *weakhash = fingerprint;
        return n;
    }

    if (n > MaxSize)
        n = MaxSize;
    else if (n < Mid)
        Mid = n;

    while (i < Mid) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);

        if ((!(fingerprint & FING_GEAR_32KB_64))) {
            MinSize = originalMinSize;
            *weakhash = fingerprint;
            return i;
        }

        i++;
    }

    while (i < n) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);

        if ((!(fingerprint & FING_GEAR_02KB_64))) {
            MinSize = originalMinSize;
            *weakhash = fingerprint;
            return i;
        }

        i++;
    }

    MinSize = originalMinSize;
    *weakhash = fingerprint;
    return n;
}

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
        if (result <= 0) {
            return result;
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
    
    unsigned char buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send_all(sock, buffer, bytes_read) <= 0) {
            printf("Failed to send file content\n");
            break;
        }
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
                     int *boundary, uint64_t *local_fastfps, FastFpData *upload_fastfps, 
                     int upload_count) {
    if (send_all(server_sock, &upload_count, sizeof(int)) <= 0) {
        printf("Failed to send upload count\n");
        return;
    }
    
    printf("Sending %d chunks to server\n", upload_count);
    
    for (int i = 0; i < upload_count; i++) {
        uint64_t fastfp = upload_fastfps[i].fastfp;
        
        // 在本地FastFp数组中找到对应索引
        int chunk_idx = -1;
        for (int j = 0; j < upload_count; j++) {
            if (local_fastfps[j] == fastfp) {
                chunk_idx = j;
                break;
            }
        }
        
        if (chunk_idx == -1) continue;
        
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

// 客户端主逻辑
int process_file_on_client(const char* filename, const char* server1_ip, int server1_port, 
                          const char* server2_ip, int server2_port) {
    printf("Starting distributed FastCDC client for file: %s\n", filename);
    
    // 连接到两个服务器
    int server1_sock = connect_to_server(server1_ip, server1_port);
    int server2_sock = connect_to_server(server2_ip, server2_port);
    
    if (server1_sock < 0 || server2_sock < 0) {
        printf("Failed to connect to one or both servers\n");
        if (server1_sock >= 0) close(server1_sock);
        if (server2_sock >= 0) close(server2_sock);
        return -1;
    }
    
    printf("Connected to both servers successfully\n");
    
    set_socket_timeout(server1_sock, 60);
    set_socket_timeout(server2_sock, 60);
    
    // 向两个服务器发送文件信息
    printf("Sending file info to servers...\n");
    send_file_data(server1_sock, filename);
    send_file_data(server2_sock, filename);
    
    // 接收两个服务器的FastFp列表
    printf("Receiving FastFp lists from servers...\n");
    FastFpList server1_fastfps = receive_fastfp_list(server1_sock);
    FastFpList server2_fastfps = receive_fastfp_list(server2_sock);
    
    printf("Received FastFp lists from servers:\n");
    printf("Server1: %d entries\n", server1_fastfps.count);
    printf("Server2: %d entries\n", server2_fastfps.count);
    
    // 合并两个服务器的FastFp列表
    int total_server_fastfps = server1_fastfps.count + server2_fastfps.count;
    FastFpData *all_server_fastfps = malloc(total_server_fastfps * sizeof(FastFpData));
    if (!all_server_fastfps) {
        printf("Memory allocation failed\n");
        close(server1_sock);
        close(server2_sock);
        if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
        if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
        return -1;
    }
    
    int idx = 0;
    for (int i = 0; i < server1_fastfps.count; i++) {
        all_server_fastfps[idx++] = server1_fastfps.fastfps[i];
    }
    for (int i = 0; i < server2_fastfps.count; i++) {
        all_server_fastfps[idx++] = server2_fastfps.fastfps[i];
    }
    
    // 读取本地文件进行分块
    FILE* local_file = fopen(filename, "rb");
    if (!local_file) {
        perror("Cannot open local file");
        close(server1_sock);
        close(server2_sock);
        if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
        if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
        free(all_server_fastfps);
        return -1;
    }
    
    unsigned char *fileCache = malloc(MAX_CACHE_SIZE);
    if (!fileCache) {
        perror("Memory allocation failed");
        fclose(local_file);
        close(server1_sock);
        close(server2_sock);
        if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
        if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
        free(all_server_fastfps);
        return -1;
    }
    
    size_t fileSize = fread(fileCache, 1, MAX_CACHE_SIZE, local_file);
    fclose(local_file);
    
    if (fileSize == 0) {
        printf("File is empty\n");
        free(fileCache);
        close(server1_sock);
        close(server2_sock);
        if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
        if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
        free(all_server_fastfps);
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
        close(server1_sock);
        close(server2_sock);
        if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
        if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
        free(all_server_fastfps);
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
    
    // 为每个服务器分别检查匹配的FastFp
    FastFpData *server1_matching_fastfps = malloc(chunk_num * sizeof(FastFpData));
    FastFpData *server2_matching_fastfps = malloc(chunk_num * sizeof(FastFpData));
    int server1_match_count = 0;
    int server2_match_count = 0;
    
    // 检查与server1的匹配
    for (int i = 0; i < chunk_num; i++) {
        uint64_t current_fastfp = local_fastfps[i];
        for (int j = 0; j < server1_fastfps.count; j++) {
            if (server1_fastfps.fastfps[j].fastfp == current_fastfp) {
                server1_matching_fastfps[server1_match_count].fastfp = current_fastfp;
                server1_matching_fastfps[server1_match_count].server_id = server1_fastfps.fastfps[j].server_id;
                server1_match_count++;
                printf("Chunk %d (FastFp: 0x%016lx) matches server%d\n", i, current_fastfp, server1_fastfps.fastfps[j].server_id);
                break;
            }
        }
    }
    
    // 检查与server2的匹配
    for (int i = 0; i < chunk_num; i++) {
        uint64_t current_fastfp = local_fastfps[i];
        for (int j = 0; j < server2_fastfps.count; j++) {
            if (server2_fastfps.fastfps[j].fastfp == current_fastfp) {
                server2_matching_fastfps[server2_match_count].fastfp = current_fastfp;
                server2_matching_fastfps[server2_match_count].server_id = server2_fastfps.fastfps[j].server_id;
                server2_match_count++;
                printf("Chunk %d (FastFp: 0x%016lx) matches server%d\n", i, current_fastfp, server2_fastfps.fastfps[j].server_id);
                break;
            }
        }
    }
    
    printf("Found %d matches with server1 and %d matches with server2\n", 
           server1_match_count, server2_match_count);
    
    // 向服务器发送匹配的FastFp列表
    printf("Sending matching FastFp list to server1...\n");
    send_matching_fastfps(server1_sock, server1_matching_fastfps, server1_match_count);
    
    printf("Sending matching FastFp list to server2...\n");
    send_matching_fastfps(server2_sock, server2_matching_fastfps, server2_match_count);
    
    // 接收服务器返回的SHA1哈希
    int server1_actual_matches = 0;
    int server2_actual_matches = 0;
    unsigned char *server1_sha1_hashes = NULL;
    unsigned char *server2_sha1_hashes = NULL;
    
    if (server1_match_count > 0) {
        server1_sha1_hashes = malloc(server1_match_count * SHA_DIGEST_LENGTH);
        if (server1_sha1_hashes) {
            if (receive_sha1_hashes(server1_sock, server1_sha1_hashes, server1_match_count) == 0) {
                for (int i = 0; i < server1_match_count; i++) {
                    uint64_t current_fastfp = server1_matching_fastfps[i].fastfp;
                    
                    // 在本地FastFp数组中找到对应索引
                    int chunk_idx = -1;
                    for (int j = 0; j < chunk_num; j++) {
                        if (local_fastfps[j] == current_fastfp) {
                            chunk_idx = j;
                            break;
                        }
                    }
                    
                    if (chunk_idx != -1) {
                        int calc_offset = 0;
                        for (int j = 0; j < chunk_idx; j++) {
                            calc_offset += boundary[j];
                        }
                        
                        unsigned char local_sha1[SHA_DIGEST_LENGTH];
                        calculate_sha1(fileCache + calc_offset, boundary[chunk_idx], local_sha1);
                        
                        int match = 1;
                        for (int j = 0; j < SHA_DIGEST_LENGTH; j++) {
                            if (server1_sha1_hashes[i * SHA_DIGEST_LENGTH + j] != local_sha1[j]) {
                                match = 0;
                                break;
                            }
                        }
                        
                        if (match) {
                            server1_actual_matches++;
                        }
                    }
                }
            } else {
                printf("Failed to receive SHA1 hashes from server1, assuming all matches are valid\n");
                server1_actual_matches = server1_match_count;
            }
        } else {
            printf("Memory allocation failed for server1 SHA1 hashes\n");
        }
    }
    
    if (server2_match_count > 0) {
        server2_sha1_hashes = malloc(server2_match_count * SHA_DIGEST_LENGTH);
        if (server2_sha1_hashes) {
            if (receive_sha1_hashes(server2_sock, server2_sha1_hashes, server2_match_count) == 0) {
                for (int i = 0; i < server2_match_count; i++) {
                    uint64_t current_fastfp = server2_matching_fastfps[i].fastfp;
                    
                    // 在本地FastFp数组中找到对应索引
                    int chunk_idx = -1;
                    for (int j = 0; j < chunk_num; j++) {
                        if (local_fastfps[j] == current_fastfp) {
                            chunk_idx = j;
                            break;
                        }
                    }
                    
                    if (chunk_idx != -1) {
                        int calc_offset = 0;
                        for (int j = 0; j < chunk_idx; j++) {
                            calc_offset += boundary[j];
                        }
                        
                        unsigned char local_sha1[SHA_DIGEST_LENGTH];
                        calculate_sha1(fileCache + calc_offset, boundary[chunk_idx], local_sha1);
                        
                        int match = 1;
                        for (int j = 0; j < SHA_DIGEST_LENGTH; j++) {
                            if (server2_sha1_hashes[i * SHA_DIGEST_LENGTH + j] != local_sha1[j]) {
                                match = 0;
                                break;
                            }
                        }
                        
                        if (match) {
                            server2_actual_matches++;
                        }
                    }
                }
            } else {
                printf("Failed to receive SHA1 hashes from server2, assuming all matches are valid\n");
                server2_actual_matches = server2_match_count;
            }
        } else {
            printf("Memory allocation failed for server2 SHA1 hashes\n");
        }
    }
    
    // 计算需要上传到每个服务器的块
    FastFpData *server1_upload_fastfps = malloc(chunk_num * sizeof(FastFpData));
    FastFpData *server2_upload_fastfps = malloc(chunk_num * sizeof(FastFpData));
    int server1_upload_count = 0;
    int server2_upload_count = 0;
    
    for (int i = 0; i < chunk_num; i++) {
        uint64_t current_fastfp = local_fastfps[i];
        int is_server1_match = 0;
        int is_server2_match = 0;
        
        for (int j = 0; j < server1_match_count; j++) {
            if (server1_matching_fastfps[j].fastfp == current_fastfp) {
                is_server1_match = 1;
                break;
            }
        }
        
        for (int j = 0; j < server2_match_count; j++) {
            if (server2_matching_fastfps[j].fastfp == current_fastfp) {
                is_server2_match = 1;
                break;
            }
        }
        
        if (is_server1_match || is_server2_match) {
            // 已经匹配，不需要上传
            continue;
        } else {
            // 不匹配任何服务器，按索引分配
            if (i % 2 == 0) {
                server1_upload_fastfps[server1_upload_count].fastfp = current_fastfp;
                server1_upload_fastfps[server1_upload_count].server_id = SERVER1_ID;
                server1_upload_count++;
            } else {
                server2_upload_fastfps[server2_upload_count].fastfp = current_fastfp;
                server2_upload_fastfps[server2_upload_count].server_id = SERVER2_ID;
                server2_upload_count++;
            }
        }
    }
    
    printf("Uploading %d new chunks to server1 and %d new chunks to server2...\n", 
           server1_upload_count, server2_upload_count);
    
    // 发送需要上传的块到对应的服务器
    send_new_chunks(server1_sock, fileCache, boundary, local_fastfps, 
                    server1_upload_fastfps, server1_upload_count);
    send_new_chunks(server2_sock, fileCache, boundary, local_fastfps, 
                    server2_upload_fastfps, server2_upload_count);
    
    printf("Client processed %d chunks, server1 matched %d, server2 matched %d\n", 
           chunk_num, server1_actual_matches, server2_actual_matches);
    printf("Uploaded %d chunks to server1 and %d chunks to server2\n", 
           server1_upload_count, server2_upload_count);
    
    // 清理资源
    free(fileCache);
    free(boundary);
    free(local_fastfps);
    free(server1_matching_fastfps);
    free(server2_matching_fastfps);
    free(server1_upload_fastfps);
    free(server2_upload_fastfps);
    if (server1_sha1_hashes) free(server1_sha1_hashes);
    if (server2_sha1_hashes) free(server2_sha1_hashes);
    free(all_server_fastfps);
    
    if (server1_fastfps.fastfps) free(server1_fastfps.fastfps);
    if (server2_fastfps.fastfps) free(server2_fastfps.fastfps);
    
    close(server1_sock);
    close(server2_sock);
    
    return 0;
}

void print_usage(const char* program_name) {
    printf("Usage: %s <filename>\n", program_name);
    printf("Example: %s myfile.txt\n", program_name);
    printf("Default server1: 127.0.0.1:8080, server2: 127.0.0.1:8080\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }
    
    const char* filename = argv[1];
    const char* server1_ip = "127.0.0.1";
    const char* server2_ip = "127.0.0.1";
    int server1_port = DEFAULT_SERVER_PORT;
    int server2_port = DEFAULT_SERVER_PORT1;
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    int result = process_file_on_client(filename, server1_ip, server1_port, 
                                       server2_ip, server2_port);
    
    gettimeofday(&end, NULL);
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Total processing time: %.6f seconds\n", total_time);
    
    return result;
}