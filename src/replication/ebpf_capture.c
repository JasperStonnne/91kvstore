/*
 * 用户态的 BPF 管理模块。
 *
 * 负责加载 BPF 程序、挂到当前 Primary 的成功点，
 * 并在失败或退出时清理资源。
 *
 * 每次写入触发后的数据采集，由 BPF 程序 on_committed() 完成。
 * kvs_ebpf_read() 每次非阻塞取出一条事件，复制到调用方的记录里。
 * TCP 发送和 AOF 缺口补偿由复制模块负责。
 */
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "ebpf_capture.h"
#include "log.h"

/* 保存 libbpf 打开的对象，以及它所管理的 BPF 程序和 map。 */
static struct bpf_object *capture_object;
/* 保存 on_committed 与当前 Primary 成功点之间的挂载关系。 */
static struct bpf_link *capture_link;
/* 管理 ring buffer 的用户态映射。 */
static struct ring_buffer *capture_queue;
static int failure_map_fd = -1;

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
        source->end > LLONG_MAX || source->end < source->start ||
        source->end - source->start != source->length)
        return -EPROTO;

    /*
     * 队列里的位置读完后会被复用，所以复制整条记录给调用方。
     * 随即结束本轮消费，把后续记录留给下一次 kvs_ebpf_read()。
     */
    memcpy(context->destination, source, sizeof(*source));
    return KVS_EBPF_EVENT_READY;
}

int kvs_ebpf_is_ready(void)
{
    return capture_link != NULL && capture_queue != NULL;
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
    bpf_link__destroy(capture_link);
    capture_link = NULL;
    ring_buffer__free(capture_queue);
    capture_queue = NULL;
    failure_map_fd = -1;
    read_context.destination = NULL;
    bpf_object__close(capture_object);
    capture_object = NULL;
}

int kvs_ebpf_start(const char *object_path)
{
    if (capture_link != NULL)
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
        .func_name = "kvs_aof_committed"
    };
    /* 4. 只挂当前进程自己的成功点；调用该点时由内核触发 BPF。 */
    capture_link = bpf_program__attach_uprobe_opts(
        program, getpid(), "/proc/self/exe", 0, &options);
    error = libbpf_get_error(capture_link);
    if (capture_link == NULL || error != 0) {
        capture_link = NULL;
        if (error == 0)
            error = -errno;
        goto failed;
    }
    return 0;

failed:
    /* 任一步失败，都收回前面已获取的资源，并向调用方报告原因。 */
    kvs_ebpf_stop();
    errno = (int)-error;
    return -1;
}
