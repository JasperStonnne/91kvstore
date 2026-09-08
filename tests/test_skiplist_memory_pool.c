// 验证跳表创建和销毁时的基础内存生命周期

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "kvstore.h"

#ifndef KVS_SKIPLIST_USE_MEMORY_POOL
#define KVS_SKIPLIST_USE_MEMORY_POOL 1
#endif

// 独立测试不链接 src/kvstore.c，
// 因此在这里提供 kvs_malloc/kvs_free。
void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}

int main(void) {
    // 所有字段先初始化为 0
    kvs_skiplist_t list = {0};

    // 创建空跳表
    assert(kvs_skiplist_create(&list) == 0);

    // header 已创建，目前没有普通业务节点
    assert(list.header != NULL);
    assert(list.level == 0);
    assert(list.header->forward[0] == NULL);

#if KVS_SKIPLIST_USE_MEMORY_POOL
    // header 使用 malloc，因此此时 node_pool 仍未创建 Chunk
    assert(list.node_pool.chunk_list == NULL);
    assert(list.node_pool.free_list == NULL);
#endif
    // 插入第一个普通业务节点
    assert(kvs_skiplist_set(&list, "name", "Jasper") == 0);

// 验证业务数据能够正常查询
    char *value = kvs_skiplist_get(&list, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);

// 目前只有一个普通节点，它位于第 0 层的第一个位置
kvs_skiplist_node_t *first_node =
    list.header->forward[0];

    assert(first_node != NULL);
    assert(strcmp(first_node->key, "name") == 0);

#if KVS_SKIPLIST_USE_MEMORY_POOL
// 第一个普通节点触发 node_pool 创建 Chunk
    assert(list.node_pool.chunk_list != NULL);
    #endif
        // 删除刚才插入的普通节点
    assert(kvs_skiplist_del(&list, "name") == 0);

    // 删除后无法再查询到旧 key
    assert(kvs_skiplist_get(&list, "name") == NULL);

    // 当前没有普通节点，第 0 层重新变为空
    assert(list.header->forward[0] == NULL);

    #if KVS_SKIPLIST_USE_MEMORY_POOL
    // 普通节点外壳已经归还，并位于 Free List 表头
    assert(
        list.node_pool.free_list ==
        (free_node_t *)first_node
    );
    #endif
    // 插入一个不同的业务节点
    assert(
        kvs_skiplist_set(
            &list,
            "city",
            "Shanghai"
        ) == 0
    );

    // 它是当前第 0 层的第一个普通节点
    kvs_skiplist_node_t *second_node =
        list.header->forward[0];

    assert(second_node != NULL);

    #if KVS_SKIPLIST_USE_MEMORY_POOL
    // Free List 头插头取，应复用刚才删除的节点外壳
    assert(second_node == first_node);
    #endif

    // 验证复用节点后，新数据没有受到旧内容影响
    value = kvs_skiplist_get(&list, "city");
    assert(value != NULL);
    assert(strcmp(value, "Shanghai") == 0);

    printf("first node  = %p\n", (void *)first_node);

    #if KVS_SKIPLIST_USE_MEMORY_POOL
    printf("second node = %p (reused)\n", (void *)second_node);
    #else
    printf("second node = %p (malloc mode)\n", (void *)second_node);
    #endif
    // 插入多个节点，让随机层数和不同长度的 forward 数组参与测试
    char key_buffer[64];
    char value_buffer[64];

    for (int i = 0; i < 2000; i++) {
        snprintf(
            key_buffer,
            sizeof(key_buffer),
            "key-%04d",
            i
        );

        snprintf(
            value_buffer,
            sizeof(value_buffer),
            "value-%04d",
            i
        );

        assert(
            kvs_skiplist_set(
                &list,
                key_buffer,
                value_buffer
            ) == 0
        );
    }
    #if KVS_SKIPLIST_USE_MEMORY_POOL
    // 2001 个存活节点超过一个 Chunk 的 1024 个 Block，
    // 所以 chunk_list 中至少应该存在两个 Chunk。
    assert(list.node_pool.chunk_list != NULL);
    assert(list.node_pool.chunk_list->next != NULL);
    #endif 
   // 销毁包含多个普通节点的跳表
    kvs_skiplist_destory(&list);

    // 销毁后不再保留 header
    assert(list.header == NULL);
    assert(list.level == 0);

#if KVS_SKIPLIST_USE_MEMORY_POOL
    // 节点池的链表也应处于清空状态
    assert(list.node_pool.chunk_list == NULL);
    assert(list.node_pool.free_list == NULL);
#endif

    printf("skiplist allocator integration test passed\n");
    return 0;
}