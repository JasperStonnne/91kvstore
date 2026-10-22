/* 验证 Array 在 SYSTEM、POOL 后端下的内存生命周期和失败处理。 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kvstore.h"
#include "kvs_allocator.h"

/*
 * 仅用于测试的分配失败开关：
 * -1：正常分配。
 *  0：强制失败，直到恢复为 -1。
 * 正数：允许指定次数的申请通过，之后强制失败。
 */
static long fail_after = -1;

/* 独立测试不链接 src/kvstore.c，在这里提供统一入口。 */
void *kvs_malloc(size_t size)
{
    if (fail_after == 0) {
        return NULL;
    }

    if (fail_after > 0) {
        fail_after--;
    }

    return kvs_allocator_alloc(size);
}

void kvs_free(void *ptr)
{
    kvs_allocator_free(ptr);
}

static void test_array_backend(kvs_allocator_backend_t backend)
{
    assert(kvs_allocator_init(backend) == 0);

    kvs_array_t array = {0};
    kvs_pool_stats_t stats;

    /* 表申请失败后，应保持未创建状态，并允许重试。 */
    fail_after = 0;
    assert(kvs_array_create(&array) < 0);
    fail_after = -1;

    assert(array.table == NULL);
    assert(array.total == 0);

    /* 正常创建 Array。 */
    assert(kvs_array_create(&array) == 0);
    assert(array.table != NULL);
    assert(array.total == 0);

    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 1);

        /* 当前64位环境中，1024项的表超过4 KiB。 */
        assert(stats.large_count == 1);
    }

    /* 两条记录：一个表、两个 key、两个 value。 */
    assert(kvs_array_set(&array, "name", "Jasper") == 0);
    assert(kvs_array_set(&array, "city", "Shanghai") == 0);
    assert(array.total == 2);

    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 5);
    }

    char *value = kvs_array_get(&array, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);

    value = kvs_array_get(&array, "city");
    assert(value != NULL);
    assert(strcmp(value, "Shanghai") == 0);

    /* SET 覆盖已有 value，不增加记录数量。 */
    assert(kvs_array_set(&array, "name", "Another") == 0);
    assert(array.total == 2);

    value = kvs_array_get(&array, "name");
    assert(value != NULL);
    assert(strcmp(value, "Another") == 0);

    /* MOD 修改已有 value。 */
    assert(kvs_array_mod(&array, "name", "Stone") == 0);

    value = kvs_array_get(&array, "name");
    assert(value != NULL);
    assert(strcmp(value, "Stone") == 0);

    /* 删除 name 后，移动到前面的 city 仍然有效。 */
    assert(kvs_array_del(&array, "name") == 0);
    assert(array.total == 1);
    assert(kvs_array_get(&array, "name") == NULL);

    value = kvs_array_get(&array, "city");
    assert(value != NULL);
    assert(strcmp(value, "Shanghai") == 0);

    /* 删除不存在的 key 不改变记录数量。 */
    assert(kvs_array_del(&array, "missing") == 1);
    assert(array.total == 1);

    /* 填满 Array。 */
    for (int i = array.total; i < KVS_ARRAY_SIZE; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "capacity_%d", i);
        assert(kvs_array_set(&array, key, "value") == 0);
    }

    assert(array.total == KVS_ARRAY_SIZE);

    /* 满容量时，覆盖已有 key 仍成功。 */
    assert(kvs_array_set(&array, "city", "Beijing") == 0);

    value = kvs_array_get(&array, "city");
    assert(value != NULL);
    assert(strcmp(value, "Beijing") == 0);
    assert(array.total == KVS_ARRAY_SIZE);

    /* 满容量时，插入新 key 失败。 */
    assert(kvs_array_set(&array, "overflow", "value") < 0);
    assert(array.total == KVS_ARRAY_SIZE);

    /* 销毁满的 Array，释放全部 key/value 和整个表。 */
    kvs_array_destory(&array);

    assert(array.table == NULL);
    assert(array.total == 0);
    assert(array.idx == 0);

    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 0);
        assert(stats.large_count == 0);
    }

    assert(kvs_allocator_destroy() == 0);

    printf("[PASS] Array allocator=%s\n",
           backend == KVS_ALLOCATOR_SYSTEM ? "system" : "pool");
}

static void test_array_failures(kvs_allocator_backend_t backend)
{
    assert(kvs_allocator_init(backend) == 0);

    kvs_array_t array = {0};
    kvs_pool_stats_t stats;

    assert(kvs_array_create(&array) == 0);
    assert(kvs_array_set(&array, "stable", "old-value") == 0);

    /*
     * 分别让新记录的 key 申请失败、value 申请失败。
     * value 失败时，已经申请的 key 必须被回收。
     */
    for (long allowed = 0; allowed <= 1; allowed++) {
        fail_after = allowed;
        int result = kvs_array_set(&array, "new-key", "new-value");
        fail_after = -1;

        assert(result < 0);
        assert(array.total == 1);
        assert(kvs_array_get(&array, "new-key") == NULL);

        char *value = kvs_array_get(&array, "stable");
        assert(value != NULL);
        assert(strcmp(value, "old-value") == 0);

        if (backend == KVS_ALLOCATOR_POOL) {
            assert(kvs_allocator_pool_stats(&stats) == 0);
            assert(stats.live_objects == 3);
            assert(stats.large_count == 1);
        }
    }

    /* SET 覆盖和 MOD 申请新 value 失败时，必须保留旧值。 */
    for (int operation = 0; operation < 2; operation++) {
        fail_after = 0;

        int result = operation == 0
            ? kvs_array_set(&array, "stable", "replacement")
            : kvs_array_mod(&array, "stable", "replacement");

        fail_after = -1;

        assert(result < 0);
        assert(array.total == 1);

        char *value = kvs_array_get(&array, "stable");
        assert(value != NULL);
        assert(strcmp(value, "old-value") == 0);

        if (backend == KVS_ALLOCATOR_POOL) {
            assert(kvs_allocator_pool_stats(&stats) == 0);
            assert(stats.live_objects == 3);
            assert(stats.large_count == 1);
        }
    }

    /* MOD 不存在的 key，不应创建记录。 */
    fail_after = 0;
    int result = kvs_array_mod(&array, "missing", "value");
    fail_after = -1;

    assert(result == 1);
    assert(array.total == 1);
    assert(kvs_array_get(&array, "missing") == NULL);

    /* 恢复正常分配后，之前失败的新 key 应能成功插入。 */
    assert(kvs_array_set(&array, "new-key", "new-value") == 0);
    assert(array.total == 2);

    char *value = kvs_array_get(&array, "new-key");
    assert(value != NULL);
    assert(strcmp(value, "new-value") == 0);

    /* 空字符串是合法 value，不能当成 key 不存在。 */
    assert(kvs_array_set(&array, "empty", "") == 0);

    value = kvs_array_get(&array, "empty");
    assert(value != NULL);
    assert(strcmp(value, "") == 0);
    assert(array.total == 3);

    /* 长 value 超过小对象规格，走独立大对象路径。 */
    char long_value[8193];
    memset(long_value, 'L', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    assert(kvs_array_set(&array, "stable", long_value) == 0);

    value = kvs_array_get(&array, "stable");
    assert(value != NULL);
    assert(strcmp(value, long_value) == 0);
    assert(array.total == 3);

    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 7);
        assert(stats.large_count == 2); /* 表和长 value */
    }

    /* 从大 value 改回小 value，旧的大对象区域必须释放。 */
    assert(kvs_array_mod(&array, "stable", "short") == 0);

    value = kvs_array_get(&array, "stable");
    assert(value != NULL);
    assert(strcmp(value, "short") == 0);

    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 7);
        assert(stats.large_count == 1);
    }

    kvs_array_destory(&array);

    assert(array.table == NULL);
    assert(array.total == 0);
    assert(array.idx == 0);

    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 0);
        assert(stats.large_count == 0);
    }

    assert(kvs_allocator_destroy() == 0);

    printf("[PASS] Array failures and value lengths allocator=%s\n",
           backend == KVS_ALLOCATOR_SYSTEM ? "system" : "pool");
}

int main(void)
{
    test_array_backend(KVS_ALLOCATOR_SYSTEM);
    test_array_failures(KVS_ALLOCATOR_SYSTEM);

    test_array_backend(KVS_ALLOCATOR_POOL);
    test_array_failures(KVS_ALLOCATOR_POOL);

    return 0;
}