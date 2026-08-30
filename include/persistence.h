#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H
int kvs_aof_open(const char *path);
int kvs_aof_append(char **tokens, int count);
int kvs_aof_close(void);
typedef int (*aof_replay_handler)(
    char *msg,
    int length,
    char *response
);
int kvs_aof_replay(const char *path,long long offset,aof_replay_handler handler);
int kvs_snapshot_save(const char *path);
long long kvs_aof_get_offset(void);
long long kvs_snapshot_load(const char* path,aof_replay_handler handler);
#endif