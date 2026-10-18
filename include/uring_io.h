#ifndef KVS_URING_IO_H
#define KVS_URING_IO_H

#include <stddef.h>
#include <sys/types.h>
#include <liburing.h>

/* 队列最多容纳的请求数量，不是缓冲区的字节大小。 */
#define KVS_URING_QUEUE_DEPTH 8U

/*
 * 一个 io_uring 实例。
 * 使用前初始化为 {0}，初始化成功后不能直接复制这个对象。
 * 同一个实例的操作按顺序调用，不允许并发使用。
 */
typedef struct {
    struct io_uring ring;
    int initialized;
} kvs_uring_io_t;

/* 描述一块需要写入的数据，本结构不拥有 data 指向的内存。 */
typedef struct {
    const void *data;  /* 调用方提供的数据。 */
    size_t length;    /* 需要写入的字节数。 */
    off_t offset;     /* 写入文件的起始字节位置，必须非负。 */
} kvs_uring_write_t;



/* 创建队列。成功返回 0；失败返回 -1，并设置 errno。 */
int kvs_uring_io_init(kvs_uring_io_t *io);

/*
 * 批量写入，使用每块数据指定的文件位置。
 *
 * fd 由调用方管理，不能以 O_APPEND 模式打开。
 * 同一批写入范围不能重叠，请求数不能超过队列容量。
 *
 * 返回前，必须处理完本次全部已提交请求，包括失败情况。
 * 调用期间，调用方必须保持数据有效且内容不变。
 * 返回后，调用方才能复用或释放数据。
 *
 * 成功返回 0：全部字节写完，但还没有保证同步落盘。
 * 失败返回 -1，并设置 errno；文件可能已经写入部分数据。
 */
int kvs_uring_io_write_batch(
    kvs_uring_io_t *io,
    int fd,
    const kvs_uring_write_t *writes,
    size_t count);

/*
 * 同步文件，并等待同步请求完成。
 * 使用与原 fsync 相同的同步要求。
 * 成功返回 0；失败返回 -1，并设置 errno。
 */
int kvs_uring_io_sync(kvs_uring_io_t *io, int fd);

/*
 * 释放队列，不关闭调用方的文件，也不自动同步文件。
 * 调用前，前面的操作必须已经返回。
 */
void kvs_uring_io_close(kvs_uring_io_t *io);

#endif