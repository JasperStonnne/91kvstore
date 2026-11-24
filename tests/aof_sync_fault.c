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

int __real_kvs_uring_io_write_and_datasync(kvs_uring_io_t *, int,
    const void *, size_t, off_t);

int __wrap_kvs_uring_io_write_and_datasync(kvs_uring_io_t *io, int fd,
    const void *data, size_t length, off_t offset)
{
    const char *trigger = getenv("KVS_TEST_SYNC_TRIGGER");
    if (trigger && access(trigger, F_OK) == 0) {
        kvs_uring_write_t write = { .data = data, .length = length, .offset = offset };
        if (kvs_uring_io_write_batch(io, fd, &write, 1) < 0) return -1;
        return __wrap_kvs_uring_io_sync(io, fd);
    }
    return __real_kvs_uring_io_write_and_datasync(io, fd, data, length, offset);
}
