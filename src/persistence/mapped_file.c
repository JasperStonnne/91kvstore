#include "mapped_file.h"

#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int kvs_mapped_file_close(kvs_mapped_file_t *file)
{
    /* 必须传入一个有效的管理对象。 */
    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 非空文件才真正建立过映射，需要通过 munmap 释放。 */
    if (file->length > 0) {
        if (munmap((void *)file->data, file->length) < 0) {
            /* 释放失败时保留信息，不假装已经释放成功。 */
            return -1;
        }
    }

    /* 释放成功，或本来就是空对象，统一重置状态。 */
    file->data = NULL;
    file->length = 0;
    return 0;
}

int kvs_mapped_file_open(
    const char *path,
    kvs_mapped_file_t *file)
{
    /* 不允许覆盖尚未释放的映射。 */
    if (path == NULL || path[0] == '\0' || file == NULL ||
        file->data != NULL || file->length != 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    int result = -1;
    int saved_errno;
    size_t length = 0;
    void *address = MAP_FAILED;
    struct stat info;

    /* 从已经打开的文件取得类型和大小。 */
    if (fstat(fd, &info) < 0) {
        goto done;
    }

    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        errno = EINVAL;
        goto done;
    }

    /* 文件大小必须能用 size_t 表示。 */
    if ((uintmax_t)info.st_size > (uintmax_t)SIZE_MAX) {
        errno = EOVERFLOW;
        goto done;
    }

    length = (size_t)info.st_size;

    /* 空文件不调用 mmap，以空对象表示。 */
    if (length > 0) {
        address = mmap(
            NULL,         /* 让内核选择映射地址。 */
            length,       /* 映射整个文件。 */
            PROT_READ,    /* 只允许读取。 */
            MAP_PRIVATE,  /* 私有映射，不用于写回文件。 */
            fd,
            0             /* 从文件开头开始。 */
        );

        if (address == MAP_FAILED) {
            goto done;
        }
    }

    result = 0;

done:
    saved_errno = errno;

    /*
     * 映射成功后也可以关闭 fd，映射仍然有效。
     * 使用系统调用，避免进入 NtyCo 的 close hook。
     */
    if (syscall(SYS_close, fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    if (result < 0) {
        /* 如果已经映射，但后续关闭文件失败，撤销本次映射。 */
        if (address != MAP_FAILED) {
            munmap(address, length);
        }

        errno = saved_errno;
        return -1;
    }

    /* 全部成功后，才把映射交给调用方管理。 */
    file->data = length == 0 ? NULL : address;
    file->length = length;
    return 0;
}
