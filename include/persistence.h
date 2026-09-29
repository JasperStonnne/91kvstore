#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H
int kvs_aof_open(const char *path);
int kvs_aof_append(char **tokens, int count);
int kvs_aof_close(void);
/*
 * 将本地 AOF 文件长度和写入位置调整到指定复制 offset。
 * Replica 安装全量 Snapshot 后使用。
 */
int kvs_aof_reset_to_offset(long long offset);
typedef int (*aof_replay_handler)(
    char *msg,
    int length,
    char *response
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
long long kvs_snapshot_load(const char* path,aof_replay_handler handler);
#endif