#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "kvs_allocator.h"
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

    kvs_allocator_free(small);
    kvs_allocator_free(large);

    /* 对象已释放；大对象区域已归还，空 slab 仍保留。 */
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);
    assert(stats.slab_count == 1);
    assert(stats.large_count == 0);
    assert(stats.mapped_bytes > 0);
    size_t retained_bytes = stats.mapped_bytes;

    /* 同规格再次申请，不应增加保留的 slab 和映射空间。 */
    small = kvs_allocator_alloc(25);
    assert(small != NULL);

    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 1);
    assert(stats.slab_count == 1);
    assert(stats.mapped_bytes == retained_bytes);

    kvs_allocator_free(small);
    assert(kvs_allocator_destroy() == 0);

    puts("[PASS] allocator stats and reuse");
}

int main(void)
{

    /* 未知模式必须失败，且不影响后续正常初始化。 */
    assert(kvs_allocator_init(
        (kvs_allocator_backend_t)999) == -1);

    assert(kvs_allocator_init(KVS_ALLOCATOR_SYSTEM) == 0);

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
    puts("[PASS] allocator lifecycle");
    return 0;
}
