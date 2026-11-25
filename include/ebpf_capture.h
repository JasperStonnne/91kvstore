#ifndef KVS_EBPF_CAPTURE_H
#define KVS_EBPF_CAPTURE_H

#include <sys/types.h>
#include "ebpf_event.h"
#include "ebpf_state.h"

/* 独立进程附着到指定 Primary 的复制提交点。 */
int kvs_ebpf_start_replication(const char *object_path, pid_t primary_pid);

/* 仅由当前采集进程的同一线程调用；1=取到一条，0=暂无数据，负数=错误。 */
int kvs_ebpf_read(struct kvs_ebpf_event *event);
int kvs_ebpf_read_ready(kvs_replica_ready_event_t *event);

/* 读取前后边界版本一致的状态；边界未知/更新中返回 -EAGAIN。 */
int kvs_ebpf_read_state(struct kvs_ebpf_state *state);

/* 累计失败计数不依赖后续事件，可用于发现末条丢失。 */
int kvs_ebpf_is_ready(void);
int kvs_ebpf_get_failures(struct kvs_ebpf_failures *failures);
void kvs_ebpf_stop(void);

#endif
