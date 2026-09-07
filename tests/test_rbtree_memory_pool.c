// 验证红黑树接入内存池后的节点申请、归还、复用和销毁

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvstore.h"

// 独立测试不链接 src/kvstore.c，
// 因此在测试中提供 kvs_malloc/kvs_free。
void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}
int main(void) {
    // 创建一个初始全为 0 的红黑树实例
    kvs_rbtree_t tree = {0};

    // create 应该初始化 nil 哨兵和 node_pool
    assert(kvs_rbtree_create(&tree) == 0);

    // nil 是一个真实存在的哨兵节点
    assert(tree.nil != NULL);

    // 刚创建的树没有普通节点，所以 root 指向 nil
    assert(tree.root == tree.nil);

    // 节点池使用懒扩容；尚未 SET，所以还没有 Chunk
    assert(tree.node_pool.chunk_list == NULL);
    assert(tree.node_pool.free_list == NULL);

    // 插入第一个普通红黑树节点
    assert(kvs_rbtree_set(&tree, "name", "Jasper") == 0);

    // 插入后，根节点不再是 nil
    assert(tree.root != tree.nil);

    // 第一次申请节点会触发 node_pool 创建 Chunk
    assert(tree.node_pool.chunk_list != NULL);

    // 验证业务数据正常
    char *value = kvs_rbtree_get(&tree, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);

    // 当前树只有一个普通节点，所以 root 就是刚申请的节点
    rbtree_node *first_node = tree.root;
    assert(first_node != NULL);
      // 删除唯一的普通节点
    // 重复插入相同 key，应当被拒绝
    assert(kvs_rbtree_set(&tree, "name", "Another") == 1);

    // 根节点没有被替换，也没有创建第二个节点
    assert(tree.root == first_node);

    // 原来的 value 不能被修改
    value = kvs_rbtree_get(&tree, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);
    assert(kvs_rbtree_del(&tree, "name") == 0);

    // 节点删除后，树重新变成空树
    assert(tree.root == tree.nil);

    // 被删除的节点 Block 应该位于 free_list 表头
    assert(
        tree.node_pool.free_list ==
        (free_node_t *)first_node
    );

    // 插入一个不同的业务节点
    assert(kvs_rbtree_set(&tree, "city", "Shanghai") == 0);

    // 当前又只有一个普通节点，所以它就是 root
    rbtree_node *second_node = tree.root;
    assert(second_node != tree.nil);

    // Free List 采用头插、头取，应该复用刚才归还的 Block
    assert(second_node == first_node);

    // 验证复用 Block 后，新 key/value 没有受到旧数据影响
    value = kvs_rbtree_get(&tree, "city");
    assert(value != NULL);
    assert(strcmp(value, "Shanghai") == 0);

    printf("first node  = %p\n", (void *)first_node);
    printf("second node = %p (reused)\n", (void *)second_node);
        // 再插入多个节点，构造不止一个节点的红黑树
    assert(kvs_rbtree_set(&tree, "alpha", "1") == 0);
    assert(kvs_rbtree_set(&tree, "echo", "5") == 0);
    assert(kvs_rbtree_set(&tree, "bravo", "2") == 0);
    assert(kvs_rbtree_set(&tree, "delta", "4") == 0);

    // 销毁红黑树：
    // 释放所有 key/value、普通节点、nil 和内存池 Chunk
    kvs_rbtree_destory(&tree);

    // 销毁后不应保留失效的树指针
    assert(tree.root == NULL);
    assert(tree.nil == NULL);

    // 内存池持有的 Chunk 和空闲链表都应该被清空
    assert(tree.node_pool.chunk_list == NULL);
    assert(tree.node_pool.free_list == NULL);

    printf("rbtree memory pool integration test passed\n");

    return 0;
}