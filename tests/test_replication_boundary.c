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

/* 接收路径使用真实协议回调；仅替换网络身份和单调时钟。 */
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <time.h>
static long long advance_ms;
static int wrong_peer;
void ntyco_shutdown_connection(int fd) { (void)fd; }
int getpeername(int fd, struct sockaddr *address, socklen_t *size)
{
    (void)fd;
    assert(*size >= sizeof(struct sockaddr_in));
    struct sockaddr_in *peer = (struct sockaddr_in *)address;
    *peer = (struct sockaddr_in){.sin_family = AF_INET};
    assert(inet_pton(AF_INET, wrong_peer ? "127.0.0.2" : "127.0.0.1", &peer->sin_addr) == 1);
    *size = sizeof(*peer);
    return 0;
}
int clock_gettime(clockid_t clock_id, struct timespec *now)
{
    int result = (int)syscall(SYS_clock_gettime, clock_id, now);
    if (result == 0 && clock_id == CLOCK_MONOTONIC) {
        now->tv_sec += advance_ms / 1000;
        now->tv_nsec += (advance_ms % 1000) * 1000000;
        if (now->tv_nsec >= 1000000000) { now->tv_sec++; now->tv_nsec -= 1000000000; }
    }
    return result;
}

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
static int disconnect_before_ack;
static int transport_closes;
static int install_fail;
static int apply_fail;
static unsigned int advertised_capture_port;

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

/* 安装回调核对传给加载层的临时文件，不模拟引擎数据结构。 */
static int verify_snapshot(const char *path)
{
    char actual[sizeof(snapshot)];
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    size_t length = fread(actual, 1, sizeof(actual), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(length == sizeof(snapshot) - 1);
    assert(memcmp(actual, snapshot, length) == 0);
    if (install_fail)
        return -1;
    ++installed;
    return 0;
}

static int apply_command(const kvs_slice_t *fields, size_t count,
                         char *response, int capacity)
{
    assert(count == 3);
    (void)fields;
    if (apply_fail)
        return snprintf(response, (size_t)capacity, "-ERR rejected\r\n");
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
    if ((disconnect_before_ack && installed) || transport.ack_sent || (early_disconnect && transport.ready_sent) ||
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
        ++transport_closes;
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
    advance_ms = wrong_peer = 0;
    disconnect_before_ack = transport_closes = 0;
    install_fail = apply_fail = 0;
    assert(kvs_replication_init(KVS_ROLE_REPLICA, "127.0.0.1", 29000,
                                verify_snapshot, apply_command) == 0);
    assert(kvs_replication_set_service_port(29001) == 0);
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

static int drive_replica(kvs_connector_config_t *connector)
{
    char output[256], expected[128];
    int sent = 0;
    int length = snprintf(expected, sizeof(expected), "SYNC_FINISHED %s 112\r\n", sync_id);
    for (int i = 0; i < 20; i++) {
        int result = connector->stream_handler(123, output, sizeof(output));
        if (result > 0) {
            assert(result == length && memcmp(output, expected, length) == 0 && sent++ == 0);
            assert(installed == 1 && transport.ack_sent && !transport.disconnected);
            assert(access("snapshot.db.replication.tmp", F_OK) < 0 && errno == ENOENT);
        } else if (result == 0 || result != KVS_STREAM_WAIT) {
            return result;
        }
    }
    assert(0 && "Replica failed to finish control handshake");
    return -1;
}

static void teardown(void)
{
    kvs_replication_destroy();
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
    strcpy(input, "FULLRESYNC 0123456789abcdef0123456789abcdef01234567 21\r\n");
    assert(feed(&connector, input, (int)strlen(input), &consumed) < 0);
    assert(starts == 0);

    int header_length = snprintf(input, sizeof(input),
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 %zu 29747 %s 112 0\r\n",
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

    int result = drive_replica(&connector);
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
    static const char control[] = "ONLINE 112\r\n";
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

static void test_control_boundaries(void)
{
    recv_before_send = early_disconnect = partial_disconnect = oversized_chunk = 0;
    static const char *headers[] = {
        "FULLRESYNC_RDMA short 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 -1 29747 abcdef0123456789abcdef0123456789abcdef01 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 0 29747 abcdef0123456789abcdef0123456789abcdef01 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 0 abcdef0123456789abcdef0123456789abcdef01 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 65536 abcdef0123456789abcdef0123456789abcdef01 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 short 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 0 extra\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 0 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 65536\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 -1\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 -0\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 29000\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 29747\r\n",
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 21 29747 abcdef0123456789abcdef0123456789abcdef01 112 1 extra\r\n"
    };
    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
        kvs_connector_config_t connector = setup();
        char input[256];
        strcpy(input, headers[i]);
        int consumed = 0;
        assert(feed(&connector, input, (int)strlen(input), &consumed) < 0);
        assert(starts == 0 && installed == 0 && applied == 0);
        assert(!kvs_replication_is_online());
        teardown();
    }
    static const char *controls[] = {
        "ONLINE -1\r\n", "ONLINE 111\r\n", "ONLINE abc\r\n", "ONLINE 112 extra\r\n"
    };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        kvs_connector_config_t connector = setup();
        char input[256];
        int consumed = 0;
        int length = snprintf(input, sizeof(input),
            "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 %zu 29747 %s 112 0\r\n",
            sizeof(snapshot) - 1, sync_id);
        for (int prefix = 1; prefix < length; prefix++) {
            assert(feed(&connector, input, prefix, &consumed) == 0);
            assert(consumed == 0 && starts == 0);
        }
        assert(feed(&connector, input, length, &consumed) == 0);
        int result = drive_replica(&connector);
        assert(result == 0 && installed == 1 && !kvs_replication_is_online());

        static const char online[] = "ONLINE 112\r\n";
        for (size_t prefix = 1; prefix < sizeof(online) - 1; prefix++) {
            memcpy(input, online, sizeof(online));
            assert(feed(&connector, input, (int)prefix, &consumed) == 0);
            assert(consumed == 0 && !kvs_replication_is_online());
        }
        strcpy(input, controls[i]);
        assert(feed(&connector, input, (int)strlen(input), &consumed) < 0);
        assert(applied == 0 && !kvs_replication_is_online());
        teardown();
    }
    puts("[PASS] fixed full-sync headers, all control prefixes and invalid ONLINE boundaries");
}

static kvs_connector_config_t loaded_replica(void)
{
    recv_before_send = early_disconnect = partial_disconnect = oversized_chunk = 0;
    kvs_connector_config_t connector = setup();
    char input[256], output[256], expected[128];
    int length = snprintf(input, sizeof(input),
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 %zu 29747 %s 112 %u\r\n",
        sizeof(snapshot) - 1, sync_id, advertised_capture_port);
    int consumed = 0;
    assert(feed(&connector, input, length, &consumed) == 0);
    int expected_length = snprintf(expected, sizeof(expected), "SYNC_FINISHED %s 112\r\n", sync_id);
    for (int i = 0; i < 12; i++) {
        int result = connector.stream_handler(123, output, sizeof(output));
        if (result > 0) {
            assert(result == expected_length && memcmp(output, expected, (size_t)result) == 0);
            assert(installed == 1 && transport.ack_sent && !transport.disconnected);
            return connector;
        }
        assert(result == KVS_STREAM_WAIT);
    }
    assert(0 && "no SYNC_FINISHED after successful load");
    return connector;
}

static void test_finish_handshake(void)
{
    kvs_connector_config_t connector = loaded_replica();
    char input[512], output[256];
    const char online[] = "ONLINE 112\r\n";
    memcpy(input, online, sizeof(online) - 1);
    memcpy(input + sizeof(online) - 1, command, sizeof(command) - 1);
    int tail = (int)sizeof(online) + (int)sizeof(command) - 2;
    int consumed = -1;
    kvs_output_buffer_t response = { .data = output, .capacity = sizeof(output) };
    assert(connector.message_handler(124, input, tail, &response, &consumed) < 0);
    assert(feed(&connector, input, tail, &consumed) == 0 && consumed == 0);
    assert(!kvs_replication_is_online() && applied == 0); /* 增量先到，CM 尚未关闭。 */
    assert(connector.stream_handler(123, output, sizeof(output)) == 0 && transport.disconnected);
    assert(transport_closes == 1);
    assert(feed(&connector, input, tail, &consumed) == 0 && consumed == (int)sizeof(online) - 1);
    assert(kvs_replication_is_online() && applied == 0);
    memmove(input, input + consumed, sizeof(command) - 1);
    assert(feed(&connector, input, sizeof(command) - 1, &consumed) == 0 && applied == 1);
    int length = snprintf(input, sizeof(input), "SYNC_FINISHED_OK %s 112\r\n", sync_id);
    assert(feed(&connector, input, length, &consumed) < 0); /* 旧确认不再属于协议。 */
    teardown();

    connector = setup();
    install_fail = 1;
    length = snprintf(input, sizeof(input),
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 %zu 29747 %s 112 0\r\n",
        sizeof(snapshot) - 1, sync_id);
    assert(feed(&connector, input, length, &consumed) == 0);
    int result = KVS_STREAM_WAIT;
    for (int i = 0; i < 12 && result == KVS_STREAM_WAIT; i++)
        result = connector.stream_handler(123, output, sizeof(output));
    assert(result < 0 && result != KVS_STREAM_WAIT && !transport.ack_sent && installed == 0);
    assert(access("snapshot.db.replication.tmp", F_OK) < 0 && errno == ENOENT);
    teardown();
    connector = setup();
    install_fail = 0;
    disconnect_before_ack = 1;
    length = snprintf(input, sizeof(input),
        "FULLRESYNC_RDMA 0123456789abcdef0123456789abcdef01234567 %zu 29747 %s 112 0\r\n",
        sizeof(snapshot) - 1, sync_id);
    assert(feed(&connector, input, length, &consumed) == 0);
    result = KVS_STREAM_WAIT;
    for (int i = 0; i < 12 && result == KVS_STREAM_WAIT; i++)
        result = connector.stream_handler(123, output, sizeof(output));
    assert(result < 0 && result != KVS_STREAM_WAIT);
    assert(installed == 1 && !transport.ack_sent && !kvs_replication_is_online());
    connector.close_handler(123);
    assert(transport_closes == 1);
    assert(access("snapshot.db.replication.tmp", F_OK) < 0 && errno == ENOENT);
    teardown();
    assert(transport_closes == 1); /* 重复清理不重复释放连接。 */
    puts("[PASS] SYNC_FINISHED: load failure, wrong FD, early coalesced increment waits for RDMA, no OK round trip, disconnect before final ACK, idempotent cleanup");
}


static int capture_feed(int fd, char *input, int length, char *output, int *consumed)
{
    kvs_output_buffer_t response = {.data = output, .capacity = 256};
    return kvs_replication_capture_network_protocol(fd, input, length, &response, consumed);
}
static kvs_connector_config_t capture_replica(void)
{
    advertised_capture_port = 1;
    kvs_connector_config_t control = loaded_replica();
    char output[256], begin[160]; int consumed;
    int length = snprintf(begin, sizeof(begin), "BEGIN 0123456789abcdef0123456789abcdef01234567 %s 112\r\n", sync_id);
    assert(capture_feed(456, begin, length, output, &consumed) < 0); /* CM 尚未释放。 */
    assert(control.stream_handler(123, output, sizeof(output)) == 0 && transport.disconnected);
    wrong_peer = 1;
    assert(capture_feed(456, begin, length, output, &consumed) < 0);
    wrong_peer = 0;
    begin[6] = 'f';
    assert(capture_feed(456, begin, length, output, &consumed) < 0); /* Primary ID */
    begin[6] = '0'; begin[47] = '0';
    assert(capture_feed(456, begin, length, output, &consumed) < 0); /* sync ID */
    begin[47] = sync_id[0];
    for (int i = 1; i < length; i++)
        assert(capture_feed(456, begin, i, output, &consumed) == 0 && consumed == 0);
    int result = capture_feed(456, begin, length, output, &consumed);
    assert(result == 13 && consumed == length && !memcmp(output, "APPLIED 112\r\n", 13));
    assert(capture_feed(457, begin, length, output, &consumed) < 0); /* 唯一活动链路 */
    return control;
}
static void test_capture_connection(void)
{
    char input[512], output[256], expected[64]; int consumed;
    kvs_connector_config_t control = capture_replica();
    int header = snprintf(input, sizeof(input), "DATA 112 %zu\r\n", sizeof(command)-1);
    memcpy(input+header, command, sizeof(command)-1);
    int length = header + sizeof(command)-1;
    for (int i = 1; i < length; i++)
        assert(capture_feed(456, input, i, output, &consumed) == 0 && consumed == 0);
    int reply = capture_feed(456, input, length, output, &consumed);
    snprintf(expected, sizeof(expected), "APPLIED %zu\r\n", 112+sizeof(command)-1);
    assert(reply == (int)strlen(expected) && !memcmp(output, expected, (size_t)reply) && applied == 1);
    kvs_replication_capture_closed(456); /* ACK 丢失，使用旧确认偏移重连。 */
    length = snprintf(input, sizeof(input), "BEGIN 0123456789abcdef0123456789abcdef01234567 %s 112\r\n", sync_id);
    assert(capture_feed(457, input, length, output, &consumed) == reply);
    assert(!memcmp(output, expected, (size_t)reply) && applied == 1);
    kvs_replication_capture_closed(456); /* 旧 FD 关闭通知不能取消新连接。 */
    length = snprintf(input, sizeof(input), "ONLINE %zu\r\n", 112+sizeof(command)-1);
    assert(capture_feed(457, input, length, output, &consumed) == 0 && kvs_replication_is_online());
    control.close_handler(123);
    assert(!kvs_replication_is_online());
    teardown();

    static const char *bad[] = {"DATA 111 1\r\nx", "DATA 113 1\r\nx", "DATA 112 0\r\n", "DATA 112 -1\r\n", "DATA 112 65537\r\n",
        "ONLINE 113\r\n", "ERROR CACHE_EVICTED\r\n"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        capture_replica(); strcpy(input, bad[i]);
        assert(capture_feed(456, input, (int)strlen(input), output, &consumed) < 0);
        assert(!kvs_replication_is_online() && applied == 0);
        teardown();
    }
    capture_replica();
    header = snprintf(input, sizeof(input), "DATA 112 5\r\n");
    memcpy(input + header, command, 5);
    assert(capture_feed(456, input, header + 5, output, &consumed) == 0 && applied == 0);
    header = snprintf(input, sizeof(input), "DATA 117 %zu\r\n", sizeof(command) - 6);
    memcpy(input + header, command + 5, sizeof(command) - 6);
    reply = capture_feed(456, input, header + sizeof(command) - 6, output, &consumed);
    assert(reply == (int)strlen(expected) && applied == 1);
    assert(!memcmp(output, expected, (size_t)reply));
    /* 已应用区间不能重复执行。 */
    header = snprintf(input, sizeof(input), "DATA 112 %zu\r\n", sizeof(command) - 1);
    memcpy(input + header, command, sizeof(command) - 1);
    assert(capture_feed(456, input, header + sizeof(command) - 1, output, &consumed) < 0);
    assert(applied == 1 && !kvs_replication_is_online());
    teardown();
    capture_replica();
    apply_fail = 1;
    header = snprintf(input, sizeof(input), "DATA 112 %zu\r\n", sizeof(command) - 1);
    memcpy(input + header, command, sizeof(command) - 1);
    assert(capture_feed(456, input, header + sizeof(command) - 1, output, &consumed) < 0);
    assert(applied == 0 && !kvs_replication_is_online());
    teardown();
    control = capture_replica();
    kvs_replication_capture_closed(456);
    advance_ms = 6000;
    kvs_replication_maintenance();
    assert(!kvs_replication_is_online() && kvs_replication_take_fullsync_retry() == 1);
    assert(kvs_replication_take_fullsync_retry() == 0);
    teardown();
    advertised_capture_port = 0;
    puts("[PASS] capture push: readiness/IP/IDs, split BEGIN/DATA, APPLIED, lost-ACK resume, unique link, stale FD, ERROR, timeout/fullsync");
}

int main(void)
{
    char directory[] = "/tmp/91kv-boundary.XXXXXX";
    int original_directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(original_directory >= 0 && mkdtemp(directory) != NULL);
    assert(chdir(directory) == 0);
    test_control_boundaries();
    test_session(0, 0, 0);
    test_session(1, 0, 0);
    test_session(0, 1, 0);
    test_session(0, 2, 0);
    test_session(0, 0, 1);
    test_finish_handshake();
    test_capture_connection();
    assert(fchdir(original_directory) == 0);
    assert(close(original_directory) == 0 && rmdir(directory) == 0);
    return 0;
}
