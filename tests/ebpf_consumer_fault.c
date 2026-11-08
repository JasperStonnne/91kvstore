/*
 * 仅用于真实 eBPF 专项的 LD_PRELOAD 故障注入。
 * 测试创建标记文件后，在队列确实有待消费事件时，让一次 consume 返回 -EIO。
 * 这样可重复验证“命令已经成功且事件已排队，但用户态消费中断”的 AOF 修复。
 */
#define _GNU_SOURCE
#include <bpf/libbpf.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int ring_buffer__consume(struct ring_buffer *queue)
{
    typedef int (*consume_fn)(struct ring_buffer *);
    static consume_fn original;
    static int interrupted;

    if (original == NULL)
        original = (consume_fn)dlsym(RTLD_NEXT, "ring_buffer__consume");
    if (original == NULL)
        return -ENOSYS;

    const char *marker = getenv("KVS_EBPF_FAULT_FILE");
    if (!interrupted && marker != NULL && access(marker, F_OK) == 0 &&
        ring__avail_data_size(ring_buffer__ring(queue, 0)) > 0) {
        interrupted = 1;
        return -EIO;
    }
    return original(queue);
}
