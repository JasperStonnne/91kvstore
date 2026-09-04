#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvstore.h"

#ifndef KVS_HASH_USE_MEMORY_POOL
#define KVS_HASH_USE_MEMORY_POOL 1
#endif
/*
 * 独立测试不链接 src/kvstore.c，
 * 因此在这里提供 kvs_malloc/kvs_free 的简单实现。
 */
void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}

/*
 * 遍历所有 Hash 桶，找到指定 key 对应的节点。
 * 测试需要取得节点地址，以验证节点 Block 是否被复用。
 */
static hashnode_t *find_node(kvs_hash_t *hash,
                             const char *key) {
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

int main(void) {
    kvs_hash_t hash = {0};

    /*
     * 第一部分：创建空 Hash。
     */
    assert(kvs_hash_create(&hash) == 0);
    assert(hash.nodes != NULL);
    assert(hash.count == 0);

    /*
     * 内存池采用懒加载。
     * 还没有插入节点，因此尚未创建 Chunk。
     */
#if KVS_HASH_USE_MEMORY_POOL
    assert(hash.node_pool.chunk_list == NULL);
    assert(hash.node_pool.free_list == NULL);
#endif

    /*
     * 第二部分：插入第一个节点。
     * 第一次申请 hashnode_t 会触发 Chunk 扩容。
     */
    assert(kvs_hash_set(&hash, "name", "Jasper") == 0);
    assert(hash.count == 1);
#if KVS_HASH_USE_MEMORY_POOL
    assert(hash.node_pool.chunk_list != NULL);
#endif
    /*
     * 验证 key/value 能够正常查询。
     */
    char *value = kvs_hash_get(&hash, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);

    /*
     * 保存第一个节点的地址。
     */
    hashnode_t *first_node = find_node(&hash, "name");
    assert(first_node != NULL);

    /*
     * 第三部分：删除节点。
     * key/value 被释放，节点 Block 被归还 node_pool。
     */
    assert(kvs_hash_del(&hash, "name") == 0);
    assert(hash.count == 0);
    assert(kvs_hash_get(&hash, "name") == NULL);

    /*
     * 第四部分：重新插入节点。
     * 内存池应该优先复用刚才归还的 Block。
     */
    assert(kvs_hash_set(&hash, "city", "Shanghai") == 0);
    assert(hash.count == 1);

    hashnode_t *second_node = find_node(&hash, "city");
    assert(second_node != NULL);

    /*
     * 两次业务节点不同，但底层 hashnode_t Block 地址相同，
     * 证明删除的节点已经被内存池复用。
     */
    printf("first node  = %p\n", (void *)first_node);
    printf("second node = %p\n", (void *)second_node);

#if KVS_HASH_USE_MEMORY_POOL
    assert(second_node == first_node);
    printf("memory pool reused the released node block\n");
#else
    printf("malloc mode does not require address reuse\n");
#endif
    /*
     * 第五部分：销毁 Hash。
     * 销毁剩余 key/value、桶数组以及 node_pool 的所有 Chunk。
     */
    kvs_hash_destory(&hash);
    assert(hash.nodes == NULL);
    assert(hash.count == 0);

#if KVS_HASH_USE_MEMORY_POOL
    assert(hash.node_pool.chunk_list == NULL);
    assert(hash.node_pool.free_list == NULL);
#endif

    printf("hash memory pool integration test passed\n");

    return 0;
}