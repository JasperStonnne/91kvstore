#ifndef KVS_EBPF_EVENT_H
#define KVS_EBPF_EVENT_H
#include <linux/types.h>

/* 超限时计数并令对应复制区间不可用，禁止截断命令。 */
#define KVS_EBPF_MAX_PAYLOAD 4096
#define KVS_EBPF_RING_BYTES (16u * 1024u * 1024u)

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

#define KVS_EBPF_ID_BYTES 40

struct kvs_ebpf_failures {
    __u64 oversize;
    __u64 ring_full;
    __u64 read_error;
};

/* generation 为 0 表示未见边界；奇数表示内核正在更新。 */
struct kvs_ebpf_snapshot_boundary {
    __u64 generation;
    __u64 stream_offset;
    struct kvs_ebpf_failures failures;
    unsigned char primary_id[KVS_EBPF_ID_BYTES];
    unsigned char full_sync_id[KVS_EBPF_ID_BYTES];
};

#endif
