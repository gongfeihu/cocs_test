#include "fastcdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <openssl/sha.h>

// USE_CHUNKING_METHOD
#define USE_CHUNKING_METHOD 1
#define STEP 32
#define MAX_CACHE_SIZE (100 * 1024 * 1024)  // 限制为100MB而不是1GB

// 创建目录的辅助函数
int create_directory_if_not_exists(const char *dir) {
    struct stat st = {0};
    if (stat(dir, &st) == -1) {
        #ifdef _WIN32
            return mkdir(dir);
        #else
            return mkdir(dir, 0755);  // 创建目录，设置权限
        #endif
    }
    return 0;
}

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

// 检查文件是否已存在于服务器目录中的辅助函数（FastFp + SHA1 双重校验）
int file_exists_in_server_with_strong_hash(const char *new_chunk_data, size_t new_chunk_size, uint64_t fastfp) {
    char filepath1[512], filepath2[512];
    snprintf(filepath1, sizeof(filepath1), "./server1/%016lx.chunk", fastfp);
    snprintf(filepath2, sizeof(filepath2), "./server2/%016lx.chunk", fastfp);
    
    // 检查 server1
    FILE *f1 = fopen(filepath1, "rb");
    if (f1) {
        // 读取现有分块内容
        fseek(f1, 0, SEEK_END);
        long existing_size = ftell(f1);
        fseek(f1, 0, SEEK_SET);
        
        if (existing_size == (long)new_chunk_size) {
            unsigned char *existing_data = malloc(existing_size);
            if (existing_data) {
                fread(existing_data, 1, existing_size, f1);
                
                // 计算现有分块的 SHA1
                unsigned char existing_sha1[SHA_DIGEST_LENGTH];
                calculate_sha1(existing_data, existing_size, existing_sha1);
                
                // 计算新分块的 SHA1
                unsigned char new_sha1[SHA_DIGEST_LENGTH];
                calculate_sha1(new_chunk_data, new_chunk_size, new_sha1);
                
                // 比较 SHA1 哈希
                int sha1_match = 1;
                for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
                    if (existing_sha1[i] != new_sha1[i]) {
                        sha1_match = 0;
                        break;
                    }
                }
                
                printf("  -> FastFp 0x%016lx found in server1 (size: %ld), SHA1 comparison: ", fastfp, existing_size);
                if (sha1_match) {
                    printf("MATCH\n");
                    print_sha1_hash(existing_sha1, "Existing SHA1");
                    print_sha1_hash(new_sha1, "New SHA1");
                    printf("  -> Identical content, skipping upload\n");
                } else {
                    printf("MISMATCH\n");
                    print_sha1_hash(existing_sha1, "Existing SHA1");
                    print_sha1_hash(new_sha1, "New SHA1");
                    printf("  -> Different content despite same FastFp, will upload new chunk\n");
                }
                
                free(existing_data);
                
                if (sha1_match) {
                    fclose(f1);
                    return 1; // 找到相同的分块在 server1
                }
            }
        } else {
            printf("  -> FastFp 0x%016lx found in server1 but size differs (%ld vs %zu), will upload new chunk\n", 
                   fastfp, existing_size, new_chunk_size);
        }
        fclose(f1);
    }
    
    // 检查 server2
    FILE *f2 = fopen(filepath2, "rb");
    if (f2) {
        // 读取现有分块内容
        fseek(f2, 0, SEEK_END);
        long existing_size = ftell(f2);
        fseek(f2, 0, SEEK_SET);
        
        if (existing_size == (long)new_chunk_size) {
            unsigned char *existing_data = malloc(existing_size);
            if (existing_data) {
                fread(existing_data, 1, existing_size, f2);
                
                // 计算现有分块的 SHA1
                unsigned char existing_sha1[SHA_DIGEST_LENGTH];
                calculate_sha1(existing_data, existing_size, existing_sha1);
                
                // 计算新分块的 SHA1
                unsigned char new_sha1[SHA_DIGEST_LENGTH];
                calculate_sha1(new_chunk_data, new_chunk_size, new_sha1);
                
                // 比较 SHA1 哈希
                int sha1_match = 1;
                for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
                    if (existing_sha1[i] != new_sha1[i]) {
                        sha1_match = 0;
                        break;
                    }
                }
                
                printf("  -> FastFp 0x%016lx found in server2 (size: %ld), SHA1 comparison: ", fastfp, existing_size);
                if (sha1_match) {
                    printf("MATCH\n");
                    print_sha1_hash(existing_sha1, "Existing SHA1");
                    print_sha1_hash(new_sha1, "New SHA1");
                    printf("  -> Identical content, skipping upload\n");
                } else {
                    printf("MISMATCH\n");
                    print_sha1_hash(existing_sha1, "Existing SHA1");
                    print_sha1_hash(new_sha1, "New SHA1");
                    printf("  -> Different content despite same FastFp, will upload new chunk\n");
                }
                
                free(existing_data);
                
                if (sha1_match) {
                    fclose(f2);
                    return 2; // 找到相同的分块在 server2
                }
            }
        } else {
            printf("  -> FastFp 0x%016lx found in server2 but size differs (%ld vs %zu), will upload new chunk\n", 
                   fastfp, existing_size, new_chunk_size);
        }
        fclose(f2);
    }
    
    return 0; // 文件不存在或内容不同
}

// 清理旧分块文件，只保留当前文件的分块
void cleanup_old_chunks(uint64_t *current_fastfps, int chunk_num) {
    DIR *d;
    struct dirent *entry;
    int i, j;
    int is_current_chunk;
    
    // 检查 server1
    d = opendir("./server1");
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            // 检查是否是 .chunk 文件
            if (strstr(entry->d_name, ".chunk") != NULL) {
                // 从文件名解析 FastFp
                uint64_t chunk_fastfp;
                if (sscanf(entry->d_name, "%16lx.chunk", &chunk_fastfp) == 1) {
                    // 检查这个 FastFp 是否在当前文件的分块列表中
                    is_current_chunk = 0;
                    for (i = 0; i < chunk_num; i++) {
                        if (current_fastfps[i] == chunk_fastfp) {
                            is_current_chunk = 1;
                            break;
                        }
                    }
                    
                    if (!is_current_chunk) {
                        // 不是当前文件的分块，删除它
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "./server1/%s", entry->d_name);
                        remove(filepath);
                        printf("Deleted old chunk: %s\n", filepath);
                    }
                }
            }
        }
        closedir(d);
    }
    
    // 检查 server2
    d = opendir("./server2");
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            // 检查是否是 .chunk 文件
            if (strstr(entry->d_name, ".chunk") != NULL) {
                // 从文件名解析 FastFp
                uint64_t chunk_fastfp;
                if (sscanf(entry->d_name, "%16lx.chunk", &chunk_fastfp) == 1) {
                    // 检查这个 FastFp 是否在当前文件的分块列表中
                    is_current_chunk = 0;
                    for (i = 0; i < chunk_num; i++) {
                        if (current_fastfps[i] == chunk_fastfp) {
                            is_current_chunk = 1;
                            break;
                        }
                    }
                    
                    if (!is_current_chunk) {
                        // 不是当前文件的分块，删除它
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "./server2/%s", entry->d_name);
                        remove(filepath);
                        printf("Deleted old chunk: %s\n", filepath);
                    }
                }
            }
        }
        closedir(d);
    }
}

// 分块函数
int fastcdc_chunking(FILE *fp, unsigned char *fileCache) {
    uint64_t feature = 0;
    uint64_t weakhash = 0;
    size_t readStatus = 0;
 
    int chunk_num = 0;

    memset(fileCache, 0, MAX_CACHE_SIZE);

    int offset = 0, chunkLength = 0;
    fastCDC_init();

    if (fp == NULL) {
        perror("Fail to open file");
        exit(-1);
    }

    readStatus = fread(fileCache, 1, MAX_CACHE_SIZE, fp);
    int end = readStatus;
    int maxchunksum = (readStatus / MinSize) + 1;
    int *boundary = malloc(maxchunksum * sizeof(int));  // 动态分配边界数组
    uint64_t *fastfps = malloc(maxchunksum * sizeof(uint64_t));  // 存储每个块的 FastFp
    
    if (boundary == NULL || fastfps == NULL) {
        perror("Memory allocation failed");
        free(boundary);
        free(fastfps);
        exit(-1);
    }
         
    switch (USE_CHUNKING_METHOD)
    {
        case ORIGIN_CDC:
            chunking = cdc_origin_64;
            break;

        case ROLLING_2Bytes:
            chunking = rolling_data_2byes_64;
            break;
        
        case NORMALIZED_CDC:
            chunking = normalized_chunking_64;
            break;

        case NORMALIZED_2Bytes:
            chunking = normalized_chunking_2byes_64;
            break;
        
        default:
            fprintf(stderr, "No implement other chunking method");
    }
         
    // fastcdc分块
    for (;;) {
        weakhash = 0;  // 重置 weakhash 为当前块的 FastFp
        chunkLength = chunking(fileCache + offset, end - offset, &feature, &weakhash);
        boundary[chunk_num] = chunkLength;
        fastfps[chunk_num] = weakhash;  // 保存当前块的 FastFp
        offset += chunkLength;
        chunk_num += 1;
        if (offset >= end) 
            break;
    }

    // 创建目录
    create_directory_if_not_exists("./server1");
    create_directory_if_not_exists("./server2");
    
    // 输出分块信息（包含 FastFp）并写入块文件
    offset = 0;
    for(int i = 0; i < chunk_num; i++){
        printf("Chunk %d: offset=%d, length=%d, fastfp=0x%016lx\n", 
               i, offset, boundary[i], fastfps[i]);
        
        // 获取当前块的数据
        unsigned char *current_chunk_data = fileCache + offset;
        size_t current_chunk_size = boundary[i];
        
        // 检查是否已存在相同内容的分块（FastFp + SHA1 双重校验）
        int exists = file_exists_in_server_with_strong_hash(current_chunk_data, current_chunk_size, fastfps[i]);
        
        if (exists) {
            printf("  -> Chunk with fastfp 0x%016lx already exists in server%d, skipping upload\n", fastfps[i], exists);
        } else {
            // 决定保存到哪个目录：偶数块到server1，奇数块到server2
            char filename[64];
            if (i % 2 == 0) {
                sprintf(filename, "./server1/%016lx.chunk", fastfps[i]);
            } else {
                sprintf(filename, "./server2/%016lx.chunk", fastfps[i]);
            }
            
            // 写入块文件
            FILE *chunk_file = fopen(filename, "wb");
            if (chunk_file != NULL) {
                fwrite(current_chunk_data, 1, current_chunk_size, chunk_file);
                fclose(chunk_file);
                printf("  -> Saved to %s\n", filename);
            } else {
                printf("  -> Failed to save to %s\n", filename);
            }
        }
        
        offset += boundary[i];
    }
    
    // 清理旧的分块文件，只保留当前文件的分块
    cleanup_old_chunks(fastfps, chunk_num);
    
    printf("Total chunk number is %d\n", chunk_num);

    free(boundary);  // 释放边界数组内存
    free(fastfps);   // 释放 FastFp 数组内存
    return offset;
}

// functions
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

    // 64 bit init
    for (int i = 0; i < SymbolCount; i++) {
        LEARv2[i] = GEARv2[i] << 1;
    }

    MinSize = 8192 / 4;
    MaxSize = 8192 * 4;    // 32768;
    Mask_15 = 0xf9070353;  //  15个1
    Mask_11 = 0xd9000353;  //  11个1
    Mask_11_64 = 0x0000d90003530000;
    Mask_15_64 = 0x0000f90703530000;
    MinSize_divide_by_2 = MinSize / 2;
}

int normalized_chunking_64(unsigned char *p, int n,uint64_t *feature,uint64_t *weakhash) {
    uint64_t fingerprint = 0, digest;
    int originalMinSize = MinSize;
    MinSize = 6 * 1024;
    int i = MinSize, Mid = 8 * 1024;

    // the minimal subChunk Size.
    if (n <= MinSize)  
        return n;

    if (n > MaxSize)
        n = MaxSize;
    else if (n < Mid)
        Mid = n;

    while (i < Mid) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);

        if ((!(fingerprint & FING_GEAR_32KB_64))) {
            MinSize = originalMinSize;  // 恢复原始MinSize
            return i;
        }

        i++;
    }

    while (i < n) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);

        if ((!(fingerprint & FING_GEAR_02KB_64))) {
            MinSize = originalMinSize;  // 恢复原始MinSize
            return i;
        }

        i++;
    }

    MinSize = originalMinSize;  // 恢复原始MinSize
    return n;
}

int normalized_chunking_2byes_64(unsigned char *p, int n,uint64_t *feature,uint64_t *weakhash) {
    uint64_t fingerprint = 0, digest;
    int originalMinSize = MinSize;
    MinSize = 6 * 1024;
    int i = MinSize / 2, Mid = 8 * 1024;

    // the minimal subChunk Size.
    if (n <= MinSize) 
        return n;

    if (n > MaxSize)
        n = MaxSize;
    else if (n < Mid)
        Mid = n;

    while (i < Mid / 2) {
        int a = i * 2;
        fingerprint = (fingerprint << 2) + (LEARv2[p[a]]);

        if ((!(fingerprint & FING_GEAR_32KB_ls_64))) {
            MinSize = originalMinSize;  // 恢复原始MinSize
            return a;
        }

        fingerprint += GEARv2 [p[a + 1]];  

        if ((!(fingerprint & FING_GEAR_32KB_64))) {
            MinSize = originalMinSize;  // 恢复原始MinSize
            return a + 1;
        }

        i++;
    }

    while (i < n / 2) {
        int a = i * 2;
        fingerprint = (fingerprint << 2) + (LEARv2[p[a]]);

        if ((!(fingerprint & FING_GEAR_02KB_ls_64))) {
            MinSize = originalMinSize;  // 恢复原始MinSize
            return a;
        }

        fingerprint += GEARv2[p[a + 1]];

        if ((!(fingerprint & FING_GEAR_02KB_64))) {
            MinSize = originalMinSize;  // 恢复原始MinSize
            return a + 1;
        }

        i++;
    }

    MinSize = originalMinSize;  // 恢复原始MinSize
    return n;
}

int rolling_data_2byes_64(unsigned char *p, int n,uint64_t *feature,uint64_t *weakhash) {
    uint64_t fingerprint = 0, digest;
    int i = MinSize_divide_by_2;

    // the minimal subChunk Size.
    if (n <= MinSize) 
        return n;

    if (n > MaxSize) n = MaxSize;

    while (i < n / 2) {
        int a = i * 2;
        fingerprint = (fingerprint << 2) + (LEARv2[p[a]]);

        if ((!(fingerprint & FING_GEAR_08KB_ls_64))) {
            return a;
        }

        fingerprint += GEARv2[p[a + 1]];

        if ((!(fingerprint & FING_GEAR_08KB_64))) {
            return a + 1;
        }

        i++;
    }

    return n;
}

int cdc_origin_64(unsigned char *p, int n,uint64_t *feature,uint64_t *weakhash){
    uint64_t fingerprint = 0, digest;
    *weakhash = 0;
    int i = 0, ptr = 0;

    // The chunk size cannot exceed remaining length of file and MaxSize.
    n = n < MaxSize ? n : MaxSize;

    while (i < n) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);
        *feature = *feature > fingerprint ? *feature : fingerprint;
        if (i > MinSize && (!(fingerprint & FING_GEAR_08KB_64))) {
            return i + 1;
        }
        i++;
        ptr++;
        if(ptr % STEP == 0){
            *weakhash += fingerprint;
        }
    }

    return n;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        exit(-1);
    }
    
    gettimeofday(&tmStart, NULL);
    FILE* read = fopen(argv[1], "rb");  // 使用命令行参数指定的文件名
    if (read == NULL) {
        perror("Fail to open file");
        exit(-1);
    }
    
    // 使用较小的缓存大小以避免内存问题
    unsigned char *fileCache = (unsigned char *)malloc(MAX_CACHE_SIZE);
    if (fileCache == NULL) {
        perror("Memory allocation failed for file cache");
        fclose(read);
        exit(-1);
    }
    
    int n = fastcdc_chunking(read, fileCache);
    
    printf("Processed %d bytes\n", n);
    
    gettimeofday(&tmEnd, NULL);
    totalTm = (tmEnd.tv_sec - tmStart.tv_sec) * 1000000 + tmEnd.tv_usec - tmStart.tv_usec;
    printf("Total time is %f s\n", totalTm / 1000000);
    
    free(fileCache);
    fclose(read);
    
    return 0;
}