/* 仅测试程序链接：所有注入限定到控制器指定的快照和触发文件。 */
#include "uring_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int claimed;

static int armed(const char *mode)
{
    const char *selected = getenv("KVS_TEST_SNAPSHOT_FAULT");
    const char *trigger = getenv("KVS_TEST_SNAPSHOT_TRIGGER");
    return !claimed && selected && trigger && strcmp(selected, mode) == 0 &&
           access(trigger, F_OK) == 0;
}

static int temporary_path(const char *path)
{
    const char *target = getenv("KVS_TEST_SNAPSHOT_PATH");
    if (!target) return 0;
    size_t length = strlen(target);
    return strlen(path) == length + 11 && strncmp(path, target, length) == 0 &&
           strncmp(path + length, ".tmp.", 5) == 0;
}

static int descriptor_path(int fd, char *path, size_t capacity)
{
    char descriptor[64];
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(descriptor, path, capacity - 1);
    if (n < 0) return 0;
    path[n] = '\0';
    return 1;
}

static void pause_at(const char *point)
{
    const char *marker = getenv("KVS_TEST_SNAPSHOT_REACHED");
    if (!marker) _exit(125);
    int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) _exit(125);
    size_t length = strlen(point);
    long written = syscall(SYS_write, fd, point, length);
    syscall(SYS_close, fd);
    if (written < 0 || (size_t)written != length) _exit(125);
    fprintf(stderr, "test: paused Snapshot at %s\n", point);
    raise(SIGSTOP);
}

int __real_kvs_uring_io_write_batch(kvs_uring_io_t *, int,
                                  const kvs_uring_write_t *, size_t);
int __wrap_kvs_uring_io_write_batch(kvs_uring_io_t *io, int fd,
                                  const kvs_uring_write_t *writes, size_t count)
{
    char path[PATH_MAX];
    if (armed("crash-during-write") && count > 0 && writes &&
        descriptor_path(fd, path, sizeof(path)) && temporary_path(path)) {
        claimed = 1;
        /* 真正写出前 16 字节，再停住；不伪造内核完成结果。 */
        kvs_uring_write_t partial = writes[0];
        if (partial.length > 16) partial.length = 16;
        int result = __real_kvs_uring_io_write_batch(io, fd, &partial, 1);
        if (result == 0) {
            pause_at("during-write");
            _exit(125); /* 控制器应 SIGKILL，不允许把部分写冒充整批成功。 */
        }
        return result;
    }
    return __real_kvs_uring_io_write_batch(io, fd, writes, count);
}

int __real_kvs_uring_io_sync(kvs_uring_io_t *, int);
int __wrap_kvs_uring_io_sync(kvs_uring_io_t *io, int fd)
{
    char path[PATH_MAX];
    if ((armed("file-sync") || armed("crash-before-file-sync") ||
         armed("crash-after-file-sync")) &&
        descriptor_path(fd, path, sizeof(path)) && temporary_path(path)) {
        int fail = armed("file-sync");
        int before = armed("crash-before-file-sync");
        claimed = 1;
        if (fail) {
            fprintf(stderr, "test: injected Snapshot temporary-file sync EIO\n");
            errno = EIO;
            return -1;
        }
        if (before) pause_at("before-file-sync");
        int result = __real_kvs_uring_io_sync(io, fd);
        if (!before && result == 0) pause_at("after-file-sync");
        return result;
    }
    return __real_kvs_uring_io_sync(io, fd);
}

int __real_rename(const char *, const char *);
int __wrap_rename(const char *oldpath, const char *newpath)
{
    const char *target = getenv("KVS_TEST_SNAPSHOT_PATH");
    char old_absolute[PATH_MAX], new_absolute[PATH_MAX];
    if ((armed("rename") || armed("crash-before-rename") ||
         armed("crash-after-rename")) && target &&
        realpath(oldpath, old_absolute) && realpath(newpath, new_absolute) &&
        strcmp(new_absolute, target) == 0 && temporary_path(old_absolute)) {
        int fail = armed("rename");
        int before = armed("crash-before-rename");
        claimed = 1;
        if (fail) {
            fprintf(stderr, "test: injected Snapshot rename EIO\n");
            errno = EIO;
            return -1;
        }
        if (before) pause_at("before-rename");
        int result = __real_rename(oldpath, newpath);
        if (!before && result == 0) pause_at("after-rename");
        return result;
    }
    return __real_rename(oldpath, newpath);
}

int __real_fsync(int);
int __wrap_fsync(int fd)
{
    const char *target = getenv("KVS_TEST_SNAPSHOT_PATH");
    struct stat info;
    char path[PATH_MAX];
    if ((armed("dir-sync") || armed("crash-after-dir-sync")) && target &&
        fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) &&
        descriptor_path(fd, path, sizeof(path))) {
        const char *slash = strrchr(target, '/');
        if (slash && strlen(path) == (size_t)(slash - target) &&
            strncmp(path, target, strlen(path)) == 0) {
            int fail = armed("dir-sync");
            claimed = 1;
            if (fail) {
                fprintf(stderr, "test: injected Snapshot parent-directory sync EIO\n");
                errno = EIO;
                return -1;
            }
            int result = __real_fsync(fd);
            if (result == 0) pause_at("after-dir-sync");
            return result;
        }
    }
    return __real_fsync(fd);
}
