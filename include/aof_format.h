#ifndef KVS_AOF_FORMAT_H
#define KVS_AOF_FORMAT_H

/* 配置层和持久化层共同依赖此枚举。 */
typedef enum {
    KVS_AOF_CHECKSUM_NONE = 0,
    KVS_AOF_CHECKSUM_FILE
} kvs_aof_checksum_t;

/* file 格式使用大端整数，CRC32/ISO-HDLC 覆盖头、命令区和长度字段。 */
#define KVS_AOF_MAGIC "91KVAOF1"
#define KVS_AOF_HEADER_SIZE 16u
#define KVS_AOF_TRAILER_SIZE 12u
#define KVS_AOF_VERSION 1u

#endif
