#include "persistence.h"
#include "kvstore.h"

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

extern kvs_array_t global_array;
extern kvs_rbtree_t global_rbtree;
extern kvs_hash_t global_hash;
extern kvs_skiplist_t global_skiplist;

/* 遍历写入时，每条记录共用的信息。 */
typedef struct {
    int fd;
    kvs_snapshot_engine_t engine;
    uint32_t *crc;
} snapshot_write_context_t;


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


/* ============================================================
 * 二、基础文件操作
 * ============================================================ */

/* 成功写完全部字节返回 0，否则返回 -1。 */
static int snapshot_write_all(
    int fd,
    const unsigned char *data,
    size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t result = syscall(
            SYS_write,
            fd,
            data + written,
            length - written
        );

        if (result > 0) {
            written += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }

    return 0;
}

/* 读满指定字节数返回 0；提前结束或读取错误返回 -1。 */
static int snapshot_read_all(
    int fd,
    unsigned char *data,
    size_t length)
{
    size_t received = 0;

    while (received < length) {
        ssize_t result = syscall(
            SYS_read,
            fd,
            data + received,
            length - received
        );

        if (result > 0) {
            received += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            if (result == 0) {
                errno = EIO;
            }
            return -1;
        }
    }

    return 0;
}

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

    char *directory = strdup(path);
    if (directory == NULL) {
        return -1;
    }

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

    free(directory);

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

/* 写入整个文件的头：标识、版本和记录总数。 */
static int snapshot_write_header(
    int fd,
    uint64_t record_count,
    uint32_t *crc)
{
    unsigned char header[SNAPSHOT_HEADER_SIZE];

    memcpy(header, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_SIZE);
    encode_u32_be(header + 8, SNAPSHOT_VERSION);
    encode_u64_be(header + 12, record_count);

    if (snapshot_write_all(fd, header, sizeof(header)) < 0) {
        return -1;
    }

    *crc = snapshot_crc32_update(*crc, header, sizeof(header));
    return 0;
}

/* 写入一条完整记录：记录头、key、value。 */
static int snapshot_write_record(
    int fd,
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

    unsigned char header[SNAPSHOT_RECORD_HEADER_SIZE];

    header[0] = (unsigned char)engine;
    encode_u32_be(header + 1, (uint32_t)key_length);
    encode_u32_be(header + 5, (uint32_t)value_length);

    if (snapshot_write_all(fd, header, sizeof(header)) < 0) {
        return -1;
    }

    if (snapshot_write_all(
            fd, (const unsigned char *)key, key_length) < 0) {
        return -1;
    }

    if (snapshot_write_all(
            fd, (const unsigned char *)value, value_length) < 0) {
        return -1;
    }

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
        ctx->fd,
        ctx->engine,
        key,
        value,
        ctx->crc
    );
}

/* 依次写入四种引擎的全部记录。 */
static int snapshot_write_records(int fd, uint32_t *crc)
{
    if (crc == NULL) {
        return -1;
    }

    snapshot_write_context_t context = {
        .fd = fd,
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

/* 写完整文件内容；不负责打开、同步或关闭文件。 */
static int snapshot_write_contents(int fd)
{
    uint64_t record_count = 0;

    if (snapshot_count_records(&record_count) < 0) {
        return -1;
    }

    uint32_t crc = UINT32_C(0xffffffff);

    if (snapshot_write_header(fd, record_count, &crc) < 0) {
        return -1;
    }

    if (snapshot_write_records(fd, &crc) < 0) {
        return -1;
    }

    uint32_t checksum = crc ^ UINT32_C(0xffffffff);
    unsigned char checksum_bytes[SNAPSHOT_CHECKSUM_SIZE];

    encode_u32_be(checksum_bytes, checksum);

    return snapshot_write_all(
        fd, checksum_bytes, sizeof(checksum_bytes)
    );
}


/* ============================================================
 * 四、第一遍：校验快照，不修改引擎
 * ============================================================ */

/* 检查文件头，并读出记录总数。 */
static int snapshot_read_header(
    int fd,
    uint64_t *record_count,
    uint32_t *crc)
{
    if (record_count == NULL || crc == NULL) {
        errno = EINVAL;
        return -1;
    }

    unsigned char header[SNAPSHOT_HEADER_SIZE];

    if (snapshot_read_all(fd, header, sizeof(header)) < 0) {
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

    *record_count = decode_u64_be(header + 12);
    *crc = snapshot_crc32_update(*crc, header, sizeof(header));

    return 0;
}

/* 分块检查一个字段，不保留字段内容。 */
static int snapshot_check_field(
    int fd,
    uint32_t length,
    uint32_t *crc)
{
    if (crc == NULL) {
        errno = EINVAL;
        return -1;
    }

    unsigned char buffer[4096];
    size_t remaining = length;

    while (remaining > 0) {
        size_t chunk = remaining;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }

        if (snapshot_read_all(fd, buffer, chunk) < 0) {
            return -1;
        }

        if (memchr(buffer, '\0', chunk) != NULL) {
            errno = EINVAL;
            return -1;
        }

        *crc = snapshot_crc32_update(*crc, buffer, chunk);
        remaining -= chunk;
    }

    return 0;
}

/* 检查一条记录的类型、字段内容，并累计 CRC。 */
static int snapshot_check_record(int fd, uint32_t *crc)
{
    if (crc == NULL) {
        errno = EINVAL;
        return -1;
    }

    unsigned char record_header[SNAPSHOT_RECORD_HEADER_SIZE];

    if (snapshot_read_all(
            fd, record_header, sizeof(record_header)) < 0) {
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
        *crc, record_header, sizeof(record_header));

    if (snapshot_check_field(fd, key_length, crc) < 0) {
        return -1;
    }

    if (snapshot_check_field(fd, value_length, crc) < 0) {
        return -1;
    }

    return 0;
}

/* 检查整个文件的结构、CRC 和结束位置。 */
static int snapshot_validate(int fd)
{
    struct stat info;

    if (fstat(fd, &info) < 0) {
        return -1;
    }

    if (!S_ISREG(info.st_mode) ||
        info.st_size < SNAPSHOT_HEADER_SIZE + SNAPSHOT_CHECKSUM_SIZE) {
        errno = EINVAL;
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    uint64_t record_count = 0;
    uint32_t crc = UINT32_C(0xffffffff);

    if (snapshot_read_header(fd, &record_count, &crc) < 0) {
        return -1;
    }

    /* 每条记录至少需要 9 字节。 */
    uint64_t payload_size = (uint64_t)info.st_size
        - SNAPSHOT_HEADER_SIZE - SNAPSHOT_CHECKSUM_SIZE;

    if (record_count > payload_size / SNAPSHOT_RECORD_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }

    for (uint64_t i = 0; i < record_count; ++i) {
        if (snapshot_check_record(fd, &crc) < 0) {
            return -1;
        }
    }

    unsigned char checksum_bytes[SNAPSHOT_CHECKSUM_SIZE];

    if (snapshot_read_all(
            fd, checksum_bytes, sizeof(checksum_bytes)) < 0) {
        return -1;
    }

    uint32_t expected = decode_u32_be(checksum_bytes);
    uint32_t actual = crc ^ UINT32_C(0xffffffff);

    if (actual != expected) {
        errno = EINVAL;
        return -1;
    }

    /* CRC 后面必须立即结束。 */
    unsigned char extra;
    ssize_t result;

    do {
        result = syscall(SYS_read, fd, &extra, 1);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        return -1;
    }

    if (result > 0) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}


/* ============================================================
 * 五、第二遍：将数据恢复到已初始化的空引擎
 * ============================================================ */

/* 返回临时 C 字符串，由调用方负责 free。 */
static char *snapshot_read_field(int fd, uint32_t length)
{
    size_t capacity = (size_t)length + 1;

    if (capacity <= (size_t)length) {
        errno = EOVERFLOW;
        return NULL;
    }

    char *text = malloc(capacity);
    if (text == NULL) {
        return NULL;
    }

    if (snapshot_read_all(
            fd, (unsigned char *)text, length) < 0) {
        int saved_errno = errno;
        free(text);
        errno = saved_errno;
        return NULL;
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
/* 读出一条记录，插入引擎，再释放临时字符串。 */
static int snapshot_load_record(int fd)
{
    unsigned char record_header[SNAPSHOT_RECORD_HEADER_SIZE];

    if (snapshot_read_all(
            fd, record_header, sizeof(record_header)) < 0) {
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

    char *key = snapshot_read_field(fd, key_length);
    if (key == NULL) {
        return -1;
    }

    char *value = snapshot_read_field(fd, value_length);
    if (value == NULL) {
        int saved_errno = errno;
        free(key);
        errno = saved_errno;
        return -1;
    }

    int result = snapshot_insert_record(
        (kvs_snapshot_engine_t)engine, key, value);

    int saved_errno = errno;
    free(key);
    free(value);

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

    char *temp_path = malloc(path_length + sizeof(suffix));
    if (temp_path == NULL) {
        return -1;
    }

    memcpy(temp_path, path, path_length);
    memcpy(temp_path + path_length, suffix, sizeof(suffix));

    int directory_fd = snapshot_open_parent_directory(path);
    if (directory_fd < 0) {
        int saved_errno = errno;
        free(temp_path);
        errno = saved_errno;
        return -1;
    }

    int file_fd = -1;
    int temp_exists = 0;
    int result = -1;
    int saved_errno;

    /* 1. 在正式文件旁边创建唯一临时文件。 */
    file_fd = mkstemp(temp_path);
    if (file_fd < 0) {
        goto done;
    }
    temp_exists = 1;

    /* 2. 写完整内容，同步并关闭临时文件。 */
    if (snapshot_write_contents(file_fd) < 0) {
        goto done;
    }

    if (snapshot_sync_fd(file_fd) < 0) {
        goto done;
    }

    int close_result = (int)syscall(SYS_close, file_fd);
    file_fd = -1;
    if (close_result < 0) {
        goto done;
    }

    /* 3. 替换正式文件，再同步父目录。 */
    if (rename(temp_path, path) < 0) {
        goto done;
    }
    temp_exists = 0;

    if (snapshot_sync_fd(directory_fd) < 0) {
        goto done;
    }

    result = 0;

done:
    saved_errno = errno;

    if (file_fd >= 0) {
        syscall(SYS_close, file_fd);
    }

    if (temp_exists) {
        unlink(temp_path);
    }

    if (syscall(SYS_close, directory_fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    free(temp_path);
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

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 1 : -1;
    }

    int result = -1;
    int saved_errno;

    /* 第一遍：检查完整文件，不修改引擎。 */
    if (snapshot_validate(fd) < 0) {
        goto done;
    }

    /* 第二遍：回到文件开头，准备恢复数据。 */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        goto done;
    }

    uint64_t record_count = 0;
    uint32_t crc = UINT32_C(0xffffffff);

    if (snapshot_read_header(fd, &record_count, &crc) < 0) {
        goto done;
    }

    for (uint64_t i = 0; i < record_count; ++i) {
        if (snapshot_load_record(fd) < 0) {
            goto done;
        }
    }

    result = 0;

done:
    saved_errno = errno;

    if (syscall(SYS_close, fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    errno = saved_errno;
    return result;
}