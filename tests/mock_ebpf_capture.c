/* 文件事件源替身，仅与测试版采集进程链接。真实 BPF 路径由单独验收覆盖。 */
#include "ebpf_capture.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

static char state_path[PATH_MAX];

static int source_fd = -1;
static off_t position;
static off_t ready_position;
static char ready_path[PATH_MAX];

int kvs_ebpf_start_replication(const char *path, pid_t primary_pid)
{
    (void)primary_pid;
    if (snprintf(state_path, sizeof(state_path), "%s.state", path) >= (int)sizeof(state_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (snprintf(ready_path, sizeof(ready_path), "%s.ready", path) >= (int)sizeof(ready_path)) return -1;
    ready_position = 0;
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

int kvs_ebpf_get_failures(struct kvs_ebpf_failures *failures)
{
    *failures = (struct kvs_ebpf_failures){0};
    return 0;
}

/* 仅测试替身使用的原子替换文本文件；生产状态来自内核 Map。 */
int kvs_ebpf_read_state(struct kvs_ebpf_state *state)
{
    FILE *file = fopen(state_path, "r");
    if (file == NULL)
        return errno == ENOENT ? -EAGAIN : -errno;
    char primary[41], sync[41];
    unsigned long long generation, offset, end, over, ring, read, old_over, old_ring, old_read;
    int count = fscanf(file, "%llu %40s %40s %llu %llu %llu %llu %llu %llu %llu %llu",
        &generation, primary, sync, &offset, &end, &over, &ring, &read,
        &old_over, &old_ring, &old_read);
    fclose(file);
    if (count != 11 || strlen(primary) != 40 || strlen(sync) != 40)
        return -EPROTO;
    struct kvs_ebpf_state sample = {
        .boundary = { .generation = generation, .stream_offset = offset,
                      .failures = { old_over, old_ring, old_read } },
        .stream_end = end, .failures = { over, ring, read }
    };
    memcpy(sample.boundary.primary_id, primary, 40);
    memcpy(sample.boundary.full_sync_id, sync, 40);
    int result = kvs_ebpf_validate_state(&sample);
    if (result == 0)
        *state = sample;
    return result;
}

void kvs_ebpf_stop(void)
{
    if (source_fd >= 0) {
        close(source_fd);
        source_fd = -1;
    }
}

int kvs_ebpf_read_ready(kvs_replica_ready_event_t *event)
{
    int fd = open(ready_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -errno;
    ssize_t n = pread(fd, event, sizeof(*event), ready_position);
    int saved = errno;
    close(fd);
    if (n < 0) return -saved;
    if (n != sizeof(*event)) return 0;
    ready_position += n;
    return event->version == KVS_EBPF_EVENT_VERSION ? 1 : -EPROTO;
}
