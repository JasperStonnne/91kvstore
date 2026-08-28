#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H
int kvs_aof_open(const char *path);
int kvs_aof_append(char **tokens, int count);
int kvs_aof_close(void);


#endif