#include "kvs_allocator.h"
#include<stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

/*
 * 小对象 slab 的总大小，包含管理信息、位图和 block 数据区。
 * 最大 block 规格为 4 KiB；较大的容器和 value 走独立大对象路径。
 */
#define KVS_SLAB_SIZE (64u * 1024u)

typedef struct kvs_free_block {
    struct kvs_free_block *next;
} kvs_free_block_t;

/* 区域开头的类型标记，供释放入口识别。 */
typedef enum {
    KVS_REGION_SLAB = 1,
    KVS_REGION_LARGE
} kvs_region_kind_t;

typedef struct kvs_slab {
    kvs_region_kind_t kind;  // 必须是第一个字段
    size_t block_size;
    size_t capacity;
    size_t used_count;

    kvs_free_block_t *free_list;
    unsigned char *bitmap;
    unsigned char *data;

    struct kvs_slab *next;
} kvs_slab_t;

/* 管理一段独立的大对象映射。 */
typedef struct kvs_large {
    kvs_region_kind_t kind;  // 必须是第一个字段
    size_t mapping_size;    // 整段映射长度，供 munmap 使用

    struct kvs_large *prev;
    struct kvs_large *next;
} kvs_large_t;

typedef struct kvs_size_class {
    size_t block_size;
    kvs_slab_t *slabs;
} kvs_size_class_t;

/* 每个规格池直接保存块大小；slabs 自动初始化为 NULL。 */
static kvs_size_class_t pools[] = {
    { .block_size = 16 },
    { .block_size = 32 },
    { .block_size = 48 },
    { .block_size = 64 },
    { .block_size = 96 },
    { .block_size = 128 },
    { .block_size = 192 },
    { .block_size = 256 },
    { .block_size = 384 },
    { .block_size = 512 },
    { .block_size = 768 },
    { .block_size = 1024 },
    { .block_size = 1536 },
    { .block_size = 2048 },
    { .block_size = 3072 },
    { .block_size = 4096 }
};

#define SIZE_CLASS_COUNT \
    (sizeof(pools) / sizeof(pools[0]))

static int allocator_initialized = 0;
/* 实现在文件后面，供公开的销毁函数调用。 */
static int pool_destroy(void);
static kvs_allocator_backend_t allocator_backend = KVS_ALLOCATOR_SYSTEM;
/* 内部函数在文件后面实现。 */
static void *pool_alloc_small(size_t size);
static int pool_free_small(void *ptr);
static void *pool_alloc_large(size_t size);
static int pool_free_large(kvs_large_t *large);
/* 记录当前仍在使用的大对象区域。 */
static kvs_large_t *large_regions = NULL;



int kvs_allocator_init(kvs_allocator_backend_t backend)
{
    if (allocator_initialized) {
        return -1;
    }

    /* 目前支持 SYSTEM 和小对象 POOL，其他模式明确拒绝。 */
    if (backend != KVS_ALLOCATOR_SYSTEM &&
        backend != KVS_ALLOCATOR_POOL) {
        return -1;
    }
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        pools[i].slabs = NULL;
    }
    allocator_backend = backend;
    allocator_initialized = 1;
    return 0;
}

int kvs_allocator_destroy(void)
{
    /* 尚未初始化或已经销毁，允许安全返回。 */
    if (!allocator_initialized) {
        return 0;
    }

    /* 大对象链表非空，说明还有业务对象没有释放。 */
    if (large_regions != NULL) {
        return -1;
    }

    /* 清理失败时保留初始化状态，调用方可以处理后重试。 */
    if (pool_destroy() != 0) {
        return -1;
    }

    allocator_initialized = 0;
    return 0;
}

void *kvs_allocator_alloc(size_t size)
{
    if (!allocator_initialized || size == 0) {
        return NULL;
    }

    if (allocator_backend == KVS_ALLOCATOR_POOL) {
    /* 超过最大规格的请求，独立分配大对象。 */
    if (size > pools[SIZE_CLASS_COUNT - 1].block_size) {
        return pool_alloc_large(size);
    }

    return pool_alloc_small(size);
}

    return malloc(size);
}

void kvs_allocator_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    /* 非空对象必须在 allocator 销毁之前释放。 */
    if (!allocator_initialized) {
        abort();
    }

if (allocator_backend == KVS_ALLOCATOR_POOL) {
    /* 合法返回指针位于区域的第一个64 KiB对齐单元内。 */
    uintptr_t start =
        (uintptr_t)ptr & ~((uintptr_t)KVS_SLAB_SIZE - 1);

    /* 两种管理结构的第一个字段都是 kind。 */
    kvs_region_kind_t kind = *(kvs_region_kind_t *)start;
    int result;

    if (kind == KVS_REGION_SLAB) {
        result = pool_free_small(ptr);
    } else if (kind == KVS_REGION_LARGE) {
        result = pool_free_large((kvs_large_t *)start);
    } else {
        abort();
    }

    if (result != 0) {
        abort();
    }

    return;
}

    free(ptr);
}

/* 找到能容纳请求的最小规格池；超出规格范围返回 NULL。 */
static kvs_size_class_t *find_size_class(size_t size)
{
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        if (size <= pools[i].block_size) {
            return &pools[i];
        }
    }

    return NULL;
}

/* 从已初始化的 slab 中取出一个空闲块。 */
static void *slab_alloc_block(kvs_slab_t *slab)
{
    if (slab == NULL || slab->free_list == NULL) {
        return NULL;
    }

    kvs_free_block_t *block = slab->free_list;

    /* 指针转为字节指针，计算该块在数据区中的编号。 */
    size_t index =
        ((unsigned char *)block - slab->data) / slab->block_size;

    /*
     * 先保存剩余空闲链表。
     * block 交给业务后，其内部的 next 会被业务数据覆盖。
     */
    slab->free_list = block->next;

    /* 每字节记录 8 个块；将当前块对应的状态位设为 1。 */
    slab->bitmap[index / 8] |=
        (unsigned char)(1u << (index % 8));

    slab->used_count++;

    return block;
}

/*
 * 申请 mapping_size 字节，起点按 64 KiB 对齐。
 * 长度必须是系统页大小的整数倍。
 */
static void *region_memory_alloc(size_t mapping_size)
{
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0 ||
        KVS_SLAB_SIZE % (size_t)page_size != 0 ||
        mapping_size == 0 ||
        mapping_size % (size_t)page_size != 0) {
        return NULL;
    }

    /* 多申请一段，给起点对齐留空间；先检查加法溢出。 */
    if (mapping_size > SIZE_MAX - KVS_SLAB_SIZE) {
        return NULL;
    }

    size_t reserved_size = mapping_size + KVS_SLAB_SIZE;

    unsigned char *raw = mmap(
        NULL, reserved_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (raw == MAP_FAILED) {
        return NULL;
    }

    /* 在申请到的区域内，向上找到最近的对齐起点。 */
    uintptr_t aligned =
        ((uintptr_t)raw + KVS_SLAB_SIZE - 1) &
        ~((uintptr_t)KVS_SLAB_SIZE - 1);

    size_t prefix = aligned - (uintptr_t)raw;
    size_t suffix = reserved_size - prefix - mapping_size;

    if (prefix != 0 && munmap(raw, prefix) != 0) {
        munmap(raw, reserved_size);
        return NULL;
    }

    /* 保留 mapping_size 字节，裁掉后面多余的部分。 */
    if (suffix != 0 &&
        munmap((void *)(aligned + mapping_size), suffix) != 0) {
        munmap((void *)aligned, reserved_size - prefix);
        return NULL;
    }

    return (void *)aligned;
}

/* 创建一个指定块大小的 slab；成功后交给调用方挂入规格池。 */
static kvs_slab_t *slab_create(size_t block_size)
{
    size_t alignment = _Alignof(max_align_t);

    /* 块既要容纳空闲链表指针，也要满足对象对齐要求。 */
    if (block_size < sizeof(kvs_free_block_t) ||
        block_size % alignment != 0 ||
        block_size > pools[SIZE_CLASS_COUNT - 1].block_size) {
        return NULL;
    }

    /* 先按块数上限预留位图，再计算实际数据区位置。 */
    size_t upper_count =
        (KVS_SLAB_SIZE - sizeof(kvs_slab_t)) / block_size;
    size_t bitmap_bytes =
        upper_count / 8 + (upper_count % 8 != 0);

    size_t offset = sizeof(kvs_slab_t) + bitmap_bytes;

    /* 数据区起点向上对齐。 */
    offset = ((offset + alignment - 1) / alignment) * alignment;

    if (offset >= KVS_SLAB_SIZE ||
        (KVS_SLAB_SIZE - offset) / block_size == 0) {
        return NULL;
    }

    kvs_slab_t *slab = region_memory_alloc(KVS_SLAB_SIZE);
    if (slab == NULL) {
        return NULL;
    }

    slab->kind = KVS_REGION_SLAB;
    slab->block_size = block_size;
    slab->capacity = (KVS_SLAB_SIZE - offset) / block_size;
    slab->used_count = 0;
    slab->free_list = NULL;
    slab->bitmap = (unsigned char *)slab + sizeof(*slab);
    slab->data = (unsigned char *)slab + offset;
    slab->next = NULL;

    /* 初始时，所有块都是空闲状态。 */
    memset(slab->bitmap, 0, bitmap_bytes);

    /* 把数据区切成等大的 block，并串成空闲链表。 */
    for (size_t i = 0; i < slab->capacity; i++) {
        kvs_free_block_t *block =
            (kvs_free_block_t *)(slab->data + i * block_size);

        block->next = slab->free_list;
        slab->free_list = block;
    }

    return slab;
}

/* 小对象分配路径；由上层保证 allocator 已初始化。 */
static void *pool_alloc_small(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    kvs_size_class_t *pool = find_size_class(size);

    /* 没有合适规格，由上层走大对象路径。 */
    if (pool == NULL) {
        return NULL;
    }
    kvs_slab_t *slab = pool->slabs;

    /* 跳过已经没有空闲块的 slab。 */
    while (slab != NULL && slab->free_list == NULL) {
        slab = slab->next;
    }

    if (slab == NULL) {
        slab = slab_create(pool->block_size);
        if (slab == NULL) {
            return NULL;
        }

        /* 创建完整后，才挂入这个规格池。 */
        slab->next = pool->slabs;
        pool->slabs = slab;
    }

    return slab_alloc_block(slab);
}

/* 成功返回 0；发现块位置或状态不正确时返回 -1。 */
static int pool_free_small(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    /* slab 按固定大小对齐，因此可以从对象地址找回开头。 */
    uintptr_t address = (uintptr_t)ptr;
    kvs_slab_t *slab = (kvs_slab_t *)(
        address & ~((uintptr_t)KVS_SLAB_SIZE - 1));

    uintptr_t data_address = (uintptr_t)slab->data;
    if (address < data_address) {
        return -1;
    }

    /* 必须指向某个 block 的起点，不能指向块内部。 */
    size_t offset = address - data_address;
    if (offset % slab->block_size != 0 ||
        offset / slab->block_size >= slab->capacity) {
        return -1;
    }

    size_t index = offset / slab->block_size;
    unsigned char mask = (unsigned char)(1u << (index % 8));

    /* 已经空闲的块不能再次归还，防止破坏链表和计数。 */
    if ((slab->bitmap[index / 8] & mask) == 0 ||
        slab->used_count == 0) {
        return -1;
    }

    slab->bitmap[index / 8] &= (unsigned char)~mask;

    /* 业务已放弃该块，现在可以借用其开头保存 next。 */
    kvs_free_block_t *block = ptr;
    block->next = slab->free_list;
    slab->free_list = block;
    slab->used_count--;

    return 0;
}

/* 成功返回 0；存在未释放对象或解除映射失败时返回 -1。 */
static int pool_destroy(void)
{
    /* 先检查全部 slab，避免销毁到一半才发现仍有对象存活。 */
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        kvs_slab_t *slab = pools[i].slabs;

        while (slab != NULL) {
            if (slab->used_count != 0) {
                return -1;
            }

            slab = slab->next;
        }
    }

    /* 所有 block 都已归还，可以释放各个 slab。 */
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        while (pools[i].slabs != NULL) {
            kvs_slab_t *slab = pools[i].slabs;

            /* 必须在 munmap 前保存 next，释放后不能再读 slab。 */
            kvs_slab_t *next = slab->next;

            if (munmap(slab, KVS_SLAB_SIZE) != 0) {
                return -1;
            }

            pools[i].slabs = next;
        }
    }

    return 0;
}

static void *pool_alloc_large(size_t size)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || size == 0) {
        return NULL;
    }

    /* 管理信息后面的业务空间也要满足对象对齐要求。 */
    size_t alignment = _Alignof(max_align_t);
    size_t offset =
        ((sizeof(kvs_large_t) + alignment - 1) / alignment) * alignment;

    /* 返回指针必须位于第一个64 KiB对齐单元内。 */
    if (offset >= KVS_SLAB_SIZE || size > SIZE_MAX - offset) {
        return NULL;
    }

    /* 总长度按系统页补齐，并检查加法溢出。 */
    size_t total = offset + size;
    size_t page_size = (size_t)page;
    size_t remainder = total % page_size;
    size_t padding = remainder == 0 ? 0 : page_size - remainder;

    if (total > SIZE_MAX - padding) {
        return NULL;
    }

    size_t mapping_size = total + padding;
    kvs_large_t *large = region_memory_alloc(mapping_size);
    if (large == NULL) {
        return NULL;
    }

    large->kind = KVS_REGION_LARGE;
    large->mapping_size = mapping_size;

    /* 区域准备完整后，头插到大对象双向链表。 */
    large->prev = NULL;
    large->next = large_regions;
    if (large_regions != NULL) {
        large_regions->prev = large;
    }
    large_regions = large;

    /* 业务只能使用管理信息后面的空间。 */
    return (unsigned char *)large + offset;
}

/* 从大对象链表摘除，并释放整段映射。 */
static int pool_free_large(kvs_large_t *large)
{
    /* 释放后不能再读管理信息，因此先保存这些字段。 */
    kvs_large_t *prev = large->prev;
    kvs_large_t *next = large->next;
    size_t mapping_size = large->mapping_size;

    /* 先摘链，同时处理头节点和尾节点。 */
    if (prev != NULL) {
        prev->next = next;
    } else {
        large_regions = next;
    }

    if (next != NULL) {
        next->prev = prev;
    }

    if (munmap(large, mapping_size) == 0) {
        return 0;
    }

    /* 解除映射失败时，恢复链表，保留对这段区域的管理。 */
    if (prev != NULL) {
        prev->next = large;
    } else {
        large_regions = large;
    }

    if (next != NULL) {
        next->prev = large;
    }

    return -1;
}

int kvs_allocator_pool_stats(kvs_pool_stats_t *stats)
{
    if (stats == NULL || !allocator_initialized ||
        allocator_backend != KVS_ALLOCATOR_POOL) {
        return -1;
    }

    kvs_pool_stats_t result = {0};

    /* 汇总小对象：对象释放后，空 slab 目前仍会保留。 */
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        kvs_slab_t *slab = pools[i].slabs;

        while (slab != NULL) {
            result.live_objects += slab->used_count;
            result.slab_count++;
            result.mapped_bytes += KVS_SLAB_SIZE;

            slab = slab->next;
        }
    }

    /* 大对象释放后立即解除映射，并从链表移除。 */
    kvs_large_t *large = large_regions;
    while (large != NULL) {
        result.live_objects++;
        result.large_count++;
        result.mapped_bytes += large->mapping_size;

        large = large->next;
    }

    *stats = result;
    return 0;
}