/* 真实 BPF 测试助手：stdin 请求状态，stdout 返回一行 JSON；不发送网络数据。 */
#include "ebpf_capture.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 3)
        return EXIT_FAILURE;
    char *end;
    errno = 0;
    long pid = strtol(argv[1], &end, 10);
    if (errno || *end || pid <= 0 || pid > INT_MAX)
        return EXIT_FAILURE;
    if (kvs_ebpf_start_replication(argv[2], (pid_t)pid) < 0) {
        perror("attach state probes");
        return EXIT_FAILURE;
    }
    puts("READY");
    fflush(stdout);
    char line[256], primary_id[41], sync_id[41];
    uint64_t offset, cached;
    int status = EXIT_SUCCESS;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%40s %40s %" SCNu64 " %" SCNu64,
                   primary_id, sync_id, &offset, &cached) != 4) {
            status = EXIT_FAILURE;
            break;
        }
        struct kvs_ebpf_event event;
        unsigned int events = 0;
        int result;
        while ((result = kvs_ebpf_read(&event)) > 0)
            events++;
        if (result < 0) {
            status = EXIT_FAILURE;
            break;
        }
        struct kvs_ebpf_state state = {0};
        int read_result = kvs_ebpf_read_state(&state);
        int match = read_result < 0 ? read_result :
            kvs_ebpf_match_boundary(&state, primary_id, sync_id, offset);
        int progress = read_result < 0 ? read_result :
            kvs_ebpf_check_progress(&state, cached);
        printf("{\"read\":%d,\"match\":%d,\"progress\":%d,\"events\":%u,"
               "\"generation\":%" PRIu64 ",\"S\":%" PRIu64 ",\"end\":%" PRIu64 ","
               "\"oversize\":%" PRIu64 ",\"ring_full\":%" PRIu64 ",\"read_error\":%" PRIu64 "}\n",
               read_result, match, progress, events,
               (uint64_t)state.boundary.generation, (uint64_t)state.boundary.stream_offset,
               state.stream_end, (uint64_t)state.failures.oversize,
               (uint64_t)state.failures.ring_full, (uint64_t)state.failures.read_error);
        fflush(stdout);
    }
    kvs_ebpf_stop();
    return ferror(stdin) ? EXIT_FAILURE : status;
}
