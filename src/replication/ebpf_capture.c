/*
 * 用户态的 BPF 管理模块。
 *
 * 负责加载 BPF 程序、挂到当前 Primary 的成功点，
 * 并在失败或退出时清理资源。
 *
 * 每次写入触发后的数据采集，由 BPF 程序 on_committed() 完成。
 * kvs_ebpf_read() 每次非阻塞取出一条事件，复制到调用方的记录里。
 * 本模块只采集事件和边界/水位，不负责网络转发。
 */
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "ebpf_capture.h"
#include "log.h"

/* 保存 libbpf 打开的对象，以及它所管理的 BPF 程序和 map。 */
static struct bpf_object *capture_object;
/* 保存 on_committed 与当前 Primary 成功点之间的挂载关系。 */
static struct bpf_link *capture_link;
static struct bpf_link *boundary_link;
static struct bpf_link *ready_link;
static struct ring_buffer *ready_queue;
static kvs_replica_ready_event_t *ready_destination;
/* 管理 ring buffer 的用户态映射。 */
static struct ring_buffer *capture_queue;
static int failure_map_fd = -1;
static int stream_map_fd = -1;
static int boundary_map_fd = -1;

/* 只在同步读取期间借用调用方的目标记录，不保留队列内存指针。 */
struct capture_read_context {
    struct kvs_ebpf_event *destination;
};
static struct capture_read_context read_context;

/* libbpf 收到负数才会停止本轮消费；用内部标记表示已取到一条。 */
#define KVS_EBPF_EVENT_READY (-EINPROGRESS)

static int receive_event(void *ctx, void *data, size_t size)
{
    struct capture_read_context *context = ctx;
    const struct kvs_ebpf_event *source = data;

    if (size != sizeof(*source)) {
        kvs_log_message(KVS_LOG_ERROR,
                        "ebpf: event size mismatch got=%zu expected=%zu\n",
                        size, sizeof(*source));
        return -EPROTO;
    }
    if (source->version != KVS_EBPF_EVENT_VERSION) {
        kvs_log_message(KVS_LOG_ERROR,
                        "ebpf: unsupported event version=%u expected=%u\n",
                        source->version, KVS_EBPF_EVENT_VERSION);
        return -EPROTONOSUPPORT;
    }
    if (source->length == 0 || source->length > sizeof(source->payload) ||
        source->end < source->start ||
        source->end - source->start != source->length)
        return -EPROTO;

    /*
     * 队列里的位置读完后会被复用，所以复制整条记录给调用方。
     * 随即结束本轮消费，把后续记录留给下一次 kvs_ebpf_read()。
     */
    memcpy(context->destination, source, sizeof(*source));
    return KVS_EBPF_EVENT_READY;
}

static int receive_ready(void *ctx, void *data, size_t size)
{
    (void)ctx;
    if (size != sizeof(*ready_destination) ||
        ((kvs_replica_ready_event_t *)data)->version != KVS_EBPF_EVENT_VERSION)
        return -EPROTO;
    memcpy(ready_destination, data, size);
    return KVS_EBPF_EVENT_READY;
}

int kvs_ebpf_read_ready(kvs_replica_ready_event_t *event)
{
    if (!event || !ready_queue) return -ENODEV;
    ready_destination = event;
    int result = ring_buffer__consume(ready_queue);
    ready_destination = NULL;
    return result == KVS_EBPF_EVENT_READY ? 1 : result < 0 ? result : 0;
}

int kvs_ebpf_is_ready(void)
{
    return capture_link != NULL && boundary_link != NULL && ready_link != NULL &&
           capture_queue != NULL && ready_queue != NULL;
}

int kvs_ebpf_get_failures(struct kvs_ebpf_failures *failures)
{
    if (failures == NULL)
        return -EINVAL;
    if (!kvs_ebpf_is_ready() || failure_map_fd < 0)
        return -ENODEV;

    __u64 counts[3];
    for (__u32 key = 0; key < 3; key++) {
        if (bpf_map_lookup_elem(failure_map_fd, &key, &counts[key]) < 0)
            return -errno;
    }
    failures->oversize = counts[0];
    failures->ring_full = counts[1];
    failures->read_error = counts[2];
    return 0;
}

int kvs_ebpf_read_state(struct kvs_ebpf_state *state)
{
    if (state == NULL)
        return -EINVAL;
    if (!kvs_ebpf_is_ready() || stream_map_fd < 0 || boundary_map_fd < 0)
        return -ENODEV;

    const __u32 key = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        struct kvs_ebpf_state sample = {0};
        struct kvs_ebpf_snapshot_boundary after;
        if (bpf_map_lookup_elem(boundary_map_fd, &key, &sample.boundary) < 0)
            return -errno;
        if (sample.boundary.generation == 0 || (sample.boundary.generation & 1))
            continue;
        if (bpf_map_lookup_elem(stream_map_fd, &key, &sample.stream_end) < 0)
            return -errno;
        int result = kvs_ebpf_get_failures(&sample.failures);
        if (result < 0)
            return result;
        if (bpf_map_lookup_elem(boundary_map_fd, &key, &after) < 0)
            return -errno;
        if (sample.boundary.generation != after.generation || (after.generation & 1))
            continue;
        result = kvs_ebpf_validate_state(&sample);
        if (result < 0)
            return result;
        *state = sample; /* 失败时不覆盖调用方状态，避免误用半份数据。 */
        return 0;
    }
    return -EAGAIN;
}

int kvs_ebpf_read(struct kvs_ebpf_event *event)
{
    if (event == NULL)
        return -EINVAL;
    if (capture_queue == NULL)
        return -ENODEV;

    read_context.destination = event;
    /* 只处理已经存在的事件，没有事件就立即返回。 */
    int result = ring_buffer__consume(capture_queue);
    read_context.destination = NULL;

    if (result == KVS_EBPF_EVENT_READY)
        return 1;
    if (result < 0)
        return result;
    return 0;
}

void kvs_ebpf_stop(void)
{
    /* 先解除探针挂载，再释放 BPF 对象；NULL 也可以安全清理。 */
    bpf_link__destroy(ready_link);
    ready_link = NULL;
    ring_buffer__free(ready_queue);
    ready_queue = NULL;
    ready_destination = NULL;
    bpf_link__destroy(boundary_link);
    boundary_link = NULL;
    bpf_link__destroy(capture_link);
    capture_link = NULL;
    ring_buffer__free(capture_queue);
    capture_queue = NULL;
    failure_map_fd = -1;
    stream_map_fd = -1;
    boundary_map_fd = -1;
    read_context.destination = NULL;
    bpf_object__close(capture_object);
    capture_object = NULL;
}

/* 拒绝不匹配的对象布局，lookup 前保证目标缓冲区大小正确。 */
static int kvs_ebpf_state_map_fd(const char *name, size_t value_bytes)
{
    struct bpf_map *map = bpf_object__find_map_by_name(capture_object, name);
    if (map == NULL)
        return -ENOENT;
    if (bpf_map__type(map) != BPF_MAP_TYPE_ARRAY ||
        bpf_map__key_size(map) != sizeof(__u32) ||
        bpf_map__value_size(map) != value_bytes || bpf_map__max_entries(map) != 1)
        return -EPROTO;
    return bpf_map__fd(map);
}

static int kvs_ebpf_attach(const char *object_path, pid_t pid,
                           const char *executable)
{
    if (kvs_ebpf_is_ready())
        return 0;
    if (object_path == NULL || object_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* 1. 在用户态打开并解析 .bpf.o，此时尚未加载到内核。 */
    capture_object = bpf_object__open_file(object_path, NULL);
    long error = libbpf_get_error(capture_object);
    if (capture_object == NULL || error != 0) {
        capture_object = NULL;
        if (error != 0)
            errno = (int)-error;
        return -1;
    }

    /* 2. 创建 map 并把程序加载到内核，由内核验证；此时尚未挂载。 */
    int result = bpf_object__load(capture_object);
    if (result < 0) {
        error = result;
        goto failed;
    }

    /* 在挂探针前准备接收队列，登记用户态的 receive_event 回调。 */
    int map_fd = bpf_object__find_map_fd_by_name(capture_object, "events");
    if (map_fd < 0) {
        error = map_fd;
        goto failed;
    }
    failure_map_fd = bpf_object__find_map_fd_by_name(capture_object, "failure_count");
    if (failure_map_fd < 0) {
        error = failure_map_fd;
        goto failed;
    }
    stream_map_fd = kvs_ebpf_state_map_fd("stream_end", sizeof(__u64));
    boundary_map_fd = kvs_ebpf_state_map_fd("snapshot_boundary",
        sizeof(struct kvs_ebpf_snapshot_boundary));
    if (stream_map_fd < 0 || boundary_map_fd < 0) {
        error = stream_map_fd < 0 ? stream_map_fd : boundary_map_fd;
        goto failed;
    }
    capture_queue = ring_buffer__new(map_fd, receive_event, &read_context, NULL);
    if (capture_queue == NULL) {
        error = -errno;
        goto failed;
    }

    /* 3. 找到负责复制命令到 ring buffer 的 BPF 程序。 */
    struct bpf_program *program = bpf_object__find_program_by_name(
        capture_object, "on_committed");
    if (program == NULL) {
        error = -ENOENT;
        goto failed;
    }

    struct bpf_uprobe_opts options = {
        .sz = sizeof(options),
        .func_name = "kvs_replication_committed"
    };
    /* 4. 只挂指定进程；调用目标函数时由内核触发 BPF。 */
    capture_link = bpf_program__attach_uprobe_opts(
        program, pid, executable, 0, &options);
    error = libbpf_get_error(capture_link);
    if (capture_link == NULL || error != 0) {
        capture_link = NULL;
        if (error == 0)
            error = -errno;
        goto failed;
    }

    program = bpf_object__find_program_by_name(capture_object, "on_snapshot_boundary");
    if (program == NULL) {
        error = -ENOENT;
        goto failed;
    }
    options.func_name = "kvs_replication_snapshot_boundary";
    boundary_link = bpf_program__attach_uprobe_opts(
        program, pid, executable, 0, &options);
    error = libbpf_get_error(boundary_link);
    if (boundary_link == NULL || error != 0) {
        boundary_link = NULL;
        if (error == 0)
            error = -errno;
        goto failed;
    }
    map_fd = bpf_object__find_map_fd_by_name(capture_object, "ready_events");
    if (map_fd < 0) { error = map_fd; goto failed; }
    ready_queue = ring_buffer__new(map_fd, receive_ready, NULL, NULL);
    if (!ready_queue) { error = -errno; goto failed; }
    program = bpf_object__find_program_by_name(capture_object, "on_replica_ready");
    if (!program) { error = -ENOENT; goto failed; }
    options.func_name = "kvs_replication_replica_ready";
    ready_link = bpf_program__attach_uprobe_opts(program, pid, executable, 0, &options);
    error = libbpf_get_error(ready_link);
    if (!ready_link || error) {
        ready_link = NULL;
        if (!error) error = -errno;
        goto failed;
    }
    return 0;

failed:
    /* 任一步失败，都收回前面已获取的资源，并向调用方报告原因。 */
    kvs_ebpf_stop();
    errno = (int)-error;
    return -1;
}

int kvs_ebpf_start_replication(const char *object_path, pid_t primary_pid)
{
    if (primary_pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    char executable[64];
    int length = snprintf(executable, sizeof(executable), "/proc/%ld/exe",
                          (long)primary_pid);
    if (length < 0 || (size_t)length >= sizeof(executable)) {
        errno = EINVAL;
        return -1;
    }
    return kvs_ebpf_attach(object_path, primary_pid, executable);
}
