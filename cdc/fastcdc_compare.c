// fastcdc_compare.c - 本地使用 FastCDC 计算两个文件的冗余率
// 用法:
//   ./fastcdc_compare <new_file> <old_file>
// 说明:
//   计算 new_file 相比于 old_file 的冗余率。冗余率 = 从 old_file 复用的数据量 / new_file 大小。
//   采用与分布式客户端相同的 FastCDC 分块算法与弱指纹(weakhash)，并通过 SHA1 强校验消除碰撞。

#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/md5.h>

// 引入与客户端一致的 FastCDC 头（位于上级目录 test/fastcdc.h）
#include "../fastcdc.h"

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

// ------------------ 来自客户端的 FastCDC 初始化与分块实现 ------------------
void fastCDC_init(void) {
    unsigned char md5_digest[16];
    uint8_t seed[SeedLength];
    for (int i = 0; i < SymbolCount; i++) {
        for (int j = 0; j < SeedLength; j++) seed[j] = i;
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
    Mask_11_64 = 0x0000d90003530000ULL;
    Mask_15_64 = 0x0000f90703530000ULL;
    MinSize_divide_by_2 = MinSize / 2;
}

int normalized_chunking_64(unsigned char *p, int n, uint64_t *feature, uint64_t *weakhash) {
    uint64_t fingerprint = 0;
    int originalMinSize = MinSize;
    MinSize = 6 * 1024;
    int i = MinSize, Mid = 8 * 1024;

    if (n <= MinSize) {
        for (int j = 0; j < n; j++) fingerprint = (fingerprint << 1) + (GEARv2[p[j]]);
        MinSize = originalMinSize;
        *weakhash = fingerprint;
        return n;
    }

    if (n > MaxSize) n = MaxSize; else if (n < Mid) Mid = n;

    while (i < Mid) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);
        if ((!(fingerprint & FING_GEAR_32KB_64))) {
            MinSize = originalMinSize; *weakhash = fingerprint; return i;
        }
        i++;
    }
    while (i < n) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);
        if ((!(fingerprint & FING_GEAR_02KB_64))) {
            MinSize = originalMinSize; *weakhash = fingerprint; return i;
        }
        i++;
    }
    MinSize = originalMinSize; *weakhash = fingerprint; return n;
}
// -------------------------------------------------------------------------

static void sha1_of(const unsigned char *data, size_t len, unsigned char out[SHA_DIGEST_LENGTH]) {
    SHA1(data, len, out);
}

static char *read_file_fully(const char *path, size_t *out_size) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, sz, fp);
    fclose(fp);
    if (rd != sz) { free(buf); return NULL; }
    *out_size = sz;
    return buf;
}

typedef struct {
    int count;
    int *sizes;           // 每块大小
    uint64_t *weak;       // 弱指纹（weakhash）
    unsigned char *sha1;  // 每块 SHA1 (count * 20)
} ChunkList;

static void free_chunks(ChunkList *cl) {
    if (!cl) return;
    free(cl->sizes); free(cl->weak); free(cl->sha1);
    cl->count = 0; cl->sizes = NULL; cl->weak = NULL; cl->sha1 = NULL;
}

static int chunk_file_fastcdc(const unsigned char *data, size_t size, ChunkList *out) {
    fastCDC_init();
    int (*chunking)(unsigned char*, int, uint64_t*, uint64_t*) = normalized_chunking_64;

    int maxchunks = (int)(size / MinSize) + 2;
    int *boundary = (int *)malloc(sizeof(int) * maxchunks);
    uint64_t *weak = (uint64_t *)malloc(sizeof(uint64_t) * maxchunks);
    if (!boundary || !weak) { free(boundary); free(weak); return -1; }

    int cnt = 0; int offset = 0; int end = (int)size;
    while (offset < end) {
        uint64_t feature = 0, w = 0;
        int clen = chunking((unsigned char*)data + offset, end - offset, &feature, &w);
        if (clen <= 0) break;
        if (cnt >= maxchunks) break;
        boundary[cnt] = clen; weak[cnt] = w; cnt++; offset += clen;
    }

    unsigned char *sha1s = (unsigned char *)malloc(cnt * SHA_DIGEST_LENGTH);
    if (!sha1s) { free(boundary); free(weak); return -1; }

    // 计算每块 SHA1
    int off = 0;
    for (int i = 0; i < cnt; i++) {
        sha1_of((const unsigned char*)data + off, boundary[i], sha1s + i*SHA_DIGEST_LENGTH);
        off += boundary[i];
    }

    out->count = cnt;
    out->sizes = boundary;
    out->weak = weak;
    out->sha1 = sha1s;
    return 0;
}

// 简单哈希表（链地址）用于把 old 的 weak -> 列表索引映射
typedef struct WeakNode { int idx; struct WeakNode *next; } WeakNode;

typedef struct {
    WeakNode **buckets; // 大小为 cap
    int cap;
} WeakIndex;

static uint32_t hash64to32(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return (uint32_t)x;
}

static int weak_index_build(const ChunkList *oldc, WeakIndex *wx) {
    int cap = 1;
    while (cap < oldc->count * 2) cap <<= 1; // 简单取 >=2x 的 2 的幂
    if (cap < 8) cap = 8;
    wx->cap = cap;
    wx->buckets = (WeakNode**)calloc(cap, sizeof(WeakNode*));
    if (!wx->buckets) return -1;
    for (int i = 0; i < oldc->count; i++) {
        uint32_t h = hash64to32(oldc->weak[i]) & (cap - 1);
        WeakNode *node = (WeakNode*)malloc(sizeof(WeakNode));
        if (!node) return -1;
        node->idx = i; node->next = wx->buckets[h]; wx->buckets[h] = node;
    }
    return 0;
}

static void weak_index_free(WeakIndex *wx) {
    if (!wx || !wx->buckets) return;
    for (int i = 0; i < wx->cap; i++) {
        WeakNode *p = wx->buckets[i];
        while (p) { WeakNode *n = p->next; free(p); p = n; }
    }
    free(wx->buckets); wx->buckets = NULL; wx->cap = 0;
}

static int sha1_equal(const unsigned char *a, const unsigned char *b) {
    return memcmp(a, b, SHA_DIGEST_LENGTH) == 0;
}

int main(int argc, char *argv[]) {
    const char *new_path = (argc >= 2) ? argv[1] : "10M1.txt";
    const char *old_path = (argc >= 3) ? argv[2] : "10M.txt";

    size_t new_sz = 0, old_sz = 0;
    char *new_buf = read_file_fully(new_path, &new_sz);
    char *old_buf = read_file_fully(old_path, &old_sz);
    if (!new_buf) { fprintf(stderr, "[错误] 无法读取源文件: %s\n", new_path); return 1; }
    if (!old_buf) { fprintf(stderr, "[警告] 无法读取旧文件: %s (将视为完全不冗余)\n", old_path); old_sz = 0; }

    ChunkList newc = {0}, oldc = {0};
    if (chunk_file_fastcdc((unsigned char*)new_buf, new_sz, &newc) != 0) {
        fprintf(stderr, "[错误] new 文件分块失败\n"); free(new_buf); free(old_buf); return 1;
    }
    if (old_sz > 0 && chunk_file_fastcdc((unsigned char*)old_buf, old_sz, &oldc) != 0) {
        fprintf(stderr, "[错误] old 文件分块失败\n"); free_chunks(&newc); free(new_buf); free(old_buf); return 1;
    }

    // 构建 old 的弱指纹索引
    WeakIndex wx = {0};
    if (oldc.count > 0 && weak_index_build(&oldc, &wx) != 0) {
        fprintf(stderr, "[错误] 构建弱指纹索引失败\n");
        free_chunks(&newc); free_chunks(&oldc); free(new_buf); free(old_buf); return 1;
    }

    long long matched_bytes = 0;
    long long matched_blocks = 0;

    // 按“新文件视角”计数：新文件中的每个块只要在旧文件出现过（weak+SHA1 命中），即计入 matched；
    // 允许新文件中的重复块重复计数（因为上传时可以全部复用）。
    for (int i = 0; i < newc.count; i++) {
        if (oldc.count == 0) break;
        uint64_t w = newc.weak[i];
        uint32_t h = hash64to32(w) & (wx.cap - 1);
        WeakNode *p = wx.buckets[h];
        while (p) {
            int oi = p->idx;
            if (oldc.weak[oi] == w) {
                if (sha1_equal(newc.sha1 + i*SHA_DIGEST_LENGTH, oldc.sha1 + oi*SHA_DIGEST_LENGTH)) {
                    matched_bytes += newc.sizes[i];
                    matched_blocks += 1;
                    break; // 新文件的该块已匹配，转向下一个新块
                }
            }
            p = p->next;
        }
    }

    long long total_bytes = (long long)new_sz;
    long long literal_bytes = total_bytes - matched_bytes;
    double redundancy = (total_bytes > 0) ? (matched_bytes * 100.0 / total_bytes) : 0.0;

    printf("========== FastCDC 冗余率(本地) ==========\n");
    printf("新文件: %s\n", new_path);
    printf("旧文件: %s\n", old_path);
    printf("新文件大小: %lld bytes\n", total_bytes);
    printf("新文件分块数: %d\n", newc.count);
    printf("旧文件分块数: %d\n", oldc.count);
    printf("\n匹配统计:\n");
    printf("  匹配块数: %lld\n", matched_blocks);
    printf("  匹配数据量: %lld bytes\n", matched_bytes);
    printf("  新数据量(需上传): %lld bytes\n", literal_bytes);
    printf("\n冗余率: %.2f%%\n", redundancy);
    printf("========================================\n");

    // 清理
    weak_index_free(&wx);
    free_chunks(&newc); free_chunks(&oldc);
    free(new_buf); free(old_buf);
    return 0;
}

