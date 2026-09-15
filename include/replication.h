#ifndef KVS_REPLICATION_H
#define KVS_REPLICATION_H

typedef enum {
    KVS_ROLE_STANDALONE = 0,
    KVS_ROLE_PRIMARY,
    KVS_ROLE_REPLICA
} kvs_role_t;

typedef struct {//保存启动配置
    kvs_role_t role;//当前进程角色
    unsigned short service_port;//客户端链接端口
    unsigned short replication_port;//主从复制链接使用的端口
    const char *primary_host; //要链接的primary地址

}kvs_server_config_t;

#endif
