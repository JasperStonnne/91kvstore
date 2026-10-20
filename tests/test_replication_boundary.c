#include "replication.h"
#include "persistence.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include<fcntl.h>
#include <unistd.h>

/* 独立测试不链接含 main() 的 kvstore.c。 */
void *kvs_malloc(size_t size) { return malloc(size); }
void kvs_free(void *ptr) { free(ptr); }

/* 验证接收的文件字节；这里不测试引擎加载。 */
static int verify_snapshot(const char *path)
{
    static const char expected[] = "AOF_OFFSET 0\n";
    char actual[sizeof(expected)];

    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    size_t length = fread(actual, 1, sizeof(actual), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);

    assert(length == sizeof(expected) - 1);
    assert(memcmp(actual, expected, length) == 0);
    return 0;
}

static int unexpected_command(
    const kvs_slice_t *fields, size_t count,
    char *response, int capacity)
{
    (void)fields;
    (void)count;
    (void)response;
    (void)capacity;
    assert(0 && "No incremental command expected");
    return -1;
}

int main(void)
{
    char directory[] = "/tmp/91kv-boundary.XXXXXX";
    int original_directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(original_directory >= 0);
    assert(mkdtemp(directory) != NULL);
    assert(chdir(directory) == 0);

    assert(kvs_aof_open("appendonly.aof") == 0);
    assert(kvs_replication_init(
        KVS_ROLE_REPLICA, "127.0.0.1", 29000,
        verify_snapshot, unexpected_command) == 0);

    kvs_connector_config_t connector = {0};
    assert(kvs_replication_build_connector_config(&connector) == 0);

    char output[256];
    /* 这里只传递连接标识，不创建或读写网络 socket。 */
    assert(connector.open_handler(123, output, sizeof(output)) == 6);

    static const char snapshot[] = "AOF_OFFSET 0\n";
    char input[256];
    int header_length = snprintf(
        input, sizeof(input),
        "FULLRESYNC 0123456789abcdef0123456789abcdef01234567 0 %zu\r\n",
        sizeof(snapshot) - 1);
    assert(header_length > 0);
    assert((size_t)header_length + sizeof(snapshot) <= sizeof(input));

    memcpy(input + header_length, snapshot, sizeof(snapshot) - 1);

    kvs_output_buffer_t response = {
        .data = output,
        .capacity = sizeof(output)
    };
    int consumed = 0;
    int result = connector.message_handler(
        123, input, header_length + sizeof(snapshot) - 1,
        &response, &consumed);

    int passed =
        result == 0 &&
        consumed == header_length &&
        memcmp(input + header_length, snapshot, sizeof(snapshot) - 1) == 0;

    printf(
        "[%s] FULLRESYNC boundary: result=%d, consumed=%d, expected=%d\n",
        passed ? "PASS" : "FAIL", result, consumed, header_length);

        if (passed) {
        /* 模拟外层继续处理缓冲区中剩余的 Snapshot。 */
        consumed = 0;
        result = connector.message_handler(
            123, input + header_length, sizeof(snapshot) - 1,
            &response, &consumed);

        assert(result == 0);
        assert(consumed == (int)sizeof(snapshot) - 1);

        /*
         * Snapshot 已安装，现在一次交入：
         * ONLINE 控制行 + 一条完整 RESP 命令。
         */
        static const char control[] = "ONLINE 0\r\n";
        static const char command[] =
            "*3\r\n$4\r\nHSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
        char combined[sizeof(control) + sizeof(command)];

        memcpy(combined, control, sizeof(control) - 1);
        memcpy(
            combined + sizeof(control) - 1,
            command, sizeof(command) - 1);

        consumed = 0;
        result = connector.message_handler(
            123, combined,
            (int)(sizeof(control) + sizeof(command) - 2),
            &response, &consumed);

        passed =
            result == 0 &&
            consumed == (int)sizeof(control) - 1 &&
            memcmp(
                combined + sizeof(control) - 1,
                command, sizeof(command) - 1) == 0;

        printf(
            "[%s] ONLINE boundary: result=%d, consumed=%d, expected=%zu\n",
            passed ? "PASS" : "FAIL",
            result, consumed, sizeof(control) - 1);

        assert(unlink("snapshot.db") == 0);
    }
    kvs_replication_destroy();
    assert(kvs_aof_close() == 0);
    assert(unlink("appendonly.aof") == 0);
    assert(fchdir(original_directory) == 0);
    assert(close(original_directory) == 0);
    assert(rmdir(directory) == 0);
    return passed ? 0 : 1;
}