#ifndef KVS_REPLICATION_H
#define KVS_REPLICATION_H
#include "server.h"
typedef enum {
    KVS_ROLE_STANDALONE = 0,
    KVS_ROLE_PRIMARY,
    KVS_ROLE_REPLICA
} kvs_role_t;

typedef enum {
    KVS_REPLICATION_STATE_DISCONNECTED = 0, // 尚未建立复制连接
    KVS_REPLICATION_STATE_CONNECTING,       // Replica 正在连接 Primary
    KVS_REPLICATION_STATE_HANDSHAKE,        // 已连接，正在交换同步信息
    KVS_REPLICATION_STATE_FULL_SYNC,        // 正在传输和加载 Snapshot
    KVS_REPLICATION_STATE_CATCH_UP,          // 正在补齐 Snapshot 之后的增量命令
    KVS_REPLICATION_STATE_ONLINE             // 已追平，持续接收实时写命令
} kvs_replication_state_t;

typedef struct {
    int connection_fd;                    // Primary 与这台 Replica 通信使用的 socket
    kvs_replication_state_t state;        // 当前复制阶段
    long long acknowledged_offset;        // Replica 已确认处理完成的位置
} kvs_replica_connection_t;

typedef struct {//保存启动配置
    kvs_role_t role;//当前进程角色
    unsigned short service_port;//客户端链接端口
    unsigned short replication_port;//主从复制链接使用的端口
    const char *primary_host; //要链接的primary地址

}kvs_server_config_t;

int kvs_replication_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    char *response,
    int response_capacity,
    int *consumed_length
);
int kvs_replication_build_connector_config(kvs_connector_config_t *connector);
int kvs_replication_init(const kvs_server_config_t *config);
void kvs_replication_destroy(void);

#endif
