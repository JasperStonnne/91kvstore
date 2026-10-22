#ifndef KVS_ALLOCATOR_H
#define KVS_ALLOCATOR_H
#include <stddef.h>

/* 自研池统计，不包含 system 后端。 */
typedef struct kvs_pool_stats {
    size_t live_objects;   // 当前仍在使用的对象数
    size_t slab_count;     // 保留的 slab 数量
    size_t large_count;    // 仍存活的大对象数量
    size_t mapped_bytes;   // 当前保留的映射总字节数，不是 RSS
} kvs_pool_stats_t;


typedef enum {
    KVS_ALLOCATOR_SYSTEM = 0,
    KVS_ALLOCATOR_POOL
} kvs_allocator_backend_t;

/* 在创建引擎之前初始化；成功返回 0，失败返回 -1。 */
int kvs_allocator_init(kvs_allocator_backend_t backend);

/* 在所有引擎及其对象释放之后销毁。 */
int kvs_allocator_destroy(void);
void *kvs_allocator_alloc(size_t size);
void kvs_allocator_free(void *ptr);
/* 查询自研池；未初始化、非 POOL 模式或参数无效时返回 -1。 */
int kvs_allocator_pool_stats(kvs_pool_stats_t *stats);
/* 业务模块统一使用的内存申请、释放入口。 */
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
#endif
