#include<stdio.h>
#include <unistd.h>
#include<errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include<string.h>
#include <fcntl.h>       // open、O_RDONLY
#include <sys/stat.h>    // fstat、struct stat、S_ISREG
#include <sys/syscall.h> // SYS_close，绕过 NtyCo 的 close hook
#include <stdint.h>
#include <sys/mman.h>
#include "mapped_file.h"
#include "uring_io.h"
#include"persistence.h"
#include "protocol.h"
#include "kvstore.h"

/*
 * AOF 写入侧的状态。
 * 打开时创建资源，追加时复用，关闭时释放。
 */
typedef struct {
    /* AOF 文件描述符，由打开函数取得、关闭函数释放。 */
    int fd;

    /*
     * 下一条命令写入文件的起始字节位置。
     * 打开已有文件时，从文件末尾继续。
     */
    off_t offset;
    kvs_aof_checksum_t checksum;
    uint32_t crc;
    int failed;

    /* 保存期间复用的 io_uring 队列。 */
    kvs_uring_io_t io;
} aof_writer_t;

/* fd=-1 表示当前没有打开 AOF。 */
static aof_writer_t aof_writer = {
    .fd = -1
};

/*
 * 直接调用 Linux 内核关闭普通文件，
 * 避免进入 NtyCo 为网络 socket 重写的 close()。
 */
static int kvs_aof_file_close(int fd)
{
    if(fd<0){
        return -1;
    }

    return (int)syscall(SYS_close,fd);
}
/* file 模式缓存头部和命令区的 CRC 中间状态，旧尾部不参与下一次追加。 */
typedef struct {
    size_t command_start;
    size_t command_end;
    uint32_t crc;
} aof_layout_t;

static int aof_valid_checksum(kvs_aof_checksum_t checksum)
{
    return checksum == KVS_AOF_CHECKSUM_NONE || checksum == KVS_AOF_CHECKSUM_FILE;
}

static void aof_encode_u32(unsigned char *bytes, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++) {
        bytes[i] = (unsigned char)(value >> (24 - i * 8));
    }
}

static uint32_t aof_decode_u32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void aof_encode_u64(unsigned char *bytes, uint64_t value)
{
    for (unsigned int i = 0; i < 8; i++) {
        bytes[i] = (unsigned char)(value >> (56 - i * 8));
    }
}

static uint64_t aof_decode_u64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; i++) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

static uint32_t aof_crc32_update(uint32_t crc, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ UINT32_C(0xedb88320);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* 尾部长度参与最终 CRC，返回值不改变缓存的命令区 CRC。 */
static void aof_encode_trailer(unsigned char *trailer, uint64_t command_bytes,
                               uint32_t prefix_crc)
{
    aof_encode_u64(trailer, command_bytes);
    uint32_t crc = aof_crc32_update(prefix_crc, trailer, 8);
    aof_encode_u32(trailer + 8, crc ^ UINT32_C(0xffffffff));
}

/* 校验配置与格式、尾部长度及完整 CRC；恢复和重新打开追加复用此入口。 */
static int aof_inspect(const kvs_mapped_file_t *file, kvs_aof_checksum_t checksum,
                       aof_layout_t *layout)
{
    int has_header = file->length >= 8 && memcmp(file->data, KVS_AOF_MAGIC, 8) == 0;
    if (checksum == KVS_AOF_CHECKSUM_NONE) {
        if (has_header) {
            fprintf(stderr, "aof: aof_checksum=none requires raw RESP; found file-checksum format\n");
            errno = EINVAL;
            return -1;
        }
        *layout = (aof_layout_t){.command_end = file->length};
        return 0;
    }
    if (!has_header || file->length < KVS_AOF_HEADER_SIZE + KVS_AOF_TRAILER_SIZE) {
        fprintf(stderr, "aof: aof_checksum=file requires a complete 91KVAOF1 header and trailer\n");
        errno = EINVAL;
        return -1;
    }
    if (aof_decode_u32(file->data + 8) != KVS_AOF_VERSION ||
        aof_decode_u32(file->data + 12) != 0) {
        fprintf(stderr, "aof: invalid file-checksum version or reserved field\n");
        errno = EINVAL;
        return -1;
    }
    size_t command_end = file->length - KVS_AOF_TRAILER_SIZE;
    const unsigned char *trailer = file->data + command_end;
    if (aof_decode_u64(trailer) != command_end - KVS_AOF_HEADER_SIZE) {
        fprintf(stderr, "aof: command length does not match file-checksum trailer\n");
        errno = EINVAL;
        return -1;
    }
    uint32_t crc = aof_crc32_update(UINT32_C(0xffffffff), file->data, command_end);
    uint32_t actual = aof_crc32_update(crc, trailer, 8) ^ UINT32_C(0xffffffff);
    if (actual != aof_decode_u32(trailer + 8)) {
        fprintf(stderr, "aof: whole-file CRC32 mismatch\n");
        errno = EINVAL;
        return -1;
    }
    *layout = (aof_layout_t){
        .command_start = KVS_AOF_HEADER_SIZE, .command_end = command_end, .crc = crc
    };
    return 0;
}

/* 校验即将用于追加的同一个 fd，避免重新按路径打开另一个 inode。 */
static int aof_inspect_fd(int fd, size_t length, kvs_aof_checksum_t checksum,
                          aof_layout_t *layout)
{
    kvs_mapped_file_t file = {.length = length};
    if (length > 0) {
        void *data = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            return -1;
        }
        file.data = data;
    }
    int result = aof_inspect(&file, checksum, layout);
    int saved_errno = errno;
    if (kvs_mapped_file_close(&file) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    errno = saved_errno;
    return result;
}

/* 新文件先持久化合法的空文件，已有的零字节 file 日志按损坏拒绝。 */
static int aof_initialize_file(int fd, kvs_uring_io_t *io, aof_layout_t *layout)
{
    unsigned char bytes[KVS_AOF_HEADER_SIZE + KVS_AOF_TRAILER_SIZE] = {0};
    memcpy(bytes, KVS_AOF_MAGIC, 8);
    aof_encode_u32(bytes + 8, KVS_AOF_VERSION);
    uint32_t crc = aof_crc32_update(UINT32_C(0xffffffff), bytes, KVS_AOF_HEADER_SIZE);
    aof_encode_trailer(bytes + KVS_AOF_HEADER_SIZE, 0, crc);
    kvs_uring_write_t request = {.data = bytes, .length = sizeof(bytes), .offset = 0};
    if (kvs_uring_io_write_batch(io, fd, &request, 1) < 0 ||
        kvs_uring_io_sync(io, fd) < 0) {
        return -1;
    }
    *layout = (aof_layout_t){
        .command_start = KVS_AOF_HEADER_SIZE, .command_end = KVS_AOF_HEADER_SIZE, .crc = crc
    };
    return 0;
}

int kvs_aof_open_with_checksum(const char *path, kvs_aof_checksum_t checksum)
{
    if (path == NULL || path[0] == '\0' || !aof_valid_checksum(checksum)) {
        errno = EINVAL;
        return -1;
    }
    if (aof_writer.fd >= 0 || aof_writer.io.initialized) {
        errno = EALREADY;
        return -1;
    }

    /* O_EXCL 区分新文件和已有空文件，已有文件始终保留原内容。 */
    int created = 1;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0 && errno == EEXIST) {
        created = 0;
        fd = open(path, O_RDWR | O_CLOEXEC);
    }
    if (fd < 0) {
        return -1;
    }
    int result = -1;
    struct stat info;
    aof_layout_t layout = {0};
    if (fstat(fd, &info) < 0) {
        goto done;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uintmax_t)info.st_size > SIZE_MAX) {
        errno = EINVAL;
        goto done;
    }
    if (!created && aof_inspect_fd(fd, (size_t)info.st_size, checksum, &layout) < 0) {
        goto done;
    }
    if (kvs_uring_io_init(&aof_writer.io) < 0) {
        goto done;
    }
    if (created && checksum == KVS_AOF_CHECKSUM_FILE &&
        aof_initialize_file(fd, &aof_writer.io, &layout) < 0) {
        goto done;
    }
    aof_writer.fd = fd;
    aof_writer.offset = (off_t)layout.command_end;
    aof_writer.checksum = checksum;
    aof_writer.crc = layout.crc;
    aof_writer.failed = 0;
    result = 0;

done:
    if (result < 0) {
        int saved_errno = errno;
        kvs_uring_io_close(&aof_writer.io);
        kvs_aof_file_close(fd);
        errno = saved_errno;
    }
    return result;
}

int kvs_aof_open(const char *path)
{
    return kvs_aof_open_with_checksum(path, KVS_AOF_CHECKSUM_NONE);
}

int kvs_aof_close(void)
{
    if (aof_writer.fd < 0) {
        errno = EBADF;
        return -1;
    }

    /*
     * 追加函数返回前，必须已经处理完提交的请求。
     * 因此这里可以释放队列，再关闭文件。
     */
    kvs_uring_io_close(&aof_writer.io);

    int close_result = kvs_aof_file_close(aof_writer.fd);
    int saved_errno = errno;

    /*
     * 即使 close 报错，也不再次关闭同一个 fd。
     * 清除状态，避免后续误用。
     */
    aof_writer.fd = -1;
    aof_writer.offset = 0;
    aof_writer.crc = 0;
    aof_writer.failed = 0;
    aof_writer.checksum = KVS_AOF_CHECKSUM_NONE;

    errno = saved_errno;
    return close_result;
}

int kvs_aof_append(char **tokens, int count)
{
    if (aof_writer.fd < 0 || !aof_writer.io.initialized || tokens == NULL ||
        (count != 2 && count != 3)) {
        errno = EINVAL;
        return -1;
    }
    if (aof_writer.failed) {
        errno = EIO;
        return -1;
    }
    kvs_slice_t fields[3];
    for (int i = 0; i < count; i++) {
        if (tokens[i] == NULL) {
            errno = EINVAL;
            return -1;
        }
        fields[i].data = tokens[i];
        fields[i].length = strlen(tokens[i]);
    }
    size_t required;
    if (kvs_encoded_command_size(fields, (size_t)count, &required) != 0) {
        return -1;
    }
    size_t trailer_size = 0;
    if (aof_writer.checksum == KVS_AOF_CHECKSUM_FILE) {
        trailer_size = KVS_AOF_TRAILER_SIZE;
    }
    size_t capacity;
    off_t next_offset, file_end;
    if (__builtin_add_overflow(required, trailer_size, &capacity) ||
        __builtin_add_overflow(aof_writer.offset, required, &next_offset) ||
        __builtin_add_overflow(next_offset, trailer_size, &file_end)) {
        errno = EOVERFLOW;
        return -1;
    }
    char stack_buf[4096];
    char *encoded = capacity <= sizeof(stack_buf) ? stack_buf : kvs_malloc(capacity);
    if (encoded == NULL) {
        return -1;
    }
    int result = -1;
    size_t encoded_length;
    if (kvs_encode_command(fields, (size_t)count, encoded, required, &encoded_length) != 0) {
        goto done;
    }
    uint32_t next_crc = aof_writer.crc;
    if (aof_writer.checksum == KVS_AOF_CHECKSUM_FILE) {
        /* 缓存状态只包括头部和命令，追加只计算新命令字节。 */
        next_crc = aof_crc32_update(aof_writer.crc, encoded, encoded_length);
        aof_encode_trailer((unsigned char *)encoded + encoded_length,
                           (uint64_t)(next_offset - KVS_AOF_HEADER_SIZE), next_crc);
    }
    /* offset 指向旧文件尾起点，新命令和新尾部作为一个连续缓冲写入。 */
    if (kvs_uring_io_write_and_datasync(&aof_writer.io, aof_writer.fd,
            encoded, encoded_length + trailer_size, aof_writer.offset) < 0) {
        /* file 模式 I/O 失败后必须关闭并重新校验，不能沿旧状态继续追加。 */
        aof_writer.failed = aof_writer.checksum == KVS_AOF_CHECKSUM_FILE;
        goto done;
    }
    aof_writer.offset = next_offset;
    aof_writer.crc = next_crc;
    result = 0;

done:
    {
        int saved_errno = errno;
        if (encoded != stack_buf) kvs_free(encoded);
        errno = saved_errno;
    }
    return result;
}

int kvs_aof_replay_with_checksum(
    const char *path, kvs_aof_checksum_t checksum,
    long long offset,
    aof_replay_handler handler)
{
    if (path == NULL || handler == NULL || offset < 0 || !aof_valid_checksum(checksum)) {
        errno = EINVAL;
        return -1;
    }

    /* 映射文件；文件不存在时，保持原有的空数据启动行为。 */
    kvs_mapped_file_t file = {0};

    if (kvs_mapped_file_open(path, &file) < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    int replay_result = 0;
    int saved_errno;

    aof_layout_t layout;
    if (aof_inspect(&file, checksum, &layout) < 0) {
        replay_result = -1;
        goto done;
    }
    /* offset 是命令字节位置；校验覆盖整文件，不受回放起点影响。 */
    size_t position = layout.command_end;
    if ((uintmax_t)offset < layout.command_end - layout.command_start) {
        position = layout.command_start + (size_t)offset;
    }

    while (position < layout.command_end) {
        /* 从当前位置到文件末尾，还有多少字节。 */
        size_t remaining = layout.command_end - position;

        /*
         * fields 借用映射中的字段内容，不复制命令字节。
         * 当前 AOF 的每条写命令最多包含：命令名、key、value。
         */
        kvs_slice_t fields[3];
        size_t field_count = 0;
        size_t parsed_bytes = 0;

        int parse_result = kvs_parse_command(
            (const char *)file.data + position,
            remaining,
            fields,
            3,
            &field_count,
            &parsed_bytes
        );

        /*
         * 已经提供到文件末尾，仍不完整，说明尾部命令被截断。
         * 格式错误也必须拒绝恢复。
         */
        if (parse_result <= 0) {
            errno = parse_result == 0 ? EIO : EINVAL;
            replay_result = -1;
            break;
        }

        /* 防止位置不前进，或者越过当前映射范围。 */
        if (parsed_bytes == 0 || parsed_bytes > remaining) {
            errno = EINVAL;
            replay_result = -1;
            break;
        }

        /*
         * 继续使用原来的恢复回调。
         * 回调返回前，映射一直有效，fields 指向的字节可以读取。
         */
        char response[1024] = {0};
        int response_length = handler(
            fields,
            field_count,
            response,
            sizeof(response)
        );

        if (response_length <= 0 ||
            response_length > (int)sizeof(response)) {
            errno = EINVAL;
            replay_result = -1;
            break;
        }

        /* 当前命令恢复成功，移动到下一条命令的开头。 */
        position += parsed_bytes;
    }

done:
    /* 无论恢复成功还是失败，都要解除映射。 */
    saved_errno = errno;

    if (kvs_mapped_file_close(&file) < 0 && replay_result == 0) {
        replay_result = -1;
        saved_errno = errno;
    }

    errno = saved_errno;
    return replay_result;
}

int kvs_aof_replay(const char *path, long long offset, aof_replay_handler handler)
{
    return kvs_aof_replay_with_checksum(path, KVS_AOF_CHECKSUM_NONE, offset, handler);
}
