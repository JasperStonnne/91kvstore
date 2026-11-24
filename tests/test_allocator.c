#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "kvs_allocator.h"
/* 仅本单测覆盖 munmap；按指定 slab 注入失败，其余调用交给内核。 */
static void *fail_unmap_address;
static unsigned int fail_unmap_count;
static unsigned int unmap_failures;

int munmap(void *address, size_t length)
{
    if (address == fail_unmap_address && fail_unmap_count > 0) {
        assert(length == 64u * 1024u);
        fail_unmap_count--;
        unmap_failures++;
        errno = ENOMEM;
        return -1;
    }
    return (int)syscall(SYS_munmap, address, length);
}

static uintptr_t slab_address(void *block)
{
    return (uintptr_t)block & ~((uintptr_t)(64u * 1024u) - 1);
}

/* 4096 规格下 45 个对象填满三个 slab，按申请顺序记录尾、中、头。 */
static void allocate_three_slabs(unsigned char **blocks, uintptr_t *regions)
{
    size_t count = 0;
    for (size_t i = 0; i < 45; i++) {
        blocks[i] = kvs_allocator_alloc(4096);
        assert(blocks[i] != NULL);
        blocks[i][0] = (unsigned char)i;
        blocks[i][4095] = (unsigned char)(i + 100);
        uintptr_t region = slab_address(blocks[i]);
        if (count == 0 || regions[count - 1] != region) {
            assert(count < 3);
            regions[count++] = region;
        }
    }
    assert(count == 3);
}

static void free_region(unsigned char **blocks, uintptr_t region)
{
    for (size_t i = 0; i < 45; i++) {
        if (blocks[i] != NULL && slab_address(blocks[i]) == region) {
            kvs_allocator_free(blocks[i]);
            blocks[i] = NULL;
        }
    }
}

static void check_survivors(unsigned char **blocks, size_t expected_slabs)
{
    size_t live = 0;
    for (size_t i = 0; i < 45; i++) {
        if (blocks[i] != NULL) {
            assert(blocks[i][0] == (unsigned char)i);
            assert(blocks[i][4095] == (unsigned char)(i + 100));
            live++;
        }
    }
    kvs_pool_stats_t stats;
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == live);
    assert(stats.slab_count == expected_slabs);
    assert(stats.large_count == 0);
    assert(stats.mapped_bytes == expected_slabs * 64u * 1024u);
}

static void test_empty_slab_reclaim(void)
{
    const size_t orders[][3] = {{1, 2, 0}, {0, 1, 2}, {2, 0, 1}};
    for (size_t round = 0; round < 3; round++) {
        assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);
        unsigned char *blocks[45];
        uintptr_t regions[3];
        allocate_three_slabs(blocks, regions);
        check_survivors(blocks, 3);
        for (size_t step = 0; step < 3; step++) {
            free_region(blocks, regions[orders[round][step]]);
            check_survivors(blocks, 2 - step);
        }
        /* 完全回收后重新申请、写入并释放，池仍能正常工作。 */
        unsigned char *again = kvs_allocator_alloc(4096);
        assert(again != NULL);
        again[4095] = 7;
        assert(again[4095] == 7);
        kvs_allocator_free(again);
        check_survivors(blocks, 0);
        assert(kvs_allocator_destroy() == 0);
    }
    puts("[PASS] allocator empty head/middle/tail slabs reclaimed; survivors and reallocation valid");
}

static void test_slab_unmap_failure(void)
{
    /* 每个链表位置都验证失败后恢复链接、随后复用及再次成功释放。 */
    for (size_t victim = 0; victim < 3; victim++) {
        assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);
        unsigned char *blocks[45];
        uintptr_t regions[3];
        allocate_three_slabs(blocks, regions);
        fail_unmap_address = (void *)regions[victim];
        fail_unmap_count = 1;
        unsigned int before = unmap_failures;
        free_region(blocks, regions[victim]);
        assert(unmap_failures == before + 1 && fail_unmap_count == 0);
        check_survivors(blocks, 3);
        assert(kvs_allocator_destroy() == -1); /* 其他两个 slab 仍有存活对象。 */
        unsigned char *reused = kvs_allocator_alloc(4096);
        assert(reused != NULL && slab_address(reused) == regions[victim]);
        reused[4095] = 9;
        assert(reused[4095] == 9);
        kvs_allocator_free(reused);
        check_survivors(blocks, 2);
        for (size_t other = 0; other < 3; other++) {
            if (other != victim) {
                free_region(blocks, regions[other]);
            }
        }
        check_survivors(blocks, 0);
        assert(kvs_allocator_destroy() == 0);
    }

    /* 销毁时已成功释放头节点、随后中间节点失败，也能保持可用。 */
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);
    unsigned char *blocks[45];
    uintptr_t regions[3];
    allocate_three_slabs(blocks, regions);
    for (size_t i = 0; i < 3; i++) {
        fail_unmap_address = (void *)regions[i];
        fail_unmap_count = 1;
        free_region(blocks, regions[i]);
        assert(fail_unmap_count == 0);
    }
    check_survivors(blocks, 3);
    fail_unmap_address = (void *)regions[1];
    fail_unmap_count = 1;
    assert(kvs_allocator_destroy() == -1);
    assert(fail_unmap_count == 0);
    check_survivors(blocks, 2);
    void *reused = kvs_allocator_alloc(4096);
    assert(reused != NULL && slab_address(reused) == regions[1]);
    kvs_allocator_free(reused);
    check_survivors(blocks, 1);
    assert(kvs_allocator_destroy() == 0);
    fail_unmap_address = NULL;
    puts("[PASS] allocator munmap failure restores head/middle/tail links and partial destroy is retryable");
}

static void test_pool_multiple_slabs(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    /*
     * 每块申请 4096 字节。
     * 一个 64 KiB slab 扣除管理区后不足以容纳 16 块，
     * 因此 40 个同时存活的对象必然需要多个 slab。
     */
    unsigned char *blocks[40];

    for (size_t i = 0; i < 40; i++) {
        blocks[i] = kvs_allocator_alloc(4096);
        assert(blocks[i] != NULL);

        blocks[i][0] = (unsigned char)i;
        blocks[i][4095] = (unsigned char)(i + 100);
    }

    /* 所有对象分配完后，再检查数据没有互相覆盖。 */
    for (size_t i = 0; i < 40; i++) {
        assert(blocks[i][0] == (unsigned char)i);
        assert(blocks[i][4095] == (unsigned char)(i + 100));
    }

    /* 交错释放，检查释放路径不依赖申请顺序。 */
    for (size_t i = 0; i < 40; i += 2) {
        kvs_allocator_free(blocks[i]);
    }

    for (size_t i = 1; i < 40; i += 2) {
        assert(blocks[i][0] == (unsigned char)i);
        assert(blocks[i][4095] == (unsigned char)(i + 100));
        kvs_allocator_free(blocks[i]);
    }

    assert(kvs_allocator_destroy() == 0);
    puts("[PASS] allocator multiple slabs");
}

static void test_pool_small_objects(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    unsigned char *a = kvs_allocator_alloc(25);
    unsigned char *b = kvs_allocator_alloc(25);
    assert(a != NULL && b != NULL && a != b);

    /* 两个存活对象应拥有互不重叠的空间。 */
    a[0] = 11;
    a[24] = 22;
    b[0] = 33;
    b[24] = 44;
    assert(a[0] == 11 && a[24] == 22);

    /* 还有对象存活，销毁必须失败，并保留可继续释放的状态。 */
    assert(kvs_allocator_destroy() == -1);

    kvs_allocator_free(a);

    /* 释放 a 不能影响仍在使用的 b。 */
    assert(b[0] == 33 && b[24] == 44);
    kvs_allocator_free(b);

    assert(kvs_allocator_destroy() == 0);
    puts("[PASS] allocator pool small objects");
}

static void test_pool_large_object(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    size_t size = 100000;
    unsigned char *ptr = kvs_allocator_alloc(size);
    assert(ptr != NULL);

    /* 写满整个请求范围，检查大对象空间确实可用。 */
    for (size_t i = 0; i < size; i++) {
        ptr[i] = (unsigned char)(i % 251);
    }

    /* 大对象仍存活，不能销毁 allocator。 */
    assert(kvs_allocator_destroy() == -1);

    for (size_t i = 0; i < size; i++) {
        assert(ptr[i] == (unsigned char)(i % 251));
    }

    kvs_allocator_free(ptr);
    assert(kvs_allocator_destroy() == 0);

    puts("[PASS] allocator large object");
}

static void test_pool_mixed_objects(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    unsigned char *small = kvs_allocator_alloc(25);
    unsigned char *a = kvs_allocator_alloc(100000);
    unsigned char *b = kvs_allocator_alloc(200000);
    unsigned char *c = kvs_allocator_alloc(300000);
    assert(small != NULL && a != NULL && b != NULL && c != NULL);

    small[24] = 7;
    a[0] = 11;
    a[99999] = 12;
    b[0] = 21;
    b[199999] = 22;
    c[0] = 31;
    c[299999] = 32;

    /* 大对象链表当前为 C → B → A，先释放中间节点 B。 */
    kvs_allocator_free(b);
    assert(a[0] == 11 && a[99999] == 12);
    assert(c[0] == 31 && c[299999] == 32);

    /* 再释放头节点 C，剩下的 A 必须仍然有效。 */
    kvs_allocator_free(c);
    assert(a[0] == 11 && a[99999] == 12);

    /* 释放最后一个大对象，小对象仍应保持有效。 */
    kvs_allocator_free(a);
    assert(small[24] == 7);
    assert(kvs_allocator_destroy() == -1);

    kvs_allocator_free(small);
    assert(kvs_allocator_destroy() == 0);

    puts("[PASS] allocator mixed objects");
}

static void test_pool_boundaries(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    const size_t sizes[] = {
        1, 16, 17, 32, 33, 48, 49, 64, 65,
        96, 97, 128, 129, 192, 193, 256, 257,
        384, 385, 512, 513, 768, 769, 1024, 1025,
        1536, 1537, 2048, 2049, 3072, 3073,
        4096, 4097, 65536, 65537
    };

    size_t count = sizeof(sizes) / sizeof(sizes[0]);
    unsigned char *ptrs[sizeof(sizes) / sizeof(sizes[0])];

    for (size_t i = 0; i < count; i++) {
        ptrs[i] = kvs_allocator_alloc(sizes[i]);
        assert(ptrs[i] != NULL);

        /* 返回地址必须满足普通 C 对象的对齐要求。 */
        assert((uintptr_t)ptrs[i] % _Alignof(max_align_t) == 0);

        for (size_t j = 0; j < sizes[i]; j++) {
            ptrs[i][j] = (unsigned char)(i + 1);
        }
    }

    assert(kvs_allocator_alloc(0) == NULL);
    assert(kvs_allocator_alloc(SIZE_MAX) == NULL);

    /* 失败的申请不能破坏此前已分配的对象。 */
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < sizes[i]; j++) {
            assert(ptrs[i][j] == (unsigned char)(i + 1));
        }
        kvs_allocator_free(ptrs[i]);
    }

    assert(kvs_allocator_destroy() == 0);
    puts("[PASS] allocator boundaries");
}

static void test_pool_stats(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    kvs_pool_stats_t stats;
    kvs_pool_detail_stats_t details;
    assert(kvs_allocator_pool_details(NULL) == -1);
    assert(kvs_allocator_pool_details(&details) == 0);
    assert(details.class_count == 16 && details.mapped_bytes_total == 0);

    /* 刚初始化，还没有申请任何区域。 */
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);
    assert(stats.slab_count == 0);
    assert(stats.large_count == 0);
    assert(stats.mapped_bytes == 0);

    void *small = kvs_allocator_alloc(25);
    void *large = kvs_allocator_alloc(100000);
    assert(small != NULL && large != NULL);

    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 2);
    assert(stats.slab_count == 1);
    assert(stats.large_count == 1);
    assert(kvs_allocator_pool_details(&details) == 0);
    assert(details.classes[1].block_size == 32);
    assert(details.classes[1].slab_count == 1 && details.classes[1].used_blocks == 1);
    assert(details.classes[1].capacity_blocks > 1);
    assert(details.classes[1].mapped_bytes == 65536);
    assert(details.classes[1].slabs_created == 1);
    assert(details.large_count == 1 && details.large_mapped_bytes >= 100000);
    assert(details.mapped_bytes == stats.mapped_bytes);
    assert(details.mapped_bytes_total == details.mapped_bytes);
    size_t historical = details.mapped_bytes_total;

    kvs_allocator_free(small);
    kvs_allocator_free(large);

    /* 对象已释放，小对象 slab 和大对象映射都已归还。 */
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);
    assert(stats.slab_count == 0);
    assert(stats.large_count == 0);
    assert(stats.mapped_bytes == 0);

    assert(kvs_allocator_pool_details(&details) == 0);
    assert(details.mapped_bytes == 0 && details.mapped_bytes_total == historical);
    assert(details.classes[1].slab_count == 0 && details.classes[1].slabs_created == 1);

    /* 同规格再次申请会重新创建 slab，仍可正常读写和释放。 */
    small = kvs_allocator_alloc(25);
    assert(small != NULL);

    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 1);
    assert(stats.slab_count == 1);
    assert(stats.mapped_bytes == 64u * 1024u);

    assert(kvs_allocator_pool_details(&details) == 0);
    assert(details.classes[1].slabs_created == 2);
    assert(details.mapped_bytes_total == historical + 65536);
    kvs_allocator_free(small);
    assert(kvs_allocator_destroy() == 0);
    assert(kvs_allocator_pool_details(&details) == -1);

    puts("[PASS] allocator stats and reuse");
}

int main(void)
{

    /* 未知模式必须失败，且不影响后续正常初始化。 */
    assert(kvs_allocator_init(
        (kvs_allocator_backend_t)999) == -1);

    assert(kvs_allocator_init(KVS_ALLOCATOR_SYSTEM) == 0);
    kvs_pool_detail_stats_t unavailable;
    assert(kvs_allocator_pool_details(&unavailable) == -1);


    /* 使用过程中不能重复初始化。 */
    assert(kvs_allocator_init(KVS_ALLOCATOR_SYSTEM) == -1);

    assert(kvs_allocator_alloc(0) == NULL);

    unsigned char *ptr = kvs_allocator_alloc(32);
    assert(ptr != NULL);

    ptr[0] = 10;
    ptr[31] = 20;
    assert(ptr[0] == 10 && ptr[31] == 20);

    kvs_allocator_free(ptr);
    kvs_allocator_free(NULL);

    assert(kvs_allocator_destroy() == 0);

    /* 重复销毁允许安全返回。 */
    assert(kvs_allocator_destroy() == 0);

    /* 销毁之后，可以重新初始化。 */
    assert(kvs_allocator_init(KVS_ALLOCATOR_SYSTEM) == 0);
    assert(kvs_allocator_destroy() == 0);
    test_pool_small_objects();
    test_pool_multiple_slabs();
    test_pool_large_object();
    test_pool_mixed_objects();
    test_pool_boundaries();
    test_pool_stats();
    test_empty_slab_reclaim();
    test_slab_unmap_failure();
    puts("[PASS] allocator lifecycle");
    return 0;
}
