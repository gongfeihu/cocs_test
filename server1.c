// server1.c - 服务端1代码
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h> 
#include <errno.h>

#define PORT 8082
#define MAX_CACHE_SIZE (100 * 1024 * 1024)
#define STORAGE_DIR "./server1file"
#define TIMEOUT_SECONDS 60
#define SERVER_ID 1

typedef struct {
    uint64_t fastfp;
    int server_id;  // 服务器ID (1 或 2)
} FastFpData;

// 计算 SHA1 哈希
void calculate_sha1(const unsigned char *data, size_t len, unsigned char *sha1_hash) {
    SHA1(data, len, sha1_hash);
}

// 创建目录
int create_directory_if_not_exists(const char *dir) {
    struct stat st = {0};
    if (stat(dir, &st) == -1) {
        #ifdef _WIN32
            return mkdir(dir);
        #else
            return mkdir(dir, 0755);
        #endif
    }
    return 0;
}

// 从目录中收集所有chunk文件的FastFp
FastFpData* get_all_fastfps_from_dir(const char* dir_path, int* count) {
    DIR *d;
    struct dirent *entry;
    FastFpData *fastfps = malloc(1000 * sizeof(FastFpData));
    if (!fastfps) {
        *count = 0;
        return NULL;
    }
    *count = 0;
    
    d = opendir(dir_path);
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            if (strstr(entry->d_name, ".chunk") != NULL) {
                uint64_t fastfp;
                if (sscanf(entry->d_name, "%016lx.chunk", &fastfp) == 1) {
                    fastfps[*count].fastfp = fastfp;
                    fastfps[*count].server_id = SERVER_ID;  // 设置服务器ID
                    (*count)++;
                    if (*count >= 1000) {
                        FastFpData *temp = realloc(fastfps, (*count + 1000) * sizeof(FastFpData));
                        if (temp == NULL) {
                            closedir(d);
                            free(fastfps);
                            *count = 0;
                            return NULL;
                        }
                        fastfps = temp;
                    }
                }
            }
        }
        closedir(d);
    }
    
    return fastfps;
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

// 检查FastFp是否存在于本地存储中
int fastfp_exists_locally(uint64_t fastfp) {
    char chunk_path[512];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%016lx.chunk", STORAGE_DIR, fastfp);
    
    FILE *file = fopen(chunk_path, "rb");
    if (file) {
        fclose(file);
        return 1;
    }
    return 0;
}

// 删除目录中不在当前文件分块列表中的chunk文件
void cleanup_chunks_not_in_list(const char* dir_path, uint64_t *current_fastfps, int count) {
    if (!current_fastfps || count <= 0) return;
    
    DIR *d;
    struct dirent *entry;
    
    d = opendir(dir_path);
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            if (strstr(entry->d_name, ".chunk") != NULL) {
                uint64_t fastfp;
                if (sscanf(entry->d_name, "%016lx.chunk", &fastfp) == 1) {
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
                        char chunk_path[512];
                        snprintf(chunk_path, sizeof(chunk_path), "%s/%s", dir_path, entry->d_name);
                        if (remove(chunk_path) == 0) {
                            printf("Deleted old chunk file: %s\n", chunk_path);
                        } else {
                            printf("Failed to delete old chunk file: %s\n", chunk_path);
                        }
                    }
                }
            }
        }
        closedir(d);
    }
}

// 处理客户端连接
void handle_client(int client_socket, struct sockaddr_in *client_addr) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Handling client connection from %s\n", client_ip);
    
    // 确保目录存在
    create_directory_if_not_exists(STORAGE_DIR);
    
    // 接收文件名
    int name_len;
    if (recv_all(client_socket, &name_len, sizeof(int)) <= 0) {
        printf("Failed to receive filename length from %s: %s\n", client_ip, strerror(errno));
        return;
    }
    
    if (name_len <= 0 || name_len > 255) {
        printf("Invalid filename length: %d\n", name_len);
        return;
    }
    
    char filename[name_len + 1];
    if (recv_all(client_socket, filename, name_len) <= 0) {
        printf("Failed to receive filename from %s: %s\n", client_ip, strerror(errno));
        return;
    }
    filename[name_len] = '\0';
    
    printf("Received file: %s from client %s\n", filename, client_ip);
    
    // 接收文件大小
    long file_size = 0;
    if (recv_all(client_socket, &file_size, sizeof(long)) <= 0) {
        printf("Failed to receive file size from %s: %s\n", client_ip, strerror(errno));
        return;
    }
    
    printf("Receiving file of size: %ld bytes\n", file_size);
    
    // 丢弃文件内容
    unsigned char buffer[4096];
    long total_size = 0;
    ssize_t bytes_read;
    while (total_size < file_size && (bytes_read = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        total_size += bytes_read;
    }
    
    if (total_size != file_size) {
        printf("Warning: Expected %ld bytes but received %ld bytes\n", file_size, total_size);
    }
    
    // 收集当前目录中的所有FastFp
    int fastfp_count = 0;
    FastFpData *all_fastfps = get_all_fastfps_from_dir(STORAGE_DIR, &fastfp_count);
    
    printf("Found %d existing chunks in %s directory\n", fastfp_count, STORAGE_DIR);
    
    // 发送FastFp列表给客户端（包含服务器ID）
    if (send_all(client_socket, &fastfp_count, sizeof(int)) <= 0) {
        printf("Failed to send FastFp count to %s: %s\n", client_ip, strerror(errno));
        if (all_fastfps) free(all_fastfps);
        return;
    }
    if (fastfp_count > 0) {
        if (send_all(client_socket, all_fastfps, fastfp_count * sizeof(FastFpData)) <= 0) {
            printf("Failed to send FastFp list to %s: %s\n", client_ip, strerror(errno));
            free(all_fastfps);
            return;
        }
        printf("Sent %d FastFp values to client %s\n", fastfp_count, client_ip);
    } else {
        printf("No existing chunks in directory, sent 0 count to client %s\n", client_ip);
    }
    
    // 接收当前文件的所有FastFp（用于清理旧块）
    int current_file_chunk_count = 0;
    uint64_t *current_file_fastfps_from_client = NULL;
    
    if (recv_all(client_socket, &current_file_chunk_count, sizeof(int)) <= 0) {
        printf("Failed to receive current file chunk count from %s: %s\n", client_ip, strerror(errno));
        if (all_fastfps) free(all_fastfps);
        return;
    }
    
    if (current_file_chunk_count > 0) {
        current_file_fastfps_from_client = malloc(current_file_chunk_count * sizeof(uint64_t));
        if (!current_file_fastfps_from_client) {
            printf("Memory allocation failed for current file FastFps\n");
            if (all_fastfps) free(all_fastfps);
            return;
        }
        if (recv_all(client_socket, current_file_fastfps_from_client, current_file_chunk_count * sizeof(uint64_t)) <= 0) {
            printf("Failed to receive current file FastFp list from %s: %s\n", client_ip, strerror(errno));
            free(all_fastfps);
            free(current_file_fastfps_from_client);
            return;
        }
    }
    
    // 接收匹配的FastFp列表
    int match_count = 0;
    if (recv_all(client_socket, &match_count, sizeof(int)) <= 0) {
        printf("Failed to receive match count from %s: %s\n", client_ip, strerror(errno));
        if (all_fastfps) free(all_fastfps);
        if (current_file_fastfps_from_client) free(current_file_fastfps_from_client);
        return;
    }
    
    if (match_count < 0 || match_count > 10000) {
        printf("Invalid match count received: %d\n", match_count);
        if (all_fastfps) free(all_fastfps);
        return;
    }
    
    printf("Client reported %d matching FastFps\n", match_count);
    
    FastFpData *matching_fastfps = NULL;
    if (match_count > 0) {
        matching_fastfps = malloc(match_count * sizeof(FastFpData));
        if (!matching_fastfps) {
            printf("Memory allocation failed for matching FastFps\n");
            if (all_fastfps) free(all_fastfps);
            return;
        }
        if (recv_all(client_socket, matching_fastfps, match_count * sizeof(FastFpData)) <= 0) {
            printf("Failed to receive matching FastFp list from %s: %s\n", client_ip, strerror(errno));
            free(all_fastfps);
            free(matching_fastfps);
            return;
        }
        
        // 为匹配的FastFp计算SHA1哈希
        unsigned char *sha1_hashes = malloc(match_count * SHA_DIGEST_LENGTH);
        if (!sha1_hashes) {
            printf("Memory allocation failed for SHA1 hashes\n");
            free(all_fastfps);
            free(matching_fastfps);
            return;
        }
        
        for (int i = 0; i < match_count; i++) {
            if (fastfp_exists_locally(matching_fastfps[i].fastfp)) {
                char chunk_path[512];
                snprintf(chunk_path, sizeof(chunk_path), "%s/%016lx.chunk", STORAGE_DIR, matching_fastfps[i].fastfp);
                
                FILE *chunk_file = fopen(chunk_path, "rb");
                if (chunk_file) {
                    fseek(chunk_file, 0, SEEK_END);
                    long size = ftell(chunk_file);
                    if (size > 0) {
                        fseek(chunk_file, 0, SEEK_SET);
                        
                        unsigned char *chunk_data = malloc(size);
                        if (chunk_data) {
                            if (fread(chunk_data, 1, size, chunk_file) == size) {
                                calculate_sha1(chunk_data, size, sha1_hashes + i * SHA_DIGEST_LENGTH);
                                printf("Calculated SHA1 for existing chunk 0x%016lx\n", matching_fastfps[i].fastfp);
                            } else {
                                memset(sha1_hashes + i * SHA_DIGEST_LENGTH, 0, SHA_DIGEST_LENGTH);
                                printf("Failed to read chunk 0x%016lx, sending empty SHA1\n", matching_fastfps[i].fastfp);
                            }
                            free(chunk_data);
                        } else {
                            memset(sha1_hashes + i * SHA_DIGEST_LENGTH, 0, SHA_DIGEST_LENGTH);
                            printf("Memory allocation failed for chunk 0x%016lx, sending empty SHA1\n", matching_fastfps[i].fastfp);
                        }
                    } else {
                        memset(sha1_hashes + i * SHA_DIGEST_LENGTH, 0, SHA_DIGEST_LENGTH);
                        printf("Chunk 0x%016lx is empty, sending empty SHA1\n", matching_fastfps[i].fastfp);
                    }
                    fclose(chunk_file);
                } else {
                    memset(sha1_hashes + i * SHA_DIGEST_LENGTH, 0, SHA_DIGEST_LENGTH);
                    printf("Chunk 0x%016lx not found, sending empty SHA1\n", matching_fastfps[i].fastfp);
                }
            } else {
                memset(sha1_hashes + i * SHA_DIGEST_LENGTH, 0, SHA_DIGEST_LENGTH);
                printf("Chunk 0x%016lx not found locally, sending empty SHA1\n", matching_fastfps[i].fastfp);
            }
        }
        
        // 发送SHA1哈希给客户端
        if (send_all(client_socket, sha1_hashes, match_count * SHA_DIGEST_LENGTH) <= 0) {
            printf("Failed to send SHA1 hashes to %s: %s\n", client_ip, strerror(errno));
            free(all_fastfps);
            free(matching_fastfps);
            free(sha1_hashes);
            return;
        }
        
        free(sha1_hashes);
    } else {
        printf("No matching FastFps to verify, skipping SHA1 calculation\n");
    }
    
    // 接收需要上传的新块
    int upload_count = 0;
    if (recv_all(client_socket, &upload_count, sizeof(int)) <= 0) {
        printf("Failed to receive upload count from %s: %s\n", client_ip, strerror(errno));
        if (all_fastfps) free(all_fastfps);
        if (matching_fastfps) free(matching_fastfps);
        return;
    }
    
    if (upload_count < 0 || upload_count > 10000) {
        printf("Invalid upload count received: %d\n", upload_count);
        if (all_fastfps) free(all_fastfps);
        if (matching_fastfps) free(matching_fastfps);
        return;
    }
    
    printf("Receiving %d new chunks from client %s\n", upload_count, client_ip);
    
    // 保存当前文件的FastFp列表，用于后续清理
    uint64_t *current_file_fastfps = NULL;
    int current_fastfp_count = 0;
    int error_occurred = 0;
    
    // 先添加匹配的FastFp
    if (match_count > 0 && matching_fastfps) {
        current_file_fastfps = malloc((upload_count + match_count) * sizeof(uint64_t));
        if (!current_file_fastfps) {
            printf("Memory allocation failed for current file FastFps\n");
            error_occurred = 1;
        } else {
            for (int i = 0; i < match_count; i++) {
                current_file_fastfps[current_fastfp_count++] = matching_fastfps[i].fastfp;
            }
        }
    }
    
    if (!error_occurred) {
        for (int i = 0; i < upload_count; i++) {
            // 接收FastFp
            uint64_t fastfp;
            if (recv_all(client_socket, &fastfp, sizeof(uint64_t)) <= 0) {
                printf("Failed to receive FastFp from %s: %s\n", client_ip, strerror(errno));
                error_occurred = 1;
                break;
            }
            
            // 接收块大小
            int chunk_size;
            if (recv_all(client_socket, &chunk_size, sizeof(int)) <= 0) {
                printf("Failed to receive chunk size from %s: %s\n", client_ip, strerror(errno));
                error_occurred = 1;
                break;
            }
            
            if (chunk_size <= 0 || chunk_size > MAX_CACHE_SIZE) {
                printf("Invalid chunk size received: %d\n", chunk_size);
                error_occurred = 1;
                break;
            }
            
            // 接收块数据
            unsigned char *chunk_data = malloc(chunk_size);
            if (!chunk_data) {
                printf("Memory allocation failed for chunk data\n");
                error_occurred = 1;
                break;
            }
            
            int total_received = 0;
            int chunk_error = 0;
            while (total_received < chunk_size) {
                int bytes_to_receive = (chunk_size - total_received > 4096) ? 4096 : (chunk_size - total_received);
                int bytes_received = recv(client_socket, chunk_data + total_received, bytes_to_receive, 0);
                if (bytes_received <= 0) {
                    printf("Failed to receive chunk data from %s: %s\n", client_ip, strerror(errno));
                    free(chunk_data);
                    chunk_error = 1;
                    error_occurred = 1;
                    break;
                }
                total_received += bytes_received;
            }
            
            if (chunk_error) {
                break;
            }
            
            if (total_received != chunk_size) {
                printf("Incomplete chunk data received from %s\n", client_ip);
                free(chunk_data);
                error_occurred = 1;
                break;
            }
            
            // 保存到文件
            char chunk_filename[256];
            snprintf(chunk_filename, sizeof(chunk_filename), "%s/%016lx.chunk", STORAGE_DIR, fastfp);
            
            FILE *out_file = fopen(chunk_filename, "wb");
            if (out_file) {
                if (fwrite(chunk_data, 1, chunk_size, out_file) == chunk_size) {
                    printf("Saved chunk to %s (size: %d) from client %s\n", chunk_filename, chunk_size, client_ip);
                } else {
                    printf("Failed to write chunk to %s\n", chunk_filename);
                }
                fclose(out_file);
            } else {
                printf("Failed to save chunk to %s\n", chunk_filename);
            }
            
            // 添加到当前文件的FastFp列表
            if (current_file_fastfps) {
                current_file_fastfps[current_fastfp_count++] = fastfp;
            }
            
            free(chunk_data);
        }
    }
    
    // 只有在没有发生错误且有当前文件的FastFp列表时才执行清理
    if (!error_occurred && current_file_fastfps && current_fastfp_count > 0) {
        printf("Cleaning up chunks not in current file...\n");
        cleanup_chunks_not_in_list(STORAGE_DIR, current_file_fastfps, current_fastfp_count);
    } else if (error_occurred) {
        printf("Error occurred during processing, skipping cleanup\n");
    }
    
    // 清理资源
    if (all_fastfps) free(all_fastfps);
    if (matching_fastfps) free(matching_fastfps);
    if (current_file_fastfps) free(current_file_fastfps);
    
    printf("Finished handling client %s on server%d\n", client_ip, SERVER_ID);
}

int main(int argc, char *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    int port = PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    printf("Starting server%d on port %d\n", SERVER_ID, port);
    
    // 确保目录存在
    create_directory_if_not_exists(STORAGE_DIR);
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("Server%d listening on port %d, storing chunks in %s/\n", SERVER_ID, port, STORAGE_DIR);
    
    while(1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            continue;
        }
        
        handle_client(new_socket, &address);
        close(new_socket);
    }
    
    return 0;
}