#include<stdio.h>
#include <unistd.h>
#include<errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include<string.h>
#include <fcntl.h>       // open、O_RDONLY
#include <sys/stat.h>    // fstat、struct stat、S_ISREG
#include <sys/syscall.h> // SYS_close，绕过 NtyCo 的 close hook
#include"persistence.h"
#include "protocol.h"

static FILE *aof_fp=NULL;
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
int kvs_aof_open(const char* path){
    if(path==NULL){
        return -1;
    }
    if(aof_fp!=NULL){
        return -1;
    }
    aof_fp=fopen(path,"a");
    if(aof_fp==NULL){
        return -1;
    }

    if(fseeko(aof_fp,0,SEEK_END)!=0){
        fclose(aof_fp);
        aof_fp=NULL;
        return -1;

    }

    return 0;
}
int kvs_aof_close(void){
    if(aof_fp==NULL){
        return -1;
    }
    int ret=fclose(aof_fp);
    aof_fp=NULL;
    if(ret==0){
        return 0;
    }else{
        return -1;
    }
}
/*
 * 让 Replica 本地 AOF 使用与 Primary 相同的 offset 坐标。
 *
 * 例如 Snapshot 对应 Primary offset=27：
 * - 本地 AOF 小于 27：扩展到 27 字节；
 * - 本地 AOF 大于 27：截断到 27 字节；
 * - 后续复制命令从第 27 字节之后追加。
 */
int kvs_aof_reset_to_offset(long long offset)
{
    if(aof_fp==NULL || offset<0){
        return -1;
    }

    /*
     * 先把 stdio 缓冲区已有内容交给内核，
     * 避免随后调整文件长度时与缓冲内容冲突。
     */
    if(fflush(aof_fp)!=0){
        return -1;
    }

    int fd=fileno(aof_fp);
    if(fd<0){
        return -1;
    }

    /*
     * 将文件长度调整到 Snapshot 对应的 Primary AOF offset。
     * 文件较短时扩展，较长时截断。
     */
    if(ftruncate(fd,(off_t)offset)!=0){
        return -1;
    }

    /* 后续 kvs_aof_append() 从调整后的文件末尾继续写。 */
    if(fseeko(aof_fp,0,SEEK_END)!=0){
        return -1;
    }

    /* 确保文件长度变化持久化。 */
    if(fsync(fd)!=0){
        return -1;
    }

    return 0;
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

    // 单独只读打开 AOF，不影响全局 aof_fp 的追加位置。
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
    if (aof_fp == NULL ||
        tokens == NULL ||
        (count != 2 && count != 3)) {
        return -1;
    }

    kvs_slice_t fields[3];

    for (int i = 0; i < count; i++) {
        if (tokens[i] == NULL) return -1;

        fields[i].data = tokens[i];
        fields[i].length = strlen(tokens[i]);
    }

    size_t required = 0;
    if (kvs_encoded_command_size(
            fields, (size_t)count, &required) != 0) {
        return -1;
    }

    char *encoded = malloc(required);
    if (encoded == NULL) return -1;

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

    size_t written = fwrite(encoded, 1, encoded_length, aof_fp);
    free(encoded);

    if (written != encoded_length ||
        fflush(aof_fp) != 0 ||
        fsync(fileno(aof_fp)) != 0) {
        return -1;
    }

    return 0;
}
int kvs_aof_replay(
    const char *path,
    long long offset,
    aof_replay_handler handler)
{
    if (path == NULL || handler == NULL || offset < 0) {
        return -1;
    }

    /* 使用二进制模式读取，协议解析按字节工作，不依赖文本换行。 */
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    /* 从 Snapshot 对应的 AOF 位置开始回放后续命令。 */
    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char *input = NULL;              /* 当前正在读取的命令字节 */
    size_t input_length = 0;         /* 已读入的命令字节数 */
    size_t input_capacity = 0;       /* input 当前分配的容量 */
    int replay_result = 0;

    while (1) {
        int byte = fgetc(fp);        /* 每次从 AOF 读取一个字节 */

        if (byte == EOF) {
            /*
             * EOF 时若还剩半条命令，说明 AOF 末尾不完整；
             * ferror 则表示读取文件本身发生错误。
             */
            if (ferror(fp) || input_length != 0) {
                replay_result = -1;
            }
            break;
        }

        /* 当前命令占满缓冲区时扩容，避免长字段被固定长度限制。 */
        if (input_length == input_capacity) {
            size_t new_capacity = input_capacity == 0
                ? 1024
                : input_capacity * 2;

            /* 容量翻倍后变小，表示 size_t 溢出了。 */
            if (new_capacity < input_capacity) {
                replay_result = -1;
                break;
            }

            char *new_input = realloc(input, new_capacity);
            if (new_input == NULL) {
                replay_result = -1;
                break;
            }

            input = new_input;
            input_capacity = new_capacity;
        }

        /* 把刚读到的字节追加到当前命令末尾。 */
        input[input_length++] = (char)byte;

        /*
         * AOF 中保存的是写命令，目前每条最多包含：
         * 命令名、key、value 三个字段。
         */
        kvs_slice_t fields[3];
        size_t field_count = 0;
        size_t parsed_bytes = 0;

        int parse_result = kvs_parse_command(
            input,
            input_length,
            fields,
            3,
            &field_count,
            &parsed_bytes
        );

        if (parse_result == 0) {
            continue; /* 命令尚未读完整，继续读取后续字节。 */
        }

        /*
         * -1 表示格式错误。
         * parsed_bytes 必须等于当前缓冲区长度，因为我们逐字节读取，
         * 解析成功时缓冲区里应该刚好只有这一条命令。
         */
        if (parse_result < 0 || parsed_bytes != input_length) {
            replay_result = -1;
            break;
        }

        char response[1024] = {0};
        int response_length = handler(
            fields,
            field_count,
            response,
            sizeof(response)
        );

        /* 恢复命令必须成功，并返回约定的 OK 响应。 */
        if (response_length <= 0 ||
            response_length > (int)sizeof(response) ||
            strcmp(response, "OK\r\n") != 0) {
            replay_result = -1;
            break;
        }

        /* 当前命令已解析并执行，清空逻辑长度后读取下一条。 */
        input_length = 0;
    }

    /* 释放动态命令缓冲区，并检查文件是否成功关闭。 */
    free(input);

    if (fclose(fp) != 0) {
        replay_result = -1;
    }

    return replay_result;
}

/* 返回当前 AOF 文件的写入位置，供 Snapshot 和复制记录 offset 使用。 */
long long kvs_aof_get_offset(void)
{
    if (aof_fp == NULL) {
        return -1;
    }

    off_t offset = ftello(aof_fp);
    if (offset == (off_t)-1) {
        return -1;
    }

    return (long long)offset;
}