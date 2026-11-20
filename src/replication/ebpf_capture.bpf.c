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

/* 只有一个 8 字节值；即使事件投递失败，也保留最新提交位置。 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} stream_end SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct kvs_ebpf_snapshot_boundary);
} snapshot_boundary SEC(".maps");

static __always_inline void count_failure(__u32 reason)
{
    __u64 *count = bpf_map_lookup_elem(&failure_count, &reason);
    if (count)
        __sync_fetch_and_add(count, 1);
}

/* Primary 在同一调度线程固定 S 后调用；不产生命令事件。 */
SEC("uprobe")
int on_snapshot_boundary(struct pt_regs *ctx)
{
    __u32 key = 0;
    struct kvs_ebpf_snapshot_boundary *boundary =
        bpf_map_lookup_elem(&snapshot_boundary, &key);
    __u64 *watermark = bpf_map_lookup_elem(&stream_end, &key);
    if (!boundary || !watermark)
        return 0;

    /* 奇数版本令并发用户态读取暂时失败，完成后发布偶数版本。 */
    __u64 generation = boundary->generation;
    boundary->generation = (generation | 1);
    const void *primary_id = (const void *)PT_REGS_PARM1(ctx);
    const void *sync_id = (const void *)PT_REGS_PARM2(ctx);
    __u64 offset = PT_REGS_PARM3(ctx);
    if (offset < *watermark ||
        bpf_probe_read_user(boundary->primary_id, KVS_EBPF_ID_BYTES, primary_id) < 0 ||
        bpf_probe_read_user(boundary->full_sync_id, KVS_EBPF_ID_BYTES, sync_id) < 0) {
        count_failure(2);
        return 0; /* 保持奇数，禁止使用不完整的边界。 */
    }
    boundary->stream_offset = offset;
    __u32 reason = 0;
    __u64 *count = bpf_map_lookup_elem(&failure_count, &reason);
    boundary->failures.oversize = count ? *count : 0;
    reason = 1;
    count = bpf_map_lookup_elem(&failure_count, &reason);
    boundary->failures.ring_full = count ? *count : 0;
    reason = 2;
    count = bpf_map_lookup_elem(&failure_count, &reason);
    boundary->failures.read_error = count ? *count : 0;

    /* 挂载前的写入由快照覆盖；没有新写入时也能从 S 初始化。 */
    *watermark = offset;
    __sync_fetch_and_add(&boundary->generation, 1);
    return 0;
}

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

    /* 成功提交先记水位；任何采集失败分支都不能把它退回。 */
    __u32 key = 0;
    __u64 *watermark = bpf_map_lookup_elem(&stream_end, &key);
    if (watermark)
        *watermark = end;

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
    count_failure(reason);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
