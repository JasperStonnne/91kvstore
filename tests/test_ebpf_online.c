/*
 * 验证真实 ONLINE 发送逻辑；仅以可控事件队列和内存 AOF 替代采集及文件输入。
 * 不加载 BPF、不访问 RDMA。直接包含实现以核对分段中间状态，不新增业务测试接口。
 */
#include "../src/replication/replication.c"
#include <assert.h>

static char aof_data[16384];
static long long aof_end;
static struct kvs_ebpf_event input_events[128];
static size_t input_count;
static size_t input_next;
static int capture_ready;
static int read_error;
static struct kvs_ebpf_failures failures;

int kvs_ebpf_is_ready(void) { return capture_ready; }
void kvs_ebpf_stop(void) { capture_ready = 0; }
int kvs_ebpf_get_failures(struct kvs_ebpf_failures *result)
{
    *result = failures;
    return 0;
}
int kvs_ebpf_read(struct kvs_ebpf_event *event)
{
    if (read_error)
        return read_error;
    if (input_next == input_count)
        return 0;
    *event = input_events[input_next++];
    return 1;
}
long long kvs_aof_get_offset(void) { return aof_end; }
int kvs_aof_reader_open(kvs_aof_reader_t *reader, const char *path,
                        long long start, long long end)
{
    (void)path;
    assert(start >= 0 && end >= start && end <= aof_end);
    reader->fd = 1;
    reader->offset = start;
    reader->remaining = end - start;
    return 0;
}
int kvs_aof_reader_read(kvs_aof_reader_t *reader, char *output, int capacity)
{
    size_t bytes = (size_t)reader->remaining;
    if (bytes > (size_t)capacity)
        bytes = (size_t)capacity;
    memcpy(output, aof_data + reader->offset, bytes);
    reader->offset += bytes;
    reader->remaining -= bytes;
    return (int)bytes;
}
void kvs_aof_reader_close(kvs_aof_reader_t *reader)
{
    reader->fd = -1;
    reader->offset = 0;
    reader->remaining = 0;
}

static void reset(long long length, long long sent)
{
    kvs_replication_ebpf_reset();
    replication_manager.replication_offset = sent;
    replication_manager.aof_reader.fd = -1;
    aof_end = length;
    input_count = input_next = 0;
    capture_ready = 1;
    read_error = 0;
    memset(&failures, 0, sizeof(failures));
    for (size_t i = 0; i < sizeof(aof_data); i++)
        aof_data[i] = (char)('a' + i % 26);
}

static void enqueue(long long start, long long end)
{
    assert(input_count < sizeof(input_events) / sizeof(input_events[0]));
    assert(end > start && end - start <= KVS_EBPF_MAX_PAYLOAD);
    struct kvs_ebpf_event *event = &input_events[input_count++];
    event->version = KVS_EBPF_EVENT_VERSION;
    event->start = start;
    event->end = end;
    event->length = (unsigned int)(end - start);
    memcpy(event->payload, aof_data + start, event->length);
}

static void drain(long long start, long long end)
{
    char output[1024];
    long long position = start;
    while (position < end) {
        int bytes = kvs_replication_ebpf_stream(output, sizeof(output));
        assert(bytes > 0 && bytes <= (int)sizeof(output));
        assert(position + bytes <= end);
        assert(memcmp(output, aof_data + position, (size_t)bytes) == 0);
        position += bytes;
    }
    assert(replication_manager.replication_offset == end);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == KVS_STREAM_WAIT);
}

int main(void)
{
    kvs_log_set_level(KVS_LOG_ERROR);

    reset(2500, 0);
    enqueue(0, 2500);
    char output[1024];
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == 1024);
    assert(replication_manager.pending_event_sent == 1024);
    assert(replication_manager.replication_offset == 0);
    assert(memcmp(output, aof_data, 1024) == 0);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == 1024);
    assert(memcmp(output, aof_data + 1024, 1024) == 0);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == 452);
    assert(memcmp(output, aof_data + 2048, 452) == 0);
    assert(replication_manager.pending_event.length == 0);
    assert(replication_manager.replication_offset == 2500);
    assert(input_next == 1);
    assert(replication_manager.ebpf_stats.payload_events == 1);
    puts("[PASS] one event spans 1024/1024/452 bytes without rereading or truncation");

    reset(24, 12);
    enqueue(0, 12);
    enqueue(12, 24);
    drain(12, 24);
    assert(replication_manager.ebpf_stats.duplicate_events == 1);
    assert(replication_manager.ebpf_stats.compensation_ranges == 0);
    puts("[PASS] catch-up fence skips covered events and starts exactly at the boundary");

    reset(36, 0);
    enqueue(0, 12);
    enqueue(24, 36);
    drain(0, 36);
    assert(replication_manager.ebpf_stats.payload_events == 2);
    assert(replication_manager.ebpf_stats.compensation_ranges == 1);
    assert(replication_manager.ebpf_stats.compensation_bytes == 12);
    puts("[PASS] middle gap is replayed before the retained later event");

    reset(24, 0);
    enqueue(0, 12);
    failures.ring_full = 1;
    drain(0, 24);
    assert(replication_manager.ebpf_stats.payload_events == 1);
    assert(replication_manager.ebpf_stats.compensation_bytes == 12);
    puts("[PASS] lost final event is detected without a later event");

    reset(5000, 0);
    failures.oversize = 1;
    drain(0, 5000);
    assert(replication_manager.ebpf_stats.payload_events == 0);
    assert(replication_manager.ebpf_stats.compensation_bytes == 5000);
    puts("[PASS] oversize compensation preserves all 5000 bytes");

    reset(12, 12);
    for (int i = 0; i < 100; i++)
        enqueue(0, 12);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == KVS_STREAM_WAIT);
    assert(input_next == KVS_EBPF_DRAIN_BUDGET);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == KVS_STREAM_WAIT);
    assert(input_next == 100);
    puts("[PASS] stale event drain is bounded per call");

    reset(12, 6);
    enqueue(0, 12);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == -1);
    reset(12, 0);
    read_error = -EIO;
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) ==
           KVS_EBPF_AOF_FALLBACK);
    assert(!capture_ready);
    reset(0, 12);
    assert(kvs_replication_ebpf_stream(output, sizeof(output)) == -1);
    puts("[PASS] overlap, capture failure and AOF truncation have explicit outcomes");
    return 0;
}
