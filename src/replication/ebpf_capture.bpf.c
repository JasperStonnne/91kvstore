#include <linux/bpf.h>
#include <asm/ptrace.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "ebpf_event.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, KVS_EBPF_RING_BYTES);
} events SEC(".maps");

/* 累计失败原因：0=长度超限或为零，1=队列满，2=读取命令内存失败。 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 3);
    __type(key, __u32);
    __type(value, __u64);
} failure_count SEC(".maps");

/* 具体进程和成功点由用户态管理模块在启动时指定。 */
SEC("uprobe")
int on_committed(struct pt_regs *ctx)
{
    const void *source = (const void *)PT_REGS_PARM1(ctx);
    __u64 length = PT_REGS_PARM2(ctx);
    __u64 start = PT_REGS_PARM3(ctx);
    __u64 end = PT_REGS_PARM4(ctx);
    struct kvs_ebpf_event *event;
    __u32 reason = 0;

    if (length == 0 || length > sizeof(event->payload))
        goto failed;

    event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event) {
        reason = 1;
        goto failed;
    }

    /* 逐字节清零，避免编译器生成 BPF 不支持的 memset 调用。 */
    volatile unsigned char *bytes = (volatile unsigned char *)event;
    for (__u32 i = 0; i < sizeof(*event); i++)
        bytes[i] = 0;
    event->version = KVS_EBPF_EVENT_VERSION;
    event->start = start;
    event->end = end;
    event->length = length;

    if (bpf_probe_read_user(event->payload, length, source) < 0) {
        bpf_ringbuf_discard(event, 0);
        reason = 2;
        goto failed;
    }

    bpf_ringbuf_submit(event, 0);
    return 0;

failed:
    {
        __u64 *count = bpf_map_lookup_elem(&failure_count, &reason);
        if (count)
            __sync_fetch_and_add(count, 1);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
