#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "kvs_allocator.h"

/* 测试通过统一入口模拟申请失败。 */
static int fail_alloc = 0;

void *kvs_malloc(size_t size)
{
    if (fail_alloc) {
        return NULL;
    }
    return kvs_allocator_alloc(size);
}

void kvs_free(void *ptr)
{
    kvs_allocator_free(ptr);
}

static void test_input(void)
{
    kvs_input_buffer_t input = {0};

    /* 首次申请失败，不应留下半初始化状态。 */
    fail_alloc = 1;
    assert(kvs_input_buffer_ensure_space(&input, 4) < 0);
    fail_alloc = 0;

    assert(input.data == NULL);
    assert(input.length == 0);
    assert(input.capacity == 0);

    assert(kvs_input_buffer_ensure_space(&input, 4) == 0);
    memcpy(input.data, "data", 4);
    input.length = 4;

    char *old_data = input.data;
    int old_capacity = input.capacity;

    /* 请求的剩余空间超过当前可用空间，强制触发扩容。 */
    fail_alloc = 1;
    assert(kvs_input_buffer_ensure_space(
               &input, old_capacity) < 0);
    fail_alloc = 0;

    assert(input.data == old_data);
    assert(input.capacity == old_capacity);
    assert(input.length == 4);
    assert(memcmp(input.data, "data", 4) == 0);

    /* 恢复申请后扩容成功，有效数据保持完整。 */
    assert(kvs_input_buffer_ensure_space(
               &input, old_capacity) == 0);

    assert(input.capacity > old_capacity);
    assert(input.capacity - input.length >= old_capacity);
    assert(input.length == 4);
    assert(memcmp(input.data, "data", 4) == 0);

    /* 空间足够时，不应再次申请。 */
    fail_alloc = 1;
    assert(kvs_input_buffer_ensure_space(&input, 1) == 0);
    fail_alloc = 0;

    /* 容量计算溢出必须拒绝，旧数据不变。 */
    assert(kvs_input_buffer_ensure_space(&input, INT_MAX) < 0);
    assert(input.length == 4);
    assert(memcmp(input.data, "data", 4) == 0);

    /* 消费前两个字节后，剩余数据移到开头。 */
    assert(kvs_input_buffer_consume(&input, 2) == 0);
    assert(input.length == 2);
    assert(memcmp(input.data, "ta", 2) == 0);

    kvs_input_buffer_free(&input);
    assert(input.data == NULL);
    assert(input.length == 0);
    assert(input.capacity == 0);

    /* 清空后的重复释放应安全返回。 */
    kvs_input_buffer_free(&input);
}

static void test_output(void)
{
    kvs_output_buffer_t output = {0};

    fail_alloc = 1;
    assert(kvs_output_buffer_ensure_space(&output, 4) < 0);
    fail_alloc = 0;

    assert(output.data == NULL);
    assert(output.length == 0);
    assert(output.offset == 0);
    assert(output.capacity == 0);

    assert(kvs_output_buffer_ensure_space(&output, 4) == 0);
    memcpy(output.data, "data", 4);
    output.length = 4;
    output.offset = 2; /* 模拟已经发送前两个字节。 */

    char *old_data = output.data;
    int old_capacity = output.capacity;

    fail_alloc = 1;
    assert(kvs_output_buffer_ensure_space(
               &output, old_capacity) < 0);
    fail_alloc = 0;

    assert(output.data == old_data);
    assert(output.capacity == old_capacity);
    assert(output.length == 4);
    assert(output.offset == 2);
    assert(memcmp(output.data, "data", 4) == 0);

    assert(kvs_output_buffer_ensure_space(
               &output, old_capacity) == 0);

    assert(output.capacity > old_capacity);
    assert(output.capacity - output.length >= old_capacity);
    assert(output.length == 4);
    assert(output.offset == 2);
    assert(memcmp(output.data, "data", 4) == 0);
    assert(memcmp(output.data + output.offset, "ta", 2) == 0);

    /* 非法发送位置必须拒绝。 */
    output.offset = output.length + 1;
    assert(kvs_output_buffer_ensure_space(&output, 1) < 0);
    output.offset = 2;

    kvs_output_buffer_free(&output);
    assert(output.data == NULL);
    assert(output.length == 0);
    assert(output.offset == 0);
    assert(output.capacity == 0);

    kvs_output_buffer_free(&output);
}

static void test_backend(kvs_allocator_backend_t backend)
{
    assert(kvs_allocator_init(backend) == 0);

    test_input();
    test_output();

    if (backend == KVS_ALLOCATOR_POOL) {
        kvs_pool_stats_t stats;
        assert(kvs_allocator_pool_stats(&stats) == 0);
        assert(stats.live_objects == 0);
        assert(stats.large_count == 0);
    }

    assert(kvs_allocator_destroy() == 0);

    printf("[PASS] Buffer allocation and failures allocator=%s\n",
           backend == KVS_ALLOCATOR_SYSTEM ? "system" : "pool");
}

int main(void)
{
    test_backend(KVS_ALLOCATOR_SYSTEM);
    test_backend(KVS_ALLOCATOR_POOL);
    return 0;
}