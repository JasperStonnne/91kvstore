#ifndef KVS_EBPF_CAPTURE_H
#define KVS_EBPF_CAPTURE_H

#include "ebpf_event.h"

struct kvs_ebpf_failures {
    __u64 oversize;
    __u64 ring_full;
    __u64 read_error;
};

/* 加载 BPF 对象、准备队列读取，并附着到当前 Primary。 */
int kvs_ebpf_start(const char *object_path);

/* 仅由 Primary 的同一线程调用；1=取到一条，0=暂无数据，负数=错误。 */
int kvs_ebpf_read(struct kvs_ebpf_event *event);

/* 可用性和累计失败计数用于复制模块显式选择数据源、审计 AOF 补偿。 */
int kvs_ebpf_is_ready(void);
int kvs_ebpf_get_failures(struct kvs_ebpf_failures *failures);
void kvs_ebpf_stop(void);

#endif
