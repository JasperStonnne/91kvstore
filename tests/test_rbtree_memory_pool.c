/* 验证红黑树通过统一 allocator 管理节点、nil、key/value。 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kvs_allocator.h"
#include "kvstore.h"

/*
 * -1：不注入失败。
 *  0：下一次申请失败。
 *  N：允许 N 次申请成功，之后申请失败。
 */
static long fail_after = -1;

/* 独立测试通过这两个入口连接真实 allocator。 */
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

/* 只在 pool 模式检查内部存活对象数。 */
static void check_pool(
    kvs_allocator_backend_t backend,
    size_t live_objects,
    size_t large_count)
{
    if (backend != KVS_ALLOCATOR_POOL) {
        return;
    }

    kvs_pool_stats_t stats;
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == live_objects);
    assert(stats.large_count == large_count);
}

static void check_value(
    kvs_rbtree_t *tree,
    char *key,
    const char *expected)
{
    char *value = kvs_rbtree_get(tree, key);

    assert(value != NULL);
    assert(strcmp(value, expected) == 0);
}

static void test_backend(kvs_allocator_backend_t backend)
{
    kvs_rbtree_t tree = {0};
    fail_after = -1;

    assert(kvs_allocator_init(backend) == 0);

    /* 创建时 nil 申请失败，不应留下半初始化的树。 */
    fail_after = 0;
    assert(kvs_rbtree_create(&tree) < 0);
    fail_after = -1;

    assert(tree.nil == NULL);
    assert(tree.root == NULL);
    check_pool(backend, 0, 0);

    /* 空树只有一个动态对象：nil 哨兵。 */
    assert(kvs_rbtree_create(&tree) == 0);
    assert(tree.nil != NULL);
    assert(tree.root == tree.nil);
    check_pool(backend, 1, 0);

    /*
     * 新增记录需要三次申请：节点、key、value。
     * 分别让这三处失败，检查之前申请的对象是否被清理。
     */
    for (long allowed = 0; allowed < 3; allowed++) {
        fail_after = allowed;
        assert(kvs_rbtree_set(&tree, "failed", "value") < 0);
        fail_after = -1;

        assert(tree.root == tree.nil);
        assert(kvs_rbtree_get(&tree, "failed") == NULL);
        check_pool(backend, 1, 0);
    }

    assert(kvs_rbtree_set(&tree, "name", "Jasper") == 0);
    check_value(&tree, "name", "Jasper");

    /* nil + 普通节点 + key + value。 */
    check_pool(backend, 4, 0);

    rbtree_node *original_node = tree.root;

    /* SET 覆盖失败，旧 value 和节点必须保持有效。 */
    fail_after = 0;
    assert(kvs_rbtree_set(&tree, "name", "Another") < 0);
    fail_after = -1;

    assert(tree.root == original_node);
    check_value(&tree, "name", "Jasper");
    check_pool(backend, 4, 0);

    /* MOD 失败同样必须保留旧 value。 */
    fail_after = 0;
    assert(kvs_rbtree_mod(&tree, "name", "Stone") < 0);
    fail_after = -1;

    check_value(&tree, "name", "Jasper");
    check_pool(backend, 4, 0);

    /* 成功覆盖只替换 value，不增加节点。 */
    assert(kvs_rbtree_set(&tree, "name", "Another") == 0);
    assert(tree.root == original_node);
    check_value(&tree, "name", "Another");

    assert(kvs_rbtree_mod(&tree, "name", "Stone") == 0);
    check_value(&tree, "name", "Stone");
    check_pool(backend, 4, 0);

    /* MOD 不得创建不存在的 key。 */
    assert(kvs_rbtree_mod(&tree, "missing", "value") == 1);
    assert(kvs_rbtree_get(&tree, "missing") == NULL);
    check_pool(backend, 4, 0);

    /* 空字符串仍需要存放结尾的 '\0'。 */
    assert(kvs_rbtree_mod(&tree, "name", "") == 0);
    check_value(&tree, "name", "");
    check_pool(backend, 4, 0);

    /* 超过 4096 字节的 value 应走自研池大对象路径。 */
    char long_value[8193];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    assert(kvs_rbtree_mod(&tree, "name", long_value) == 0);
    check_value(&tree, "name", long_value);
    check_pool(backend, 4, 1);

    /* 大 value 改回小 value，旧的大对象必须被释放。 */
    assert(kvs_rbtree_mod(&tree, "name", "small") == 0);
    check_value(&tree, "name", "small");
    check_pool(backend, 4, 0);

    assert(kvs_rbtree_del(&tree, "name") == 0);
    assert(tree.root == tree.nil);
    assert(kvs_rbtree_get(&tree, "name") == NULL);
    check_pool(backend, 1, 0);

    assert(kvs_rbtree_del(&tree, "missing") == 1);
    check_pool(backend, 1, 0);

    /*
     * 构造多节点树，删除有两个孩子的根节点。
     * 验证删除过程中交换 key/value 后，剩余数据仍然正确。
     */
    char *keys[] = {"m", "g", "t", "d", "j", "p", "w"};
    char *values[] = {"M", "G", "T", "D", "J", "P", "W"};
    const size_t count = sizeof(keys) / sizeof(keys[0]);

    for (size_t i = 0; i < count; i++) {
        assert(kvs_rbtree_set(&tree, keys[i], values[i]) == 0);
    }
    check_pool(backend, 1 + 3 * count, 0);

    assert(tree.root->left != tree.nil);
    assert(tree.root->right != tree.nil);

    /* 删除会释放原 key，必须先复制到独立的栈空间。 */
    char deleted_key[32];
    snprintf(deleted_key, sizeof(deleted_key), "%s", tree.root->key);

    assert(kvs_rbtree_del(&tree, deleted_key) == 0);
    assert(kvs_rbtree_get(&tree, deleted_key) == NULL);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(keys[i], deleted_key) != 0) {
            check_value(&tree, keys[i], values[i]);
        }
    }
    check_pool(backend, 1 + 3 * (count - 1), 0);

    /* 故意保留剩余节点，由 destroy 释放整棵树及 nil。 */
    kvs_rbtree_destory(&tree);

    assert(tree.root == NULL);
    assert(tree.nil == NULL);
    check_pool(backend, 0, 0);

    assert(kvs_allocator_destroy() == 0);

    printf("[PASS] RBTree lifecycle and failures allocator=%s\n",
           backend == KVS_ALLOCATOR_SYSTEM ? "system" : "pool");
}

int main(void)
{
    test_backend(KVS_ALLOCATOR_SYSTEM);
    test_backend(KVS_ALLOCATOR_POOL);
    return 0;
}