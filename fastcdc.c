#include "fastcdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>

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

// 删除目录中所有 .chunk 文件的辅助函数
void delete_chunk_files_in_directory(const char *dir) {
    DIR *d;
    struct dirent *entry;
    
    d = opendir(dir);
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            // 检查是否是 .chunk 文件
            if (strstr(entry->d_name, ".chunk") != NULL) {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", dir, entry->d_name);
                remove(filepath);
                printf("Deleted: %s\n", filepath);
            }
        }
        closedir(d);
    }
}

// 删除元数据文件的辅助函数
void delete_metadata_file(const char *input_filename) {
    char metadata_filename[256];
    snprintf(metadata_filename, sizeof(metadata_filename), "./%s.metadata", input_filename);
    
    if (remove(metadata_filename) == 0) {
        printf("Deleted metadata file: %s\n", metadata_filename);
    }
}

// 检查文件是否已存在于服务器目录中的辅助函数
int file_exists_in_server(const char *filename) {
    char filepath1[512], filepath2[512];
    snprintf(filepath1, sizeof(filepath1), "./server1/%s", filename);
    snprintf(filepath2, sizeof(filepath2), "./server2/%s", filename);
    
    FILE *f1 = fopen(filepath1, "rb");
    if (f1) {
        fclose(f1);
        return 1; // 文件在 server1 中存在
    }
    
    FILE *f2 = fopen(filepath2, "rb");
    if (f2) {
        fclose(f2);
        return 2; // 文件在 server2 中存在
    }
    
    return 0; // 文件不存在
}

// 分块函数
int fastcdc_chunking(FILE *fp, unsigned char *fileCache, const char* input_filename) {
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

    // 创建目录并清理之前的 .chunk 文件
    create_directory_if_not_exists("./server1");
    create_directory_if_not_exists("./server2");
    
    printf("Cleaning old chunk files...\n");
    delete_chunk_files_in_directory("./server1");
    delete_chunk_files_in_directory("./server2");
    
    // 删除旧的元数据文件
    delete_metadata_file(input_filename);
    
    // 创建元数据文件，记录块的顺序和位置信息
    char metadata_filename[256];
    snprintf(metadata_filename, sizeof(metadata_filename), "./%s.metadata", input_filename);
    FILE *metadata_file = fopen(metadata_filename, "w");
    if (metadata_file) {
        fprintf(metadata_file, "filename=%s\n", input_filename);
        fprintf(metadata_file, "chunk_count=%d\n", chunk_num);
        fprintf(metadata_file, "chunk_server=server1,server2\n"); // 记录块分布策略
        fclose(metadata_file);
    }
    
    // 输出分块信息（包含 FastFp）并写入块文件到不同目录
    offset = 0;
    for(int i = 0; i < chunk_num; i++){
        printf("Chunk %d: offset=%d, length=%d, fastfp=0x%016lx\n", 
               i, offset, boundary[i], fastfps[i]);
        
        // 生成文件名
        char filename[64];
        sprintf(filename, "%016lx.chunk", fastfps[i]);
        
        // 检查文件是否已存在
        int exists = file_exists_in_server(filename);
        if (exists) {
            printf("  -> File %s already exists in server%d, skipping upload\n", filename, exists);
        } else {
            // 决定保存到哪个目录：偶数块到server1，奇数块到server2
            char filepath[64];
            if (i % 2 == 0) {
                sprintf(filepath, "./server1/%s", filename);
            } else {
                sprintf(filepath, "./server2/%s", filename);
            }
            
            // 写入块文件
            FILE *chunk_file = fopen(filepath, "wb");
            if (chunk_file != NULL) {
                fwrite(fileCache + offset, 1, boundary[i], chunk_file);
                fclose(chunk_file);
                printf("  -> Saved to %s\n", filepath);
            } else {
                printf("  -> Failed to save to %s\n", filepath);
            }
        }
        
        offset += boundary[i];
    }
    
    // 更新元数据文件，添加每个块的详细信息
    metadata_file = fopen(metadata_filename, "a");
    if (metadata_file) {
        for(int i = 0; i < chunk_num; i++) {
            fprintf(metadata_file, "chunk_%d=0x%016lx,%s,%s\n", 
                   i, fastfps[i], 
                   (i % 2 == 0) ? "server1" : "server2",
                   (i % 2 == 0) ? "./server1" : "./server2");
        }
        fclose(metadata_file);
    }
    
    printf("Total chunk number is %d\n", chunk_num);

    free(boundary);  // 释放边界数组内存
    free(fastfps);   // 释放 FastFp 数组内存
    return offset;
}

// 恢复文件函数
int restore_file(const char* input_metadata_filename, const char* output_filename) {
    FILE *output_file = fopen(output_filename, "wb");
    if (!output_file) {
        perror("Failed to open output file for restoration");
        return -1;
    }
    
    printf("Restoring file from chunks using metadata: %s\n", input_metadata_filename);
    
    // 读取元数据文件
    FILE *metadata_file = fopen(input_metadata_filename, "r");
    if (!metadata_file) {
        perror("Failed to open metadata file");
        fclose(output_file);
        return -1;
    }
    
    char line[512];
    int chunk_count = 0;
    char filename[256];
    
    // 读取基本元数据
    while (fgets(line, sizeof(line), metadata_file)) {
        if (strncmp(line, "chunk_count=", 12) == 0) {
            sscanf(line, "chunk_count=%d", &chunk_count);
        } else if (strncmp(line, "filename=", 9) == 0) {
            sscanf(line, "filename=%s", filename);
        }
    }
    
    // 重置文件指针以读取块信息
    rewind(metadata_file);
    
    // 创建一个临时数组来存储块信息
    char **chunk_fps = malloc(chunk_count * sizeof(char*));
    char **chunk_servers = malloc(chunk_count * sizeof(char*));
    for(int i = 0; i < chunk_count; i++) {
        chunk_fps[i] = malloc(17); // 16 hex chars + null terminator
        chunk_servers[i] = malloc(8); // "server1" or "server2" + null terminator
    }
    
    // 读取块信息
    while (fgets(line, sizeof(line), metadata_file)) {
        int chunk_idx;
        char chunk_fp[17], server[8];
        if (sscanf(line, "chunk_%d=0x%16s,%7s,", &chunk_idx, chunk_fp, server) == 3) {
            if (chunk_idx >= 0 && chunk_idx < chunk_count) {
                strcpy(chunk_fps[chunk_idx], chunk_fp);
                strcpy(chunk_servers[chunk_idx], server);
            }
        }
    }
    
    fclose(metadata_file);
    
    // 按顺序恢复文件
    for (int i = 0; i < chunk_count; i++) {
        char chunk_filename[64];
        snprintf(chunk_filename, sizeof(chunk_filename), "%s.chunk", chunk_fps[i]);
        
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "./%s/%s", chunk_servers[i], chunk_filename);
        
        FILE *chunk_file = fopen(filepath, "rb");
        if (chunk_file) {
            // 获取文件大小
            fseek(chunk_file, 0, SEEK_END);
            long chunk_size = ftell(chunk_file);
            fseek(chunk_file, 0, SEEK_SET);
            
            // 读取并写入块数据
            unsigned char *chunk_data = malloc(chunk_size);
            if (chunk_data) {
                fread(chunk_data, 1, chunk_size, chunk_file);
                fwrite(chunk_data, 1, chunk_size, output_file);
                free(chunk_data);
            }
            fclose(chunk_file);
            printf("  -> Restored chunk %d from %s\n", i, filepath);
        } else {
            printf("  -> Chunk %d not found at %s\n", i, filepath);
        }
    }
    
    // 释放内存
    for(int i = 0; i < chunk_count; i++) {
        free(chunk_fps[i]);
        free(chunk_servers[i]);
    }
    free(chunk_fps);
    free(chunk_servers);
    
    fclose(output_file);
    printf("File restoration completed: %s\n", output_filename);
    return 0;
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
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file> [restore_output_file]\n", argv[0]);
        fprintf(stderr, "  Example: %s myfile.bin                    # 分块处理\n", argv[0]);
        fprintf(stderr, "  Example: %s myfile.bin restored.bin      # 恢复文件\n", argv[0]);
        exit(1);
    }
    
    gettimeofday(&tmStart, NULL);
    
    if (argc == 2) {
        // 分块处理模式
        FILE* read = fopen(argv[1], "rb");  // 从命令行参数获取输入文件
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
        
        int n = fastcdc_chunking(read, fileCache, argv[1]);
        
        printf("Processed %d bytes from %s\n", n, argv[1]);
        
        gettimeofday(&tmEnd, NULL);
        totalTm = (tmEnd.tv_sec - tmStart.tv_sec) * 1000000 + tmEnd.tv_usec - tmStart.tv_usec;
        printf("Total time is %f s\n", totalTm / 1000000);
        
        free(fileCache);
        fclose(read);
    } else if (argc == 3) {
        // 恢复模式
        char metadata_filename[256];
        snprintf(metadata_filename, sizeof(metadata_filename), "./%s.metadata", argv[1]);
        printf("Restoring file to: %s using metadata: %s\n", argv[2], metadata_filename);
        restore_file(metadata_filename, argv[2]);
    }
    
    return 0;
}