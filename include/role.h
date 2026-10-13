#ifndef KVS_ROLE_H
#define KVS_ROLE_H

/* 三种服务角色共用的定义，不属于配置或复制模块中的任何一方。 */
typedef enum {
    KVS_ROLE_STANDALONE = 0,
    KVS_ROLE_PRIMARY,
    KVS_ROLE_REPLICA
} kvs_role_t;

#endif