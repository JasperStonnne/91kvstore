/* 验证 Hash 在 SYSTEM、POOL 后端下的生命周期和失败处理。 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kvstore.h"
#include "kvs_allocator.h"

/* -1：正常；0：强制失败；正数：允许若干次申请后失败。 */
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

/* 查找真实节点，用于检查覆盖 value 时节点没有被替换。 */
static hashnode_t *find_node(kvs_hash_t *hash, const char *key)
{
    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];

        while (node != NULL) {
            if (strcmp(node->key, key) == 0) {
                return node;
            }
            node = node->next;
        }
    }

    return NULL;
}

static void expect_value(kvs_hash_t *hash,
                         char *key,
                         const char *expected)
{
    char *value = kvs_hash_get(hash, key);
    assert(value != NULL);
    assert(strcmp(value, expected) == 0);
}

/* 统计接口只适用于自研池。 */
static void expect_pool(kvs_allocator_backend_t backend,
                        size_t objects,
                        size_t large_objects)
{
    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t stats;
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == objects);
        assert(stats.large_count == large_objects);
    }
}

static void test_hash_backend(kvs_allocator_backend_t backend)
{
    assert(kvs_allocator_init(backend) == 0);

    kvs_hash_t hash = {0};

    /* 桶表申请失败后，应允许重新创建。 */
    fail_after = 0;
    assert(kvs_hash_create(&hash) < 0);
    fail_after = -1;

    assert(hash.nodes == NULL);
    assert(hash.count == 0);
    expect_pool(backend, 0, 0);

    assert(kvs_hash_create(&hash) == 0);
    assert(hash.nodes != NULL);
    assert(hash.count == 0);

    /* 当前64位环境中，1024个桶指针占8 KiB。 */
    expect_pool(backend, 1, 1);

    /*
     * 当前 Hash 使用字符求和。
     * 这三个 key 的字符和相同，会形成同一条桶链。
     */
    assert(kvs_hash_set(&hash, "abc", "one") == 0);
    assert(kvs_hash_set(&hash, "acb", "two") == 0);
    assert(kvs_hash_set(&hash, "bac", "three") == 0);
    assert(hash.count == 3);

    /* 一个桶表，每条记录各有节点、key、value。 */
    expect_pool(backend, 10, 1);

    expect_value(&hash, "abc", "one");
    expect_value(&hash, "acb", "two");
    expect_value(&hash, "bac", "three");

    hashnode_t *original = find_node(&hash, "abc");
    assert(original != NULL);

    /* 分别让新记录的节点、key、value 申请失败。 */
    for (long allowed = 0; allowed <= 2; allowed++) {
        fail_after = allowed;
        int result = kvs_hash_set(&hash, "new-key", "new-value");
        fail_after = -1;

        assert(result < 0);
        assert(hash.count == 3);
        assert(kvs_hash_get(&hash, "new-key") == NULL);
        expect_value(&hash, "abc", "one");

        /* 中途失败必须清理已经准备好的节点和字符串。 */
        expect_pool(backend, 10, 1);
    }

    /* SET 覆盖、MOD 失败时都应保留旧值。 */
    for (int operation = 0; operation < 2; operation++) {
        fail_after = 0;

        int result = operation == 0
            ? kvs_hash_set(&hash, "abc", "replacement")
            : kvs_hash_mod(&hash, "abc", "replacement");

        fail_after = -1;

        assert(result < 0);
        assert(hash.count == 3);
        assert(find_node(&hash, "abc") == original);
        expect_value(&hash, "abc", "one");
        expect_pool(backend, 10, 1);
    }

    /* 成功覆盖只替换 value，不替换节点。 */
    assert(kvs_hash_set(&hash, "abc", "updated") == 0);
    assert(find_node(&hash, "abc") == original);
    assert(hash.count == 3);
    expect_value(&hash, "abc", "updated");

    assert(kvs_hash_mod(&hash, "abc", "changed") == 0);
    expect_value(&hash, "abc", "changed");
    expect_pool(backend, 10, 1);

    /* MOD 不存在的 key，不应创建记录。 */
    assert(kvs_hash_mod(&hash, "missing", "value") == 1);
    assert(hash.count == 3);
    assert(kvs_hash_get(&hash, "missing") == NULL);

    /* 空字符串仍然是存在的 value。 */
    assert(kvs_hash_set(&hash, "abc", "") == 0);
    expect_value(&hash, "abc", "");

    /* 长 value 使用独立大对象区域。 */
    char long_value[8193];
    memset(long_value, 'L', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    assert(kvs_hash_set(&hash, "abc", long_value) == 0);
    expect_value(&hash, "abc", long_value);
    expect_pool(backend, 10, 2);

    /* 改回短 value 后，旧的大对象区域应已释放。 */
    assert(kvs_hash_mod(&hash, "abc", "short") == 0);
    expect_value(&hash, "abc", "short");
    expect_pool(backend, 10, 1);

    /* 插入顺序形成 bac → acb → abc，先删除中间节点。 */
    assert(kvs_hash_del(&hash, "acb") == 0);
    assert(hash.count == 2);
    assert(kvs_hash_get(&hash, "acb") == NULL);
    expect_value(&hash, "abc", "short");
    expect_value(&hash, "bac", "three");
    expect_pool(backend, 7, 1);

    /* 再删除头节点及最后一个节点。 */
    assert(kvs_hash_del(&hash, "bac") == 0);
    assert(hash.count == 1);
    expect_value(&hash, "abc", "short");
    expect_pool(backend, 4, 1);

    assert(kvs_hash_del(&hash, "abc") == 0);
    assert(hash.count == 0);
    expect_pool(backend, 1, 1);

    /* 当前 Hash 删除不存在的 key 返回负数。 */
    assert(kvs_hash_del(&hash, "missing") < 0);
    assert(hash.count == 0);

    /* 同规格重新插入，不应增加保留的 slab 和映射空间。 */
    kvs_pool_stats_t before;
    if (backend == KVS_ALLOCATOR_POOL) {
        assert(kvs_allocator_pool_stats(&before) == 0);
    }

    assert(kvs_hash_set(&hash, "name", "Jasper") == 0);
    expect_value(&hash, "name", "Jasper");
    expect_pool(backend, 4, 1);

    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t after;
        assert(kvs_allocator_pool_stats(&after) == 0);
        assert(after.slab_count == before.slab_count);
        assert(after.mapped_bytes == before.mapped_bytes);
    }

    /* 保留多条记录，让 Hash 销毁负责遍历回收。 */
    for (int i = 0; i < 128; i++) {
        char key[32];
        char value[32];

        snprintf(key, sizeof(key), "bulk_%d", i);
        snprintf(value, sizeof(value), "value_%d", i);

        assert(kvs_hash_set(&hash, key, value) == 0);
    }

    assert(hash.count == 129);

    for (int i = 0; i < 128; i++) {
        char key[32];
        char value[32];

        snprintf(key, sizeof(key), "bulk_%d", i);
        snprintf(value, sizeof(value), "value_%d", i);
        expect_value(&hash, key, value);
    }

    expect_pool(backend, 1 + 3 * (size_t)hash.count, 1);

    kvs_hash_destory(&hash);
    assert(hash.nodes == NULL);
    assert(hash.count == 0);
    assert(hash.max_slots == 0);
    expect_pool(backend, 0, 0);

    assert(kvs_allocator_destroy() == 0);

    printf("[PASS] Hash lifecycle and failures allocator=%s\n",
           backend == KVS_ALLOCATOR_SYSTEM ? "system" : "pool");
}

int main(void)
{
    test_hash_backend(KVS_ALLOCATOR_SYSTEM);
    test_hash_backend(KVS_ALLOCATOR_POOL);
    return 0;
}