#include "replication.h"
#include "persistence.h"
#include "server.h"
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <errno.h>
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
typedef struct {
    kvs_role_t role;               // 当前进程是单机、Primary 还是 Replica
    long long replication_offset;  // 当前复制进度
    long long snapshot_file_size; // 当前全量同步 Snapshot 文件的总字节数
    kvs_snapshot_reader_t snapshot_reader; // Primary 当前正在分块读取的复制快照
    kvs_aof_reader_t aof_reader; // Primary 当前正在分块读取的增量 AOF
    kvs_snapshot_writer_t snapshot_writer;
    int snapshot_connection_fd; // Primary 当前向哪条 Replica 连接发送 Snapshot
    char replication_id[KVS_REPLICATION_ID_HEX_LENGTH + 1]; // Primary 当前数据历史的唯一标识
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
    .primary_host = {0},
    .primary_port = 0,
    .primary_connection_fd = -1,
    .snapshot_install_handler = NULL,
    .command_handler = NULL,
    .state = KVS_REPLICATION_STATE_DISCONNECTED,
    .initialized = 0

};

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
    kvs_snapshot_writer_abort(
        &replication_manager.snapshot_writer,
        KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH
    );
    replication_manager.primary_connection_fd = -1;     // 当前已经没有上游连接
    replication_manager.state =
        KVS_REPLICATION_STATE_DISCONNECTED;              // 等待后续重新连接
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
        if(frame_length >=11 &&
       memcmp(frame,"FULLRESYNC ",11) == 0){

        char primary_replication_id[
            KVS_REPLICATION_ID_HEX_LENGTH + 1
        ] = {0};

        long long primary_offset=-1;
        long long primary_snapshot_file_size=-1; // Primary 接下来要发送的 Snapshot 总字节数
        int parsed_length=0;

        int matched=sscanf(
            frame,
            "FULLRESYNC %40[0-9a-f] %lld %lld%n",
            primary_replication_id,
            &primary_offset,
            &primary_snapshot_file_size,
            &parsed_length
        );

        if(matched != 3 ||
           parsed_length != frame_length ||
           strlen(primary_replication_id) != KVS_REPLICATION_ID_HEX_LENGTH ||
           primary_offset < 0||
        primary_snapshot_file_size<=0){
            return -1; // FULLRESYNC 格式、ID 长度或 offset 不合法
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

        replication_manager.replication_offset=primary_offset;
        replication_manager.snapshot_file_size =primary_snapshot_file_size;
        replication_manager.state=KVS_REPLICATION_STATE_FULL_SYNC;
        printf(
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
static int kvs_replication_catch_up_frame_protocol(
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
       replication_manager.command_handler==NULL ||
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

        printf(
            "replication: enter ONLINE offset=%lld fd=%d\n",
            local_offset,
            connection_fd
        );

        return 0; // 消费 ONLINE 消息，不向 Primary 回包
    }
    int command_response_length=
        replication_manager.command_handler(
            frame,
            frame_length,
            response
        );

    /*
     * 当前 AOF 只记录执行成功的写命令；
     * 重放成功应当得到 OK\r\n。
     */
    if(command_response_length!=4 ||
       memcmp(response,"OK\r\n",4)!=0){
        return -1;
    }
        /*
     * command_handler 已经执行命令并通过 kvs_aof_append()
     * 写入 Replica 本地 AOF，因此当前文件末尾就是最新复制进度。
     */
    long long current_offset=kvs_aof_get_offset();

    if(current_offset<0){
        return -1;
    }

    replication_manager.replication_offset=current_offset;
    /*
     * 这里的 0 表示网络响应长度为 0。
     * 命令已经执行并写入 Replica 本地 AOF。
     */
    return 0;
}
static int kvs_replication_upstream_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    char *response,
    int response_capacity,
    int *consumed_length)
{
    if(!replication_manager.initialized ||
    replication_manager.role!=KVS_ROLE_REPLICA ||
    connection_fd<0 ||
    connection_fd!=replication_manager.primary_connection_fd ||
    msg==NULL ||
    length<=0 ||
    response==NULL ||
    response_capacity<=0 ||
    consumed_length==NULL){
    return -1;
    }
    if(replication_manager.state==KVS_REPLICATION_STATE_HANDSHAKE){
    return kvs_line_batch_protocol(
        connection_fd,
        msg,
        length,
        response,
        response_capacity,
        consumed_length,
        kvs_replication_upstream_frame_protocol      // 每条上游消息交给 PONG 处理函数
    );
    }
    if(replication_manager.state==KVS_REPLICATION_STATE_FULL_SYNC){
        int written = kvs_snapshot_writer_write(
            &replication_manager.snapshot_writer,
            msg,
            length
        );
        if (written<0){
            return -1;
        }
        *consumed_length=written;

        /*
         * remaining 变成 0，说明 FULLRESYNC 协商的 Snapshot
         * 已经一字节不少地写入临时文件。
         */
        if(replication_manager.snapshot_writer.remaining==0){
            if(kvs_snapshot_writer_commit(
                    &replication_manager.snapshot_writer,
                    KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH,
                    KVS_REPLICATION_REPLICA_SNAPSHOT_PATH) < 0){

                /*
                 * 落盘、关闭或 rename 失败时，
                 * 删除临时文件，避免下次同步误用半成品。
                 */
                kvs_snapshot_writer_abort(
                    &replication_manager.snapshot_writer,
                    KVS_REPLICATION_REPLICA_SNAPSHOT_TEMP_PATH
                );
                return -1;
            }

                        /*
             * 文件已经完整提交为 snapshot.db，
             * 现在通知 kvstore 模块清空旧 Engine 并加载它。
             */
            if(replication_manager.snapshot_install_handler==NULL ||
               replication_manager.snapshot_install_handler(
                   KVS_REPLICATION_REPLICA_SNAPSHOT_PATH) < 0){
                return -1;
            }

            /*
             * Snapshot 已经写入磁盘并加载进内存。
             * 接下来需要从 replication_offset 开始补齐增量 AOF。
             */
            replication_manager.state =
                KVS_REPLICATION_STATE_CATCH_UP;

            printf(
                "replication: Snapshot installed, enter CATCH_UP "
                "offset=%lld size=%lld fd=%d\n",
                replication_manager.replication_offset,
                replication_manager.snapshot_file_size,
                connection_fd
            );
        }

        return 0;
    }
if(replication_manager.state==KVS_REPLICATION_STATE_CATCH_UP ||
   replication_manager.state==KVS_REPLICATION_STATE_ONLINE){

        return kvs_line_batch_protocol(
            connection_fd,
            msg,
            length,
            response,
            response_capacity,
            consumed_length,
            kvs_replication_catch_up_frame_protocol
        );
    }
    return -1;
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
    connector->open_handler =
        kvs_replication_on_primary_connected;                           // 连接成功后生成 PING
    connector->message_handler =
        kvs_replication_upstream_network_protocol;                      // 收到数据后处理 PONG
    connector->close_handler =
        kvs_replication_on_primary_disconnected;                        // 断开后恢复状态

    return 0;
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

    int result=0;

    if(replication_manager.snapshot_reader.fd>=0){
        result=kvs_snapshot_reader_read(
            &replication_manager.snapshot_reader,
            output,
            output_capacity
        );

    if(result<0){
        // Snapshot 没有完整读完，关闭文件并取消这次发送任务。
        kvs_snapshot_reader_close(
            &replication_manager.snapshot_reader
        );
        replication_manager.snapshot_connection_fd=-1;
        return -1;
    }

    if(result==0){
        // Snapshot 已经全部读取完成。
        kvs_snapshot_reader_close(
            &replication_manager.snapshot_reader
        );

        /*
         * 记录此刻 Primary AOF 的末尾。
         * 本轮需要追赶的范围是：
         * [Snapshot offset, 当前 AOF 末尾)
         */
        long long catch_up_end=kvs_aof_get_offset();
        if(catch_up_end<
           replication_manager.replication_offset){
            replication_manager.snapshot_connection_fd=-1;
            return -1;
        }

        // 清理上一次可能残留的增量读取任务。
        kvs_aof_reader_close(
            &replication_manager.aof_reader
        );

        if(kvs_aof_reader_open(
                &replication_manager.aof_reader,
                KVS_REPLICATION_PRIMARY_AOF_PATH,
                replication_manager.replication_offset,
                catch_up_end) < 0){

            replication_manager.snapshot_connection_fd=-1;
            return -1;
        }

        printf(
            "replication: Snapshot stream completed, "
            "start AOF catch-up range=[%lld,%lld) fd=%d\n",
            replication_manager.replication_offset,
            catch_up_end,
            connection_fd
        );
    }
}// 结束 snapshot_reader.fd>=0 的判断
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
            replication_manager.snapshot_connection_fd=-1;
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

        if(latest_offset<sent_offset){
            replication_manager.snapshot_connection_fd=-1;
            return -1; // AOF 被异常截断，无法继续按原 offset 追赶
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

            replication_manager.snapshot_connection_fd=-1;
            return -1;
        }

        printf(
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
            replication_manager.snapshot_connection_fd=-1;
            return -1;
        }

        replication_manager.replication_offset=sent_offset;
        replication_manager.state=KVS_REPLICATION_STATE_ONLINE;

        printf(
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
        replication_manager.snapshot_connection_fd=-1;
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

        replication_manager.snapshot_connection_fd=-1;
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
        replication_manager.snapshot_connection_fd=-1;
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

    int response_length;
    int is_full_resync_response=0; // 标记本次是否生成了 FULLRESYNC 响应
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

        kvs_snapshot_metadata_t snapshot_metadata;

        if(kvs_snapshot_save(
                KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                &snapshot_metadata) < 0){
            return -1;
        }
        kvs_snapshot_reader_close(
            &replication_manager.snapshot_reader
        ); // 清理上一次可能残留的读取任务

        if(kvs_snapshot_reader_open(
                &replication_manager.snapshot_reader,
                KVS_REPLICATION_PRIMARY_SNAPSHOT_PATH,
                snapshot_metadata.file_size) < 0){
            return -1;
        }
        replication_manager.replication_offset =snapshot_metadata.aof_offset;
        replication_manager.snapshot_file_size =snapshot_metadata.file_size;
        replication_manager.snapshot_connection_fd =connection_fd; // Snapshot 只能由当前发来 PSYNC 的连接读取
        response_length=snprintf(
            response,
            response_capacity,
            "FULLRESYNC %s %lld %lld\r\n",
            replication_manager.replication_id,
            replication_manager.replication_offset,
            replication_manager.snapshot_file_size
        );
        is_full_resync_response=1;
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
        printf(
            "replication: FULLRESYNC response ready id=%s offset=%lld snapshot_size=%lld fd=%d\n",
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
    char *response,
    int response_capacity,
    int *consumed_length)
{
    return kvs_line_batch_protocol(
        connection_fd,                        // 当前 Replica 连接
        msg,                                  // TCP 输入缓冲区
        length,                               // 当前收到的数据长度
        response,                             // 响应缓冲区
        response_capacity,                    // 响应缓冲区容量
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

int kvs_replication_init(const kvs_server_config_t *config,kvs_snapshot_install_handler snapshot_install_handler, kvs_replication_command_handler command_handler)
{
    if (config == NULL || command_handler == NULL||replication_manager.initialized) { // 配置必须存在，并且不能重复初始化
        return -1;
    }

    if (config->role != KVS_ROLE_STANDALONE &&
        config->role != KVS_ROLE_PRIMARY &&
        config->role != KVS_ROLE_REPLICA) {                  // 检查角色是否合法
        return -1;
    }

    size_t primary_host_length = 0;
    char generated_replication_id[KVS_REPLICATION_ID_HEX_LENGTH + 1] = {0};
    if (config->role == KVS_ROLE_REPLICA) {
        if (config->primary_host == NULL ||
            config->primary_host[0] == '\0' ||
            config->replication_port == 0) {                 // Replica 必须具有有效的 Primary 地址和端口
            return -1;
        }

        primary_host_length = strlen(config->primary_host);
        if (primary_host_length >=
            sizeof(replication_manager.primary_host)) {      // 地址必须能够放入管理器
            return -1;
        }
    }

    long long offset = kvs_aof_get_offset();// 取得本地 AOF 当前末尾位置
        if (offset < 0) {
        return -1;
    }
    if(config->role == KVS_ROLE_PRIMARY){
    if(kvs_replication_generate_id(
            generated_replication_id,
            sizeof(generated_replication_id)) < 0){
        return -1; // Primary 无法生成身份时不继续启动复制模块
    }
}


    replication_manager.role = config->role;
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
    if(config->role == KVS_ROLE_PRIMARY){
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

    if (config->role == KVS_ROLE_REPLICA) {
        memcpy(
            replication_manager.primary_host,
            config->primary_host,
            primary_host_length + 1                           // 连同字符串末尾的 \0 一起复制
        );
        replication_manager.primary_port = config->replication_port;
    } else {
        replication_manager.primary_host[0] = '\0';           // Primary 和单机没有上游地址
        replication_manager.primary_port = 0;
    }

    replication_manager.initialized = 1;

    return 0;
}
void kvs_replication_destroy(void)
{
    if (!replication_manager.initialized) {                   // 未初始化就不需要销毁
        return;
    }

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
    replication_manager.initialized = 0;
}