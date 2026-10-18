#ifndef KVS_MAPPED_FILE_H
#define KVS_MAPPED_FILE_H

#include <stddef.h>

/* 一份只读文件映射，由调用方以 {0} 初始化。 */
typedef struct {
    const unsigned char *data; /* 文件内容的访问起点。 */
    size_t length;            /* 文件的字节数。 */
} kvs_mapped_file_t;

/*
 * 打开普通文件并建立只读映射。
 *
 * 调用前，file 必须已清零，且没有持有尚未释放的映射。
 * 成功返回 0。
 * 失败返回 -1，通过 errno 提供错误原因。
 *
 * 空文件也返回成功，以 data=NULL、length=0 表示，不调用 mmap。
 * 文件是否符合 Snapshot/AOF 格式，由上层判断。
 *
 * 使用期间，文件不得被其他路径改写或截断。
 */
int kvs_mapped_file_open(
    const char *path,
    kvs_mapped_file_t *file
);

/*
 * 释放映射，成功后将 data 和 length 清零。
 * 返回 0 表示成功，-1 表示失败，并设置 errno。
 */
int kvs_mapped_file_close(kvs_mapped_file_t *file);

#endif