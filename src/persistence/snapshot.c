#include <stdio.h>
#include<unistd.h>
#include <errno.h>
#include<stdlib.h>
#include <fcntl.h>      // open、O_RDONLY 打开文件
#include <sys/stat.h>   // fstat、struct stat 查询文件实际大小
#include <sys/syscall.h> // SYS_read、SYS_write、SYS_close
#include "kvstore.h"
#include "persistence.h"
/*
 * Snapshot 使用普通文件 fd。
 * 直接调用系统调用，避免 kvs_snapshot_file_read/kvs_snapshot_file_write/kvs_snapshot_file_close 被 NtyCo 的 socket hook 接管。
 */
static ssize_t kvs_snapshot_file_read(
    int fd,
    void *buffer,
    size_t length)
{
    return (ssize_t)syscall(SYS_read,fd,buffer,length);
}

static ssize_t kvs_snapshot_file_write(
    int fd,
    const void *buffer,
    size_t length)
{
    return (ssize_t)syscall(SYS_write,fd,buffer,length);
}

static int kvs_snapshot_file_close(int fd)
{
    return (int)syscall(SYS_close,fd);
}
#define PATH_MAX 128

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPLIST
extern kvs_skiplist_t global_skiplist;
#endif

typedef struct snapshot_write_context {
    FILE *fp;
    const char *command;
} snapshot_write_context_t;

/* 将一个键值对编码成长度格式命令并写入 Snapshot。 */
static int snapshot_write_item(
    const char *key,
    const char *value,
    void *context)
{
    if (key == NULL || value == NULL || context == NULL) {
        return -1;
    }

    snapshot_write_context_t *ctx = context;
    if (ctx->fp == NULL || ctx->command == NULL) {
        return -1;
    }

    /* 三个字段分别是命令名、key 和 value。 */
    kvs_slice_t fields[3] = {
        { .data = ctx->command, .length = strlen(ctx->command) },
        { .data = key,          .length = strlen(key) },
        { .data = value,        .length = strlen(value) }
    };

    /* 先算出完整命令大小，再分配恰好够用的临时空间。 */
    size_t required = 0;
    if (kvs_encoded_command_size(fields, 3, &required) != 0) {
        return -1;
    }

    char *encoded = malloc(required);
    if (encoded == NULL) {
        return -1;
    }

    size_t encoded_length = 0;
    if (kvs_encode_command(
            fields, 3, encoded, required, &encoded_length) != 0) {
        free(encoded);
        return -1;
    }

    /* fwrite 按字节写入；帧与帧之间不需要再加换行分隔。 */
    size_t written = fwrite(encoded, 1, encoded_length, ctx->fp);
    free(encoded);

    return written == encoded_length ? 0 : -1;
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

int kvs_snapshot_save(const char *path,kvs_snapshot_metadata_t *metadata){
    if(path==NULL){
        return -1;
    }
    if(metadata != NULL){
    metadata->aof_offset=-1;
    metadata->file_size=-1;
}
    long long aof_offset=kvs_aof_get_offset();
    if(aof_offset<0){
        return -1;
    }
    char temp_path[PATH_MAX];
    int path_length=snprintf(temp_path,sizeof(temp_path),"%s.tmp",path);
    if (path_length < 0 ||
    (size_t)path_length >= sizeof(temp_path)) {
    return -1;
}
    FILE *fp=fopen(temp_path,"w");
    if(fp==NULL){
        return -1;
    }
    snapshot_write_context_t ctx={
        .fp=fp,
        .command="SET"
    };
    int result=0;
    long long snapshot_file_size=-1;
    int offset_written=fprintf(fp,"AOF_OFFSET %lld\n",aof_offset);
    if(offset_written<0){
        result=-1;
    }
#if ENABLE_ARRAY
    if(result==0){
    if(kvs_array_foreach(&global_array,snapshot_write_item,&ctx)<0){
        result=-1;
    }
    }
#endif
#if ENABLE_RBTREE
 if (result == 0) {
        ctx.command = "RSET";
        if (kvs_rbtree_foreach(
                &global_rbtree,
                snapshot_write_item,
                &ctx) < 0) {
            result = -1;
        }
    }
#endif
#if ENABLE_HASH
    if (result == 0) {
        ctx.command = "HSET";

        if (kvs_hash_foreach(
                &global_hash,
                snapshot_write_item,
                &ctx) < 0) {
            result = -1;
        }
    }
#endif
#if ENABLE_SKIPLIST
    if (result == 0) {
        ctx.command = "SSET";

        if (kvs_skiplist_foreach(
                &global_skiplist,
                snapshot_write_item,
                &ctx) < 0) {
            result = -1;
        }
    }
#endif
    if (result == 0) {
        int flush_result = fflush(fp);

        if (flush_result != 0) {
            result = -1;
        }
    }
    if(result==0){
        int sync_result=fsync(fileno(fp));
        if(sync_result!=0){
            result= -1;
        }
    }
    if(result==0){
        off_t end_position = ftello(fp);
        if(end_position==(off_t)-1){
            result=-1;
        }else{
            snapshot_file_size=(long long)end_position;
        }
    }
    int close_result=fclose(fp);
    if(close_result!=0){
        result=-1;
    }
    if (result < 0) {
        unlink(temp_path);
        return -1;
    }
    int rename_result=rename(temp_path,path);
    if(rename_result!=0){
        unlink(temp_path);
        return -1;
    }
    if(metadata != NULL){
    metadata->aof_offset=aof_offset;
    metadata->file_size=snapshot_file_size;
}
    return 0;
}
long long kvs_snapshot_load(const char* path,snapshot_replay_handler handler){
    if(path==NULL||handler==NULL){
        return -1;
    }
    FILE *fp=fopen(path,"rb");
   if (fp == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
    return -1;
}
    char *line=NULL;//保存读到的第一行
    size_t capacity=0;//getline 为line分配的空间大小
    ssize_t line_length=getline(&line,&capacity,fp);
    if(line_length==-1){
        free(line);
        fclose(fp);
        return -1;
    }
    long long aof_offset=-1;
    int parsed=sscanf(line,"AOF_OFFSET %lld",&aof_offset);
    if(parsed!=1||aof_offset<0){
        free(line);
        fclose(fp);
        return -1;
    }
        int load_result = 0;

    /* 头部已经读完，释放 getline 使用的缓冲区。 */
    free(line);
    line = NULL;
    capacity = 0;

    char *input = NULL;             /* 当前 Snapshot 命令的字节 */
    size_t input_length = 0;        /* 已读入的字节数 */
    size_t input_capacity = 0;      /* input 当前容量 */

    while (1) {
        int byte = fgetc(fp);       /* 每次读取一个字节 */

        if (byte == EOF) {
            /* 文件末尾不能留下半条命令；同时检查文件读取错误。 */
            if (ferror(fp) || input_length != 0) {
                load_result = -1;
            }
            break;
        }

        /* 当前命令缓冲区不够时扩容，支持较长的 key/value。 */
        if (input_length == input_capacity) {
            size_t new_capacity = input_capacity == 0
                ? 1024
                : input_capacity * 2;

            /* 容量翻倍后变小，表示 size_t 溢出。 */
            if (new_capacity < input_capacity) {
                load_result = -1;
                break;
            }

            char *new_input = realloc(input, new_capacity);
            if (new_input == NULL) {
                load_result = -1;
                break;
            }

            input = new_input;
            input_capacity = new_capacity;
        }

        input[input_length++] = (char)byte;

        /* Snapshot 每条记录包含命令名、key、value 三个字段。 */
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
            continue; /* 帧还不完整，继续读文件。 */
        }

        if (parse_result < 0 || parsed_bytes != input_length) {
            load_result = -1;
            break;
        }

        char response[1024] = {0};
        int response_length = handler(
            fields,
            field_count,
            response,
            sizeof(response)
        );

        if (response_length <= 0 ||
            response_length > (int)sizeof(response) ||
            strcmp(response, "OK\r\n") != 0) {
            load_result = -1;
            break;
        }

        /* 当前帧已加载，复用缓冲区读取下一条记录。 */
        input_length = 0;
    }

    free(input);
    if(ferror(fp)){
        load_result=-1;
    }
    free(line);
    if(fclose(fp)!=0){
        load_result=-1;
    }
    if(load_result<0){
        return -1;
    }
    return aof_offset;
}