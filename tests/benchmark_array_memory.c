// 测量 Array 使用不同分配器时的 VSZ/RSS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kvstore.h"

void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}

static void wait_for_measurement(const char *phase) {
#if defined(KVS_BENCHMARK_JEMALLOC)
    const char *allocator = "jemalloc";
#else
    const char *allocator = "glibc_malloc";
#endif

    printf("allocator: %s\n", allocator);
    printf("phase: %s\n", phase);
    printf("PID: %ld\n", (long)getpid());
    printf("press Enter to continue...\n");

    fflush(stdout);
    getchar();
}

int main(void) {
    const int entries = KVS_ARRAY_SIZE;

    kvs_array_t array = {0};

    if (entries > KVS_ARRAY_SIZE) {
        fprintf(stderr, "KVS_ARRAY_SIZE is too small\n");
        return 1;
    }

    if (kvs_array_create(&array) != 0) {
        fprintf(stderr, "failed to create array\n");
        return 1;
    }

    char key[64];
    char value[64];

    /*
     * 这里直接填充 Array 槽位。
     * 因为逐个调用 kvs_array_set 会反复线性查找，
     * 插入 20 万条数据的时间复杂度是 O(n²)。
     */
    for (int i = 0; i < entries; i++) {
        snprintf(key, sizeof(key), "key-%d", i);
        snprintf(value, sizeof(value), "value-%d", i);

        size_t key_size = strlen(key) + 1;
        size_t value_size = strlen(value) + 1;

        char *key_copy = kvs_malloc(key_size);
        if (key_copy == NULL) {
            fprintf(stderr, "key allocation failed at %d\n", i);
            kvs_array_destory(&array);
            return 1;
        }

        char *value_copy = kvs_malloc(value_size);
        if (value_copy == NULL) {
            kvs_free(key_copy);
            fprintf(stderr, "value allocation failed at %d\n", i);
            kvs_array_destory(&array);
            return 1;
        }

        memcpy(key_copy, key, key_size);
        memcpy(value_copy, value, value_size);

        array.table[array.total].key = key_copy;
        array.table[array.total].value = value_copy;
        array.total++;
    }

    printf("inserted items: %d\n", array.total);
    wait_for_measurement("after insert");

    /*
     * 每次删除 table[0]。
     * DEL 会用最后一个元素填补 table[0]，
     * 因此下一轮仍然删除 table[0]，避免 O(n²) 查找。
     */
    while (array.total > 0) {
        size_t key_size =
            strlen(array.table[0].key) + 1;

        if (key_size > sizeof(key)) {
            fprintf(stderr, "key is too long\n");
            kvs_array_destory(&array);
            return 1;
        }

        memcpy(key, array.table[0].key, key_size);

        if (kvs_array_del(&array, key) != 0) {
            fprintf(stderr, "array delete failed\n");
            kvs_array_destory(&array);
            return 1;
        }
    }

    printf("remaining items: %d\n", array.total);
    wait_for_measurement("after delete");

    kvs_array_destory(&array);

    wait_for_measurement("after destroy");

    return 0;
}