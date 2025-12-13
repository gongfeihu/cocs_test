#pragma once
/**
 * FastCDC 接口头文件
 */

#include <openssl/md5.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <zlib.h>

// 常量与配置宏
#define SymbolCount 256
#define SeedLength 64
#define CacheSize 1024 * 1024 * 1024

#define ORIGIN_CDC 1
#define ROLLING_2Bytes 2
#define NORMALIZED_CDC 3
#define NORMALIZED_2Bytes 4

// 公开的全局数据（由 fastcdc.c 定义）
extern uint64_t GEARv2[256];
extern uint64_t LEARv2[256];

extern uint64_t FING_GEAR_08KB_ls_64;
extern uint64_t FING_GEAR_02KB_ls_64;
extern uint64_t FING_GEAR_32KB_ls_64;
extern uint64_t FING_GEAR_08KB_64;
extern uint64_t FING_GEAR_02KB_64;
extern uint64_t FING_GEAR_32KB_64;

// 全局变量（由 fastcdc.c 定义）
extern struct timeval tmStart, tmEnd;
extern float totalTm;
extern int chunk_dist[30];
extern uint32_t g_global_matrix[SymbolCount];
extern uint32_t g_global_matrix_left[SymbolCount];
extern uint32_t expectCS;
extern uint32_t Mask_15;
extern uint32_t Mask_11;
extern uint64_t Mask_11_64, Mask_15_64;

extern uint32_t MinSize;
extern uint32_t MinSize_divide_by_2;
extern uint32_t MaxSize;
extern int sameCount;
extern int tmpCount;
extern int smalChkCnt;  // 记录小于8KB的分块

// 函数指针（可选）
extern int (*chunking)(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash);

// API：初始化/分块
void fastCDC_init(void);
int cdc_origin_64(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash);
int rolling_data_2byes_64(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash);
int normalized_chunking_64(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash);
int normalized_chunking_2byes_64(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash);
