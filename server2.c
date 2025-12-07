// server2.c - 服务端2代码
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

#define PORT 8081
#define MAX_CACHE_SIZE (100 * 1024 * 1024)

// 计算 SHA1 哈希的辅助函数
void calculate_sha1(const unsigned char *data, size_t len, unsigned char *sha1_hash) {
    SHA1(data, len, sha1_hash);
}

// 创建目录的辅助函数
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
uint64_t* get_all_fastfps_from_dir(const char* dir_path, int* count) {
    DIR *d;
    struct dirent *entry;
    uint64_t *fastfps = malloc(1000 * sizeof(uint64_t)); // 假设最多1000个chunks
    *count = 0;
    
    d = opendir(dir_path);
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            if (strstr(entry->d_name, ".chunk") != NULL) {
                uint64_t fastfp;
                if (sscanf(entry->d_name, "%16lx.chunk", &fastfp) == 1) {
                    fastfps[*count] = fastfp;
                    (*count)++;
                    if (*count >= 1000) { // 扩展空间
                        fastfps = realloc(fastfps, (*count + 1000) * sizeof(uint64_t));
                    }
                }
            }
        }
        closedir(d);
    }
    
    return fastfps;
}

// 处理客户端连接
void handle_client(int client_socket) {
    printf("Handling client connection on server2\n");
    
    // 创建存储目录
    create_directory_if_not_exists("./server2");
    
    // 接收文件名
    int name_len;
    recv(client_socket, &name_len, sizeof(int), 0);
    char filename[name_len + 1];
    recv(client_socket, filename, name_len, 0);
    filename[name_len] = '\0';
    
    printf("Received file: %s\n", filename);
    
    // 读取文件内容（虽然不需要，但为了同步）
    unsigned char temp_buffer[4096];
    int bytes_read;
    do {
        bytes_read = recv(client_socket, temp_buffer, sizeof(temp_buffer), 0);
    } while (bytes_read > 0);
    
    // 收集当前目录中的所有FastFp
    int fastfp_count;
    uint64_t *all_fastfps = get_all_fastfps_from_dir("./server2", &fastfp_count);
    
    // 发送FastFp列表给客户端
    send(client_socket, &fastfp_count, sizeof(int), 0);
    if (fastfp_count > 0) {
        send(client_socket, all_fastfps, fastfp_count * sizeof(uint64_t), 0);
    }
    
    printf("Sent %d FastFp values to client\n", fastfp_count);
    
    // 接收匹配的FastFp列表
    int match_count;
    recv(client_socket, &match_count, sizeof(int), 0);
    
    uint64_t *matching_fastfps = NULL;
    if (match_count > 0) {
        matching_fastfps = malloc(match_count * sizeof(uint64_t));
        recv(client_socket, matching_fastfps, match_count * sizeof(uint64_t), 0);
    }
    
    // 为匹配的FastFp计算SHA1哈希
    unsigned char *sha1_hashes = malloc(match_count * SHA_DIGEST_LENGTH);
    for (int i = 0; i < match_count; i++) {
        char chunk_path[512];
        snprintf(chunk_path, sizeof(chunk_path), "./server2/%016lx.chunk", matching_fastfps[i]);
        
        FILE *chunk_file = fopen(chunk_path, "rb");
        if (chunk_file) {
            fseek(chunk_file, 0, SEEK_END);
            long size = ftell(chunk_file);
            fseek(chunk_file, 0, SEEK_SET);
            
            unsigned char *chunk_data = malloc(size);
            fread(chunk_data, 1, size, chunk_file);
            
            calculate_sha1(chunk_data, size, sha1_hashes + i * SHA_DIGEST_LENGTH);
            
            free(chunk_data);
            fclose(chunk_file);
        } else {
            // 如果找不到文件，生成空的SHA1
            memset(sha1_hashes + i * SHA_DIGEST_LENGTH, 0, SHA_DIGEST_LENGTH);
        }
    }
    
    // 发送SHA1哈希给客户端（第一个是自己的，第二个是模拟server1的）
    send(client_socket, sha1_hashes, match_count * SHA_DIGEST_LENGTH, 0); // 模拟server1的哈希
    send(client_socket, sha1_hashes, match_count * SHA_DIGEST_LENGTH, 0); // 自己的哈希
    
    // 接收需要上传的新块
    int upload_count;
    recv(client_socket, &upload_count, sizeof(int), 0);
    
    printf("Receiving %d new chunks to store\n", upload_count);
    
    for (int i = 0; i < upload_count; i++) {
        // 接收FastFp
        uint64_t fastfp;
        recv(client_socket, &fastfp, sizeof(uint64_t), 0);
        
        // 接收块大小
        int chunk_size;
        recv(client_socket, &chunk_size, sizeof(int), 0);
        
        // 接收块数据
        unsigned char *chunk_data = malloc(chunk_size);
        recv(client_socket, chunk_data, chunk_size, 0);
        
        // 保存到文件
        char chunk_filename[256];
        snprintf(chunk_filename, sizeof(chunk_filename), "./server2/%016lx.chunk", fastfp);
        
        FILE *out_file = fopen(chunk_filename, "wb");
        if (out_file) {
            fwrite(chunk_data, 1, chunk_size, out_file);
            fclose(out_file);
            printf("Saved chunk to %s (size: %d)\n", chunk_filename, chunk_size);
        } else {
            printf("Failed to save chunk to %s\n", chunk_filename);
        }
        
        free(chunk_data);
    }
    
    // 清理资源
    if (all_fastfps) free(all_fastfps);
    if (matching_fastfps) free(matching_fastfps);
    if (sha1_hashes) free(sha1_hashes);
    
    printf("Finished handling client on server2\n");
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    printf("Starting server2 on port %d\n", PORT);
    
    // 创建socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    // 设置socket选项
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // 绑定
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    // 监听
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("Server2 listening on port %d\n", PORT);
    
    while(1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        
        handle_client(new_socket);
        close(new_socket);
    }
    
    return 0;
}