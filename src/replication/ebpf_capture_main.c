#define _GNU_SOURCE
/* 观察纯内存 Primary 的事件，或为指定快照边界保留连续增量。 */
#include "ebpf_capture.h"
#include "replication_capture_ipc.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define KVS_EBPF_CACHE_BYTES (8u * 1024u * 1024u)

struct capture_cache {
    unsigned char *data;
    uint64_t start;
    uint64_t end;
    int valid;
};

/* 空闲时保留最近的连续字节，接到 BEGIN 后固定本轮起点。 */
static int capture_cache_append(struct capture_cache *cache,
                                const struct kvs_ebpf_event *event, int armed)
{
    if (!armed) {
        if (!cache->valid || event->start != cache->end) {
            cache->start = cache->end = event->start;
            cache->valid = 1;
        }
        if (event->end - cache->start > KVS_EBPF_CACHE_BYTES) {
            cache->start = event->end - KVS_EBPF_CACHE_BYTES;
        }
    } else {
        if (!cache->valid || event->end <= cache->start) {
            return 0;
        }
        if (event->start != cache->end) {
            cache->valid = 0;
            errno = EPROTO;
            return -1;
        }
        if (event->length > KVS_EBPF_CACHE_BYTES - (cache->end - cache->start)) {
            cache->valid = 0;
            errno = ENOSPC;
            return -1;
        }
    }

    size_t index = cache->end % KVS_EBPF_CACHE_BYTES;
    size_t first = KVS_EBPF_CACHE_BYTES - index;
    if (first > event->length) {
        first = event->length;
    }
    memcpy(cache->data + index, event->payload, first);
    if (first < event->length) {
        memcpy(cache->data, event->payload + first, event->length - first);
    }
    cache->end = event->end;
    return 1;
}

static int capture_socket_listen(const char *path, long primary_pid)
{
    struct sockaddr_un address;
    if (kvs_capture_socket_address(&address, path) < 0) {
        return -1;
    }

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld", primary_pid);
    struct stat primary;
    if (stat(proc_path, &primary) < 0) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    mode_t previous_mask = umask(0177);
    int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
    int saved_errno = errno;
    umask(previous_mask);
    if (bound < 0) {
        close(fd);
        errno = saved_errno;
        return -1;
    }

    /* sudo 启动采集端时，仍只允许 Primary 所属用户连接。 */
    if ((primary.st_uid != geteuid() && chown(path, primary.st_uid, (gid_t)-1) < 0) ||
        listen(fd, 1) < 0) {
        saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int capture_cache_begin(struct capture_cache *cache, uint64_t offset)
{
    if (cache->valid && offset < cache->start) {
        return -1; /* 本轮需要的字节已丢失。 */
    }
    if (!cache->valid || cache->end < offset) {
        cache->end = offset;
    }
    cache->start = offset;
    cache->valid = 1;
    return 0;
}

static int capture_cache_serve(struct capture_cache *cache, int peer,
                               uint64_t *next_offset, int *armed)
{
    struct kvs_capture_read_request request;
    ssize_t size = kvs_capture_packet(peer, &request, sizeof(request), 0);
    if (size < 0 && errno == EAGAIN) {
        return 0;
    }
    if (size == 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (size < 0) {
        return -1;
    }
    if (size != sizeof(request)) {
        errno = EPROTO;
        return -1;
    }
    if (request.operation == KVS_CAPTURE_BEGIN && !*armed && request.limit == 0) {
        int32_t status = capture_cache_begin(cache, request.offset);
        *armed = 1;
        if (status == 0) {
            *next_offset = request.offset;
            fprintf(stderr, "capture session began at offset=%" PRIu64 "\n", request.offset);
        } else {
            cache->valid = 0;
            fprintf(stderr, "capture boundary unavailable at offset=%" PRIu64 "\n", request.offset);
        }
        return kvs_capture_packet(peer, &status, sizeof(status), 1) == sizeof(status) ? 0 : -1;
    }
    if (request.operation != KVS_CAPTURE_READ || !*armed ||
        request.limit == 0 || request.limit > KVS_CAPTURE_READ_BYTES) {
        errno = EPROTO;
        return -1;
    }

    struct kvs_capture_read_response response = { .length = KVS_CAPTURE_READ_ERROR };
    if (cache->valid && request.offset == *next_offset &&
        request.offset >= cache->start && request.offset <= cache->end) {
        /* 下一次取数才确认上一段已交给 TCP，并释放上一段。 */
        cache->start = request.offset;
        uint64_t available = cache->end - request.offset;
        size_t bytes = available < request.limit ? (size_t)available : request.limit;
        size_t index = request.offset % KVS_EBPF_CACHE_BYTES;
        size_t first = KVS_EBPF_CACHE_BYTES - index;
        if (first > bytes) {
            first = bytes;
        }
        memcpy(response.data, cache->data + index, first);
        if (first < bytes) {
            memcpy(response.data + first, cache->data, bytes - first);
        }
        response.length = (int32_t)bytes;
    }

    size_t packet_bytes = sizeof(response.length);
    if (response.length > 0) {
        packet_bytes += (size_t)response.length;
    }
    if (kvs_capture_packet(peer, &response, packet_bytes, 1) != (ssize_t)packet_bytes) {
        return -1;
    }
    if (response.length > 0) {
        *next_offset += (uint64_t)response.length;
    }
    return 0;
}

/* 周期及退出时输出累计内核丢事件计数；不把读计数失败伪装成零。 */
static void capture_report_failures(const struct capture_cache *cache)
{
    if (cache->data != NULL) {
        fprintf(stderr, "capture cache start=%" PRIu64 " end=%" PRIu64
                        " valid=%d\n", cache->start, cache->end, cache->valid);
    }
    struct kvs_ebpf_failures failures;
    int result = kvs_ebpf_get_failures(&failures);
    if (result < 0) {
        fprintf(stderr, "capture failure_count unavailable: %s\n", strerror(-result));
        return;
    }
    fprintf(stderr, "capture failure_count oversize=%" PRIu64
                    " ring_full=%" PRIu64 " read_error=%" PRIu64 "\n",
            (uint64_t)failures.oversize, (uint64_t)failures.ring_full,
            (uint64_t)failures.read_error);
}

static volatile sig_atomic_t stopping;

static void stop_capture(int signo)
{
    (void)signo;
    stopping = 1;
}

int main(int argc, char **argv)
{
    int socket_mode = argc == 5 && strcmp(argv[3], "--socket") == 0;
    if (argc != 3 && argc != 4 && !socket_mode) {
        fprintf(stderr, "usage: %s PRIMARY_PID BPF_OBJECT [SNAPSHOT_STREAM_OFFSET | --socket PATH]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    char *end;
    errno = 0;
    long pid = strtol(argv[1], &end, 10);
    if (errno != 0 || *end != '\0' || pid <= 0 || pid > INT_MAX) {
        fprintf(stderr, "invalid Primary PID\n");
        return EXIT_FAILURE;
    }

    struct capture_cache cache = {0};
    if (argc == 4) {
        errno = 0;
        uintmax_t offset = strtoumax(argv[3], &end, 10);
        if (argv[3][0] < '0' || argv[3][0] > '9' || *end != '\0' ||
            errno == ERANGE || offset > UINT64_MAX) {
            fprintf(stderr, "invalid snapshot stream offset\n");
            return EXIT_FAILURE;
        }
        cache.start = cache.end = (uint64_t)offset;
    }

    struct sigaction action = { .sa_handler = stop_capture };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("register capture signal handler");
        return EXIT_FAILURE;
    }
    if (kvs_ebpf_start_replication(argv[2], (pid_t)pid) < 0) {
        perror("attach replication uprobe");
        return EXIT_FAILURE;
    }

    if (argc >= 4) {
        cache.data = malloc(KVS_EBPF_CACHE_BYTES);
        if (cache.data == NULL) {
            perror("allocate capture cache");
            kvs_ebpf_stop();
            return EXIT_FAILURE;
        }
        cache.valid = 1;
        fprintf(stderr, "capture cache ready at offset=%" PRIu64 "\n", cache.start);
    }

    const char *socket_path = socket_mode ? argv[4] : NULL;
    int listener = -1;
    int peer = -1;
    uint64_t next_offset = cache.start;
    int armed = argc == 4;
    if (socket_path != NULL) {
        listener = capture_socket_listen(socket_path, pid);
        if (listener < 0) {
            perror("listen on capture socket");
            free(cache.data);
            kvs_ebpf_stop();
            return EXIT_FAILURE;
        }
    }

    fprintf(stderr, "replication uprobe attached to Primary %ld\n", pid);
    int status = EXIT_SUCCESS;
    time_t last_report = -1;
    while (!stopping) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec != last_report) {
            capture_report_failures(&cache);
            last_report = now.tv_sec;
        }
        if (listener >= 0 && peer < 0) {
            peer = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (peer >= 0) {
                struct ucred credentials;
                socklen_t size = sizeof(credentials);
                if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0 ||
                    credentials.pid != (pid_t)pid) {
                    close(peer);
                    peer = -1;
                }
            } else if (errno != EAGAIN && errno != EINTR) {
                perror("accept Primary");
                status = EXIT_FAILURE;
                break;
            }
        }
        if (peer >= 0 && capture_cache_serve(&cache, peer, &next_offset, &armed) < 0) {
            if (errno != ECONNRESET) {
                perror("serve capture cache");
            }
            close(peer);
            peer = -1;
            armed = 0;
        }

        struct kvs_ebpf_event event;
        int result = kvs_ebpf_read(&event);
        if (result < 0) {
            fprintf(stderr, "read replication event: %s\n", strerror(-result));
            status = EXIT_FAILURE;
            break;
        }
        if (result == 0) {
            if (kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
                break;
            }
            struct pollfd wakeup = { .fd = peer >= 0 ? peer : listener, .events = POLLIN };
            poll(&wakeup, 1, 1);
            continue;
        }

        if (cache.data != NULL) {
            if (capture_cache_append(&cache, &event, armed) < 0) {
                fprintf(stderr, "capture cache invalid at offset=%" PRIu64
                                ", event=[%" PRIu64 ",%" PRIu64 "): %s\n",
                        cache.end, (uint64_t)event.start, (uint64_t)event.end,
                        strerror(errno));
                if (!socket_mode) {
                    status = EXIT_FAILURE;
                }
            }
            continue;
        }

        printf("range=[%" PRIu64 ",%" PRIu64 ") length=%u payload=",
               (uint64_t)event.start, (uint64_t)event.end, event.length);
        for (__u32 i = 0; i < event.length; i++) {
            printf("%02x", (unsigned int)event.payload[i]);
        }
        if (putchar('\n') == EOF || fflush(stdout) == EOF) {
            status = EXIT_FAILURE;
            break;
        }
    }

    capture_report_failures(&cache);
    if (cache.valid) {
        fprintf(stderr, "capture cache retained [%" PRIu64 ",%" PRIu64 ")\n",
                cache.start, cache.end);
    }
    if (peer >= 0) {
        close(peer);
    }
    if (listener >= 0) {
        close(listener);
        unlink(socket_path);
    }
    free(cache.data);
    kvs_ebpf_stop();
    return status;
}
