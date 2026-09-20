#include "replication.h"
#include "persistence.h"
#include "server.h"
#include <stdio.h>
#include <string.h>
typedef struct {
    kvs_role_t role;               // 当前进程是单机、Primary 还是 Replica
    long long replication_offset;  // 当前复制进度
    int initialized;               // 是否完成初始化
} kvs_replication_manager_t;

static kvs_replication_manager_t replication_manager = {
    .role = KVS_ROLE_STANDALONE,
    .replication_offset = 0,
    .initialized = 0
};
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

    if (frame_length == 4 &&
        memcmp(frame, "PING", 4) == 0) { // 处理一条完整的 PING
        response_length = snprintf(
            response,
            response_capacity,
            "PONG\r\n"
        );
    } else {
        response_length = snprintf(
            response,
            response_capacity,
            "ERROR unknown replication command\r\n"
        );
    }

    if (response_length < 0 ||
        response_length >= response_capacity) { // 响应写入失败或者空间不足
        return -1;
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

int kvs_replication_init(kvs_role_t role){
    if(replication_manager.initialized){
        return -1;
    }

    if (role != KVS_ROLE_STANDALONE &&
    role != KVS_ROLE_PRIMARY &&
    role != KVS_ROLE_REPLICA) {
    return -1;
    }

    long long offset =kvs_aof_get_offset();
    if(offset<0){
        return -1;
    }
    replication_manager.role = role;
    replication_manager.replication_offset = offset;
    replication_manager.initialized = 1;

    return 0;
}

void kvs_replication_destroy(void)
{
    if (!replication_manager.initialized) {
        return;
    }

    replication_manager.role = KVS_ROLE_STANDALONE;
    replication_manager.replication_offset = 0;
    replication_manager.initialized = 0;
}
