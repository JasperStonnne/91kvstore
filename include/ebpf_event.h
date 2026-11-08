#ifndef KVS_EBPF_EVENT_H
#define KVS_EBPF_EVENT_H
#include <linux/types.h>

/* 当前阶段的固定容量；超限时计数并由 AOF 补偿，禁止截断复制。 */
#define KVS_EBPF_MAX_PAYLOAD 4096
#define KVS_EBPF_RING_BYTES (1024 * 1024)

/*
 * v1 事件只在 BPF 与 Primary 用户态之间传递。
 * 用户态先校验 version，再按共享布局读取；未知或旧格式拒收并显式降级。
 */
#define KVS_EBPF_EVENT_VERSION 1

struct kvs_ebpf_event {
    __u32 version;
    __u32 length;
    __u64 start;
    __u64 end;
    unsigned char payload[KVS_EBPF_MAX_PAYLOAD];
};

#endif
