#include "replication.h"
#include "persistence.h"
#include "rdma_transport.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 本单测只验证复制状态机及控制消息边界，不访问 RDMA 设备。
 * 下方实现传输层测试替身，通过公开 connector 回调驱动生产代码。
 * 真实 CM/CQ、文件格式和数据一致性由 make test-rdma 覆盖。
 */
void *kvs_malloc(size_t size) { return malloc(size); }
void kvs_free(void *ptr) { free(ptr); }

static const char snapshot[] = "opaque-snapshot-bytes";
static const char sync_id[] = "abcdef0123456789abcdef0123456789abcdef01";
static const char command[] = "*3\r\n$4\r\nHSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
static int installed;
static int applied;
static int recv_before_send;
static int early_disconnect;
static int partial_disconnect;
static int oversized_chunk;
static int starts;

struct kvs_rdma_connection {
    int established;
    int ready_sent;
    int ready_completed;
    int delivered;
    int receive_rearmed;
    int ack_sent;
    int disconnected;
};
static struct kvs_rdma_connection transport;

/* 安装回调仅核对传给加载层的文件和边界 O，不模拟引擎数据结构。 */
static int verify_snapshot(const char *path, long long aof_offset)
{
    assert(aof_offset == 0);
    char actual[sizeof(snapshot)];
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    size_t length = fread(actual, 1, sizeof(actual), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(length == sizeof(snapshot) - 1);
    assert(memcmp(actual, snapshot, length) == 0);
    ++installed;
    return 0;
}

static int apply_command(const kvs_slice_t *fields, size_t count,
                         char *response, int capacity)
{
    assert(count == 3);
    (void)fields;
    ++applied;
    /* 模拟命令执行器的成功响应；复制层消费命令但不把响应发回 Primary。 */
    return snprintf(response, (size_t)capacity, "+OK\r\n");
}

/* ---------- 可控的 CM/CQ 事件：只替换传输接口，不修改生产状态机 ---------- */
int kvs_rdma_connection_start(kvs_rdma_connection_t **connection,
                             const char *host, unsigned short port, int timeout_ms)
{
    assert(*connection == NULL);
    assert(strcmp(host, "127.0.0.1") == 0);
    assert(port == 29747 && timeout_ms > 0);
    memset(&transport, 0, sizeof(transport));
    *connection = &transport;
    ++starts;
    return 0;
}

int kvs_rdma_connection_poll_event(kvs_rdma_connection_t *connection,
                                  kvs_rdma_event_t *event,
                                  size_t capacity, int queue_depth)
{
    assert(connection == &transport && capacity > 0 && queue_depth > 0);
    event->connection = connection;
    if (!transport.established) {
        transport.established = 1;
        event->type = KVS_RDMA_EVENT_ESTABLISHED;
        return 1;
    }
    if (transport.ack_sent || (early_disconnect && transport.ready_sent) ||
        (partial_disconnect && transport.delivered)) {
        event->type = KVS_RDMA_EVENT_DISCONNECTED;
        return 1;
    }
    return 0;
}

int kvs_rdma_connection_send(kvs_rdma_connection_t *connection,
                            const void *data, size_t length)
{
    assert(connection == &transport);
    char expected[80];
    if (!transport.ready_sent) {
        snprintf(expected, sizeof(expected), "READY %s\r\n", sync_id);
        transport.ready_sent = 1;
    } else {
        /* 最终 ACK 必须发生在快照安装成功及 READY SEND 完成之后。 */
        assert(installed == 1 && transport.ready_completed);
        assert(transport.receive_rearmed && !transport.ack_sent);
        snprintf(expected, sizeof(expected), "SNAPSHOT_ACK %zu\r\n", sizeof(snapshot) - 1);
        transport.ack_sent = 1;
    }
    assert(length == strlen(expected));
    assert(memcmp(data, expected, length) == 0);
    return 0;
}

int kvs_rdma_connection_poll_completion(kvs_rdma_connection_t *connection,
                                       kvs_rdma_completion_t *completion)
{
    assert(connection == &transport && transport.ready_sent);
    /* RECV 可能早于本地 READY 的 SEND 完成通知，不能过早复用发送缓冲区。 */
    if (!transport.delivered && (transport.ready_completed || recv_before_send)) {
        transport.delivered = 1;
        completion->type = KVS_RDMA_COMPLETION_RECV;
        completion->data = snapshot;
        completion->length = partial_disconnect ? (sizeof(snapshot) - 1) / 2 :
            sizeof(snapshot) - 1 + (oversized_chunk ? 1 : 0);
        return 1;
    }
    if (!transport.ready_completed) {
        transport.ready_completed = 1;
        completion->type = KVS_RDMA_COMPLETION_SEND;
        return 1;
    }
    return 0;
}

int kvs_rdma_connection_receive_next(kvs_rdma_connection_t *connection)
{
    assert(connection == &transport && transport.delivered);
    transport.receive_rearmed = 1;
    return 0;
}

int kvs_rdma_connection_disconnect(kvs_rdma_connection_t *connection)
{
    assert(connection == &transport && transport.ack_sent);
    transport.disconnected = 1;
    return 0;
}

int kvs_rdma_connection_close(kvs_rdma_connection_t **connection)
{
    if (*connection != NULL) {
        assert(*connection == &transport);
        *connection = NULL;
    }
    return 0;
}

/* 此用例只启动 Replica；意外走到 Primary 监听接口立即失败。 */
kvs_rdma_listener_t *kvs_rdma_listener_create(void)
{
    assert(0 && "Replica test must not create a listener");
    return NULL;
}
int kvs_rdma_listener_start(kvs_rdma_listener_t *listener, const char *ip,
                           unsigned short port, int backlog)
{
    (void)listener; (void)ip; (void)port; (void)backlog;
    assert(0 && "unexpected listener start");
    return -1;
}
int kvs_rdma_listener_get_fd(const kvs_rdma_listener_t *listener)
{
    (void)listener;
    assert(0 && "unexpected listener fd");
    return -1;
}
int kvs_rdma_listener_poll(kvs_rdma_listener_t *listener,
                         kvs_rdma_connection_t **connection, kvs_rdma_event_t *event,
                         size_t capacity, int depth)
{
    (void)listener; (void)connection; (void)event; (void)capacity; (void)depth;
    assert(0 && "unexpected listener poll");
    return -1;
}
int kvs_rdma_listener_close(kvs_rdma_listener_t **listener)
{
    assert(*listener == NULL);
    return 0;
}

/* ---------- 用公开回调驱动协议，不依赖内部 manager 字段 ---------- */
static kvs_connector_config_t setup(void)
{
    installed = applied = starts = 0;
    assert(kvs_aof_open("appendonly.aof") == 0);
    assert(kvs_replication_init(KVS_ROLE_REPLICA, "127.0.0.1", 29000,
                                verify_snapshot, apply_command) == 0);
    kvs_connector_config_t connector = {0};
    assert(kvs_replication_build_connector_config(&connector) == 0);
    char output[256];
    assert(connector.open_handler(123, output, sizeof(output)) == 6);
    return connector;
}

static int feed(kvs_connector_config_t *connector, char *input, int length,
                int *consumed)
{
    char output[256];
    kvs_output_buffer_t response = {.data = output, .capacity = sizeof(output)};
    return connector->message_handler(123, input, length, &response, consumed);
}

static void teardown(void)
{
    kvs_replication_destroy();
    assert(kvs_aof_close() == 0);
    assert(unlink("appendonly.aof") == 0);
    if (unlink("snapshot.db") < 0) assert(errno == ENOENT);
    assert(access("snapshot.db.replication.tmp", F_OK) < 0 && errno == ENOENT);
}

static void test_session(int reordered, int disconnect_early, int oversize)
{
    partial_disconnect = disconnect_early == 2;
    recv_before_send = reordered;
    early_disconnect = disconnect_early == 1;
    oversized_chunk = oversize;
    kvs_connector_config_t connector = setup();
    char input[512];
    int consumed = 0;

    /* 旧 TCP 全量通知必须被拒绝，且不能创建 RDMA 连接。 */
    strcpy(input, "FULLRESYNC 0123456789abcdef0123456789abcdef01234567 0 21\r\n");
    assert(feed(&connector, input, (int)strlen(input), &consumed) < 0);
    assert(starts == 0);

    int header_length = snprintf(input, sizeof(input),
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 0 %zu 29747 %s\r\n",
        sizeof(snapshot) - 1, sync_id);
    assert(header_length > 0);
    memcpy(input + header_length, command, sizeof(command) - 1);
    consumed = 0;
    assert(feed(&connector, input, header_length + (int)sizeof(command) - 1, &consumed) == 0);
    assert(consumed == header_length && starts == 1 && installed == 0 && applied == 0);
    assert(memcmp(input + consumed, command, sizeof(command) - 1) == 0);

    /* 全量阶段的 TCP 数据不能成为快照，也不能提前作为增量执行。 */
    consumed = 0;
    assert(feed(&connector, (char *)command, sizeof(command) - 1, &consumed) < 0);
    assert(installed == 0 && applied == 0);

    char output[256];
    int result = KVS_STREAM_WAIT;
    for (int i = 0; i < 12 && result == KVS_STREAM_WAIT; ++i)
        result = connector.stream_handler(123, output, sizeof(output));
    if (disconnect_early || oversize) {
        assert(result < 0 && result != KVS_STREAM_WAIT);
        assert(installed == 0 && !transport.ack_sent);
        assert(!kvs_replication_is_online());
        if (partial_disconnect) {
            FILE *file = fopen("snapshot.db.replication.tmp", "rb");
            assert(file != NULL);
            assert(fseek(file, 0, SEEK_END) == 0);
            assert(ftell(file) == (long)(sizeof(snapshot) - 1) / 2);
            assert(fclose(file) == 0);
            assert(access("snapshot.db", F_OK) < 0 && errno == ENOENT);
            /* 外层网络关闭回调负责取消本轮，必须删掉未完成的临时快照。 */
            connector.close_handler(123);
            assert(access("snapshot.db.replication.tmp", F_OK) < 0 && errno == ENOENT);
        }
        teardown();
        puts(partial_disconnect ? "[PASS] half-transferred Snapshot disconnect: not installed, not ONLINE, temp removed" :
             disconnect_early ? "[PASS] premature RDMA disconnect rejected" :
                                "[PASS] oversized RDMA Snapshot chunk rejected");
        return;
    }
    assert(result == 0 && installed == 1 && transport.disconnected);

    /* ONLINE 与 RESP 粘包：只消费 ONLINE 行，下一轮才执行增量。 */
    static const char control[] = "ONLINE 0\r\n";
    memcpy(input, control, sizeof(control) - 1);
    memcpy(input + sizeof(control) - 1, command, sizeof(command) - 1);
    consumed = 0;
    assert(feed(&connector, input, sizeof(control) + sizeof(command) - 2, &consumed) == 0);
    assert(consumed == (int)sizeof(control) - 1 && applied == 0);
    assert(kvs_replication_is_online());
    assert(memcmp(input + consumed, command, sizeof(command) - 1) == 0);
    int command_length = sizeof(command) - 1;
    memmove(input, input + consumed, command_length);
    consumed = 0;
    assert(feed(&connector, input, command_length, &consumed) == 0);
    assert(consumed == command_length && applied == 1);
    teardown();
    puts(reordered ? "[PASS] RDMA RECV-before-SEND and ONLINE boundary" :
                     "[PASS] RDMA full-sync control, TCP rejection and ONLINE boundary");
}

int main(void)
{
    char directory[] = "/tmp/91kv-boundary.XXXXXX";
    int original_directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(original_directory >= 0 && mkdtemp(directory) != NULL);
    assert(chdir(directory) == 0);
    test_session(0, 0, 0);
    test_session(1, 0, 0);
    test_session(0, 1, 0);
    test_session(0, 2, 0);
    test_session(0, 0, 1);
    assert(fchdir(original_directory) == 0);
    assert(close(original_directory) == 0 && rmdir(directory) == 0);
    return 0;
}
