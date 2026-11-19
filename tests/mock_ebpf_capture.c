/* 文件事件源替身，仅与测试版采集进程链接。真实 BPF 路径由单独验收覆盖。 */
#include "ebpf_capture.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int source_fd = -1;
static off_t position;

int kvs_ebpf_start_replication(const char *path, pid_t primary_pid)
{
    (void)primary_pid;
    source_fd = open(path, O_RDONLY | O_CLOEXEC);
    position = 0;
    return source_fd < 0 ? -1 : 0;
}

int kvs_ebpf_read(struct kvs_ebpf_event *event)
{
    ssize_t count = pread(source_fd, event, sizeof(*event), position);
    if (count < 0) {
        return -errno;
    }
    if (count < (ssize_t)sizeof(*event)) {
        return 0;
    }
    if (event->version != KVS_EBPF_EVENT_VERSION || event->length == 0 ||
        event->length > sizeof(event->payload) || event->end < event->start ||
        event->end - event->start != event->length) {
        return -EPROTO;
    }
    position += count;
    return 1;
}

void kvs_ebpf_stop(void)
{
    if (source_fd >= 0) {
        close(source_fd);
        source_fd = -1;
    }
}
