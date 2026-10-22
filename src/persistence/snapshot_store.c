#include "persistence.h"
#include "kvstore.h"
#include "mapped_file.h"
#include "uring_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* 文件标识恰好 8 字节，不写入字符串末尾的 '\0'。 */
#define SNAPSHOT_MAGIC "91KVSNAP"
#define SNAPSHOT_MAGIC_SIZE 8u

#define SNAPSHOT_VERSION 1u

/* 文件头：标识 8 + 版本 4 + 记录总数 8。 */
#define SNAPSHOT_HEADER_SIZE 20u

/* 记录头：存储类型 1 + key 长度 4 + value 长度 4。 */
#define SNAPSHOT_RECORD_HEADER_SIZE 9u

/* 文件尾保存一个 CRC32。 */
#define SNAPSHOT_CHECKSUM_SIZE 4u

/* 每个写入块 64 KiB，一批最多提交 4 块。 */
#define SNAPSHOT_WRITE_BLOCK_SIZE (64u * 1024u)
#define SNAPSHOT_WRITE_BLOCK_COUNT 4u

/* 一次申请连续的 256 KiB，提交时分成最多 4 块。 */
#define SNAPSHOT_WRITE_BUFFER_SIZE \
    (SNAPSHOT_WRITE_BLOCK_SIZE * SNAPSHOT_WRITE_BLOCK_COUNT)

extern kvs_array_t global_array;
extern kvs_rbtree_t global_rbtree;
extern kvs_hash_t global_hash;
extern kvs_skiplist_t global_skiplist;


/* 一次 Snapshot 保存所使用的写入对象。 */
typedef struct {
    /* 借用临时文件的 fd，由外层 SAVE 负责关闭。 */
    int fd;

    /* 本对象拥有的 io_uring 队列。 */
    kvs_uring_io_t io;

    /* 本对象申请和释放的 256 KiB 数据缓冲区。 */
    unsigned char *buffer;

    /* 缓冲区目前装了多少待写字节。 */
    size_t buffered_bytes;

    /* 下一批数据应写到文件的哪个字节位置。 */
    off_t next_file_offset;
} snapshot_writer_t;

/* foreach 回调共用的信息。 */
typedef struct {
    snapshot_writer_t *writer;   /* 本次 SAVE 的写入对象。 */
    kvs_snapshot_engine_t engine;
    uint32_t *crc;
} snapshot_write_context_t;

/*
 * Snapshot 解析过程的读取位置。
 * data 借用文件映射，不由这个结构申请或释放。
 */
typedef struct {
    const unsigned char *data; /* 映射内容的起点。 */
    size_t length;            /* 可以读取的总字节数。 */
    size_t offset;            /* 下一次从哪个字节开始读。 */
} snapshot_read_cursor_t;

/* 检查范围，返回映射中这一段的地址，不复制数据。 */
static inline int snapshot_cursor_take(
    snapshot_read_cursor_t *cursor,
    const unsigned char **view,
    size_t length)
{
    if (cursor == NULL || view == NULL ||
        cursor->offset > cursor->length) {
        errno = EINVAL;
        return -1;
    }

    /* 文件中剩余的字节必须够用。 */
    if (length > cursor->length - cursor->offset) {
        errno = EIO;
        return -1;
    }

    if (cursor->length > 0 && cursor->data == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 把这一段的起点交给调用方，不执行 memcpy。 */
    *view = cursor->data == NULL
        ? NULL
        : cursor->data + cursor->offset;

    /* 检查成功后，推进读取位置。 */
    cursor->offset += length;
    return 0;
}

/* ============================================================
 * 一、数字编码与 CRC
 * ============================================================ */

/* 将一个 32 位无符号整数按大端序写入 4 个字节。 */
static void encode_u32_be(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

/* 将大端序的 4 个字节还原为一个 32 位无符号整数。 */
static uint32_t decode_u32_be(const unsigned char *input)
{
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static void encode_u64_be(unsigned char *output, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        output[i] = (unsigned char)(value >> (56 - i * 8));
    }
}

static uint64_t decode_u64_be(const unsigned char *input)
{
    uint64_t value = 0;

    for (unsigned int i = 0; i < 8; ++i) {
        value = (value << 8) | input[i];
    }

    return value;
}

/*
 * CRC32/ISO-HDLC：
 * 初始值为 0xffffffff，全部数据处理完后再异或 0xffffffff。
 * 返回中间状态，方便分块处理文件内容。
 */
static uint32_t snapshot_crc32_update(
    uint32_t crc,
    const unsigned char *data,
    size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (unsigned int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0) {
                crc = (crc >> 1) ^ UINT32_C(0xedb88320);
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/*
 * 初始化一次 Snapshot 保存使用的写入对象。
 * 调用方必须传入初始化为 {0} 的对象。
 * fd 由外层 SAVE 创建和关闭，本对象只借用它。
 */
static int snapshot_writer_init(snapshot_writer_t *writer, int fd)
{
    if (writer == NULL || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /* 防止重复初始化覆盖已经持有的资源。 */
    if (writer->buffer != NULL || writer->io.initialized) {
        errno = EALREADY;
        return -1;
    }

    writer->fd = fd;
    writer->buffered_bytes = 0;
    writer->next_file_offset = 0;

    /* 申请一片连续缓冲区，之后分成最多四块提交。 */
    writer->buffer = kvs_malloc(SNAPSHOT_WRITE_BUFFER_SIZE);
    if (writer->buffer == NULL) {
        return -1;
    }

    /* 创建本次保存使用的 io_uring 队列。 */
    if (kvs_uring_io_init(&writer->io) < 0) {
        int saved_errno = errno;

        /* 队列创建失败，归还已经申请的缓冲区。 */
        kvs_free(writer->buffer);
        writer->buffer = NULL;

        errno = saved_errno;
        return -1;
    }

    return 0;
}

/*
 * 释放写入对象，不关闭借用的文件 fd。
 * 调用前，所有已提交的请求必须已经收尾。
 * write_batch 的正常返回路径保证了这一点。
 */
static void snapshot_writer_close(snapshot_writer_t *writer)
{
    if (writer == NULL) {
        return;
    }

    int saved_errno = errno;

    /* 先释放队列，再释放它曾经引用的数据缓冲区。 */
    kvs_uring_io_close(&writer->io);
    kvs_free(writer->buffer);

    /* 清空对象，使重复清理安全。 */
    memset(writer, 0, sizeof(*writer));
    writer->fd = -1;

    errno = saved_errno;
}

/*
 * 写出缓冲区中的全部有效字节。
 * 成功后清空逻辑长度，允许继续装入下一批数据。
 * 本函数不执行文件同步。
 */
static int snapshot_writer_flush(snapshot_writer_t *writer)
{
    if (writer == NULL ||
        writer->buffer == NULL ||
        !writer->io.initialized ||
        writer->buffered_bytes > SNAPSHOT_WRITE_BUFFER_SIZE) {
        errno = EINVAL;
        return -1;
    }

    /* 没有待写数据，无须提交请求。 */
    if (writer->buffered_bytes == 0) {
        return 0;
    }

    /* 提前检查这一批写完后的文件位置是否会溢出。 */
    off_t batch_end;
    if (__builtin_add_overflow(
            writer->next_file_offset,
            writer->buffered_bytes,
            &batch_end)) {
        errno = EOVERFLOW;
        return -1;
    }

    kvs_uring_write_t requests[SNAPSHOT_WRITE_BLOCK_COUNT];
    size_t request_count = 0;
    size_t buffer_offset = 0;

    /* 将有效数据分成最多四个、每个不超过 64 KiB 的请求。 */
    while (buffer_offset < writer->buffered_bytes) {
        size_t block_bytes =
            writer->buffered_bytes - buffer_offset;

        if (block_bytes > SNAPSHOT_WRITE_BLOCK_SIZE) {
            block_bytes = SNAPSHOT_WRITE_BLOCK_SIZE;
        }

        /* 内存来源：当前数据块在缓冲区中的地址。 */
        requests[request_count].data =
            writer->buffer + buffer_offset;

        requests[request_count].length = block_bytes;

        /* 文件目标：本批起点，加上当前块在本批中的位置。 */
        requests[request_count].offset =
            writer->next_file_offset + (off_t)buffer_offset;

        request_count++;
        buffer_offset += block_bytes;
    }

    /*
     * 提交并等待整批写完。
     * 返回前不能修改缓冲区中的数据。
     */
    if (kvs_uring_io_write_batch(
            &writer->io,
            writer->fd,
            requests,
            request_count) < 0) {
        return -1;
    }

    /* 全部写成功，下一批紧接着当前批次的末尾。 */
    writer->next_file_offset = batch_end;

    /* 只清空有效长度，下次从缓冲区开头重新填充。 */
    writer->buffered_bytes = 0;

    return 0;
}
/*
 * 将编码好的字节追加到缓冲区。
 *
 * 空间不足时，先写出已有数据，再继续装入。
 * 返回 0 表示本次字节已全部接收，不表示已经同步落盘。
 * 保存结束时，调用方还必须 flush 最后剩余的数据。
 */
static int snapshot_writer_append(
    snapshot_writer_t *writer,
    const unsigned char *data,
    size_t length)
{
    if (writer == NULL ||
        writer->buffer == NULL ||
        !writer->io.initialized ||
        writer->buffered_bytes > SNAPSHOT_WRITE_BUFFER_SIZE ||
        (length > 0 && data == NULL)) {
        errno = EINVAL;
        return -1;
    }

    /* 本次传入的数据，已经复制了多少字节。 */
    size_t copied_bytes = 0;

    while (copied_bytes < length) {
        /* 缓冲区已满，先写出去，成功后有效长度会归零。 */
        if (writer->buffered_bytes == SNAPSHOT_WRITE_BUFFER_SIZE) {
            if (snapshot_writer_flush(writer) < 0) {
                return -1;
            }
        }

        /* 计算缓冲区还能装多少。 */
        size_t available_bytes =
            SNAPSHOT_WRITE_BUFFER_SIZE - writer->buffered_bytes;

        /* 计算本次传入的数据还有多少没复制。 */
        size_t copy_bytes = length - copied_bytes;

        /* 这一轮最多复制到缓冲区剩余空间的大小。 */
        if (copy_bytes > available_bytes) {
            copy_bytes = available_bytes;
        }

        /*
         * 目标：缓冲区当前有效数据的末尾。
         * 来源：本次输入中尚未复制的部分。
         */
        memcpy(
            writer->buffer + writer->buffered_bytes,
            data + copied_bytes,
            copy_bytes
        );

        writer->buffered_bytes += copy_bytes;
        copied_bytes += copy_bytes;
    }

    return 0;
}

/* ============================================================
 * 二、基础文件操作
 * ============================================================ */

/* 文件和目录均使用此函数同步；信号中断时重试。 */
static int snapshot_sync_fd(int fd)
{
    int result;

    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);

    return result;
}

/* 打开目标文件所在的父目录，由调用方负责关闭。 */
static int snapshot_open_parent_directory(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* 通过统一入口复制路径，后面会在副本上截取父目录。 */
    size_t directory_size = strlen(path) + 1;
    char *directory = kvs_malloc(directory_size);
    if (directory == NULL) {
        return -1;
    }

    memcpy(directory, path, directory_size);

    char *slash = strrchr(directory, '/');

    if (slash == NULL) {
        strcpy(directory, ".");
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int saved_errno = errno;

    kvs_free(directory);

    errno = saved_errno;
    return fd;
}


/* ============================================================
 * 三、保存快照内容
 * ============================================================ */

/* 每遇到一条数据，就将计数加一。 */
static int snapshot_count_item(
    const char *key,
    const char *value,
    void *context)
{
    if (key == NULL || value == NULL || context == NULL) {
        return -1;
    }

    uint64_t *count = context;

    if (*count == UINT64_MAX) {
        return -1;
    }

    ++(*count);
    return 0;
}

/* 累计四种引擎的记录总数。 */
static int snapshot_count_records(uint64_t *count)
{
    if (count == NULL) {
        return -1;
    }

    *count = 0;

    if (kvs_array_foreach(
            &global_array, snapshot_count_item, count) < 0) {
        return -1;
    }

    if (kvs_rbtree_foreach(
            &global_rbtree, snapshot_count_item, count) < 0) {
        return -1;
    }

    if (kvs_hash_foreach(
            &global_hash, snapshot_count_item, count) < 0) {
        return -1;
    }

    if (kvs_skiplist_foreach(
            &global_skiplist, snapshot_count_item, count) < 0) {
        return -1;
    }

    return 0;
}

/* 编码文件头，追加到缓冲区，并更新 CRC。 */
static int snapshot_write_header(
    snapshot_writer_t *writer,
    uint64_t record_count,
    uint32_t *crc)
{
    unsigned char header[SNAPSHOT_HEADER_SIZE];

    memcpy(header, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_SIZE);
    encode_u32_be(header + 8, SNAPSHOT_VERSION);
    encode_u64_be(header + 12, record_count);

    /* 字节先交给缓冲区管理。 */
    if (snapshot_writer_append(
            writer, header, sizeof(header)) < 0) {
        return -1;
    }

    *crc = snapshot_crc32_update(*crc, header, sizeof(header));
    return 0;
}

/* 编码一条记录：记录头、key、value，并依次追加到缓冲区。 */
static int snapshot_write_record(
    snapshot_writer_t *writer,
    kvs_snapshot_engine_t engine,
    const char *key,
    const char *value,
    uint32_t *crc)
{
    if (key == NULL || value == NULL || crc == NULL) {
        return -1;
    }

    if (engine < KVS_SNAPSHOT_ENGINE_ARRAY ||
        engine > KVS_SNAPSHOT_ENGINE_SKIPLIST) {
        return -1;
    }

    size_t key_length = strlen(key);
    size_t value_length = strlen(value);

    if (key_length > UINT32_MAX || value_length > UINT32_MAX) {
        return -1;
    }

    /* 记录头格式保持不变。 */
    unsigned char header[SNAPSHOT_RECORD_HEADER_SIZE];

    header[0] = (unsigned char)engine;
    encode_u32_be(header + 1, (uint32_t)key_length);
    encode_u32_be(header + 5, (uint32_t)value_length);

    /* 按文件格式要求的顺序追加：记录头 → key → value。 */
    if (snapshot_writer_append(
            writer, header, sizeof(header)) < 0) {
        return -1;
    }

    if (snapshot_writer_append(
            writer, (const unsigned char *)key, key_length) < 0) {
        return -1;
    }

    if (snapshot_writer_append(
            writer, (const unsigned char *)value, value_length) < 0) {
        return -1;
    }

    /* CRC 仍覆盖同样的字节，顺序也保持不变。 */
    *crc = snapshot_crc32_update(*crc, header, sizeof(header));
    *crc = snapshot_crc32_update(
        *crc, (const unsigned char *)key, key_length);
    *crc = snapshot_crc32_update(
        *crc, (const unsigned char *)value, value_length);

    return 0;
}
/* foreach 回调：将当前键值对写入文件。 */
static int snapshot_write_item(
    const char *key,
    const char *value,
    void *context)
{
    if (context == NULL) {
        return -1;
    }

    snapshot_write_context_t *ctx = context;

    return snapshot_write_record(
        ctx->writer,
        ctx->engine,
        key,
        value,
        ctx->crc
    );
}

/* 依次写入四种引擎的全部记录。 */
static int snapshot_write_records(
    snapshot_writer_t *writer,
    uint32_t *crc)
{
    if (crc == NULL) {
        return -1;
    }

    snapshot_write_context_t context = {
        .writer = writer,
        .engine = KVS_SNAPSHOT_ENGINE_ARRAY,
        .crc = crc
    };

    if (kvs_array_foreach(
            &global_array, snapshot_write_item, &context) < 0) {
        return -1;
    }

    context.engine = KVS_SNAPSHOT_ENGINE_RBTREE;
    if (kvs_rbtree_foreach(
            &global_rbtree, snapshot_write_item, &context) < 0) {
        return -1;
    }

    context.engine = KVS_SNAPSHOT_ENGINE_HASH;
    if (kvs_hash_foreach(
            &global_hash, snapshot_write_item, &context) < 0) {
        return -1;
    }

    context.engine = KVS_SNAPSHOT_ENGINE_SKIPLIST;
    if (kvs_skiplist_foreach(
            &global_skiplist, snapshot_write_item, &context) < 0) {
        return -1;
    }

    return 0;
}

/*
 * 编码并写出完整快照。
 * 文件同步、关闭和 rename 仍由外层 SAVE 负责。
 */
static int snapshot_write_contents(snapshot_writer_t *writer)
{
    uint64_t record_count = 0;

    if (snapshot_count_records(&record_count) < 0) {
        return -1;
    }

    uint32_t crc = UINT32_C(0xffffffff);

    /* 文件头和记录依次进入同一个缓冲区。 */
    if (snapshot_write_header(writer, record_count, &crc) < 0) {
        return -1;
    }

    if (snapshot_write_records(writer, &crc) < 0) {
        return -1;
    }

    /* 最后追加校验值，格式保持不变。 */
    uint32_t checksum = crc ^ UINT32_C(0xffffffff);
    unsigned char checksum_bytes[SNAPSHOT_CHECKSUM_SIZE];

    encode_u32_be(checksum_bytes, checksum);

    if (snapshot_writer_append(
            writer, checksum_bytes, sizeof(checksum_bytes)) < 0) {
        return -1;
    }

    /* 把最后未满一批的数据也写出去。 */
    return snapshot_writer_flush(writer);
}


/* ============================================================
 * 四、第一遍：校验快照，不修改引擎
 * ============================================================ */

/* 从映射中取得文件头，检查格式，并读出记录数。 */
static int snapshot_read_header(
    snapshot_read_cursor_t *cursor,
    uint64_t *record_count,
    uint32_t *crc)
{
    if (record_count == NULL || crc == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 接收文件头所在的地址，不创建文件头副本。 */
    const unsigned char *header = NULL;

    /* 确认有完整的20字节，并让 header 指向它们。 */
    if (snapshot_cursor_take(
            cursor, &header, SNAPSHOT_HEADER_SIZE) < 0) {
        return -1;
    }

    if (memcmp(header, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_SIZE) != 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t version = decode_u32_be(header + 8);
    if (version != SNAPSHOT_VERSION) {
        errno = EINVAL;
        return -1;
    }

    /* 仍按照原来的文件格式解析和计算CRC。 */
    *record_count = decode_u64_be(header + 12);
    *crc = snapshot_crc32_update(
        *crc, header, SNAPSHOT_HEADER_SIZE);

    return 0;
}

/* 直接检查映射中的字段，不复制字段内容。 */
static int snapshot_check_field(
    snapshot_read_cursor_t *cursor,
    uint32_t length,
    uint32_t *crc)
{
    if (crc == NULL) {
        errno = EINVAL;
        return -1;
    }

    const unsigned char *field = NULL;
    if (snapshot_cursor_take(cursor, &field, (size_t)length) < 0) {
        return -1;
    }

    if (length > 0) {
        /* 保留原规则：字段中不允许内嵌NUL。 */
        if (memchr(field, '\0', (size_t)length) != NULL) {
            errno = EINVAL;
            return -1;
        }

        *crc = snapshot_crc32_update(
            *crc, field, (size_t)length);
    }

    return 0;
}

/* 校验一条记录，不向引擎插入数据。 */
static int snapshot_check_record(
    snapshot_read_cursor_t *cursor,
    uint32_t *crc)
{
    if (crc == NULL) {
        errno = EINVAL;
        return -1;
    }

    const unsigned char *record_header = NULL;
    if (snapshot_cursor_take(
            cursor, &record_header, SNAPSHOT_RECORD_HEADER_SIZE) < 0) {
        return -1;
    }

    unsigned char engine = record_header[0];
    if (engine < KVS_SNAPSHOT_ENGINE_ARRAY ||
        engine > KVS_SNAPSHOT_ENGINE_SKIPLIST) {
        errno = EINVAL;
        return -1;
    }

    uint32_t key_length = decode_u32_be(record_header + 1);
    uint32_t value_length = decode_u32_be(record_header + 5);

    *crc = snapshot_crc32_update(
        *crc, record_header, SNAPSHOT_RECORD_HEADER_SIZE);

    if (snapshot_check_field(cursor, key_length, crc) < 0) {
        return -1;
    }

    if (snapshot_check_field(cursor, value_length, crc) < 0) {
        return -1;
    }

    return 0;
}

/* 第一遍校验完整文件，不修改引擎。 */
static int snapshot_validate(snapshot_read_cursor_t *cursor)
{
    /* 最小文件也需要20字节文件头和4字节CRC。 */
    if (cursor == NULL ||
        cursor->length < SNAPSHOT_HEADER_SIZE + SNAPSHOT_CHECKSUM_SIZE) {
        errno = EINVAL;
        return -1;
    }

    /* 从映射内容的开头开始检查。 */
    cursor->offset = 0;

    uint64_t record_count = 0;
    uint32_t crc = UINT32_C(0xffffffff);

    if (snapshot_read_header(cursor, &record_count, &crc) < 0) {
        return -1;
    }

    /* 扣除文件头和CRC，剩下的是记录区。 */
    uint64_t payload_size = (uint64_t)(
        cursor->length
        - SNAPSHOT_HEADER_SIZE
        - SNAPSHOT_CHECKSUM_SIZE
    );

    /* 每条记录至少需要9字节，记录数不能超过这个上限。 */
    if (record_count > payload_size / SNAPSHOT_RECORD_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }

    for (uint64_t i = 0; i < record_count; ++i) {
        if (snapshot_check_record(cursor, &crc) < 0) {
            return -1;
        }
    }

    /* 记录之后应当有完整的4字节CRC。 */
    const unsigned char *checksum_bytes = NULL;
    if (snapshot_cursor_take(
            cursor, &checksum_bytes, SNAPSHOT_CHECKSUM_SIZE) < 0) {
        return -1;
    }

    uint32_t expected = decode_u32_be(checksum_bytes);
    uint32_t actual = crc ^ UINT32_C(0xffffffff);

    if (actual != expected) {
        errno = EINVAL;
        return -1;
    }

    /* CRC之后必须正好到文件末尾，不允许额外字节。 */
    if (cursor->offset != cursor->length) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}


/* ============================================================
 * 五、第二遍：将数据恢复到已初始化的空引擎
 * ============================================================ */

/* 从映射中取字段，转换成由调用方负责 free 的临时字符串。 */
static char *snapshot_read_field(
    snapshot_read_cursor_t *cursor,
    uint32_t length)
{
    /* 多申请1字节，存放字符串结尾。 */
    size_t capacity = (size_t)length + 1;
    if (capacity <= (size_t)length) {
        errno = EOVERFLOW;
        return NULL;
    }

    /* 先确认字段完整，再取得映射中的地址。 */
    const unsigned char *field = NULL;
    if (snapshot_cursor_take(cursor, &field, (size_t)length) < 0) {
        return NULL;
    }

    char *text = kvs_malloc(capacity);
    if (text == NULL) {
        return NULL;
    }

    /* 空字段不复制，但仍会得到一个合法的空字符串。 */
    if (length > 0) {
        memcpy(text, field, (size_t)length);
    }
    text[length] = '\0';

    return text;
}
/*
 * 返回 0：插入成功。
 * 返回 1：同一引擎中已存在该 key，拒绝重复记录。
 * 返回负数：类型无效或插入失败。
 */
static int snapshot_insert_record(
    kvs_snapshot_engine_t engine,
    char *key,
    char *value)
{
    switch (engine) {
    case KVS_SNAPSHOT_ENGINE_ARRAY:
        if (kvs_array_get(&global_array, key) != NULL) {
            return 1;
        }
        return kvs_array_set(&global_array, key, value);

    case KVS_SNAPSHOT_ENGINE_RBTREE:
        if (kvs_rbtree_get(&global_rbtree, key) != NULL) {
            return 1;
        }
        return kvs_rbtree_set(&global_rbtree, key, value);

    case KVS_SNAPSHOT_ENGINE_HASH:
        if (kvs_hash_get(&global_hash, key) != NULL) {
            return 1;
        }
        return kvs_hash_set(&global_hash, key, value);

    case KVS_SNAPSHOT_ENGINE_SKIPLIST:
        if (kvs_skiplist_get(&global_skiplist, key) != NULL) {
            return 1;
        }
        return kvs_skiplist_set(&global_skiplist, key, value);

    default:
        errno = EINVAL;
        return -1;
    }
}
/* 第二遍：读取一条记录并恢复到对应引擎。 */
static int snapshot_load_record(snapshot_read_cursor_t *cursor)
{
    /* 记录头直接借用映射，不复制。 */
    const unsigned char *record_header = NULL;
    if (snapshot_cursor_take(
            cursor, &record_header, SNAPSHOT_RECORD_HEADER_SIZE) < 0) {
        return -1;
    }

    unsigned char engine = record_header[0];
    if (engine < KVS_SNAPSHOT_ENGINE_ARRAY ||
        engine > KVS_SNAPSHOT_ENGINE_SKIPLIST) {
        errno = EINVAL;
        return -1;
    }

    uint32_t key_length = decode_u32_be(record_header + 1);
    uint32_t value_length = decode_u32_be(record_header + 5);

    /* key/value 转换为现有引擎需要的临时C字符串。 */
    char *key = snapshot_read_field(cursor, key_length);
    if (key == NULL) {
        return -1;
    }

    char *value = snapshot_read_field(cursor, value_length);
    if (value == NULL) {
        /* value准备失败，也要释放已经准备好的key。 */
        int saved_errno = errno;
        kvs_free(key);
        errno = saved_errno;
        return -1;
    }

    /*
     * 沿用现有插入函数：
     * 选择引擎，并拒绝同一引擎中的重复key。
     * 引擎成功插入时会保存自己的key/value副本。
     */
    int result = snapshot_insert_record(
        (kvs_snapshot_engine_t)engine, key, value);

    /* 无论插入成功与否，这两个临时字符串都需要释放。 */
    int saved_errno = errno;
    kvs_free(key);
    kvs_free(value);

    if (result > 0) {
        errno = EEXIST;
        return -1;
    }

    errno = saved_errno;
    return result == 0 ? 0 : -1;
}


/* ============================================================
 * 六、对外接口
 * ============================================================ */

/*
 * 成功返回 0，失败返回 -1。
 * rename 之前失败保留旧文件。
 * rename 之后若同步失败，返回失败，但正式文件可能已被替换。
 */
int kvs_snapshot_store_save(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    size_t path_length = strlen(path);
    const char suffix[] = ".tmp.XXXXXX";

    if (path_length > SIZE_MAX - sizeof(suffix)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *temp_path = kvs_malloc(path_length + sizeof(suffix));
    if (temp_path == NULL) {
        return -1;
    }

    memcpy(temp_path, path, path_length);
    memcpy(temp_path + path_length, suffix, sizeof(suffix));

    int directory_fd = snapshot_open_parent_directory(path);
    if (directory_fd < 0) {
        int saved_errno = errno;
        kvs_free(temp_path);
        errno = saved_errno;
        return -1;
    }

    int file_fd = -1;
    int temp_exists = 0;
    int result = -1;
    int saved_errno;

    /* 本次 SAVE 共用一个 writer，初始时不持有资源。 */
    snapshot_writer_t writer = {0};

    /* 1. 在正式文件旁边创建唯一临时文件。 */
    file_fd = mkstemp(temp_path);
    if (file_fd < 0) {
        goto done;
    }
    temp_exists = 1;

    /*
     * 2. 为这个临时文件准备缓冲区和 io_uring 队列。
     * writer 借用 file_fd，不负责关闭它。
     */
    if (snapshot_writer_init(&writer, file_fd) < 0) {
        goto done;
    }

    /*
     * 3. 编码并分批写入。
     * 函数末尾会 flush，成功时全部字节都已完成写入。
     */
    if (snapshot_write_contents(&writer) < 0) {
        goto done;
    }

    /*
     * 4. 写完之后，再通过 io_uring 执行完整 fsync。
     * 只有同步成功，才能继续替换正式文件。
     */
    if (kvs_uring_io_sync(&writer.io, file_fd) < 0) {
        goto done;
    }

    /* 5. 关闭临时文件；即使关闭失败，也不重复关闭同一 fd。 */
    int close_result = (int)syscall(SYS_close, file_fd);
    file_fd = -1;
    if (close_result < 0) {
        goto done;
    }

    /* 6. 替换正式文件。 */
    if (rename(temp_path, path) < 0) {
        goto done;
    }
    temp_exists = 0;

    /*
     * 7. 保留原来的父目录同步。
     * 这一步失败时，正式文件可能已经被替换。
     */
    if (snapshot_sync_fd(directory_fd) < 0) {
        goto done;
    }

    result = 0;

done:
    /* 清理操作可能改变 errno，先保留真正的失败原因。 */
    saved_errno = errno;

    /* 释放队列和缓冲区，不关闭 file_fd。 */
    snapshot_writer_close(&writer);

    if (file_fd >= 0) {
        syscall(SYS_close, file_fd);
    }

    /* rename 之前失败，删除临时文件，保留原正式文件。 */
    if (temp_exists) {
        unlink(temp_path);
    }

    if (syscall(SYS_close, directory_fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    kvs_free(temp_path);
    errno = saved_errno;
    return result;
}

/*
 * 调用前，四种引擎必须已初始化且为空。
 * 返回 0：成功；1：文件不存在；-1：失败。
 * 失败后，调用方必须清空可能已部分恢复的数据。
 */
int kvs_snapshot_store_load(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* 建立只读映射；文件不存在仍按原约定返回1。 */
    kvs_mapped_file_t file = {0};
    if (kvs_mapped_file_open(path, &file) < 0) {
        return errno == ENOENT ? 1 : -1;
    }

    /* 游标借用映射，负责记录解析位置。 */
    snapshot_read_cursor_t cursor = {
        .data = file.data,
        .length = file.length,
        .offset = 0
    };

    int result = -1;
    int saved_errno;

    /* 第一遍：校验完整文件，不修改引擎。 */
    if (snapshot_validate(&cursor) < 0) {
        goto done;
    }

    /* 第二遍：重新从头解析，再恢复数据。 */
    cursor.offset = 0;

    uint64_t record_count = 0;
    uint32_t crc = UINT32_C(0xffffffff);

    if (snapshot_read_header(&cursor, &record_count, &crc) < 0) {
        goto done;
    }

    for (uint64_t i = 0; i < record_count; ++i) {
        if (snapshot_load_record(&cursor) < 0) {
            goto done;
        }
    }

    result = 0;

done:
    /* 保留原始错误，避免释放资源时覆盖。 */
    saved_errno = errno;

    /* 成功和失败都要释放映射；释放失败也必须报告。 */
    if (kvs_mapped_file_close(&file) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    errno = saved_errno;
    return result;
}