#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
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

static void expect_file_unchanged(const char *path, const char *data, size_t length)
{
    FILE *file = fopen(path, "rb");
    char *actual = malloc(length + 1);
    assert(file != NULL && actual != NULL);
    assert(fread(actual, 1, length + 1, file) == length);
    assert(memcmp(actual, data, length) == 0);
    assert(fclose(file) == 0);
    free(actual);
}

/* 每一个截断位置都必须明确区分完整记录边界与不完整记录。 */
static void test_fixed_recovery_boundaries(const char *path)
{
    size_t first = sizeof(FIRST_COMMAND) - 1;
    for (size_t length = 0; length < sizeof(valid_log) - 1; length++) {
        write_fixture(path, valid_log, length);
        int expected = length == 0 || length == first ? 0 : -1;
        expect_replay(path, 0, expected, length >= first ? "1" : "");
        expect_file_unchanged(path, valid_log, length);
    }
    static const char *tails[] = {
        "*0\r\n", "*4\r\n", "*-1\r\n",
        "*3\r\n$-1\r\n", "*3\r\n$184467440737095516160\r\n",
        "*3\r\n$4\r\nHSET\rX"
    };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        size_t length = first + strlen(tails[i]);
        char *input = malloc(length);
        assert(input != NULL);
        memcpy(input, FIRST_COMMAND, first);
        memcpy(input + first, tails[i], strlen(tails[i]));
        write_fixture(path, input, length);
        expect_replay(path, 0, -1, "1");
        expect_file_unchanged(path, input, length);
        free(input);
    }
    /* AOF 允许重复的合法写入，恢复按记录顺序执行，不自动去重。 */
    static const char repeated[] = FIRST_COMMAND FIRST_COMMAND;
    write_fixture(path, repeated, sizeof(repeated) - 1);
    expect_replay(path, 0, 0, "11");
    expect_file_unchanged(path, repeated, sizeof(repeated) - 1);
    puts("[PASS] AOF all truncation positions, invalid tails and ordered repeated records");
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
    char large_value[8192];
    memset(large_value, '9', sizeof(large_value) - 1);
    large_value[sizeof(large_value) - 1] = '\0';
    char *failed[] = {"HSET", "k", large_value};
    char *second[] = {"HSET", "k", "2"};

    kvs_pool_stats_t stats;

    /* 写请求结束后，encoded 应当已经释放。 */
    fail_alloc = 1; /* 短命令使用栈缓冲，不依赖堆分配。 */
    assert(kvs_aof_append(first, 3) == 0);
    fail_alloc = 0;
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

/* 独立构造格式字节，校验测试不借用生产编码或 CRC 函数。 */
static unsigned int fixture_crc32(const unsigned char *bytes, size_t length)
{
    unsigned int crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
        }
    }
    return crc ^ 0xffffffffu;
}

static size_t checked_fixture(unsigned char *bytes, const char *commands, size_t length)
{
    memcpy(bytes, "91KVAOF1\0\0\0\1\0\0\0\0", 16);
    memcpy(bytes + 16, commands, length);
    for (unsigned int i = 0; i < 8; i++) {
        bytes[16 + length + i] = (unsigned char)((uint64_t)length >> (56 - i * 8));
    }
    unsigned int crc = fixture_crc32(bytes, 24 + length);
    for (unsigned int i = 0; i < 4; i++) {
        bytes[24 + length + i] = (unsigned char)(crc >> (24 - i * 8));
    }
    return length + 28;
}

static void expect_checked_replay(const char *path, long long offset,
                                  int result, const char *values)
{
    seen_count = 0;
    memset(seen_values, 0, sizeof(seen_values));
    assert(kvs_aof_replay_with_checksum(path, KVS_AOF_CHECKSUM_FILE,
                                       offset, record_command) == result);
    assert(seen_count == strlen(values));
    assert(memcmp(seen_values, values, seen_count) == 0);
}

static void test_checked_file(const char *path)
{
    unsigned char bytes[256];
    size_t length = checked_fixture(bytes, valid_log, sizeof(valid_log) - 1);
    write_fixture(path, (const char *)bytes, length);
    expect_checked_replay(path, 0, 0, "12");
    expect_checked_replay(path, sizeof(FIRST_COMMAND) - 1, 0, "2");
    expect_checked_replay(path, sizeof(valid_log), 0, "");
    expect_replay(path, 0, -1, "");
    assert(kvs_aof_open(path) == -1);
    expect_file_unchanged(path, (const char *)bytes, length);

    /* 每一个截断位置，包括零字节，都在调用回放函数前拒绝。 */
    for (size_t cut = 0; cut < length; cut++) {
        write_fixture(path, (const char *)bytes, cut);
        expect_checked_replay(path, 0, -1, "");
        expect_file_unchanged(path, (const char *)bytes, cut);
    }
    const size_t corrupt_positions[] = {
        0, 8, 12, 16 + sizeof(FIRST_COMMAND) - 4, length - 12, length - 1
    };
    for (size_t i = 0; i < sizeof(corrupt_positions) / sizeof(corrupt_positions[0]); i++) {
        size_t position = corrupt_positions[i];
        bytes[position] ^= 1;
        write_fixture(path, (const char *)bytes, length);
        expect_checked_replay(path, 0, -1, "");
        expect_checked_replay(path, 10000, -1, "");
        assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == -1);
        expect_file_unchanged(path, (const char *)bytes, length);
        bytes[position] ^= 1;
    }
    bytes[length] = 'x';
    write_fixture(path, (const char *)bytes, length + 1);
    expect_checked_replay(path, 0, -1, "");

    /* 双向格式不匹配均拒绝，不改写原文件。 */
    write_fixture(path, valid_log, sizeof(valid_log) - 1);
    expect_checked_replay(path, 0, -1, "");
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == -1);
    expect_file_unchanged(path, valid_log, sizeof(valid_log) - 1);

    /* CRC 正确仍须检查 RESP 和命令回调结果。 */
    length = checked_fixture(bytes, invalid_log, sizeof(invalid_log) - 1);
    write_fixture(path, (const char *)bytes, length);
    expect_checked_replay(path, 0, -1, "1");
    length = checked_fixture(bytes, valid_log, sizeof(valid_log) - 1);
    write_fixture(path, (const char *)bytes, length);
    reject_command = 1;
    expect_checked_replay(path, 0, -1, "1");
    reject_command = 0;
    length = checked_fixture(bytes, FIRST_COMMAND FIRST_COMMAND,
                              sizeof(FIRST_COMMAND FIRST_COMMAND) - 1);
    write_fixture(path, (const char *)bytes, length);
    expect_checked_replay(path, 0, 0, "11");

    /* 合法空容器有 28 字节，已有零字节文件不被当成新容器。 */
    length = checked_fixture(bytes, "", 0);
    write_fixture(path, (const char *)bytes, length);
    expect_checked_replay(path, 0, 0, "");
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == 0);
    assert(kvs_aof_close() == 0);
    expect_file_unchanged(path, (const char *)bytes, length);
    write_fixture(path, "", 0);
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == -1);
    assert(unlink(path) == 0);
    expect_checked_replay(path, 0, 0, "");
    assert(kvs_aof_open_with_checksum(path, (kvs_aof_checksum_t)99) == -1);
    assert(access(path, F_OK) != 0);
    puts("[PASS] AOF file layout, pre-replay CRC, truncation, format mismatch and ordered replay");
}

static void test_checked_append(const char *path)
{
    unsigned char expected[256];
    char *first[] = {"HSET", "k", "1"};
    char *second[] = {"HSET", "k", "2"};
    size_t length = checked_fixture(expected, "", 0);
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == 0);
    expect_file_unchanged(path, (const char *)expected, length);
    assert(kvs_aof_append(first, 3) == 0);
    assert(kvs_aof_close() == 0);
    length = checked_fixture(expected, FIRST_COMMAND, sizeof(FIRST_COMMAND) - 1);
    expect_file_unchanged(path, (const char *)expected, length);

    /* 重新打开时从已有命令重建 CRC，申请失败不污染下次追加。 */
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == 0);
    char large_value[8192];
    memset(large_value, '9', sizeof(large_value) - 1);
    large_value[sizeof(large_value) - 1] = '\0';
    char *large[] = {"HSET", "k", large_value};
    fail_alloc = 1;
    assert(kvs_aof_append(large, 3) == -1);
    fail_alloc = 0;
    expect_file_unchanged(path, (const char *)expected, length);
    assert(kvs_aof_append(second, 3) == 0);
    assert(kvs_aof_close() == 0);
    length = checked_fixture(expected, valid_log, sizeof(valid_log) - 1);
    expect_file_unchanged(path, (const char *)expected, length);
    expect_checked_replay(path, 0, 0, "12");

    /* 同一引擎有依赖的命令仍按文件顺序执行，不做单条跳过。 */
    assert(unlink(path) == 0);
    puts("[PASS] AOF file append overwrites trailer; reopen CRC and allocation failure verified");
}

static void test_checked_partial_write(const char *path)
{
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == 0);
    struct rlimit saved_limit;
    assert(getrlimit(RLIMIT_FSIZE, &saved_limit) == 0);
    struct sigaction saved_action;
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    assert(sigemptyset(&ignored.sa_mask) == 0);
    assert(sigaction(SIGXFSZ, &ignored, &saved_action) == 0);
    struct rlimit limited = saved_limit;
    limited.rlim_cur = 16 + 8;
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    char *fields[] = {"HSET", "k", "1"};
    int result = kvs_aof_append(fields, 3);
    int error = errno;
    assert(setrlimit(RLIMIT_FSIZE, &saved_limit) == 0);
    assert(sigaction(SIGXFSZ, &saved_action, NULL) == 0);
    assert(result == -1 && error == EFBIG);
    errno = 0;
    assert(kvs_aof_append(fields, 3) == -1 && errno == EIO);
    assert(kvs_aof_close() == 0);
    expect_checked_replay(path, 0, -1, "");
    assert(kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_FILE) == -1);
    assert(unlink(path) == 0);
    puts("[PASS] AOF file partial write invalidates writer; reopen rejects damaged trailer");
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
    test_fixed_recovery_boundaries(path);
    assert(unlink(path) == 0);

    test_checked_file(path);
    test_checked_append(path);
    test_checked_partial_write(path);
    puts("AOF replay tests passed");
    /* 所有测试对象释放后，才销毁 allocator。 */
    kvs_pool_stats_t stats;
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);
    assert(kvs_allocator_destroy() == 0);
    return 0;
}