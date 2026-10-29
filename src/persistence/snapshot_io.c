#include "persistence.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/*
 * Snapshot 文件的分块读写与临时文件管理。
 * 不负责快照编码、解码或网络传输。
 */

/* 直接使用系统调用，避免被 NtyCo 的 socket hook 接管。 */
static ssize_t kvs_snapshot_file_read(
    int fd,
    void *buffer,
    size_t length)
{
    return (ssize_t)syscall(SYS_read, fd, buffer, length);
}

static ssize_t kvs_snapshot_file_write(
    int fd,
    const void *buffer,
    size_t length)
{
    return (ssize_t)syscall(SYS_write, fd, buffer, length);
}

static int kvs_snapshot_file_close(int fd)
{
    return (int)syscall(SYS_close, fd);
}

/* 打开 Snapshot 文件，并确认文件大小与协商值一致。 */
int kvs_snapshot_reader_open(
    kvs_snapshot_reader_t *reader,
    const char *path,
    long long expected_size)
{
    if (reader == NULL ||
        path == NULL ||
        path[0] == '\0' ||
        expected_size <= 0) {
        return -1;
    }

    reader->fd = -1;
    reader->remaining = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat file_info;
    if (fstat(fd, &file_info) != 0 ||
        !S_ISREG(file_info.st_mode) ||
        (long long)file_info.st_size != expected_size) {
        kvs_snapshot_file_close(fd);
        return -1;
    }

    reader->fd = fd;
    reader->remaining = expected_size;
    return 0;
}

/* 读取 Snapshot 的下一块字节，供主从复制发送。 */
int kvs_snapshot_reader_read(
    kvs_snapshot_reader_t *reader,
    char *output,
    int output_capacity)
{
    if (reader == NULL ||
        reader->fd < 0 ||
        reader->remaining < 0 ||
        output == NULL ||
        output_capacity <= 0) {
        return -1;
    }

    if (reader->remaining == 0) {
        return 0; /* Snapshot 已全部读完。 */
    }

    int read_size = output_capacity;
    if (reader->remaining < read_size) {
        read_size = (int)reader->remaining;
    }

    ssize_t result;
    do {
        result = kvs_snapshot_file_read(
            reader->fd,
            output,
            (size_t)read_size
        );
    } while (result < 0 && errno == EINTR);

    if (result <= 0) {
        return -1; /* 文件提前结束或读取失败。 */
    }

    reader->remaining -= result;
    return (int)result;
}

/* 关闭 Snapshot 文件并清空 reader 状态。 */
void kvs_snapshot_reader_close(kvs_snapshot_reader_t *reader)
{
    if (reader == NULL) {
        return;
    }

    if (reader->fd >= 0) {
        kvs_snapshot_file_close(reader->fd);
    }

    reader->fd = -1;
    reader->remaining = 0;
}

int kvs_snapshot_writer_open(
    kvs_snapshot_writer_t *writer,
    const char *temp_path,
    long long expected_size)
{
    if(writer==NULL ||
       temp_path==NULL ||
       temp_path[0]=='\0' ||
       expected_size<=0){
        return -1;
    }

    writer->fd=-1;
    writer->remaining=0;

    int fd=open(
        temp_path,
        O_WRONLY | O_CREAT | O_TRUNC,
        0600
    );

    if(fd<0){
        return -1;
    }

    writer->fd=fd;
    writer->remaining=expected_size;
    return 0;
}

int kvs_snapshot_writer_write(
    kvs_snapshot_writer_t *writer,
    const char *data,
    int length)
{
    if(writer==NULL ||
       writer->fd<0 ||
       writer->remaining<0 ||
       data==NULL ||
       length<=0){
        return -1;
    }

    if(writer->remaining==0){
        return 0;
    }

    int write_size=length;
    if(writer->remaining<write_size){
        write_size=(int)writer->remaining; // 只消费 Snapshot 剩余字节
    }

    int written=0;

    while(written<write_size){
        ssize_t result=kvs_snapshot_file_write(
            writer->fd,
            data+written,
            (size_t)(write_size-written)
        );

        if(result>0){
            written+=(int)result;
            continue;
        }

        if(result<0 && errno==EINTR){
            continue;
        }

        return -1;
    }

    writer->remaining-=written;
    return written;
}

int kvs_snapshot_writer_commit(
    kvs_snapshot_writer_t *writer,
    const char *temp_path,
    const char *final_path)
{
    if(writer==NULL ||
       writer->fd<0 ||
       writer->remaining!=0 ||       // 必须完整收到协商好的 Snapshot 字节
       temp_path==NULL ||
       temp_path[0]=='\0' ||
       final_path==NULL ||
       final_path[0]=='\0'){
        return -1;
    }

    if(fsync(writer->fd)!=0){         // 确保临时文件内容写入磁盘
        kvs_snapshot_file_close(writer->fd);
        writer->fd=-1;
        return -1;
    }

    if(kvs_snapshot_file_close(writer->fd)!=0){
        writer->fd=-1;
        return -1;
    }

    writer->fd=-1;

    if(rename(temp_path,final_path)!=0){ // 原子地用完整文件替换正式 Snapshot
        return -1;
    }

    return 0;
}

void kvs_snapshot_writer_abort(
    kvs_snapshot_writer_t *writer,
    const char *temp_path)
{
    if(writer==NULL){
        return;
    }

    /*
     * 如果临时文件仍然打开，就先关闭它。
     * abort 是清理函数，因此这里不再关心 kvs_snapshot_file_close() 的返回值。
     */
    if(writer->fd>=0){
        kvs_snapshot_file_close(writer->fd);
    }

    // 重置 writer，表示当前没有正在进行的 Snapshot 接收任务。
    writer->fd=-1;
    writer->remaining=0;

    /*
     * 删除只接收了一部分的临时文件。
     * 正式 Snapshot 不会被删除。
     */
    if(temp_path!=NULL && temp_path[0]!='\0'){
        unlink(temp_path);
    }
}