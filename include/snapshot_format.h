#ifndef KVS_SNAPSHOT_FORMAT_H
#define KVS_SNAPSHOT_FORMAT_H

/* v3 文件头中的模式编号属于文件格式，不能重排。 */
typedef enum {
    KVS_SNAPSHOT_CHECKSUM_FILE = 0,
    KVS_SNAPSHOT_CHECKSUM_RECORD = 1
} kvs_snapshot_checksum_t;

/* 单机按文件标记读取；复制只接受能严格校验整文件的快照。 */
typedef enum {
    KVS_SNAPSHOT_LOAD_AUTO = 0,
    KVS_SNAPSHOT_LOAD_REQUIRE_FILE
} kvs_snapshot_load_policy_t;

#endif
