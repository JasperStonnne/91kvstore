#include"memory_pool.h"
#include<stdint.h>
#include<stdlib.h>
int memory_pool_init(memory_pool_t *pool,size_t block_size,size_t blocks_per_chunk){
    if(pool==NULL||block_size==0||blocks_per_chunk==0){
        return -1;
    }
    if(block_size<sizeof(free_node_t)){
        block_size=sizeof(free_node_t);
    }
    size_t alignment=sizeof(void*);
    size_t reminder=block_size%alignment;
    if(reminder!=0){
        block_size+=alignment-reminder;
    }
    pool->block_size=block_size;
    pool->blocks_per_chunk=blocks_per_chunk;
    pool->free_list=NULL;
    pool->chunk_list = NULL;
    return 0;
}
static int memory_pool_grow(memory_pool_t *pool){
    if(pool->blocks_per_chunk>SIZE_MAX/pool->block_size){//准备申请的 Chunk 大小会不会发生整数溢出
        return -1;
    }
    size_t chunk_size=pool->blocks_per_chunk*pool->block_size;

    memory_chunk_t *chunk=malloc(sizeof(*chunk));
    if(chunk==NULL){
        return -1;
    }
    chunk->memory=malloc(chunk_size);
    if(chunk->memory==NULL){
        free(chunk);
        return -1;
    }
    chunk->next = pool->chunk_list;
    pool->chunk_list = chunk;
    unsigned char *start=(unsigned char *)chunk->memory;// 将 Chunk 的通用地址转换成字节指针，方便按字节计算每个 Block 的地址
    for(size_t i=0;i<pool->blocks_per_chunk;i++){
        free_node_t *node =(free_node_t *)(start+i*pool->block_size);
        node->next=pool->free_list;
        pool->free_list=node;//头插法 还需多多熟悉
    }
    return 0;
}
void *memory_pool_alloc(memory_pool_t *pool){
    if(pool==NULL){
        return NULL;
    }
    if(pool->free_list==NULL){
        if(memory_pool_grow(pool)!=0){
            return NULL;
        }

    }
    free_node_t *node =pool->free_list;
    pool->free_list=node->next;
    return (void *) node;
}
void memory_pool_free(memory_pool_t *pool,void *ptr){
    if(pool==NULL||ptr==NULL){
        return;
    }
    free_node_t *node=(free_node_t *)ptr;
    node->next=pool->free_list;
    pool->free_list=node;
}
void memory_pool_destory(memory_pool_t *pool){
    if (pool==NULL){
        return;
    }
    memory_chunk_t *chunk=pool->chunk_list;
    while(chunk!=NULL){
        memory_chunk_t *next=chunk->next;
        free(chunk->memory);
        free(chunk);
        chunk=next;
    }
    pool->block_size = 0;                       // 清空旧配置
    pool->blocks_per_chunk = 0;
    pool->free_list = NULL;                     // block 已全部失效
    pool->chunk_list = NULL;                    // chunk 链表已经清空
}