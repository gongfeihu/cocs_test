#include "fastcdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// USE_CHUNKING_METHOD
#define USE_CHUNKING_METHOD 1
#define STEP 32
#define MAX_CACHE_SIZE (100 * 1024 * 1024)  // 限制为100MB而不是1GB

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

    // 输出分块信息（包含 FastFp）
    offset = 0;
    for(int i = 0; i < chunk_num; i++){
        printf("Chunk %d: offset=%d, length=%d, fastfp=0x%016lx\n", 
               i, offset, boundary[i], fastfps[i]);
        offset += boundary[i];
    }
    
    printf("Total chunk number is %d\n", chunk_num);
    printf("Max feature value is %lu\n", feature);

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

int main() {
    gettimeofday(&tmStart, NULL);
    FILE* read = fopen("testfile.bin", "rb");
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