#include "uring_io.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int __real_kvs_uring_io_sync(kvs_uring_io_t *io, int fd);

/* 仅 test_kvstore_sync_fault 链接此包装器；正式程序不包含故障开关。 */
int __wrap_kvs_uring_io_sync(kvs_uring_io_t *io, int fd)
{
    static int injected;
    const char *target = getenv("KVS_TEST_AOF_PATH");
    const char *trigger = getenv("KVS_TEST_SYNC_TRIGGER");
    char descriptor[64], path[4096];
    if (!injected && target && trigger && access(trigger, F_OK) == 0) {
        snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(descriptor, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = '\0';
            if (strcmp(path, target) == 0) {
                injected = 1;
                fprintf(stderr, "test: injected AOF sync EIO\n");
                errno = EIO;
                return -1;
            }
        }
    }
    return __real_kvs_uring_io_sync(io, fd);
}
