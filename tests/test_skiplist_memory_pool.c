/* 验证 Skiplist 通过统一 allocator 管理全部动态数据对象。 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvs_allocator.h"
#include "kvstore.h"

/*
 * -1：正常申请。
 *  0：申请失败。
 *  N：允许 N 次申请，之后失败。
 */
static long fail_after = -1;

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

/* system 没有池统计；pool 模式额外核对存活对象数。 */
static void check_live(
    kvs_allocator_backend_t backend,
    size_t expected)
{
    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t stats;
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == expected);
    }
}

static void check_value(
    kvs_skiplist_t *list,
    char *key,
    const char *expected)
{
    char *value = kvs_skiplist_get(list, key);
    assert(value != NULL);
    assert(strcmp(value, expected) == 0);
}

/* 第 0 层应包含全部普通节点，并严格按 key 排序。 */
static void check_order(kvs_skiplist_t *list, size_t expected)
{
    kvs_skiplist_node_t *previous = NULL;
    kvs_skiplist_node_t *node = list->header->forward[0];
    size_t count = 0;

    while (node != NULL) {
        /* 同时避免错误链表形成环后，测试无限循环。 */
        assert(count < expected);

        if (previous != NULL) {
            assert(strcmp(previous->key, node->key) < 0);
        }

        previous = node;
        node = node->forward[0];
        count++;
    }

    assert(count == expected);
}

static void test_backend(kvs_allocator_backend_t backend)
{
    kvs_skiplist_t list = {0};

    /* 两个后端使用相同的随机层数序列。 */
    srand(1);
    fail_after = -1;
    assert(kvs_allocator_init(backend) == 0);

    /*
     * header 需要申请：
     * 节点、空 key、空 value、forward。
     * 逐一模拟失败，检查中间对象是否全部清理。
     */
    for (long allowed = 0; allowed < 4; allowed++) {
        fail_after = allowed;
        assert(kvs_skiplist_create(&list) < 0);
        fail_after = -1;

        assert(list.header == NULL);
        assert(list.level == 0);
        check_live(backend, 0);
    }

    assert(kvs_skiplist_create(&list) == 0);
    assert(list.header != NULL);
    assert(list.level == 0);
    check_order(&list, 0);
    check_live(backend, 4);

    /*
     * 普通节点也需要四次申请。
     * 创建失败不能增加节点，也不能改变有效层数。
     */
    for (long allowed = 0; allowed < 4; allowed++) {
        int old_level = list.level;

        fail_after = allowed;
        assert(kvs_skiplist_set(&list, "failed", "value") < 0);
        fail_after = -1;

        assert(list.level == old_level);
        assert(kvs_skiplist_get(&list, "failed") == NULL);

        for (int i = 0; i <= KVS_SKIPLIST_MAX_LEVEL; i++) {
            assert(list.header->forward[i] == NULL);
        }

        check_order(&list, 0);
        check_live(backend, 4);
    }

    assert(kvs_skiplist_set(&list, "name", "Jasper") == 0);
    check_value(&list, "name", "Jasper");
    check_live(backend, 8);

    kvs_skiplist_node_t *original = list.header->forward[0];
    int original_level = list.level;

    /* SET 覆盖失败，必须保留旧 value。 */
    fail_after = 0;
    assert(kvs_skiplist_set(&list, "name", "Another") < 0);
    fail_after = -1;

    check_value(&list, "name", "Jasper");
    check_live(backend, 8);

    /* MOD 失败也必须保留旧 value。 */
    fail_after = 0;
    assert(kvs_skiplist_mod(&list, "name", "Stone") < 0);
    fail_after = -1;

    check_value(&list, "name", "Jasper");
    check_live(backend, 8);

    /* 成功覆盖不应替换节点或改变层数。 */
    assert(kvs_skiplist_set(&list, "name", "Another") == 0);
    check_value(&list, "name", "Another");

    assert(kvs_skiplist_mod(&list, "name", "Stone") == 0);
    check_value(&list, "name", "Stone");

    assert(list.header->forward[0] == original);
    assert(list.level == original_level);
    check_live(backend, 8);

    /* MOD 不创建不存在的 key。 */
    assert(kvs_skiplist_mod(&list, "missing", "value") == 1);
    assert(kvs_skiplist_get(&list, "missing") == NULL);
    check_live(backend, 8);

    assert(kvs_skiplist_mod(&list, "name", "") == 0);
    check_value(&list, "name", "");

    /* 记录修改前的大对象数量，随后验证它增加再恢复。 */
    size_t original_large_count = 0;
    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t stats;
        assert(kvs_allocator_pool_stats(&stats) == 0);
        original_large_count = stats.large_count;
    }

    char long_value[8193];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    assert(kvs_skiplist_mod(&list, "name", long_value) == 0);
    check_value(&list, "name", long_value);
    check_live(backend, 8);

    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t stats;
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.large_count == original_large_count + 1);
    }

    assert(kvs_skiplist_mod(&list, "name", "small") == 0);
    check_value(&list, "name", "small");

    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t stats;
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.large_count == original_large_count);
    }

    assert(kvs_skiplist_del(&list, "name") == 0);
    assert(kvs_skiplist_get(&list, "name") == NULL);
    assert(list.level == 0);
    check_order(&list, 0);
    check_live(backend, 4);

    assert(kvs_skiplist_del(&list, "missing") == 1);
    check_live(backend, 4);

    /* 批量插入，让多个 slab 和不同长度的 forward 参与测试。 */
    char key[32];
    char value[32];

    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof(key), "key-%04d", i);
        snprintf(value, sizeof(value), "value-%04d", i);
        assert(kvs_skiplist_set(&list, key, value) == 0);
    }

    check_order(&list, 2000);
    check_live(backend, 4 + 4 * 2000);

    /* 查询全部记录，覆盖跳表的多层查找路径。 */
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof(key), "key-%04d", i);
        snprintf(value, sizeof(value), "value-%04d", i);
        check_value(&list, key, value);
    }

    /* 删除偶数编号，保留奇数编号。 */
    for (int i = 0; i < 2000; i += 2) {
        snprintf(key, sizeof(key), "key-%04d", i);
        assert(kvs_skiplist_del(&list, key) == 0);
    }

    check_order(&list, 1000);
    check_live(backend, 4 + 4 * 1000);

    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof(key), "key-%04d", i);

        if (i % 2 == 0) {
            assert(kvs_skiplist_get(&list, key) == NULL);
        } else {
            snprintf(value, sizeof(value), "value-%04d", i);
            check_value(&list, key, value);
        }
    }

    /* 剩余 1000 条记录及 header 都由 destroy 释放。 */
    kvs_skiplist_destory(&list);
    assert(list.header == NULL);
    assert(list.level == 0);
    check_live(backend, 0);

    assert(kvs_allocator_destroy() == 0);

    printf("[PASS] Skiplist lifecycle and failures allocator=%s\n",
           backend == KVS_ALLOCATOR_SYSTEM ? "system" : "pool");
}

int main(void)
{
    test_backend(KVS_ALLOCATOR_SYSTEM);
    test_backend(KVS_ALLOCATOR_POOL);
    return 0;
}