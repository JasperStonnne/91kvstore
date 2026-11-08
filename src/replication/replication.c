#include "replication.h"
#include "persistence.h"
#include "server.h"
#include "log.h"
#include "rdma_transport.h"
#ifdef KVS_EBPF_STARTUP_EXPERIMENT
#include "ebpf_capture.h"
#endif
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/random.h>
#include <errno.h>
#include <stdlib.h>
#define KVS_REPLICATION_HOST_MAX_LENGTH 256
#define KVS_REPLICATION_ID_HEX_LENGTH 40 // 20 字节随机数转换成 40 个十六进制字符
// Primary 为全量同步生成并发送的 Snapshot
#define KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH "replication.snapshot"
// Primary 在 Snapshot 之后从这个 AOF 文件读取增量命令
#define KVS_REPLICATION_PRIMARY_AOF_PATH "appendonly.aof"
// Replica 接收期间写入的临时文件，未收完整时不能使用
#define KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH "snapshot.db.replication.tmp"
// Replica 完整接收后安装成正式的本地 Snapshot
#define KVS_REPLICATION_REPLICA_SNAPSHOT_PATH "snapshot.db"
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
    long long replication_offset;  // 当前复制进度
#ifdef KVS_EBPF_STARTUP_EXPERIMENT
    /* length=0 表示没有待发事件；分段期间保留完整命令及已交出字节数。 */
    struct kvs_ebpf_event pending_event;
    size_t pending_event_sent;
    struct {
        unsigned long long payload_events;
        unsigned long long payload_bytes;
        unsigned long long duplicate_events;
        unsigned long long compensation_ranges;
        unsigned long long compensation_bytes;
        struct kvs_ebpf_failures observed_failures;
    } ebpf_stats;
#endif
    long long snapshot_file_size; // 当前全量同步 Snapshot 文件的总字节数
    kvs_snapshot_reader_t snapshot_reader; // Primary 当前正在分块读取的复制快照
    kvs_aof_reader_t aof_reader; // Primary 当前正在分块读取的增量 AOF
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
    .replication_offset = 0,
    .snapshot_file_size = -1,
    .snapshot_reader = {
    .fd = -1,
    .remaining = 0},
    .aof_reader = {
    .fd = -1,
    .offset = 0,
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

#ifdef KVS_EBPF_STARTUP_EXPERIMENT
/* 仅在捕获模块发生错误时，通知外层显式改走原有 AOF ONLINE 分支。 */
#define KVS_EBPF_AOF_FALLBACK (-3)
#define KVS_EBPF_DRAIN_BUDGET 64

static void kvs_replication_ebpf_reset(void)
{
    replication_manager.pending_event.length = 0;
    replication_manager.pending_event_sent = 0;
    memset(&replication_manager.ebpf_stats, 0,
           sizeof(replication_manager.ebpf_stats));
}

static int kvs_replication_ebpf_fallback(int error)
{
    kvs_log_message(
        KVS_LOG_ERROR,
        "replication: eBPF consumer failed error=%d; ONLINE source=AOF\n",
        error);
    kvs_ebpf_stop();
    replication_manager.pending_event.length = 0;
    replication_manager.pending_event_sent = 0;
    return KVS_EBPF_AOF_FALLBACK;
}

/* 缺口两端均为命令边界；保留手上的后续事件，先把缺口补完。 */
static int kvs_replication_ebpf_replay(long long end, const char *reason)
{
    long long start = replication_manager.replication_offset;
    if (kvs_aof_reader_open(&replication_manager.aof_reader,
                           KVS_REPLICATION_PRIMARY_AOF_PATH, start, end) < 0) {
        kvs_log_message(KVS_LOG_ERROR,
                        "replication: AOF compensation open failed range=[%lld,%lld)\n",
                        start, end);
        return -1;
    }
    replication_manager.ebpf_stats.compensation_ranges++;
    kvs_log_message(
        KVS_LOG_INFO,
        "replication: AOF compensation reason=%s range=[%lld,%lld) ranges=%llu\n",
        reason, start, end,
        replication_manager.ebpf_stats.compensation_ranges);
    return 0;
}

/*
 * ONLINE 正常命令只从 eBPF 事件取 payload。
 * AOF 只用于已发现的缺口；读取尾部位置属于对账，不读取正常命令内容。
 * 当前 NtyCo 写入和本函数在同一线程，AOF 成功探针在写入返回前同步完成。
 */
static int kvs_replication_ebpf_stream(char *output, int output_capacity)
{
    for (int attempt = 0; attempt < KVS_EBPF_DRAIN_BUDGET; attempt++) {
        if (replication_manager.aof_reader.fd >= 0) {
            int bytes = kvs_aof_reader_read(&replication_manager.aof_reader,
                                            output, output_capacity);
            if (bytes < 0)
                return -1;
            if (bytes > 0) {
                replication_manager.ebpf_stats.compensation_bytes += bytes;
                /* 最后一块交给网络层时立即提交该补偿区间的边界。 */
                if (replication_manager.aof_reader.remaining == 0) {
                    replication_manager.replication_offset =
                        replication_manager.aof_reader.offset;
                    kvs_aof_reader_close(&replication_manager.aof_reader);
                }
                return bytes;
            }
            replication_manager.replication_offset =
                replication_manager.aof_reader.offset;
            kvs_aof_reader_close(&replication_manager.aof_reader);
        }

        struct kvs_ebpf_event *event = &replication_manager.pending_event;
        if (event->length == 0) {
            /*
             * 先取已提交尾部，再消费队列。即使最后一条丢失、没有后续事件，
             * 读空后仍可用这个边界识别尾部缺口。
             */
            long long committed_end = kvs_aof_get_offset();
            if (committed_end < replication_manager.replication_offset)
                return -1;

            int received = kvs_ebpf_read(event);
            if (received < 0)
                return kvs_replication_ebpf_fallback(received);
            if (received == 0) {
                struct kvs_ebpf_failures failures;
                int status = kvs_ebpf_get_failures(&failures);
                if (status < 0)
                    return kvs_replication_ebpf_fallback(status);
                if (memcmp(&failures,
                           &replication_manager.ebpf_stats.observed_failures,
                           sizeof(failures)) != 0) {
                    kvs_log_message(
                        KVS_LOG_ERROR,
                        "replication: eBPF losses oversize=%llu ring_full=%llu read_error=%llu\n",
                        (unsigned long long)failures.oversize,
                        (unsigned long long)failures.ring_full,
                        (unsigned long long)failures.read_error);
                    replication_manager.ebpf_stats.observed_failures = failures;
                }
                if (committed_end > replication_manager.replication_offset) {
                    if (kvs_replication_ebpf_replay(
                            committed_end, "queue_empty_gap") < 0)
                        return -1;
                    continue;
                }
                return KVS_STREAM_WAIT;
            }
            replication_manager.pending_event_sent = 0;
            if (event->end > (unsigned long long)committed_end)
                return -1;
        }

        if (replication_manager.pending_event_sent == 0) {
            unsigned long long expected =
                (unsigned long long)replication_manager.replication_offset;
            if (event->end <= expected) {
                /* 追赶期已通过 AOF 覆盖的事件，或补偿后到达的旧事件。 */
                replication_manager.ebpf_stats.duplicate_events++;
                event->length = 0;
                continue;
            }
            if (event->start < expected) {
                kvs_log_message(
                    KVS_LOG_ERROR,
                    "replication: overlapping eBPF event range=[%llu,%llu) expected=%llu\n",
                    (unsigned long long)event->start,
                    (unsigned long long)event->end, expected);
                return -1;
            }
            if (event->start > expected) {
                if (kvs_replication_ebpf_replay(
                        (long long)event->start, "event_offset_gap") < 0)
                    return -1;
                continue;
            }
        }

        /* 一条大命令可以分多轮填 output；整条交出后才推进命令边界。 */
        size_t remaining = event->length - replication_manager.pending_event_sent;
        size_t bytes = remaining;
        if (bytes > (size_t)output_capacity)
            bytes = (size_t)output_capacity;
        memcpy(output, event->payload + replication_manager.pending_event_sent, bytes);
        replication_manager.pending_event_sent += bytes;
        kvs_log_message(KVS_LOG_DEBUG,
                        "replication: eBPF chunk bytes=%zu progress=%zu/%u\n",
                        bytes, replication_manager.pending_event_sent, event->length);

        if (replication_manager.pending_event_sent == event->length) {
            replication_manager.replication_offset = (long long)event->end;
            replication_manager.ebpf_stats.payload_events++;
            replication_manager.ebpf_stats.payload_bytes += event->length;
            kvs_log_message(
                KVS_LOG_DEBUG,
                "replication: eBPF payload range=[%llu,%llu) bytes=%u events=%llu total_bytes=%llu\n",
                (unsigned long long)event->start,
                (unsigned long long)event->end, event->length,
                replication_manager.ebpf_stats.payload_events,
                replication_manager.ebpf_stats.payload_bytes);
            event->length = 0;
            replication_manager.pending_event_sent = 0;
        }
        return (int)bytes;
    }
    /* 限制每轮清理旧事件的工作量，让出 CPU 给客户端及其他协程。 */
    return KVS_STREAM_WAIT;
}
#endif

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

        long long primary_offset = -1;
        long long primary_snapshot_file_size = -1;
        unsigned int primary_rdma_port = 0;
        int parsed_length = 0;

        /* 只接受包含 RDMA 端口和本轮同步 ID 的全量通知。 */
        int matched = sscanf(
            frame,
            "FULLRESYNC_RDMA %40[0-9a-f] %lld %lld %5u %40[0-9a-f]%n",
            primary_replication_id,
            &primary_offset,
            &primary_snapshot_file_size,
            &primary_rdma_port,
            full_sync_id,
            &parsed_length);

        if (matched != 5 ||
            parsed_length != frame_length ||
            strlen(primary_replication_id) != KVS_REPLICATION_ID_HEX_LENGTH ||
            strlen(full_sync_id) != KVS_REPLICATION_ID_HEX_LENGTH ||
            primary_offset < 0 ||
            primary_snapshot_file_size <= 0 ||
            primary_rdma_port == 0 ||
            primary_rdma_port > 65535) {
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
        replication_manager.replication_offset=primary_offset;
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
    "replication: enter FULL_SYNC id=%s offset=%lld snapshot_size=%lld fd=%d\n",
    replication_manager.replication_id,
    replication_manager.replication_offset,
    replication_manager.snapshot_file_size,
    connection_fd
);
        return 0;// 已保存全量同步起点，当前暂时没有消息需要回发
    }
    return -1; // 握手阶段收到未知的 Primary 响应
}
/*
 * 处理 Primary 发来的单条增量 AOF 命令。
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
        /*
     * ONLINE 不是 KV 命令，而是 Primary 发来的状态切换消息。
     * 格式：ONLINE <Primary当前AOF offset>
     */
    if(frame_length>=7 &&
       memcmp(frame,"ONLINE ",7)==0){

        long long primary_online_offset=-1;
        int parsed_length=0;

        int matched=sscanf(
            frame,
            "ONLINE %lld%n",
            &primary_online_offset,
            &parsed_length
        );

        if(matched!=1 ||
           parsed_length!=frame_length ||
           primary_online_offset<0){
            return -1;
        }

        /*
         * 增量命令执行后已经写入 Replica 本地 AOF。
         * 本地 AOF 末尾必须与 Primary 声明的 offset 完全一致。
         */
        long long local_offset=kvs_aof_get_offset();

        if(local_offset<0 ||
           local_offset!=primary_online_offset){
            return -1;
        }

        replication_manager.replication_offset=local_offset;
        replication_manager.state=KVS_REPLICATION_STATE_ONLINE;

        kvs_log_message(
            KVS_LOG_INFO,
            "replication: enter ONLINE offset=%lld fd=%d\n",
            local_offset,
            connection_fd
        );

        return 0; // 消费 ONLINE 消息，不向 Primary 回包
    }
/* KV 增量命令由长度格式解析；这里出现其他文本行就是协议错误。 */
    return -1;
}

/* 执行一条长度格式的复制命令，并更新 Replica 的 AOF offset。 */
static int kvs_replication_catch_up_command_protocol(
    int connection_fd,
    const kvs_slice_t *fields,
    size_t field_count,
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
        response == NULL ||
        response_capacity <= 0) {
        return -1;
    }

    /* 执行字段；成功的写命令会由 kvstore 追加到 Replica 本地 AOF。 */
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

    /* 本地 AOF 已追加这条命令，当前位置就是最新复制进度。 */
    long long current_offset = kvs_aof_get_offset();
    if (current_offset < 0) {
        return -1;
    }

    replication_manager.replication_offset = current_offset;
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
 * Replica 收完整个快照后，提交文件、加载数据并进入增量补齐阶段。
 * connection_fd 是现有复制控制连接的 FD。
 */
static int kvs_replication_finish_full_sync(int connection_fd)
{
    if (replication_manager.state != KVS_REPLICATION_STATE_FULL_SYNC ||
        replication_manager.snapshot_writer.remaining != 0) {
        return -1;
    }

    if (kvs_snapshot_writer_commit(
            &replication_manager.snapshot_writer,
            KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH,
            KVS_REPLICATION_REPLICA_SNAPSHOT_PATH) < 0) {
        kvs_snapshot_writer_abort(
            &replication_manager.snapshot_writer,
            KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH);
        return -1;
    }

    if (replication_manager.snapshot_install_handler == NULL ||
        replication_manager.snapshot_install_handler(
            KVS_REPLICATION_REPLICA_SNAPSHOT_PATH,
            replication_manager.replication_offset) < 0) {
        return -1;
    }

    replication_manager.state = KVS_REPLICATION_STATE_CATCH_UP;

    kvs_log_message(
        KVS_LOG_INFO,
        "replication: Snapshot installed, enter CATCH_UP "
        "offset=%lld size=%lld fd=%d\n",
        replication_manager.replication_offset,
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

    /* AOF 增量命令以 * 开头，按长度字段解析。 */
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
            return -1; /* AOF 中出现格式错误。 */
        }

        /* 完整命令交给字段回调执行，并追加到 Replica 本地 AOF。 */
        if (kvs_replication_catch_up_command_protocol(
                connection_fd,
                fields,
                field_count,
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
            KVS_LOG_INFO,
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
        KVS_LOG_INFO,
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

/* 全量完成后，准备从快照边界 O 开始追赶 AOF。 */
static int kvs_replication_primary_start_catch_up(
    int connection_fd)
{
    kvs_snapshot_reader_close(
        &replication_manager.snapshot_reader);

    long long catch_up_end = kvs_aof_get_offset();

    if (catch_up_end < replication_manager.replication_offset) {
        return -1;
    }

    kvs_aof_reader_close(
        &replication_manager.aof_reader);

    if (kvs_aof_reader_open(
            &replication_manager.aof_reader,
            KVS_REPLICATION_PRIMARY_AOF_PATH,
            replication_manager.replication_offset,
            catch_up_end) < 0) {
        return -1;
    }

    replication_manager.state = KVS_REPLICATION_STATE_CATCH_UP;

    kvs_log_message(
        KVS_LOG_INFO,
        "replication: Snapshot stream completed, "
        "start AOF catch-up range=[%lld,%lld) fd=%d\n",
        replication_manager.replication_offset,
        catch_up_end,
        connection_fd);

    return 0;
}

/* 首块和后续块共用，按 RDMA 注册容量读取并提交快照块。 */
static int kvs_replication_rdma_send_snapshot_chunk(
    kvs_replication_rdma_sync_t *sync)
{
    if (sync->send_pending) {
        return KVS_STREAM_WAIT;
    }

    char *chunk = malloc(KVS_REPLICATION_RDMA_BUFFER_CAPACITY);
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
        free(chunk);
        errno = saved_errno;
        return -1;
    }

    /* send 已复制到注册缓冲区，临时读取块可以释放。 */
    free(chunk);

    sync->send_pending = 1;
    sync->snapshot_bytes += bytes;

    kvs_log_message(
        KVS_LOG_INFO,
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
#ifdef KVS_EBPF_STARTUP_EXPERIMENT
    if (replication_manager.state == KVS_REPLICATION_STATE_ONLINE &&
        kvs_ebpf_is_ready()) {
        int ebpf_result = kvs_replication_ebpf_stream(output, output_capacity);
        if (ebpf_result != KVS_EBPF_AOF_FALLBACK)
            return ebpf_result;
    }
#endif
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

    kvs_snapshot_reader_close(
        &replication_manager.snapshot_reader);

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
        KVS_LOG_INFO,
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
            KVS_LOG_INFO,
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
    int result=0;
    /*
     * Snapshot reader 已关闭、AOF reader 已打开后，
     * 从固定的增量区间分块读取数据。
     */
    while(replication_manager.aof_reader.fd>=0){
        result=kvs_aof_reader_read(
            &replication_manager.aof_reader,
            output,
            output_capacity
        );

        if(result<0){
            kvs_aof_reader_close(
                &replication_manager.aof_reader
            );
            return -1;
        }

        if(result>0){
            return result; // 网络层发送这一块 AOF，然后再次调用本函数
        }
                /*
         * result==0：本轮固定区间已经读完。
         * reader->offset 就是本轮已经发送到的位置。
         */
        long long sent_offset=
            replication_manager.aof_reader.offset;

        kvs_aof_reader_close(
            &replication_manager.aof_reader
        );

        // 再次取得 Primary 此刻最新的 AOF 末尾。
        long long latest_offset=kvs_aof_get_offset();

        if (latest_offset < sent_offset) {
            return -1;
        }
        if(latest_offset>sent_offset){
        /*
            * 发送上一轮期间 Primary 又产生了新命令，
            * 开启下一轮 [sent_offset, latest_offset)。
            */
        if(kvs_aof_reader_open(
                &replication_manager.aof_reader,
                KVS_REPLICATION_PRIMARY_AOF_PATH,
                sent_offset,
                latest_offset) < 0){

            return -1;
        }

        kvs_log_message(
            KVS_LOG_DEBUG,
            "replication: continue AOF catch-up "
            "range=[%lld,%lld) fd=%d\n",
            sent_offset,
            latest_offset,
            connection_fd
        );

        continue; // 回到 while 顶部，读取下一轮第一块
    }
    replication_manager.replication_offset=sent_offset;
    if(replication_manager.state==KVS_REPLICATION_STATE_ONLINE){
    return KVS_STREAM_WAIT;
    }
            /*
         * latest_offset == sent_offset：
         * 当前没有任何历史增量欠账，通知 Replica 进入 ONLINE。
         */
        int online_length=snprintf(
            output,
            output_capacity,
            "ONLINE %lld\r\n",
            sent_offset
        );

        if(online_length<0 ||
           online_length>=output_capacity){
            return -1;
        }

        replication_manager.replication_offset=sent_offset;
        replication_manager.state=KVS_REPLICATION_STATE_ONLINE;

        kvs_log_message(
            KVS_LOG_INFO,
            "replication: AOF catch-up completed, "
            "enter ONLINE offset=%lld fd=%d\n",
            sent_offset,
            connection_fd
        );

        /*
         * 返回 ONLINE 消息的长度，让让网络层先发送状态切换通知。
         * 下一次调用会进入 ONLINE 轮询，持续检查新的 AOF 数据。
         */
        return online_length;
    }
/*
 * 已经进入 ONLINE，并且当前没有打开的 AOF reader。
 * 检查 Primary 的 AOF 是否在上次已发送位置之后继续增长。
 */
if(replication_manager.state==KVS_REPLICATION_STATE_ONLINE){
    long long latest_offset=kvs_aof_get_offset();

    if(latest_offset<
       replication_manager.replication_offset){
        return -1; // AOF 被截断，原来的复制 offset 已经失效
    }

    if(latest_offset==
       replication_manager.replication_offset){
        return KVS_STREAM_WAIT; // 暂无新写入，通知网络层休眠后重试
    }

    /*
     * AOF 已经增长，打开：
     * [上次已发送位置, 当前 AOF 末尾)
     */
    if(kvs_aof_reader_open(
            &replication_manager.aof_reader,
            KVS_REPLICATION_PRIMARY_AOF_PATH,
            replication_manager.replication_offset,
            latest_offset) < 0){

        return -1;
    }

    /*
     * 立即读取第一块实时增量。
     * 后续块由函数前面的 AOF reader 循环继续读取。
     */
    result=kvs_aof_reader_read(
        &replication_manager.aof_reader,
        output,
        output_capacity
    );

    if(result<=0){
        kvs_aof_reader_close(
            &replication_manager.aof_reader
        );
        return -1;
    }

    return result;
}
    return result;
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
        kvs_snapshot_metadata_t snapshot_metadata = {
            .aof_offset = kvs_aof_get_offset(),
            .file_size = -1
        };

        if (snapshot_metadata.aof_offset < 0) {
            return -1;
        }

        kvs_snapshot_checkpoint_t checkpoint = {
            .has_offset = 1,
            .offset = snapshot_metadata.aof_offset
        };

        if (kvs_snapshot_store_save_with_checkpoint(
                KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                &checkpoint) < 0) {
            return -1;
        }

        struct stat snapshot_stat;
        if (stat(KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                 &snapshot_stat) < 0 ||
            !S_ISREG(snapshot_stat.st_mode) ||
            snapshot_stat.st_size <= 0) {
            return -1;
        }

        snapshot_metadata.file_size =
            (long long)snapshot_stat.st_size;

        kvs_snapshot_reader_close(
            &replication_manager.snapshot_reader
        ); // 清理上一次可能残留的读取任务
        kvs_aof_reader_close(&replication_manager.aof_reader);
        if(kvs_snapshot_reader_open(
                &replication_manager.snapshot_reader,
                KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                snapshot_metadata.file_size) < 0){
            return -1;
        }
        /* 每轮全量同步生成独立 ID，用于匹配控制连接和 RDMA 连接。 */
        if (kvs_replication_generate_id(
                replication_manager.full_sync_id,
                sizeof(replication_manager.full_sync_id)) < 0) {
            return -1;
        }


        replication_manager.replication_offset =snapshot_metadata.aof_offset;
        replication_manager.snapshot_file_size =snapshot_metadata.file_size;
        replication_manager.snapshot_connection_fd =connection_fd; // Snapshot 只能由当前发来 PSYNC 的连接读取
        replication_manager.state = KVS_REPLICATION_STATE_FULL_SYNC;

        /* TCP 发送全量启动通知，快照内容通过 RDMA 传输。 */
        response_length = snprintf(
            response,
            response_capacity,
            "FULLRESYNC_RDMA %s %lld %lld %u %s\r\n",
            replication_manager.replication_id,
            replication_manager.replication_offset,
            replication_manager.snapshot_file_size,
            (unsigned int)replication_manager.rdma_port,
            replication_manager.full_sync_id);

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
            "replication: FULLRESYNC_RDMA response ready id=%s offset=%lld snapshot_size=%lld fd=%d\n",
            replication_manager.replication_id,
            replication_manager.replication_offset,
            replication_manager.snapshot_file_size,
            connection_fd
        );
    }

    return response_length;
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

    long long offset = kvs_aof_get_offset();// 取得本地 AOF 当前末尾位置
        if (offset < 0) {
        return -1;
    }
    if(role == KVS_ROLE_PRIMARY){
    if(kvs_replication_generate_id(
            generated_replication_id,
            sizeof(generated_replication_id)) < 0){
        return -1; // Primary 无法生成身份时不继续启动复制模块
    }
}


#ifdef KVS_EBPF_STARTUP_EXPERIMENT
    kvs_replication_ebpf_reset();
#endif
    replication_manager.role = role;
    replication_manager.replication_offset = offset;
    replication_manager.snapshot_install_handler =snapshot_install_handler;
    replication_manager.command_handler = command_handler;
    replication_manager.snapshot_file_size = -1; // 初始化时还不知道全量快照大小
    replication_manager.snapshot_connection_fd = -1; // 初始化时没有 Snapshot 接收连接
    replication_manager.snapshot_reader.fd = -1;       // 当前没有打开复制快照
    replication_manager.snapshot_reader.remaining = 0; // 当前没有待发送字节
    replication_manager.aof_reader.fd = -1;        // 当前没有打开增量 AOF
    replication_manager.aof_reader.offset = 0;     // 当前没有增量读取位置
    replication_manager.aof_reader.remaining = 0;  // 当前没有待发送的增量字节
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

#ifdef KVS_EBPF_STARTUP_EXPERIMENT
    kvs_log_message(
        KVS_LOG_INFO,
        "replication: eBPF summary payload_events=%llu payload_bytes=%llu "
        "compensation_ranges=%llu compensation_bytes=%llu duplicates=%llu\n",
        replication_manager.ebpf_stats.payload_events,
        replication_manager.ebpf_stats.payload_bytes,
        replication_manager.ebpf_stats.compensation_ranges,
        replication_manager.ebpf_stats.compensation_bytes,
        replication_manager.ebpf_stats.duplicate_events);
    kvs_replication_ebpf_reset();
#endif
    /* Replica 控制连接断开，取消对应的 RDMA 全量同步。 */
    if (kvs_replication_rdma_sync_close(
            &replication_manager.rdma_sync) < 0) {
        kvs_log_message(
            KVS_LOG_ERROR,
            "replication: failed to close Primary RDMA connection: %s\n",
            strerror(errno));
    }

replication_manager.full_sync_id[0] = '\0';
    kvs_snapshot_reader_close(&replication_manager.snapshot_reader);
    kvs_aof_reader_close(&replication_manager.aof_reader);

    replication_manager.snapshot_connection_fd = -1;
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
    replication_manager.replication_offset = 0;
    kvs_snapshot_reader_close(&replication_manager.snapshot_reader); // 如果正在发送 Snapshot，关闭对应文件
    kvs_aof_reader_close(&replication_manager.aof_reader); // 如果正在发送增量 AOF，关闭对应文件
    kvs_snapshot_writer_abort(&replication_manager.snapshot_writer,KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH);
    replication_manager.snapshot_connection_fd = -1; // 销毁时解除 Snapshot 与连接的绑定
    replication_manager.snapshot_file_size = -1; // 销毁时清除上一次同步留下的大小
    replication_manager.replication_id[0] = '\0';
    replication_manager.primary_host[0] = '\0';               // 清空 Primary 地址
    replication_manager.primary_port = 0;
    replication_manager.primary_connection_fd = -1;           // 当前不存在上游连接
    replication_manager.state = KVS_REPLICATION_STATE_DISCONNECTED;
#ifdef KVS_EBPF_STARTUP_EXPERIMENT
    kvs_replication_ebpf_reset();
#endif
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
