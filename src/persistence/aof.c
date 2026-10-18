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
#include "mapped_file.h"
#include "uring_io.h"
#include"persistence.h"
#include "protocol.h"

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
/*
return: 0-success,-1-error
*/
int kvs_aof_open(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* 不能重复打开，否则会丢失已有文件和队列的资源。 */
    if (aof_writer.fd >= 0 || aof_writer.io.initialized) {
        errno = EALREADY;
        return -1;
    }

    /*
     * 文件不存在时创建；存在时保留原内容。
     * 不使用 O_TRUNC，也不使用 O_APPEND。
     */
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) {
        return -1;
    }

    /* 取得已有文件的长度，作为下一次写入的位置。 */
    struct stat file_info;
    if (fstat(fd, &file_info) < 0) {
        int saved_errno = errno;
        kvs_aof_file_close(fd);
        errno = saved_errno;
        return -1;
    }

    /* AOF 应当是普通文件。 */
    if (!S_ISREG(file_info.st_mode) || file_info.st_size < 0) {
        kvs_aof_file_close(fd);
        errno = EINVAL;
        return -1;
    }

    /* 为后续追加和同步创建队列，整个打开期间复用。 */
    if (kvs_uring_io_init(&aof_writer.io) < 0) {
        int saved_errno = errno;
        kvs_aof_file_close(fd);
        errno = saved_errno;
        return -1;
    }

    /* 资源全部准备成功后，才将文件标记为已打开。 */
    aof_writer.fd = fd;
    aof_writer.offset = file_info.st_size;
    return 0;
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

    errno = saved_errno;
    return close_result;
}

/*
 * Replica 安装全量 Snapshot 后调用。
 * 保留原语义：调整本地 AOF 长度，并对齐后续追加位置。
 */
int kvs_aof_reset_to_offset(long long offset)
{
    if (aof_writer.fd < 0 ||
        !aof_writer.io.initialized ||
        offset < 0) {
        errno = EINVAL;
        return -1;
    }

    /* 确认传入的 offset 能用文件位置类型 off_t 表示。 */
    off_t target_offset = (off_t)offset;
    if (target_offset < 0 ||
        (long long)target_offset != offset) {
        errno = EOVERFLOW;
        return -1;
    }

    /*
     * 调用前，之前的追加请求必须已经结束。
     * 文件较短时扩展，较长时截断。
     */
    int truncate_result;
    do {
        truncate_result = ftruncate(
            aof_writer.fd, target_offset);
    } while (truncate_result < 0 && errno == EINTR);

    if (truncate_result < 0) {
        return -1;
    }

    /*
     * 文件长度已经改变，追加位置也必须跟着改变。
     * 即使后续同步失败，也不能保留旧位置。
     */
    aof_writer.offset = target_offset;

    /* 同步文件长度变化；成功后才能返回 0。 */
    return kvs_uring_io_sync(
        &aof_writer.io, aof_writer.fd);
}
int kvs_aof_reader_open(
    kvs_aof_reader_t *reader,
    const char *path,
    long long start_offset,
    long long end_offset)
{
    /*
     * start 可以等于 end，表示这一轮没有增量。
     * 但不能出现负数或者 end 小于 start。
     */
    if(reader==NULL ||
       path==NULL ||
       path[0]=='\0' ||
       start_offset<0 ||
       end_offset<start_offset){
        return -1;
    }

    // 先恢复安全状态，避免调用者误用旧的 fd 和进度。
    reader->fd=-1;
    reader->offset=0;
    reader->remaining=0;

/* 复制使用独立的只读 fd，不改变 AOF 写入侧维护的 offset。 */
    int fd=open(path,O_RDONLY);
    if(fd<0){
        return -1;
    }

    struct stat file_info;

    /*
     * 确认目标是普通文件，并且本轮读取区间没有超过文件末尾。
     */
    if(fstat(fd,&file_info)!=0 ||
       !S_ISREG(file_info.st_mode) ||
       start_offset>(long long)file_info.st_size ||
       end_offset>(long long)file_info.st_size){

        kvs_aof_file_close(fd);
        return -1;
    }

    reader->fd=fd;
    reader->offset=start_offset;
    reader->remaining=end_offset-start_offset;

    return 0;
}
int kvs_aof_reader_read(
    kvs_aof_reader_t *reader,
    char *output,
    int output_capacity)
{
    if(reader==NULL ||
       reader->fd<0 ||
       reader->offset<0 ||
       reader->remaining<0 ||
       output==NULL ||
       output_capacity<=0){
        return -1;
    }

    // remaining 为 0，表示本轮 [start,end) 已经全部读取完成。
    if(reader->remaining==0){
        return 0;
    }

    int read_size=output_capacity;

    // 最后一块不能越过本轮固定的 end_offset。
    if(reader->remaining<read_size){
        read_size=(int)reader->remaining;
    }

    ssize_t result;

    do{
        /*
         * pread() 从指定 offset 读取，但不会修改 fd 自身的文件位置。
         * NtyCo 没有重写 pread()，因此可用于普通 AOF 文件。
         */
        result=pread(
            reader->fd,
            output,
            (size_t)read_size,
            (off_t)reader->offset
        );
    }while(result<0 && errno==EINTR);

    /*
     * remaining 还大于 0 时遇到 EOF，说明文件被异常截断；
     * 其他读取错误也都返回失败。
     */
    if(result<=0){
        return -1;
    }

    reader->offset+=result;
    reader->remaining-=result;

    return (int)result;
}
void kvs_aof_reader_close(kvs_aof_reader_t *reader)
{
    if(reader==NULL){
        return;
    }

    if(reader->fd>=0){
        kvs_aof_file_close(reader->fd);
    }

    // 清空全部状态，避免后续误用已经关闭的读取任务。
    reader->fd=-1;
    reader->offset=0;
    reader->remaining=0;
}
int kvs_aof_append(char **tokens, int count)
{
    if (aof_writer.fd < 0 ||
        !aof_writer.io.initialized ||
        tokens == NULL ||
        (count != 2 && count != 3)) {
        errno = EINVAL;
        return -1;
    }

    /* 1. 保留原来的编码过程：命令名、key、可选的 value。 */
    kvs_slice_t fields[3];

    for (int i = 0; i < count; i++) {
        if (tokens[i] == NULL) {
            errno = EINVAL;
            return -1;
        }

        fields[i].data = tokens[i];
        fields[i].length = strlen(tokens[i]);
    }

    size_t required = 0;
    if (kvs_encoded_command_size(
            fields, (size_t)count, &required) != 0) {
        return -1;
    }

    char *encoded = malloc(required);
    if (encoded == NULL) {
        return -1;
    }

    size_t encoded_length = 0;
    if (kvs_encode_command(
            fields,
            (size_t)count,
            encoded,
            required,
            &encoded_length) != 0) {
        free(encoded);
        return -1;
    }

    /* 2. 提前检查写完后的 offset 是否溢出。 */
    off_t next_offset;
    if (__builtin_add_overflow(
            aof_writer.offset, encoded_length, &next_offset)) {
        free(encoded);
        errno = EOVERFLOW;
        return -1;
    }

    /* 3. 指定源数据的内存地址和目标文件位置。 */
    kvs_uring_write_t request = {
        .data = encoded,
        .length = encoded_length,
        .offset = aof_writer.offset
    };

    int write_result = kvs_uring_io_write_batch(
        &aof_writer.io,
        aof_writer.fd,
        &request,
        1
    );

    /* 写请求已经结束，才能释放它借用的内存。 */
    int saved_errno = errno;
    free(encoded);
    errno = saved_errno;

    if (write_result < 0) {
        return -1;
    }

    /* 4. 保留每条命令同步成功后才返回成功的规则。 */
    if (kvs_uring_io_sync(
            &aof_writer.io, aof_writer.fd) < 0) {
        return -1;
    }

    /* 5. 写入和同步都成功，更新下一条命令的写入位置。 */
    aof_writer.offset = next_offset;
    return 0;
}
int kvs_aof_replay(
    const char *path,
    long long offset,
    aof_replay_handler handler)
{
    if (path == NULL || handler == NULL || offset < 0) {
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

    /*
     * position 表示下一条命令在文件中的起始位置。
     * offset 在文件范围内时，可以安全地转换成 size_t。
     * offset 到达或超过末尾时，不回放任何命令，保持原有行为。
     */
    size_t position = file.length;

    if ((uintmax_t)offset < (uintmax_t)file.length) {
        position = (size_t)offset;
    }

    while (position < file.length) {
        /* 从当前位置到文件末尾，还有多少字节。 */
        size_t remaining = file.length - position;

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

    /* 无论恢复成功还是失败，都要解除映射。 */
    saved_errno = errno;

    if (kvs_mapped_file_close(&file) < 0 && replay_result == 0) {
        replay_result = -1;
        saved_errno = errno;
    }

    errno = saved_errno;
    return replay_result;
}

/* 返回 AOF 当前的字节 offset，供 Snapshot 和复制模块使用。 */
long long kvs_aof_get_offset(void)
{
    if (aof_writer.fd < 0) {
        errno = EBADF;
        return -1;
    }

    return (long long)aof_writer.offset;
}