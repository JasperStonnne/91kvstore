// 不经过网络，测试 Array 的 key/value 使用不同分配器时的性能

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "kvstore.h"

/*
 * 独立测试不链接 src/kvstore.c，
 * 因此在这里提供 kvs_malloc/kvs_free。
 *
 * 普通编译时，malloc/free 来自 glibc。
 * 链接 -ljemalloc 时，malloc/free 由 jemalloc 接管。
 */
void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}

/*
 * 计算 begin 到 end 之间经过的秒数。
 */
static double elapsed_seconds(
    const struct timespec *begin,
    const struct timespec *end
) {
    double seconds =
        (double)(end->tv_sec - begin->tv_sec);

    double nanoseconds =
        (double)(end->tv_nsec - begin->tv_nsec);

    return seconds + nanoseconds / 1000000000.0;
}

int main(void) {
    /*
     * 每轮执行一次 SET 和一次 DEL。
     */
    const int rounds = 100000000;

    /*
     * Array 管理结构必须初始化为 0。
     */
    kvs_array_t array = {0};

    /*
     * create 一次性申请整个连续 table。
     */
    if (kvs_array_create(&array) != 0) {
        fprintf(stderr, "failed to create array\n");
        return 1;
    }

    /*
     * 每轮复用相同的输入字符串。
     * SET 内部会为 key/value 申请新的动态内存。
     */
    char key[] = "benchmark-key";
    char value[] = "benchmark-value";

    struct timespec begin;
    struct timespec end;

    /*
     * 使用单调时钟记录开始时间。
     * 它不会受系统时间调整影响。
     */
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) {
        perror("clock_gettime");
        kvs_array_destory(&array);
        return 1;
    }

    /*
     * 反复执行：
     *
     * SET：申请 key/value
     * DEL：释放 key/value
     *
     * Array 的 item 槽位来自预先申请的 table，
     * 所以这里主要比较 key/value 的内存分配性能。
     */
    for (int i = 0; i < rounds; i++) {
        if (kvs_array_set(&array, key, value) != 0) {
            fprintf(
                stderr,
                "array set failed at round %d\n",
                i
            );

            kvs_array_destory(&array);
            return 1;
        }

        if (kvs_array_del(&array, key) != 0) {
            fprintf(
                stderr,
                "array delete failed at round %d\n",
                i
            );

            kvs_array_destory(&array);
            return 1;
        }
    }

    /*
     * 记录结束时间。
     */
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime");
        kvs_array_destory(&array);
        return 1;
    }

    /*
     * 计算测试耗时。
     */
    double seconds = elapsed_seconds(&begin, &end);

    if (seconds <= 0.0) {
        fprintf(stderr, "invalid elapsed time\n");
        kvs_array_destory(&array);
        return 1;
    }

    /*
     * 每轮包含 SET 和 DEL 两个操作。
     */
    long long operations = (long long)rounds * 2;

    double operations_per_second =
        (double)operations / seconds;

    /*
     * 这个宏只负责正确显示本次使用的分配器名称。
     */
#if defined(KVS_BENCHMARK_JEMALLOC)
    const char *allocator = "jemalloc";
#else
    const char *allocator = "glibc_malloc";
#endif

    printf("allocator: %s\n", allocator);
    printf("rounds: %d\n", rounds);
    printf("operations: %lld\n", operations);
    printf("elapsed: %.6f seconds\n", seconds);
    printf("ops/sec: %.0f\n", operations_per_second);

    /*
     * 此时 Array 为空，但仍需释放整体 table。
     */
    kvs_array_destory(&array);

    return 0;
}