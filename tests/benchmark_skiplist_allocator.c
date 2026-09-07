//不经过网络 反复测试 内存池和 malloc 在 增加和删除一个 skiplist 节点时的性能差异。
#include <stdio.h>      // printf
#include <stdlib.h>     // malloc、free
#include <time.h>       // clock_gettime

#include "kvstore.h"    // skiplist 的结构体和接口声明

#ifndef KVS_SKIPLIST_USE_MEMORY_POOL
#define KVS_SKIPLIST_USE_MEMORY_POOL 1
#endif

/*
 * 这个 benchmark 不链接 src/kvstore.c，
 * 但 kvs_tree.c 会调用 kvs_malloc 和 kvs_free，
 * 所以在测试程序中提供简单实现。
 */
void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}

static double elapsed_seconds(const struct timespec *start, const struct timespec *end){
    //计算整数秒的差值
    double seconds = (double)(end->tv_sec - start->tv_sec);
    // 计算不足一秒部分的纳秒差值
    double nanoseconds=(double)(end->tv_nsec - start->tv_nsec);
    // 将纳秒差值转换为秒并加到整数秒差值上
    return seconds + nanoseconds / 1e9;//十亿纳秒等于一秒 
}
int main(){
    const int rounds =100000000;
     // 创建一个初始值全部为 0 的 skiplist 管理结构
    kvs_skiplist_t skiplist = {0};

    // 初始化 skiplist 的桶数组和节点分配策略
    if (kvs_skiplist_create(&skiplist) != 0) {
        fprintf(stderr, "failed to create skiplist\n");
        return 1;
    }
    char key[] = "benchmark-key";
    char value[] = "benchmark-value";
    
    struct timespec start, end;
    if(clock_gettime(CLOCK_MONOTONIC, &start) != 0){
        perror("clock_gettime");
        kvs_skiplist_destory(&skiplist);
        return 1;
    }
    // 反复创建和删除同一个 skiplist 节点
    for (int i = 0; i < rounds; i++) {
        // SET 会创建节点，并申请节点、key 和 value 的空间
        if (kvs_skiplist_set(&skiplist, key, value) != 0) {
            fprintf(stderr, "skiplist set failed at round %d\n", i);
            kvs_skiplist_destory(&skiplist);
            return 1;
        }

        // DEL 会释放 key/value，并释放或归还节点空间
        if (kvs_skiplist_del(&skiplist, key) != 0) {
            fprintf(stderr, "skiplist delete failed at round %d\n", i);
            kvs_skiplist_destory(&skiplist);
            return 1;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
    perror("clock_gettime");
    kvs_skiplist_destory(&skiplist);
    return 1;
} 
    // 将开始和结束时间转换成秒
    double seconds = elapsed_seconds(&start, &end);

    // 防止异常时间导致除以 0
    if (seconds <= 0.0) {
        fprintf(stderr, "invalid elapsed time\n");
        kvs_skiplist_destory(&skiplist);
        return 1;
    }

    // 每轮包含一次 SET 和一次 DEL，所以乘以 2
    long long operations = (long long)rounds * 2;

    // 每秒完成的 skiplist 操作数量
    double ops_per_second =
        (double)operations / seconds;

    // 根据编译宏确定当前测试的分配策略名称
// 根据编译宏确定当前测试的分配策略名称
#if KVS_SKIPLIST_USE_MEMORY_POOL
    const char *allocator = "memory_pool";
#elif defined(KVS_BENCHMARK_JEMALLOC)
    const char *allocator = "jemalloc";
#else
    const char *allocator = "glibc_malloc";
#endif

    // 输出本轮 benchmark 的最终结果
    printf("allocator: %s\n", allocator);
    printf("rounds: %d\n", rounds);
    printf("operations: %lld\n", operations);
    printf("elapsed: %.6f seconds\n", seconds);
    printf("ops/sec: %.0f\n", ops_per_second);

    // 释放桶数组、剩余节点和内存池 Chunk
    kvs_skiplist_destory(&skiplist);

    return 0;
}
