#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kvs_allocator.h"
#include "persistence.h"

/* 仅用于模拟 AOF 编码缓冲申请失败。 */
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

/* 两条完整的 AOF 命令：HSET k 1、HSET k 2。 */
#define FIRST_COMMAND  "*3\r\n$4\r\nHSET\r\n$1\r\nk\r\n$1\r\n1\r\n"
#define SECOND_COMMAND "*3\r\n$4\r\nHSET\r\n$1\r\nk\r\n$1\r\n2\r\n"

static const char valid_log[] = FIRST_COMMAND SECOND_COMMAND;
static const char invalid_log[] = FIRST_COMMAND "broken";

/* 记录回调收到的 value，用来检查执行顺序。 */
static char seen_values[3];
static size_t seen_count;
static int reject_command;

/* 模拟恢复执行：检查字段，并记录收到的 value。 */
static int record_command(
    const kvs_slice_t *fields,
    size_t field_count,
    char *response,
    int response_capacity)
{
    assert(field_count == 3);
    assert(fields[0].length == 4);
    assert(memcmp(fields[0].data, "HSET", 4) == 0);
    assert(fields[1].length == 1);
    assert(fields[1].data[0] == 'k');
    assert(fields[2].length == 1);
    assert(seen_count < sizeof(seen_values));

    /* 复制一个字符，不在回调结束后保存映射内的指针。 */
    seen_values[seen_count++] = fields[2].data[0];

    if (reject_command) {
        return -1;
    }

    assert(response_capacity >= 5);
    memcpy(response, "+OK\r\n", 5);
    return 5;
}

/* 每个测试都重新准备文件内容。 */
static void write_fixture(
    const char *path,
    const char *data,
    size_t length)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, length, file) == length);
    assert(fclose(file) == 0);
}

/* 检查返回结果，以及实际回放了哪些命令。 */
static void expect_replay(
    const char *path,
    long long offset,
    int expected_result,
    const char *expected_values)
{
    seen_count = 0;
    memset(seen_values, 0, sizeof(seen_values));

    assert(kvs_aof_replay(path, offset, record_command)
           == expected_result);

    size_t expected_count = strlen(expected_values);
    assert(seen_count == expected_count);
    assert(memcmp(seen_values, expected_values, expected_count) == 0);
}

static void test_append(void)
{
    /* 独立临时文件，不使用服务已有的 AOF。 */
    char path[] = "/tmp/91kv-aof-append-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    assert(kvs_aof_open(path) == 0);

    char *first[] = {"HSET", "k", "1"};
    char *failed[] = {"HSET", "k", "9"};
    char *second[] = {"HSET", "k", "2"};

    kvs_pool_stats_t stats;

    /* 写请求结束后，encoded 应当已经释放。 */
    assert(kvs_aof_append(first, 3) == 0);
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);

    /* 申请失败不应追加命令，也不应推进写入位置。 */
    fail_alloc = 1;
    int result = kvs_aof_append(failed, 3);
    fail_alloc = 0;

    assert(result < 0);
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);

    /* 失败之后仍能正常追加下一条命令。 */
    assert(kvs_aof_append(second, 3) == 0);
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);

    assert(kvs_aof_close() == 0);

    /* 确认实际文件只有两条成功命令，没有失败的 value=9。 */
    expect_replay(path, 0, 0, "12");

    assert(unlink(path) == 0);
    puts("[PASS] AOF append allocation failure and cleanup");
}

int main(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    test_append();

    /* 后面原有的回放测试保持原样。 */
    char path[] = "/tmp/91kv-aof-replay-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    size_t first_length = sizeof(FIRST_COMMAND) - 1;
    size_t total_length = sizeof(valid_log) - 1;

    write_fixture(path, valid_log, total_length);

    /* 从头回放：两条都执行，顺序为 1、2。 */
    expect_replay(path, 0, 0, "12");

    /* 跳过第一条：只执行第二条。 */
    expect_replay(path, (long long)first_length, 0, "2");

    /* 末尾及末尾之后：保持原有的成功空回放行为。 */
    expect_replay(path, (long long)total_length, 0, "");
    expect_replay(path, (long long)total_length + 100, 0, "");

    /* offset 落在第二条命令内部：不是合法命令开头。 */
    expect_replay(path, (long long)first_length + 1, -1, "");

    /* 负 offset 无效。 */
    expect_replay(path, -1, -1, "");
    puts("[PASS] AOF replay offsets and command order");

    /* 删除最后一个字节：第一条成功，第二条不完整。 */
    write_fixture(path, valid_log, total_length - 1);
    expect_replay(path, 0, -1, "1");

    /* 第一条后面出现错误格式，也必须返回失败。 */
    write_fixture(path, invalid_log, sizeof(invalid_log) - 1);
    expect_replay(path, 0, -1, "1");

    /* 回调拒绝第一条时，应停止，不再执行第二条。 */
    write_fixture(path, valid_log, total_length);
    reject_command = 1;
    expect_replay(path, 0, -1, "1");
    reject_command = 0;
    puts("[PASS] AOF rejects truncated, malformed and failed replay");

    /* 空文件与不存在的文件，都没有命令需要恢复。 */
    write_fixture(path, "", 0);
    expect_replay(path, 0, 0, "");

    assert(unlink(path) == 0);
    expect_replay(path, 0, 0, "");
    puts("[PASS] AOF empty and missing files");

    puts("AOF replay tests passed");
    /* 所有测试对象释放后，才销毁 allocator。 */
    kvs_pool_stats_t stats;
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);
    assert(kvs_allocator_destroy() == 0);
    return 0;
}