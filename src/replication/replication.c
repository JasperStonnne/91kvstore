#include "replication.h"
#include "replication_capture_ipc.h"
#include "kvs_allocator.h"
#include "persistence.h"
#include "server.h"
#include "log.h"
#include "rdma_transport.h"
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/random.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#define KVS_REPLICATION_HOST_MAX_LENGTH 256
#define KVS_REPLICATION_ID_HEX_LENGTH 40 // 20 字节随机数转换成 40 个十六进制字符
// Primary 为全量同步生成并发送的 Snapshot
#define KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH "replication.snapshot.tmp"
// Replica 接收期间写入的临时文件，未收完整时不能使用
#define KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH "snapshot.db.replication.tmp"
/* Replica 主动 RDMA 建连的整体超时，包含地址、路由解析。 */
#define KVS_REPLICATION_RDMA_RESOLVE_TIMEOUT_MS 3000
/* 每个方向的 RDMA 缓冲区容量：1 MiB。 */
#define KVS_REPLICATION_RDMA_BUFFER_CAPACITY (1024U * 1024U)

/* 一轮 RDMA 全量同步使用的连接和运行进度。 */
typedef struct {
    kvs_rdma_connection_t *connection;

    /* CM 已报告连接建立。 */
    int established;

    /* Primary 已提交、Replica 已写入的快照字节数。 */
    long long snapshot_bytes;

    /* Replica 已提交确认的位置；Primary 已收到确认的位置。 */
    long long acknowledged_bytes;

    /* 已提交发送，尚未取得对应的 SEND 完成通知。 */
    int send_pending;

    /* 已发起正常断开，等待 CM 的断开事件。 */
    int disconnect_pending;
} kvs_replication_rdma_sync_t;

typedef struct {
    kvs_role_t role;               // 当前进程是单机、Primary 还是 Replica
    kvs_rdma_listener_t *rdma_listener; // Primary 的 RDMA 监听器
/* 当前这一轮 RDMA 全量同步。 */
kvs_replication_rdma_sync_t rdma_sync;
    /* 监听轮询出错时保留的额外连接，退出时清理。 */
    kvs_rdma_connection_t *rdma_cleanup_connection;
    unsigned short rdma_port;          // 本机 RDMA 监听端口
    unsigned short primary_rdma_port; // Replica 从同步通知中取得的 Primary RDMA 端口
    uint64_t snapshot_stream_offset; // 本轮快照对应的复制流位置
    uint64_t send_stream_offset; // 下一段要交给 TCP 的缓存位置
    uint64_t applied_stream_offset; // Replica 已成功应用的完整命令边界
    long long snapshot_file_size; // 当前全量同步 Snapshot 文件的总字节数
    kvs_snapshot_reader_t snapshot_reader; // Primary 当前正在分块读取的复制快照
    kvs_snapshot_writer_t snapshot_writer;
    int snapshot_connection_fd; // Primary 当前向哪条 Replica 连接发送 Snapshot
    char replication_id[KVS_REPLICATION_ID_HEX_LENGTH + 1]; // Primary 当前数据历史的唯一标识
    char full_sync_id[KVS_REPLICATION_ID_HEX_LENGTH + 1];/* 本轮全量同步的标识，用来匹配 TCP 控制连接与 RDMA 数据连接。 */
    char primary_host[KVS_REPLICATION_HOST_MAX_LENGTH];//Replica要链接的primary地址
    unsigned short primary_port;//primary的复制端口
    int primary_connection_fd;//与primary通信的socket
    kvs_snapshot_install_handler snapshot_install_handler; // Replica 收完整 Snapshot 后调用
        kvs_replication_command_handler command_handler; // Replica 执行增量命令的回调
    kvs_replication_state_t state;//当前复制状态
    int initialized;
} kvs_replication_manager_t;//复制管理器 保存复制相关信息

static kvs_replication_manager_t replication_manager = {
    .role = KVS_ROLE_STANDALONE,
    .rdma_listener = NULL,
    .rdma_sync = {
        .connection = NULL,
        .established = 0,
        .snapshot_bytes = 0,
        .acknowledged_bytes = 0,
        .send_pending = 0,
        .disconnect_pending = 0
    },
    .rdma_cleanup_connection = NULL,
    .rdma_port = 0,
    .snapshot_file_size = -1,
    .snapshot_reader = {
    .fd = -1,
    .remaining = 0},
    .snapshot_writer = {
    .fd = -1,
    .remaining = 0},
    .snapshot_connection_fd = -1,
    .replication_id={0},
    .full_sync_id={0},
    .primary_host = {0},
    .primary_port = 0,
    .primary_rdma_port = 0,
    .primary_connection_fd = -1,
    .snapshot_install_handler = NULL,
    .command_handler = NULL,
    .state = KVS_REPLICATION_STATE_DISCONNECTED,
    .initialized = 0

};

/* Primary 本次运行中，成功写入在复制命令流里的累计字节数。 */
static uint64_t replication_stream_end;

/* 暂用固定容量，按字节限制单轮全量期间保留的写入。 */
#define KVS_REPLICATION_CACHE_BYTES (8u * 1024u * 1024u)

static struct {
    char *data; // NULL 表示未启用，或本轮缓存已不可用
    size_t length;
    uint64_t start;
} replication_cache;

static void kvs_replication_cache_clear(void)
{
    kvs_free(replication_cache.data);
    replication_cache.data = NULL;
    replication_cache.length = 0;
}

/* uprobe 在函数入口读取这四个参数。保留独立调用和参数。 */
__attribute__((noinline))
static void kvs_replication_committed(
    const char *payload, size_t length, uint64_t start, uint64_t end)
{
    __asm__ volatile("" : : "r"(payload), "r"(length), "r"(start), "r"(end));
}

int kvs_replication_advance_stream(const char *encoded, size_t encoded_bytes)
{
    if (encoded_bytes > UINT64_MAX - replication_stream_end) {
        errno = EOVERFLOW;
        return -1;
    }

    uint64_t start = replication_stream_end;
    replication_stream_end += encoded_bytes;
    kvs_replication_committed(encoded, encoded_bytes, start, replication_stream_end);
    kvs_log_message(KVS_LOG_DEBUG,
                    "replication: committed stream range=[%" PRIu64 ",%" PRIu64 ")\n",
                    start, replication_stream_end);

    if (replication_cache.data == NULL) {
        return 0;
    }
    if (encoded_bytes > KVS_REPLICATION_CACHE_BYTES - replication_cache.length) {
        /* 保留区间必须完整，空间不足时令本轮缓存整体失效。 */
        kvs_replication_cache_clear();
        kvs_log_message(KVS_LOG_INFO,
                        "replication: stream cache unavailable, capacity exceeded at offset=%" PRIu64 "\n",
                        start);
        return 0;
    }

    /* 复制位置持续增长，数组下标在容量处绕回。 */
    size_t index = start % KVS_REPLICATION_CACHE_BYTES;
    size_t first = KVS_REPLICATION_CACHE_BYTES - index;
    if (first > encoded_bytes) {
        first = encoded_bytes;
    }
    memcpy(replication_cache.data + index, encoded, first);
    if (first < encoded_bytes) {
        memcpy(replication_cache.data, encoded + first, encoded_bytes - first);
    }
    replication_cache.length += encoded_bytes;
    return 0;
}

/*
 * 从有效缓存中的绝对复制位置读取。读到末尾返回 0，
 * 缓存不可用或位置越界返回 -1。调用者自行推进读取位置。
 */
static int kvs_replication_cache_read(
    uint64_t offset, char *output, size_t output_capacity)
{
    if (output == NULL || output_capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    if (replication_cache.data == NULL) {
        errno = ENODATA;
        return -1;
    }

    uint64_t start = replication_cache.start;
    uint64_t end = start + replication_cache.length;
    if (offset < start || offset > end) {
        errno = ERANGE;
        return -1;
    }
    if (offset == end) {
        return 0;
    }

    size_t bytes = end - offset;
    if (bytes > output_capacity) {
        bytes = output_capacity;
    }

    size_t index = offset % KVS_REPLICATION_CACHE_BYTES;
    size_t first = KVS_REPLICATION_CACHE_BYTES - index;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(output, replication_cache.data + index, first);
    if (first < bytes) {
        memcpy(output + first, replication_cache.data, bytes - first);
    }
    return (int)bytes;
}

/* Close the reader before unlinking the Primary's transfer file. */
static int kvs_replication_remove_primary_snapshot(void)
{
    kvs_snapshot_reader_close(&replication_manager.snapshot_reader);
    if (unlink(KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH) == 0 || errno == ENOENT) {
        return 0;
    }

    int saved_errno = errno;
    kvs_log_message(KVS_LOG_ERROR, "replication: failed to remove %s: %s\n",
                    KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH, strerror(saved_errno));
    errno = saved_errno;
    return -1;
}


/* 释放本轮 RDMA 连接，并重置本轮进度。 */
static int kvs_replication_rdma_sync_close(
    kvs_replication_rdma_sync_t *sync)
{
    if (sync == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (kvs_rdma_connection_close(&sync->connection) < 0) {
        return -1;
    }

    sync->established = 0;
    sync->snapshot_bytes = 0;
    sync->acknowledged_bytes = 0;
    sync->send_pending = 0;
    sync->disconnect_pending = 0;
    return 0;
}

/* 复用已有的随机 ID 生成函数。 */
static int kvs_replication_generate_id(
    char *output,
    size_t output_capacity);

static int kvs_replication_on_primary_connected(
    int connection_fd,
    char *output,
    int output_capacity)
{
    if (!replication_manager.initialized ||             // 复制管理器必须已经初始化
        replication_manager.role != KVS_ROLE_REPLICA || // 只有 Replica 会主动连接 Primary
        connection_fd < 0 ||                            // socket 必须有效
        output == NULL ||
        output_capacity <= 0) {
        return -1;
    }

    int output_length = snprintf(
        output,
        output_capacity,
        "PING\r\n"                                      // 连接成功后的第一条复制消息
    );

    if (output_length < 0 ||
        output_length >= output_capacity) {
        return -1;
    }

    replication_manager.primary_connection_fd = connection_fd; // 记录上游连接
    replication_manager.state = KVS_REPLICATION_STATE_HANDSHAKE; // 进入复制握手阶段

    return output_length;                               // 网络层负责发送这些字节
}

static void kvs_replication_on_primary_disconnected(
    int connection_fd)
{
    if (!replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_REPLICA) {
        return;
    }

    if (replication_manager.primary_connection_fd != connection_fd) {
        return;                                          // 不处理不属于当前上游的旧连接
    }
    /* 控制连接断开，本轮全量同步取消，释放对应的 RDMA 连接。 */
    if (kvs_replication_rdma_sync_close(
            &replication_manager.rdma_sync) < 0) {
        kvs_log_message(
            KVS_LOG_ERROR,
            "replication: failed to close Replica RDMA connection: %s\n",
            strerror(errno));
    }

    replication_manager.primary_rdma_port = 0;
    replication_manager.full_sync_id[0] = '\0';
    kvs_snapshot_writer_abort(
        &replication_manager.snapshot_writer,
        KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH
    );
    replication_manager.primary_connection_fd = -1;     // 当前已经没有上游连接
    replication_manager.state =
        KVS_REPLICATION_STATE_DISCONNECTED;              // 等待后续重新连接
    kvs_log_message(
        KVS_LOG_INFO,
        "replication: Replica control connection closed; synchronization cancelled\n");
}

static int kvs_replication_parse_stream_offset(
    const char *text, size_t length, uint64_t *offset)
{
    if (length == 0 || length > 20 ||
        strspn(text, "0123456789") != length) {
        return -1;
    }

    errno = 0;
    uintmax_t value = strtoumax(text, NULL, 10);
    if (errno == ERANGE || value > UINT64_MAX) {
        return -1;
    }
    *offset = (uint64_t)value;
    return 0;
}

static int kvs_replication_upstream_frame_protocol(
    int connection_fd,
    char *frame,
    int frame_length,
    char *response,
    int response_capacity)
{


    if (!replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_REPLICA ||
        connection_fd < 0 ||
        connection_fd != replication_manager.primary_connection_fd ||
        frame == NULL ||
        frame_length <= 0||
        response==NULL||
        response_capacity<=0) {
        return -1;
    }

    if (replication_manager.state !=
        KVS_REPLICATION_STATE_HANDSHAKE) {           // 当前必须正在进行复制握手
        return -1;
    }

      if(frame_length == 4 &&
       memcmp(frame,"PONG",4) == 0){

        int response_length=snprintf(
            response,
            response_capacity,
            "PSYNC ? -1\r\n" // 首次同步：不知道 Primary ID，也没有旧 offset
        );

        if(response_length < 0 ||
           response_length >= response_capacity){
            return -1;
        }

        return response_length; // 网络层会把 PSYNC 发送给 Primary
    }
    if (frame_length >= 16 &&
        memcmp(frame, "FULLRESYNC_RDMA ", 16) == 0) {
        char primary_replication_id[
            KVS_REPLICATION_ID_HEX_LENGTH + 1
        ] = {0};

        char full_sync_id[
            KVS_REPLICATION_ID_HEX_LENGTH + 1
        ] = {0};

        char stream_offset_text[21] = {0};
        uint64_t stream_offset;
        long long primary_snapshot_file_size = -1;
        unsigned int primary_rdma_port = 0;
        int parsed_length = 0;

        /* 只接受包含 RDMA 端口和本轮同步 ID 的全量通知。 */
        int matched = sscanf(
            frame,
            "FULLRESYNC_RDMA %40[0-9a-f] %lld %5u %40[0-9a-f] %20[0-9]%n",
            primary_replication_id,
            &primary_snapshot_file_size,
            &primary_rdma_port,
            full_sync_id,
            stream_offset_text,
            &parsed_length);

        if (matched != 5 ||
            parsed_length != frame_length ||
            strlen(primary_replication_id) != KVS_REPLICATION_ID_HEX_LENGTH ||
            strlen(full_sync_id) != KVS_REPLICATION_ID_HEX_LENGTH ||
            primary_snapshot_file_size <= 0 ||
            primary_rdma_port == 0 ||
            primary_rdma_port > 65535 ||
            kvs_replication_parse_stream_offset(stream_offset_text,
                strlen(stream_offset_text), &stream_offset) < 0) {
            return -1;
        }
        kvs_snapshot_writer_abort(
        &replication_manager.snapshot_writer,
        KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH
        );

        if(kvs_snapshot_writer_open(
                &replication_manager.snapshot_writer,
                KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH,
                primary_snapshot_file_size) < 0){
            return -1;
        }
        memcpy(
            replication_manager.replication_id,
            primary_replication_id,
            sizeof(primary_replication_id)
        );

        replication_manager.primary_rdma_port =
        (unsigned short)primary_rdma_port;

    memcpy(
        replication_manager.full_sync_id,
        full_sync_id,
        sizeof(full_sync_id));
        replication_manager.snapshot_stream_offset = stream_offset;
        replication_manager.snapshot_file_size =primary_snapshot_file_size;
        /* 根据 TCP 通知中的端口，异步发起 RDMA 连接。 */
        if (kvs_rdma_connection_start(
                &replication_manager.rdma_sync.connection,
                replication_manager.primary_host,
                replication_manager.primary_rdma_port,
                KVS_REPLICATION_RDMA_RESOLVE_TIMEOUT_MS) < 0) {
            kvs_log_message(
                KVS_LOG_ERROR,
                "replication: failed to start RDMA connection host=%s port=%u: %s\n",
                replication_manager.primary_host,
                (unsigned int)replication_manager.primary_rdma_port,
                strerror(errno));
            return -1;
        }
        replication_manager.state=KVS_REPLICATION_STATE_FULL_SYNC;
kvs_log_message(
    KVS_LOG_INFO,
    "replication: enter FULL_SYNC id=%s offset=%" PRIu64 " snapshot_size=%lld fd=%d\n",
    replication_manager.replication_id,
    replication_manager.snapshot_stream_offset,
    replication_manager.snapshot_file_size,
    connection_fd
);
        return 0;// 已保存全量同步起点，当前暂时没有消息需要回发
    }
    return -1; // 握手阶段收到未知的 Primary 响应
}
/*
 * 处理 Primary 发来的单条增量命令。
 *
 * 命令交给 kvstore 模块执行；执行成功后返回 0，
 * 表示消费命令但不向 Primary 发送响应。
 */
static int kvs_replication_online_control_frame_protocol(
    int connection_fd,
    char *frame,
    int frame_length,
    char *response,
    int response_capacity)
{
    if(!replication_manager.initialized ||
       replication_manager.role!=KVS_ROLE_REPLICA ||
    (replication_manager.state!=KVS_REPLICATION_STATE_CATCH_UP &&
        replication_manager.state!=KVS_REPLICATION_STATE_ONLINE) ||
       connection_fd<0 ||
       connection_fd!=replication_manager.primary_connection_fd ||
       frame==NULL ||
       frame_length<=0 ||
       response==NULL ||
       response_capacity<5){
        return -1;
    }
    if (frame_length >= 7 && memcmp(frame, "ONLINE ", 7) == 0) {
        uint64_t offset;
        if (replication_manager.state != KVS_REPLICATION_STATE_CATCH_UP ||
            kvs_replication_parse_stream_offset(frame + 7, (size_t)frame_length - 7,
                                                &offset) < 0 ||
            offset != replication_manager.applied_stream_offset) {
            return -1;
        }

        replication_manager.state = KVS_REPLICATION_STATE_ONLINE;
        kvs_log_message(KVS_LOG_INFO,
                        "replication: enter ONLINE offset=%" PRIu64 " fd=%d\n",
                        offset, connection_fd);
        return 0;
    }
    return -1;
}

/* 执行一条长度格式的复制命令，完成后推进已应用位置。 */
static int kvs_replication_catch_up_command_protocol(
    int connection_fd,
    const kvs_slice_t *fields,
    size_t field_count,
    size_t parsed_bytes,
    char *response,
    int response_capacity)
{
    if (!replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_REPLICA ||
        (replication_manager.state != KVS_REPLICATION_STATE_CATCH_UP &&
         replication_manager.state != KVS_REPLICATION_STATE_ONLINE) ||
        connection_fd < 0 ||
        connection_fd != replication_manager.primary_connection_fd ||
        replication_manager.command_handler == NULL ||
        fields == NULL ||
        field_count == 0 ||
        parsed_bytes > UINT64_MAX - replication_manager.applied_stream_offset ||
        response == NULL ||
        response_capacity <= 0) {
        return -1;
    }

    /* 执行字段。 */
    int response_length = replication_manager.command_handler(
        fields,
        field_count,
        response,
        response_capacity
    );

    if (response_length <= 0 ||
        response_length >= response_capacity) {
        return -1;
    }

    uint64_t start = replication_manager.applied_stream_offset;
    replication_manager.applied_stream_offset += parsed_bytes;
    kvs_log_message(KVS_LOG_DEBUG,
                    "replication: applied range=[%" PRIu64 ",%" PRIu64 ")\n",
                    start, replication_manager.applied_stream_offset);
    return 0;
}

/*
 * 每次只处理一条复制控制行。
 * 控制行可能改变复制阶段，剩余数据必须交回外层重新分流。
 */
static int kvs_replication_control_line_protocol(
    int connection_fd,
    char *msg,
    int length,
    char *response,
    int response_capacity,
    int *consumed_length,
    kvs_frame_handler frame_handler)
{
    *consumed_length = 0;

    for (int i = 0; i < length; i++) {
        if (msg[i] == '\n') {
            return kvs_line_batch_protocol(
                connection_fd,
                msg,
                i + 1,
                response,
                response_capacity,
                consumed_length,
                frame_handler
            );
        }
    }

    return 0; /* 控制行尚未收完整，保留全部输入。 */
}

/*
 * Replica 收完整个快照后，从临时文件加载数据，清理后进入增量补齐阶段。
 * connection_fd 是现有复制控制连接的 FD。
 */
static int kvs_replication_finish_full_sync(int connection_fd)
{
    if (replication_manager.state != KVS_REPLICATION_STATE_FULL_SYNC ||
        replication_manager.snapshot_writer.remaining != 0) {
        return -1;
    }

    if (kvs_snapshot_writer_finish(&replication_manager.snapshot_writer) < 0 ||
        replication_manager.snapshot_install_handler == NULL ||
        replication_manager.snapshot_install_handler(
            KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH) < 0) {
        kvs_snapshot_writer_abort(
            &replication_manager.snapshot_writer,
            KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH);
        return -1;
    }

    if (unlink(KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH) < 0) {
        kvs_log_message(KVS_LOG_ERROR,
                        "replication: failed to remove received Snapshot: %s\n",
                        strerror(errno));
        return -1;
    }

    replication_manager.applied_stream_offset =
        replication_manager.snapshot_stream_offset;
    replication_manager.state = KVS_REPLICATION_STATE_CATCH_UP;

    kvs_log_message(
        KVS_LOG_INFO,
        "replication: Snapshot installed, enter CATCH_UP "
        "offset=%" PRIu64 " size=%lld fd=%d\n",
        replication_manager.snapshot_stream_offset,
        replication_manager.snapshot_file_size,
        connection_fd);

    return 0;
}

/*
 * Replica 接收一块快照数据，返回实际消费的字节数。
 * connection_fd 是本次同步的 TCP 控制连接。
 */
static int kvs_replication_receive_snapshot_data(
    int connection_fd,
    const void *data,
    int length)
{
    if (!replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_REPLICA ||
        connection_fd < 0 ||
        connection_fd != replication_manager.primary_connection_fd ||
        replication_manager.state != KVS_REPLICATION_STATE_FULL_SYNC ||
        data == NULL || length <= 0) {
        return -1;
    }

    int written = kvs_snapshot_writer_write(
        &replication_manager.snapshot_writer,
        data,
        length);
    if (written < 0) {
        return -1;
    }

    if (replication_manager.snapshot_writer.remaining == 0) {
        if (kvs_replication_finish_full_sync(connection_fd) < 0) {
            return -1;
        }
    }

    return written;
}

static int kvs_replication_upstream_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    kvs_output_buffer_t *response,
    int *consumed_length)
{
    if(!replication_manager.initialized ||
    replication_manager.role!=KVS_ROLE_REPLICA ||
    connection_fd<0 ||
    connection_fd!=replication_manager.primary_connection_fd ||
    msg==NULL ||
    length<=0 ||
    response==NULL ||
    response->capacity<=0 ||
    response->data == NULL ||
    consumed_length==NULL){
    return -1;
    }
    if(replication_manager.state==KVS_REPLICATION_STATE_HANDSHAKE){
    return kvs_replication_control_line_protocol(
        connection_fd,
        msg,
        length,
        response->data,
        response->capacity,
        consumed_length,
        kvs_replication_upstream_frame_protocol      // 每条上游消息交给 PONG 处理函数
    );
    }
    if (replication_manager.state == KVS_REPLICATION_STATE_FULL_SYNC) {
        /* 全量数据只由 RDMA 接收，此阶段收到 TCP 数据属于协议错误。 */
        return -1;
    }
if (replication_manager.state == KVS_REPLICATION_STATE_CATCH_UP ||
    replication_manager.state == KVS_REPLICATION_STATE_ONLINE) {

    /* 增量命令以 * 开头，按长度字段解析。 */
    if (msg[0] == '*') {
        kvs_slice_t fields[3];
        size_t field_count = 0;
        size_t parsed_bytes = 0;

        int parse_result = kvs_parse_command(
            msg,
            (size_t)length,
            fields,
            3,
            &field_count,
            &parsed_bytes
        );

        if (parse_result == 0) {
            *consumed_length = 0; /* 命令还没收完整，等后续网络数据。 */
            return 0;
        }
        if (parse_result < 0) {
            return -1; /* 增量流中出现格式错误。 */
        }

        /* 完整命令交给字段回调执行。 */
        if (kvs_replication_catch_up_command_protocol(
                connection_fd,
                fields,
                field_count,
                parsed_bytes,
                response->data,
                response->capacity) < 0) {
            return -1;
        }

        *consumed_length = (int)parsed_bytes;
        return 0; /* 复制命令不需要向 Primary 返回普通响应。 */
    }

   /* 只消费一条控制行，剩余数据交回外层重新判断。 */
    return kvs_replication_control_line_protocol(
        connection_fd,
        msg,
        length,
        response->data,
        response->capacity,
        consumed_length,
        kvs_replication_online_control_frame_protocol
    );
}
    return -1;
}

    /*
    * Replica 的持续工作回调。
    * 推进 RDMA 建连、快照接收和累计确认；正常关闭后恢复 TCP 增量接收。
    */
    static int kvs_replication_upstream_stream(
        int connection_fd,
        char *output,
        int output_capacity)
    {
        if (!replication_manager.initialized ||
            replication_manager.role != KVS_ROLE_REPLICA ||
            connection_fd < 0 ||
            connection_fd != replication_manager.primary_connection_fd ||
            output == NULL ||
            output_capacity <= 0) {
            return -1;
        }

        /* 尚未开始 RDMA 同步时，继续正常接收 TCP 控制消息。 */
        if (replication_manager.rdma_sync.connection == NULL) {
            return 0;
        }

        kvs_rdma_event_t event = {0};

        int result = kvs_rdma_connection_poll_event(
            replication_manager.rdma_sync.connection,
            &event,
            KVS_REPLICATION_RDMA_BUFFER_CAPACITY,
            1);

            if (result < 0) {
                kvs_log_message(
                    KVS_LOG_ERROR,
                    "replication: Replica RDMA CM poll failed host=%s port=%u: %s\n",
                    replication_manager.primary_host,
                    (unsigned int)replication_manager.primary_rdma_port,
                    strerror(errno));
                return -1;
            }

            if (result > 0) {
            if (event.type == KVS_RDMA_EVENT_DISCONNECTED) {
                kvs_replication_rdma_sync_t *finished =
                    &replication_manager.rdma_sync;

                /* 只有快照加载成功、最终确认已提交，才属于正常结束。 */
                if (replication_manager.state != KVS_REPLICATION_STATE_CATCH_UP ||
                    finished->snapshot_bytes != replication_manager.snapshot_file_size ||
                    finished->acknowledged_bytes != finished->snapshot_bytes) {
                    kvs_log_message(
                        KVS_LOG_ERROR,
                        "replication: Replica RDMA disconnected before full-sync completion "
                        "received=%lld expected=%lld ack=%lld\n",
                        finished->snapshot_bytes,
                        replication_manager.snapshot_file_size,
                        finished->acknowledged_bytes);
                    return -1;
                }

                if (kvs_rdma_connection_disconnect(finished->connection) < 0 ||
                    kvs_replication_rdma_sync_close(finished) < 0) {
                    return -1;
                }

                kvs_log_message(
                    KVS_LOG_INFO,
                    "replication: Replica RDMA closed, resume TCP catch-up\n");

                return 0;
            }

            if (event.type == KVS_RDMA_EVENT_ESTABLISHED) {
                replication_manager.rdma_sync.established = 1;
                kvs_log_message(
                    KVS_LOG_INFO,
                    "replication: Replica RDMA connection established\n");
                    /* 通过 RDMA 告诉 Primary：我要接收哪一轮快照。 */
                char ready_message[64];

                int length = snprintf(
                    ready_message,
                    sizeof(ready_message),
                    "READY %s\r\n",
                    replication_manager.full_sync_id);

                if (length < 0 ||
                    (size_t)length >= sizeof(ready_message)) {
                    return -1;
                }

                if (kvs_rdma_connection_send(
                        replication_manager.rdma_sync.connection,
                        ready_message,
                        (size_t)length) < 0) {
                    return -1;
                }
                replication_manager.rdma_sync.send_pending = 1;
            }
        }
        kvs_replication_rdma_sync_t *sync =
        &replication_manager.rdma_sync;

    if (!sync->established) {
        return KVS_STREAM_WAIT;
    }
    /* 有新写入的数据，且发送缓冲区空闲时，提交累计确认。 */
    if (!sync->send_pending &&
        sync->snapshot_bytes > sync->acknowledged_bytes) {
        char ack[64];

        int length = snprintf(
            ack,
            sizeof(ack),
            "SNAPSHOT_ACK %lld\r\n",
            sync->snapshot_bytes);

        if (length < 0 ||
            (size_t)length >= sizeof(ack)) {
            return -1;
        }

        if (kvs_rdma_connection_send(
                sync->connection,
                ack,
                (size_t)length) < 0) {
            return -1;
        }

        sync->send_pending = 1;
        sync->acknowledged_bytes = sync->snapshot_bytes;

        kvs_log_message(
            KVS_LOG_DEBUG,
            "replication: Replica submitted Snapshot ACK bytes=%lld\n",
            sync->acknowledged_bytes);
    }
    /* 最终 ACK 已提交，保留缓冲区，等待 Primary 正常断开。 */
    if (replication_manager.state == KVS_REPLICATION_STATE_CATCH_UP &&
        sync->snapshot_bytes == replication_manager.snapshot_file_size &&
        sync->acknowledged_bytes == sync->snapshot_bytes) {
        return KVS_STREAM_WAIT;
    }
    kvs_rdma_completion_t completion = {0};

    int completed = kvs_rdma_connection_poll_completion(
        sync->connection,
        &completion);

    if (completed < 0) {
        return -1;
    }

    if (completed == 0) {
        return KVS_STREAM_WAIT;
    }

    /* READY 发送完成，不是收到快照数据。 */
    if (completion.type == KVS_RDMA_COMPLETION_SEND) {
        sync->send_pending = 0;
        kvs_log_message(
            KVS_LOG_INFO,
            "replication: Replica RDMA send completed\n");

        return KVS_STREAM_WAIT;
    }

    /* 每块数据的长度必须在快照剩余范围内。 */
    if (completion.type != KVS_RDMA_COMPLETION_RECV ||
        replication_manager.state != KVS_REPLICATION_STATE_FULL_SYNC ||
        completion.length == 0 ||
        completion.length > KVS_REPLICATION_RDMA_BUFFER_CAPACITY ||
        completion.length > (unsigned long long)
            replication_manager.snapshot_writer.remaining) {
        return -1;
    }

    /* 复用现有接口，将收到的完整消息写入快照临时文件。 */
    int written = kvs_replication_receive_snapshot_data(
        connection_fd,
        completion.data,
        (int)completion.length);

    if (written != (int)completion.length) {
        return -1;
    }

    sync->snapshot_bytes += written;

    /* 数据使用完毕，再重新投递接收。 */
    if (kvs_rdma_connection_receive_next(sync->connection) < 0) {
        return -1;
    }

    kvs_log_message(
        KVS_LOG_DEBUG,
        "replication: Replica wrote Snapshot chunk "
        "bytes=%d remaining=%lld\n",
        written,
        replication_manager.snapshot_writer.remaining);
    return KVS_STREAM_WAIT;
}

int kvs_replication_build_connector_config(
    kvs_connector_config_t *connector)
{
    if (connector == NULL ||
        !replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_REPLICA ||
        replication_manager.primary_host[0] == '\0' ||
        replication_manager.primary_port == 0) {
        return -1;
    }

    connector->host = replication_manager.primary_host;                // 连接目标地址
    connector->port = replication_manager.primary_port;                // 连接目标端口
    connector->stream_handler = kvs_replication_upstream_stream;
    connector->open_handler =
        kvs_replication_on_primary_connected;                           // 连接成功后生成 PING
    connector->message_handler =
        kvs_replication_upstream_network_protocol;                      // 收到数据后处理 PONG
    connector->close_handler =
        kvs_replication_on_primary_disconnected;                        // 断开后恢复状态

    return 0;
}

static struct sockaddr_un capture_address;

int kvs_replication_set_capture_socket(const char *path)
{
    if (!replication_manager.initialized || replication_manager.role != KVS_ROLE_PRIMARY ||
        path == NULL) {
        errno = EINVAL;
        return -1;
    }
    return kvs_capture_socket_address(&capture_address, path);
}

static int capture_fd = -1;
static int capture_begin_pending;
static int capture_request_pending;
static time_t capture_wait_started = -1;

static void kvs_replication_capture_close(void)
{
    if (capture_fd >= 0) {
        close(capture_fd);
    }
    capture_fd = -1;
    capture_begin_pending = 0;
    capture_request_pending = 0;
    capture_wait_started = -1;
}

/* 先把边界放进控制连接，快照加载完成后再等待采集端确认。 */
static int kvs_replication_capture_begin(uint64_t offset)
{
    capture_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (capture_fd < 0 ||
        connect(capture_fd, (struct sockaddr *)&capture_address, sizeof(capture_address)) < 0) {
        return -1;
    }

    struct kvs_capture_read_request request = {
        .offset = offset, .operation = KVS_CAPTURE_BEGIN
    };
    if (kvs_capture_packet(capture_fd, &request, sizeof(request), 1) != sizeof(request)) {
        return -1;
    }
    capture_begin_pending = 1;
    return 0;
}

/* 有未完成的请求或采集落后时，等待有限时间后断开复制。 */
static int kvs_replication_capture_wait(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    if (capture_wait_started < 0) {
        capture_wait_started = now.tv_sec;
    } else if (now.tv_sec - capture_wait_started >= 5) {
        errno = ETIMEDOUT;
        return -1;
    }
    return KVS_STREAM_WAIT;
}

static int kvs_replication_capture_read(uint64_t offset, char *output, size_t capacity)
{
    if (capture_begin_pending) {
        int32_t status;
        ssize_t received = kvs_capture_packet(capture_fd, &status, sizeof(status), 0);
        if (received < 0 && errno == EAGAIN) {
            return kvs_replication_capture_wait();
        }
        if (received != sizeof(status) || status != 0) {
            if (received >= 0) {
                errno = received == 0 ? ECONNRESET : ENODATA;
            }
            return -1;
        }
        capture_begin_pending = 0;
        capture_wait_started = -1;
    }

    uint32_t limit = capacity > KVS_CAPTURE_READ_BYTES ?
                     KVS_CAPTURE_READ_BYTES : (uint32_t)capacity;
    if (!capture_request_pending) {
        struct kvs_capture_read_request request = { .offset = offset, .limit = limit };
        ssize_t sent = kvs_capture_packet(capture_fd, &request, sizeof(request), 1);
        if (sent != sizeof(request)) {
            if (sent < 0 && errno == EAGAIN) {
                return kvs_replication_capture_wait();
            }
            return -1;
        }
        capture_request_pending = 1;
    }

    struct kvs_capture_read_response response;
    ssize_t received = kvs_capture_packet(capture_fd, &response, sizeof(response), 0);
    if (received < 0 && errno == EAGAIN) {
        return kvs_replication_capture_wait();
    }
    capture_request_pending = 0;
    if (received < 0) {
        return -1;
    }
    if (received == 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (received < (ssize_t)sizeof(response.length) ||
        response.length > (int32_t)limit ||
        response.length < KVS_CAPTURE_READ_ERROR ||
        received != (ssize_t)sizeof(response.length) +
                    (response.length > 0 ? response.length : 0)) {
        errno = EPROTO;
        return -1;
    }
    if (response.length == KVS_CAPTURE_READ_ERROR) {
        errno = ENODATA;
        return -1;
    }
    if (offset > replication_stream_end ||
        (uint64_t)response.length > replication_stream_end - offset) {
        errno = EPROTO;
        return -1;
    }
    if (response.length == 0 && offset < replication_stream_end) {
        return kvs_replication_capture_wait();
    }
    capture_wait_started = -1;
    memcpy(output, response.data, (size_t)response.length);
    return response.length;
}

/* Replica 加载完毕后，从快照边界开始取增量。 */
static int kvs_replication_primary_start_catch_up(int connection_fd)
{
    replication_manager.send_stream_offset =
        replication_manager.snapshot_stream_offset;
    replication_manager.state = KVS_REPLICATION_STATE_CATCH_UP;

    if (capture_fd >= 0) {
        kvs_log_message(KVS_LOG_INFO,
                        "replication: start capture catch-up at offset=%" PRIu64 "\n",
                        replication_manager.send_stream_offset);
        return 0;
    }

    if (replication_cache.data == NULL) {
        kvs_log_message(KVS_LOG_ERROR,
                        "replication: catch-up cache unavailable at offset=%" PRIu64
                        " fd=%d\n",
                        replication_manager.send_stream_offset, connection_fd);
        return -1;
    }

    kvs_log_message(KVS_LOG_INFO,
                    "replication: start cache catch-up range=[%" PRIu64 ",%" PRIu64
                    ") fd=%d\n",
                    replication_cache.start, replication_stream_end, connection_fd);
    return 0;
}

static int kvs_replication_primary_cache_stream(
    int connection_fd, char *output, int output_capacity)
{
    uint64_t offset = replication_manager.send_stream_offset;

    int bytes;
    if (capture_fd >= 0) {
        bytes = kvs_replication_capture_read(offset, output, (size_t)output_capacity);
        if (bytes == KVS_STREAM_WAIT) {
            return KVS_STREAM_WAIT;
        }
    } else {
        bytes = kvs_replication_cache_read(offset, output, (size_t)output_capacity);
        if (bytes >= 0) {
            /* NtyCo 只在上一次 TCP 发送成功后再次调用 stream。 */
            replication_cache.length -= (size_t)(offset - replication_cache.start);
            replication_cache.start = offset;
        }
    }
    if (bytes < 0) {
        kvs_log_message(KVS_LOG_ERROR,
                        "replication: cache read failed at offset=%" PRIu64 " fd=%d: %s\n",
                        offset, connection_fd, strerror(errno));
        return -1;
    }

    if (bytes > 0) {
        replication_manager.send_stream_offset += (uint64_t)bytes;
        return bytes;
    }
    if (replication_manager.state == KVS_REPLICATION_STATE_ONLINE) {
        return KVS_STREAM_WAIT;
    }


    int length = snprintf(output, output_capacity, "ONLINE %" PRIu64 "\r\n", offset);
    if (length < 0 || length >= output_capacity) {
        return -1;
    }

    replication_manager.state = KVS_REPLICATION_STATE_ONLINE;
    kvs_log_message(KVS_LOG_INFO,
                    "replication: cache catch-up completed, "
                    "enter ONLINE offset=%" PRIu64 " fd=%d\n",
                    offset, connection_fd);
    return length;
}

/* 首块和后续块共用，按 RDMA 注册容量读取并提交快照块。 */
static int kvs_replication_rdma_send_snapshot_chunk(
    kvs_replication_rdma_sync_t *sync)
{
    if (sync->send_pending) {
        return KVS_STREAM_WAIT;
    }

    char *chunk = kvs_malloc(KVS_REPLICATION_RDMA_BUFFER_CAPACITY);
    if (chunk == NULL) {
        return -1;
    }

    int bytes = kvs_snapshot_reader_read(
        &replication_manager.snapshot_reader,
        chunk,
        (int)KVS_REPLICATION_RDMA_BUFFER_CAPACITY);

    if (bytes <= 0 ||
        kvs_rdma_connection_send(
            sync->connection, chunk, (size_t)bytes) < 0) {
        int saved_errno = errno;
        kvs_free(chunk);
        errno = saved_errno;
        return -1;
    }

    /* send 已复制到注册缓冲区，临时读取块可以释放。 */
    kvs_free(chunk);

    sync->send_pending = 1;
    sync->snapshot_bytes += bytes;

    kvs_log_message(
        KVS_LOG_DEBUG,
        "replication: Primary submitted Snapshot chunk "
        "bytes=%d remaining=%lld\n",
        bytes,
        replication_manager.snapshot_reader.remaining);

    return KVS_STREAM_WAIT;
}

int kvs_replication_stream(
    int connection_fd,
    char *output,
    int output_capacity)
{
    if(!replication_manager.initialized ||
       replication_manager.role != KVS_ROLE_PRIMARY ||
       connection_fd<0 ||
       output==NULL ||
       output_capacity<=0){
        return -1;
    }

    if(replication_manager.snapshot_connection_fd<0){
        return 0; // 当前还没有 PSYNC 产生的 Snapshot 发送任务
    }

    if(connection_fd !=
       replication_manager.snapshot_connection_fd){
        return 0; // 当前 Snapshot 不属于这条连接
    }
        /* RDMA 全量同步：推进建连、分块发送和确认，不从 TCP 输出快照。 */
        if (replication_manager.state == KVS_REPLICATION_STATE_FULL_SYNC) {
            if (replication_manager.rdma_cleanup_connection != NULL) {
                return -1;
            }

            kvs_rdma_event_t event = {0};

            int result = kvs_rdma_listener_poll(
                replication_manager.rdma_listener,
                &replication_manager.rdma_sync.connection,
                &event,
                KVS_REPLICATION_RDMA_BUFFER_CAPACITY,
                1);

            if (result < 0) {
                kvs_log_message(
                    KVS_LOG_ERROR,
                    "replication: Primary RDMA CM poll failed port=%u: %s\n",
                    (unsigned int)replication_manager.rdma_port,
                    strerror(errno));

                if (event.connection != NULL &&
                    event.connection != replication_manager.rdma_sync.connection) {
                    replication_manager.rdma_cleanup_connection =
                        event.connection;
                }
                return -1;
            }

            if (result > 0) {
                if (event.type == KVS_RDMA_EVENT_DISCONNECTED) {
                    kvs_replication_rdma_sync_t *finished =
                        &replication_manager.rdma_sync;

                    if (!finished->disconnect_pending ||
                        finished->send_pending ||
                        finished->snapshot_bytes != replication_manager.snapshot_file_size ||
                        finished->acknowledged_bytes != finished->snapshot_bytes) {
                        kvs_log_message(
                            KVS_LOG_ERROR,
                            "replication: Primary RDMA disconnected before full-sync completion "
                            "submitted=%lld expected=%lld ack=%lld\n",
                            finished->snapshot_bytes,
                            replication_manager.snapshot_file_size,
                            finished->acknowledged_bytes);
                        return -1;
                    }

                    if (kvs_replication_rdma_sync_close(finished) < 0 ||
                        kvs_replication_primary_start_catch_up(connection_fd) < 0) {
                        return -1;
                    }

                    kvs_log_message(
                        KVS_LOG_INFO,
                        "replication: Primary RDMA closed, start TCP catch-up\n");
                }

                if (event.type == KVS_RDMA_EVENT_ESTABLISHED) {
                    replication_manager.rdma_sync.established = 1;
                    kvs_log_message(
                        KVS_LOG_INFO,
                        "replication: Primary RDMA connection established\n");
                }
            }
    if (replication_manager.state == KVS_REPLICATION_STATE_FULL_SYNC) {
    kvs_replication_rdma_sync_t *sync =
        &replication_manager.rdma_sync;

    /* 断开已提交，继续等待 CM，暂停处理数据 CQ。 */
    if (sync->disconnect_pending) {
        return KVS_STREAM_WAIT;
    }
    /* 连接建立后，才开始检查数据收发完成通知。 */
    if (!sync->established) {
        return KVS_STREAM_WAIT;
    }
    /* 累计确认已到，且本地 SEND 完成后，才读取下一块。 */
    if (!sync->send_pending &&
    sync->snapshot_bytes > 0 &&
    sync->acknowledged_bytes == sync->snapshot_bytes &&
    replication_manager.snapshot_reader.fd >= 0) {

    if (replication_manager.snapshot_reader.remaining > 0) {
        return kvs_replication_rdma_send_snapshot_chunk(
            sync);
    }

    /* 最终 ACK 和本地 SEND 都已完成，发起正常断开。 */
    if (kvs_rdma_connection_disconnect(sync->connection) < 0) {
        return -1;
    }

    sync->disconnect_pending = 1;

    kvs_replication_remove_primary_snapshot();

    kvs_log_message(
        KVS_LOG_INFO,
        "replication: RDMA Snapshot fully acknowledged bytes=%lld\n",
        sync->snapshot_bytes);

    return KVS_STREAM_WAIT;
    }

    kvs_rdma_completion_t completion = {0};

    int completed = kvs_rdma_connection_poll_completion(
        sync->connection,
        &completion);

    if (completed < 0) {
        return -1;
    }

    if (completed == 0) {
        return KVS_STREAM_WAIT;
    }

    if (completion.type == KVS_RDMA_COMPLETION_SEND) {
        sync->send_pending = 0;
    kvs_log_message(
        KVS_LOG_DEBUG,
        "replication: Primary Snapshot chunk send completed\n");

    return KVS_STREAM_WAIT;
    }

    /* 当前 Primary 等待的第一条消息必须是 READY。 */
    if (completion.type != KVS_RDMA_COMPLETION_RECV) {
        return -1;
    }

    /* 已发送过快照块，此时接收累计字节确认。 */
    if (sync->snapshot_bytes > 0) {
        char expected_ack[64];

        int ack_length = snprintf(
            expected_ack,
            sizeof(expected_ack),
            "SNAPSHOT_ACK %lld\r\n",
            sync->snapshot_bytes);

        if (ack_length < 0 ||
            (size_t)ack_length >= sizeof(expected_ack) ||
            sync->acknowledged_bytes >= sync->snapshot_bytes ||
            completion.length != (size_t)ack_length ||
            memcmp(completion.data, expected_ack,
            (size_t)ack_length) != 0) {
            kvs_log_message(
            KVS_LOG_ERROR,
            "replication: invalid Snapshot ACK received\n");
            return -1;
        }

        if (kvs_rdma_connection_receive_next(sync->connection) < 0) {
            return -1;
        }

        sync->acknowledged_bytes = sync->snapshot_bytes;

        kvs_log_message(
            KVS_LOG_DEBUG,
            "replication: Primary received Snapshot ACK bytes=%lld\n",
            sync->acknowledged_bytes);

        return KVS_STREAM_WAIT;
    }

    char expected[64];

    int expected_length = snprintf(
        expected,
        sizeof(expected),
        "READY %s\r\n",
        replication_manager.full_sync_id);

    if (sync->snapshot_bytes != 0 ||
        expected_length < 0 ||
        (size_t)expected_length >= sizeof(expected) ||
        completion.length != (size_t)expected_length ||
        memcmp(completion.data, expected,
            (size_t)expected_length) != 0) {
        return -1;
    }

    /* 消息已使用完，重新准备接收。 */
    if (kvs_rdma_connection_receive_next(sync->connection) < 0) {
        return -1;
    }

    kvs_log_message(
        KVS_LOG_INFO,
        "replication: Primary verified RDMA READY\n");
    return kvs_replication_rdma_send_snapshot_chunk(sync);
        }
    }
    if (replication_manager.state != KVS_REPLICATION_STATE_CATCH_UP &&
        replication_manager.state != KVS_REPLICATION_STATE_ONLINE) {
        return 0;
    }
    return kvs_replication_primary_cache_stream(connection_fd, output, output_capacity);
}

static int kvs_replication_frame_protocol(
    int connection_fd,
    char *frame,
    int frame_length,
    char *response,
    int response_capacity)
{
    if (connection_fd < 0 ||          // 复制连接的 socket 必须有效
        frame == NULL ||              // 单条复制命令不能为空
        frame_length <= 0 ||          // 命令长度必须有效
        response == NULL ||           // 响应缓冲区不能为空
        response_capacity <= 0) {     // 响应缓冲区必须有剩余空间
        return -1;
    }

    int response_length = -1;
    int is_full_resync_response=0; // 标记本次是否生成了 RDMA 全量通知
    if (frame_length == 4 &&
        memcmp(frame, "PING", 4) == 0) { // 处理一条完整的 PING
        response_length = snprintf(
            response,
            response_capacity,
            "PONG\r\n"
        );
    } else if(frame_length == 10 &&
             memcmp(frame,"PSYNC ? -1",10) == 0){

        if(!replication_manager.initialized ||
           replication_manager.role != KVS_ROLE_PRIMARY ||
           replication_manager.replication_id[0] == '\0'){
            return -1;
        }
        /* 当前只管理一轮 RDMA 同步，不能被第二个 PSYNC 覆盖。 */
        if (replication_manager.snapshot_connection_fd >= 0 ||
            replication_manager.rdma_sync.connection != NULL ||
            replication_manager.rdma_cleanup_connection != NULL) {
            errno = EBUSY;
            return -1;
        }
        if (kvs_replication_remove_primary_snapshot() < 0) {
            return -1;
        }

        /* 快照生成和写入命令在同一调度线程执行，保存前取值固定本轮边界。 */
        uint64_t snapshot_stream_offset = replication_stream_end;
        if (capture_address.sun_path[0] != '\0' &&
            kvs_replication_capture_begin(snapshot_stream_offset) < 0) {
            kvs_log_message(KVS_LOG_ERROR, "replication: start capture session: %s\n",
                            strerror(errno));
            goto snapshot_setup_failed;
        }

        if (kvs_snapshot_store_save_with_checksum(
                KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH, KVS_SNAPSHOT_CHECKSUM_FILE) < 0) {
            goto snapshot_setup_failed;
        }

        struct stat snapshot_stat;
        if (stat(KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                 &snapshot_stat) < 0 ||
            !S_ISREG(snapshot_stat.st_mode) ||
            snapshot_stat.st_size <= 0) {
            goto snapshot_setup_failed;
        }

        long long snapshot_file_size = (long long)snapshot_stat.st_size;

        if(kvs_snapshot_reader_open(
                &replication_manager.snapshot_reader,
                KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                snapshot_file_size) < 0){
            goto snapshot_setup_failed;
        }
        /* 每轮全量同步生成独立 ID，用于匹配控制连接和 RDMA 连接。 */
        if (kvs_replication_generate_id(
                replication_manager.full_sync_id,
                sizeof(replication_manager.full_sync_id)) < 0) {
            goto snapshot_setup_failed;
        }


        replication_manager.snapshot_file_size =snapshot_file_size;
        replication_manager.snapshot_stream_offset = snapshot_stream_offset;
        if (capture_fd < 0) {
            /* 普通 socket 模式由 Primary 持有缓存。 */
            replication_cache.start = snapshot_stream_offset;
            replication_cache.data = kvs_malloc(KVS_REPLICATION_CACHE_BYTES);
            if (replication_cache.data == NULL) {
                kvs_log_message(KVS_LOG_ERROR, "replication: stream cache allocation failed\n");
                goto snapshot_setup_failed;
            }
        }
        replication_manager.snapshot_connection_fd =connection_fd; // Snapshot 只能由当前发来 PSYNC 的连接读取
        replication_manager.state = KVS_REPLICATION_STATE_FULL_SYNC;

        /* TCP 发送全量启动通知，快照内容通过 RDMA 传输。 */
        response_length = snprintf(
            response,
            response_capacity,
            "FULLRESYNC_RDMA %s %lld %u %s %" PRIu64 "\r\n",
            replication_manager.replication_id,
            replication_manager.snapshot_file_size,
            (unsigned int)replication_manager.rdma_port,
            replication_manager.full_sync_id,
            snapshot_stream_offset);

        kvs_log_message(KVS_LOG_DEBUG,
                        "replication: snapshot stream offset=%" PRIu64 "\n",
                        replication_manager.snapshot_stream_offset);
        is_full_resync_response = 1;
    }else{
        response_length=snprintf(
            response,
            response_capacity,
            "ERROR unknown replication command\r\n"
        );
    }

    if (response_length < 0 ||
        response_length >= response_capacity) { // 响应写入失败或者空间不足
        return -1;
    }
   if(is_full_resync_response){
        kvs_log_message(
            KVS_LOG_DEBUG,
            "replication: FULLRESYNC_RDMA response ready id=%s offset=%" PRIu64 " snapshot_size=%lld fd=%d\n",
            replication_manager.replication_id,
            replication_manager.snapshot_stream_offset,
            replication_manager.snapshot_file_size,
            connection_fd
        );
    }

    return response_length;

snapshot_setup_failed:
    {
        int saved_errno = errno;
        kvs_replication_remove_primary_snapshot();
        kvs_replication_capture_close();
        errno = saved_errno;
        return -1;
    }
}



int kvs_replication_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    kvs_output_buffer_t *response,
    int *consumed_length)
{

    if (response == NULL ||
    response->data == NULL ||
    response->capacity <= 0) {
    return -1;
}
    return kvs_line_batch_protocol(
        connection_fd,                        // 当前 Replica 连接
        msg,                                  // TCP 输入缓冲区
        length,                               // 当前收到的数据长度
        response->data,                             // 响应缓冲区
        response->capacity,                    // 响应缓冲区容量
        consumed_length,                      // 本次处理掉的输入字节数
        kvs_replication_frame_protocol        // 每条完整命令交给复制处理器
    );
}

static int kvs_replication_generate_id(
    char *output,
    size_t output_capacity)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random_bytes[KVS_REPLICATION_ID_HEX_LENGTH / 2];
    size_t received_length = 0;

    if(output == NULL ||
       output_capacity < KVS_REPLICATION_ID_HEX_LENGTH + 1){
        return -1;
    }

    // getrandom 也可能只返回部分数据，因此循环读取满 20 字节
    while(received_length < sizeof(random_bytes)){
        ssize_t result = getrandom(
            random_bytes + received_length,
            sizeof(random_bytes) - received_length,
            0
        );

        if(result > 0){
            received_length += (size_t)result;
            continue;
        }

        if(result < 0 && errno == EINTR){
            continue; // 被信号中断，不算真正失败，继续读取
        }

        return -1;
    }

    // 一个字节拆成高 4 位和低 4 位，分别转换成两个十六进制字符
    for(size_t i = 0;i < sizeof(random_bytes);i++){
        output[i * 2] = hex[random_bytes[i] >> 4];
        output[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }

    output[KVS_REPLICATION_ID_HEX_LENGTH] = '\0';
    return 0;
}

int kvs_replication_init(
    kvs_role_t role,
    const char *primary_host,
    unsigned short primary_port,
    kvs_snapshot_install_handler snapshot_install_handler,
    kvs_replication_command_handler command_handler
)
{
    if (command_handler == NULL||replication_manager.initialized) { // 配置必须存在，并且不能重复初始化
        return -1;
    }

    if (role != KVS_ROLE_STANDALONE &&
        role != KVS_ROLE_PRIMARY &&
        role != KVS_ROLE_REPLICA) {                  // 检查角色是否合法
        return -1;
    }

    size_t primary_host_length = 0;
    char generated_replication_id[KVS_REPLICATION_ID_HEX_LENGTH + 1] = {0};
    if (role == KVS_ROLE_REPLICA) {
        if (primary_host == NULL ||
            primary_host[0] == '\0' ||
            primary_port == 0) {                 // Replica 必须具有有效的 Primary 地址和端口
            return -1;
        }

        primary_host_length = strlen(primary_host);
        if (primary_host_length >=
            sizeof(replication_manager.primary_host)) {      // 地址必须能够放入管理器
            return -1;
        }
    }

    if(role == KVS_ROLE_PRIMARY){
    if(kvs_replication_generate_id(
            generated_replication_id,
            sizeof(generated_replication_id)) < 0){
        return -1; // Primary 无法生成身份时不继续启动复制模块
    }
}


    replication_manager.role = role;
    replication_manager.snapshot_install_handler =snapshot_install_handler;
    replication_manager.command_handler = command_handler;
    replication_manager.snapshot_stream_offset = 0;
    replication_manager.snapshot_file_size = -1; // 初始化时还不知道全量快照大小
    replication_manager.snapshot_connection_fd = -1; // 初始化时没有 Snapshot 接收连接
    replication_manager.snapshot_reader.fd = -1;       // 当前没有打开复制快照
    replication_manager.snapshot_reader.remaining = 0; // 当前没有待发送字节
    if(role == KVS_ROLE_PRIMARY){
    memcpy(
        replication_manager.replication_id,
        generated_replication_id,
        sizeof(generated_replication_id)
    );
    }else{
        replication_manager.replication_id[0] = '\0'; // Replica 等待 Primary 返回 ID
    }
    replication_manager.primary_connection_fd = -1;          // 当前尚未连接 Primary
    replication_manager.state = KVS_REPLICATION_STATE_DISCONNECTED;

    if (role == KVS_ROLE_REPLICA) {
        memcpy(
            replication_manager.primary_host,
            primary_host,
            primary_host_length + 1                           // 连同字符串末尾的 \0 一起复制
        );
        replication_manager.primary_port = primary_port;
    } else {
        replication_manager.primary_host[0] = '\0';           // Primary 和单机没有上游地址
        replication_manager.primary_port = 0;
    }

    replication_manager.initialized = 1;

    return 0;
}

void kvs_replication_on_replica_disconnected(int connection_fd)
{
    if (!replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_PRIMARY ||
        connection_fd < 0 ||
        replication_manager.snapshot_connection_fd != connection_fd) {
        return;
    }

    /* Replica 控制连接断开，取消对应的 RDMA 全量同步。 */
    if (kvs_replication_rdma_sync_close(
            &replication_manager.rdma_sync) < 0) {
        kvs_log_message(
            KVS_LOG_ERROR,
            "replication: failed to close Primary RDMA connection: %s\n",
            strerror(errno));
    }

replication_manager.full_sync_id[0] = '\0';
    kvs_replication_remove_primary_snapshot();
    kvs_replication_cache_clear();
    kvs_replication_capture_close();

    replication_manager.snapshot_connection_fd = -1;
    replication_manager.snapshot_stream_offset = 0;
    replication_manager.snapshot_file_size = -1;
    replication_manager.state = KVS_REPLICATION_STATE_DISCONNECTED;
        kvs_log_message(
        KVS_LOG_INFO,
        "replication: Replica control connection released fd=%d\n",
        connection_fd);
}

int kvs_replication_is_online(void)
{
    return replication_manager.initialized &&
           replication_manager.role == KVS_ROLE_REPLICA &&
           replication_manager.primary_connection_fd >= 0 &&
           replication_manager.state == KVS_REPLICATION_STATE_ONLINE;
}

void kvs_replication_destroy(void)
{
    if (!replication_manager.initialized) {                   // 未初始化就不需要销毁
        return;
    }
    if (replication_manager.role == KVS_ROLE_PRIMARY) {
        kvs_replication_remove_primary_snapshot();
        kvs_replication_cache_clear();
        kvs_replication_capture_close();
        capture_address = (struct sockaddr_un){0};
    }
    if (kvs_rdma_connection_close(
        &replication_manager.rdma_cleanup_connection) < 0) {
    kvs_log_message(
        KVS_LOG_ERROR,
        "replication: failed to close extra RDMA connection: %s\n",
        strerror(errno));
    return;
}
    /* 先释放连接，再释放连接可能共用的监听器资源。 */
    if (kvs_replication_rdma_sync_close(
            &replication_manager.rdma_sync) < 0) {
        kvs_log_message(
            KVS_LOG_ERROR,
            "replication: failed to close RDMA connection: %s\n",
            strerror(errno));
        return;
    }
    if (kvs_rdma_listener_close(&replication_manager.rdma_listener) < 0) {
        kvs_log_message(
            KVS_LOG_ERROR,
            "replication: failed to close RDMA listener: %s\n",
            strerror(errno));
        return;
    }
    replication_manager.rdma_port = 0;
    replication_manager.role = KVS_ROLE_STANDALONE;           // 恢复安全默认角色
    kvs_snapshot_reader_close(&replication_manager.snapshot_reader); // 如果正在发送 Snapshot，关闭对应文件
    kvs_snapshot_writer_abort(&replication_manager.snapshot_writer,KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH);
    replication_manager.snapshot_connection_fd = -1; // 销毁时解除 Snapshot 与连接的绑定
    replication_manager.snapshot_stream_offset = 0;
    replication_manager.snapshot_file_size = -1; // 销毁时清除上一次同步留下的大小
    replication_manager.replication_id[0] = '\0';
    replication_manager.primary_host[0] = '\0';               // 清空 Primary 地址
    replication_manager.primary_port = 0;
    replication_manager.primary_connection_fd = -1;           // 当前不存在上游连接
    replication_manager.state = KVS_REPLICATION_STATE_DISCONNECTED;
    replication_manager.initialized = 0;
}

int kvs_replication_start_rdma_listener(
    const char *bind_ip,
    unsigned short port)
{
    if (!replication_manager.initialized ||
        replication_manager.role != KVS_ROLE_PRIMARY ||
        bind_ip == NULL || port == 0) {
        errno = EINVAL;
        return -1;
    }

    if (replication_manager.rdma_listener != NULL) {
        errno = EALREADY;
        return -1;
    }

    replication_manager.rdma_listener = kvs_rdma_listener_create();
    if (replication_manager.rdma_listener == NULL) {
        return -1;
    }

    if (kvs_rdma_listener_start(
            replication_manager.rdma_listener, bind_ip, port, 1) < 0) {
        return -1;
    }

    replication_manager.rdma_port = port;
    return 0;
}
