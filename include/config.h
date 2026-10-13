#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stddef.h>
#include "log.h"
#include "role.h"

/* IPv4 文本地址最长 15 个字符，再留 1 个位置给结尾的 '\0'。 */
#define KVS_CONFIG_IPV4_CAPACITY 16
/* 上游地址由配置对象持有；当前配置文件只接受 IPv4。 */
#define KVS_CONFIG_PRIMARY_HOST_CAPACITY 256

/*
 * 当前只实现 Snapshot 恢复 + AOF 回放的混合模式。
 * 配置文件写入其他模式时，解析器必须报错，不能静默忽略。
 */
typedef enum {
    KVS_PERSISTENCE_LEGACY_HYBRID = 0
} kvs_persistence_mode_t;

/*
 * 配置对象自己保存地址字符串，不引用读取文件时使用的临时缓冲区。
 * 这样解析完成后，main() 仍可安全地把值传给网络和复制模块。
 */
typedef struct {
    kvs_role_t role;
    char bind_ip[KVS_CONFIG_IPV4_CAPACITY];
    unsigned short service_port;

    /* Primary 本机用于监听 Replica 的第二个端口。 */
    unsigned short replication_port;

    /* Replica 要连接的 Primary 地址和复制端口。 */
    char primary_host[KVS_CONFIG_PRIMARY_HOST_CAPACITY];
    unsigned short primary_port;

    kvs_log_level_t log_level;
    kvs_persistence_mode_t persistence_mode;
} kvs_config_t;

/*
 * 读取配置文件、应用默认值并校验。
 * 成功返回 0；失败返回 -1，并将原因写入调用方提供的 error。
 * 解析错误应包含文件名和行号；文件无法打开时包含文件名。
 */
int kvs_config_load(const char *path, kvs_config_t *config,
                    char *error, size_t error_capacity);

#endif