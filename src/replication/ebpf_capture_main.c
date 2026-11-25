#define _GNU_SOURCE
#include "ebpf_capture.h"
#include "capture_server.h"
#include "process_target.h"
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
        fprintf(stderr, "usage: %s PRIMARY_PID|PROCESS_NAME BPF_OBJECT [--listen IPv4:PORT]\n", argv[0]);
        return EXIT_FAILURE;
    }
    pid_t pid;
    if (kvs_resolve_process_target(argv[1], &pid, stderr) < 0)
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
        fprintf(stderr, "replication uprobe attached to Primary %ld\n", (long)pid);
        result = capture_client_run(pid, &stopping);

    }
    kvs_ebpf_stop();
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
