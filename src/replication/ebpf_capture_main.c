#define _GNU_SOURCE
#include "ebpf_capture.h"
#include "capture_server.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_capture(int signo) { (void)signo; stopping = 1; }

int main(int argc, char **argv)
{
    int listen_mode = argc == 5 && strcmp(argv[3], "--listen") == 0;
    if (argc != 3 && !listen_mode) {
        fprintf(stderr, "usage: %s PRIMARY_PID BPF_OBJECT [--listen IPv4:PORT]\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end;
    errno = 0;
    long pid = strtol(argv[1], &end, 10);
    if (errno || *end || pid <= 0 || pid > INT_MAX)
        return EXIT_FAILURE;
    struct sigaction action = { .sa_handler = stop_capture };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
        return EXIT_FAILURE;
    if (kvs_ebpf_start_replication(argv[2], (pid_t)pid) < 0) {
        perror("attach replication probes");
        return EXIT_FAILURE;
    }
    int result = 0;
    if (listen_mode) {
        result = capture_server_run(argv[4], (pid_t)pid, &stopping);
        if (result < 0)
            perror("capture TCP server");
    } else {
        fprintf(stderr, "replication uprobe attached to Primary %ld\n", pid);
        while (!stopping) {
            struct kvs_ebpf_event event;
            int count = kvs_ebpf_read(&event);
            if (count < 0) { result = -1; break; }
            if (!count) {
                if (kill((pid_t)pid, 0) < 0 && errno == ESRCH) break;
                poll(NULL, 0, 1);
                continue;
            }
            printf("range=[%" PRIu64 ",%" PRIu64 ") length=%u payload=",
                   (uint64_t)event.start, (uint64_t)event.end, event.length);
            for (__u32 i = 0; i < event.length; i++) printf("%02x", event.payload[i]);
            if (putchar('\n') == EOF || fflush(stdout) == EOF) { result = -1; break; }
        }
    }
    kvs_ebpf_stop();
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
