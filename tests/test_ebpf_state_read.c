/* 只替换 map lookup 的系统调用边界，执行生产读取逻辑；不加载 BPF。 */
#include "../src/replication/ebpf_capture.c"
#include <assert.h>

enum scenario { STABLE, UNKNOWN, UPDATING, CHANGED, BUSY, IO_ERROR, BAD_WATERMARK };
static enum scenario scenario;
static unsigned int boundary_reads;

int __wrap_bpf_map_lookup_elem(int fd, const void *key, void *value)
{
    __u32 index = *(const __u32 *)key;
    if (scenario == IO_ERROR) {
        errno = EIO;
        return -1;
    }
    if (fd == 1) {
        struct kvs_ebpf_snapshot_boundary boundary = {0};
        boundary_reads++;
        boundary.generation = scenario == UNKNOWN ? 0 :
                              scenario == UPDATING ? 1 :
                              scenario == BUSY ? 2 * boundary_reads :
                              scenario == CHANGED && boundary_reads > 1 ? 4 : 2;
        boundary.stream_offset = boundary.generation == 4 ? 120 : 100;
        memset(boundary.primary_id, 'a', KVS_EBPF_ID_BYTES);
        memset(boundary.full_sync_id, 'b', KVS_EBPF_ID_BYTES);
        memcpy(value, &boundary, sizeof(boundary));
    } else if (fd == 2) {
        *(__u64 *)value = scenario == BAD_WATERMARK ? 99 : 130;
    } else {
        assert(fd == 3 && index < 3);
        *(__u64 *)value = 0;
    }
    return 0;
}

static void reject_without_output(enum scenario next, int expected)
{
    scenario = next;
    boundary_reads = 0;
    struct kvs_ebpf_state actual, before;
    memset(&actual, 0x5a, sizeof(actual));
    memcpy(&before, &actual, sizeof(actual));
    assert(kvs_ebpf_read_state(&actual) == expected);
    assert(memcmp(&before, &actual, sizeof(actual)) == 0);
    assert(boundary_reads <= 16); /* 更新繁忙时也有重试上限。 */
}

int main(void)
{
    assert(kvs_ebpf_read_state(NULL) == -EINVAL);
    struct kvs_ebpf_state state;
    assert(kvs_ebpf_read_state(&state) == -ENODEV);
    /* 哨兵只用于可用性判定；本测试不会 attach/destroy 这些指针。 */
    capture_link = (struct bpf_link *)&state;
    boundary_link = (struct bpf_link *)&state;
    capture_queue = (struct ring_buffer *)&state;
    boundary_map_fd = 1;
    stream_map_fd = 2;
    failure_map_fd = 3;
    assert(kvs_ebpf_read_state(&state) == 0);
    assert(state.boundary.generation == 2 && state.stream_end == 130);
    reject_without_output(UNKNOWN, -EAGAIN);
    reject_without_output(UPDATING, -EAGAIN);
    reject_without_output(BUSY, -EAGAIN);
    reject_without_output(IO_ERROR, -EIO);
    reject_without_output(BAD_WATERMARK, -EPROTO);
    scenario = CHANGED;
    boundary_reads = 0;
    assert(kvs_ebpf_read_state(&state) == 0);
    assert(boundary_reads == 4 && state.boundary.generation == 4 &&
           state.boundary.stream_offset == 120 && state.stream_end == 130);
    puts("[PASS] eBPF map reads: generation change retries, bounded busy/unknown, errors preserve output");
    return 0;
}
