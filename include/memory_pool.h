#ifndef KVS_MEMORY_POOL_H
#define KVS_MEMORY_POOL_H

#include<stddef.h>

typedef struct free_node{
    struct free_node *next;
}free_node_t;

typedef struct memory_chunk{
    void *memory;
    struct memory_chunk *next;
}memory_chunk_t;

typedef struct memory_pool{
    size_t block_size;//一个block多少个字节
    size_t blocks_per_chunk;//每次扩容创建多少个block

    free_node_t *free_list;//free_list的表头指针
    memory_chunk_t *chunk_list;//chunk_list的表头指针
}memory_pool_t;

int memory_pool_init(memory_pool_t *pool,size_t block_size,size_t blocks_per_chunk);
void *memory_pool_alloc(memory_pool_t *pool);//取出一个block 返回一个通用指针因为 释放函数里面不知道里面存的数据类型
void memory_pool_free(memory_pool_t *pool,void *ptr);//释放内存 把对应的 block放回链表
void memory_pool_destory(memory_pool_t *pool);//释放所有chunk
#endif