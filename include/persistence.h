#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H
#include "protocol.h"
int kvs_aof_open(const char *path);
int kvs_aof_append(char **tokens, int count);
int kvs_aof_close(void);
/*
 * 将本地 AOF 文件长度和写入位置调整到指定复制 offset。
 * Replica 安装全量 Snapshot 后使用。
 */
int kvs_aof_reset_to_offset(long long offset);
typedef int (*aof_replay_handler)(
    const kvs_slice_t *fields,
    size_t field_count,
    char *response,
    int response_capacity
);
typedef int (*snapshot_replay_handler)(
    const kvs_slice_t *fields,
    size_t field_count,
    char *response,
    int response_capacity

);
int kvs_aof_replay(const char *path,long long offset,aof_replay_handler handler);
typedef struct {
    int fd;                  // 专门用于读取 AOF 的文件描述符
    long long offset;       // 下一次从 AOF 的哪个字节位置读取
    long long remaining;    // 本轮还需要发送多少字节
} kvs_aof_reader_t;
int kvs_aof_reader_open(kvs_aof_reader_t *reader,const char *path,long long start_offset,long long end_offset);
int kvs_aof_reader_read(kvs_aof_reader_t *reader,char *output,int output_capacity);
void kvs_aof_reader_close(kvs_aof_reader_t *reader);
typedef struct {
    long long aof_offset; // Snapshot 对应的一致性 AOF 位置
    long long file_size;  // Snapshot 文件最终字节数
} kvs_snapshot_metadata_t;
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
int kvs_snapshot_writer_commit(kvs_snapshot_writer_t *writer,const char *temp_path,const char *final_path);
void kvs_snapshot_writer_abort(kvs_snapshot_writer_t *writer,const char *temp_path);

int kvs_snapshot_save(const char *path,kvs_snapshot_metadata_t *metadata);
long long kvs_aof_get_offset(void);
long long kvs_snapshot_load(const char* path,snapshot_replay_handler handler);

/*
 * 独立二进制 Snapshot，不依赖 AOF。
 *
 * save：保存四种引擎的当前数据。
 * 返回 0 表示成功，-1 表示失败。
 *
 * load：加载到已初始化的空引擎中。
 * 返回 0 表示成功，1 表示文件不存在，-1 表示损坏或其他错误。
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

int kvs_snapshot_store_save(const char *path);
int kvs_snapshot_store_load(const char *path);

#endif