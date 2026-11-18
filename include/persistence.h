#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H
#include "protocol.h"
#include "snapshot_format.h"
int kvs_aof_open(const char *path);
int kvs_aof_append(char **tokens, int count);
int kvs_aof_close(void);
int kvs_snapshot_store_save(const char *path);
int kvs_snapshot_store_load(const char *path);

/* v1/v2 的检查点元数据，供兼容读取使用。 */
typedef struct {
    int has_offset;       /* 1：携带 O；0：不携带 O */
    long long offset;     /* has_offset 为 1 时有效 */
} kvs_snapshot_checkpoint_t;

/* 保存入口统一写 v3；未指定模式的入口默认整文件校验。 */
int kvs_snapshot_store_save_with_checksum(
    const char *path, kvs_snapshot_checksum_t checksum);

/* 加载模式来自文件，policy 决定是否允许逐条容错。 */
int kvs_snapshot_store_load_with_policy(
    const char *path, kvs_snapshot_load_policy_t policy);

/* 兼容读取 v1/v2 的检查点；v3 返回无检查点。 */
int kvs_snapshot_store_load_with_checkpoint(
    const char *path,
    kvs_snapshot_checkpoint_t *checkpoint
);

typedef int (*aof_replay_handler)(
    const kvs_slice_t *fields,
    size_t field_count,
    char *response,
    int response_capacity
);

int kvs_aof_replay(const char *path,long long offset,aof_replay_handler handler);
typedef struct {
    int fd;                  // 当前打开的 Snapshot 文件
    long long remaining;    // 还剩多少字节没有读取
} kvs_snapshot_reader_t;
typedef struct {
    int fd;                 // Replica 正在写入的临时 Snapshot 文件
    long long remaining;   // 还需要接收并写入多少字节
} kvs_snapshot_writer_t;
int kvs_snapshot_reader_open(kvs_snapshot_reader_t *reader,const char *path,long long expected_size);
int kvs_snapshot_reader_read(kvs_snapshot_reader_t *reader,char *output,int output_capacity);
void kvs_snapshot_reader_close(kvs_snapshot_reader_t *reader);
int kvs_snapshot_writer_open(kvs_snapshot_writer_t *writer,const char *temp_path,long long expected_size);
int kvs_snapshot_writer_write(kvs_snapshot_writer_t *writer,const char *data,int length);
int kvs_snapshot_writer_finish(kvs_snapshot_writer_t *writer);
void kvs_snapshot_writer_abort(kvs_snapshot_writer_t *writer,const char *temp_path);

/*
 * 独立二进制 Snapshot，不依赖 AOF。
 *
 * save：保存四种引擎的当前数据。
 * 返回 0 表示成功，-1 表示失败。
 *
 * load：加载到已初始化的空引擎中。
 * 返回 0 表示成功，record 模式可能已记录并跳过坏记录。
 * 返回 1 表示文件不存在，-1 表示整文件/结构损坏或其他错误。
 * 加载失败后，调用方不得对外提供可能已部分加载的数据。
 */

/*
 * Snapshot 文件中的存储类型编号。
 * 编号属于文件格式，不能随意修改或重新排列。
 */
typedef enum {
    KVS_SNAPSHOT_ENGINE_ARRAY = 1,
    KVS_SNAPSHOT_ENGINE_RBTREE = 2,
    KVS_SNAPSHOT_ENGINE_HASH = 3,
    KVS_SNAPSHOT_ENGINE_SKIPLIST = 4
} kvs_snapshot_engine_t;



#endif