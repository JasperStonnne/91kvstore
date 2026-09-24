#include "replication.h"
#include "persistence.h"
#include "server.h"
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <errno.h>
#define KVS_REPLICATION_HOST_MAX_LENGTH 256
#define KVS_REPLICATION_ID_HEX_LENGTH 40 // 20 字节随机数转换成 40 个十六进制字符

typedef struct {
    kvs_role_t role;               // 当前进程是单机、Primary 还是 Replica
    long long replication_offset;  // 当前复制进度
    long long snapshot_file_size; // 当前全量同步 Snapshot 文件的总字节数
    char replication_id[KVS_REPLICATION_ID_HEX_LENGTH + 1]; // Primary 当前数据历史的唯一标识
    char primary_host[KVS_REPLICATION_HOST_MAX_LENGTH];//Replica要链接的primary地址
    unsigned short primary_port;//primary的复制端口
    int primary_connection_fd;//与primary通信的socket
    kvs_replication_state_t state;//当前复制状态
    int initialized;
} kvs_replication_manager_t;//复制管理器 保存复制相关信息

static kvs_replication_manager_t replication_manager = {
    .role = KVS_ROLE_STANDALONE,
    .replication_offset = 0,
    .snapshot_file_size = -1,
    .replication_id={0},
    .primary_host = {0},
    .primary_port = 0,
    .primary_connection_fd = -1,
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
static int kvs_replication_upstream_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    char *response,
    int response_capacity,
    int *consumed_length)
{
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
                "replication.snapshot",
                &snapshot_metadata) < 0){
            return -1;
        }

        replication_manager.replication_offset =snapshot_metadata.aof_offset;
        replication_manager.snapshot_file_size =snapshot_metadata.file_size;
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

int kvs_replication_init(const kvs_server_config_t *config)
{
    if (config == NULL || replication_manager.initialized) { // 配置必须存在，并且不能重复初始化
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
    replication_manager.snapshot_file_size = -1; // 初始化时还不知道全量快照大小
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
    replication_manager.snapshot_file_size = -1; // 销毁时清除上一次同步留下的大小
    replication_manager.replication_id[0] = '\0';
    replication_manager.primary_host[0] = '\0';               // 清空 Primary 地址
    replication_manager.primary_port = 0;
    replication_manager.primary_connection_fd = -1;           // 当前不存在上游连接
    replication_manager.state = KVS_REPLICATION_STATE_DISCONNECTED;
    replication_manager.initialized = 0;
}