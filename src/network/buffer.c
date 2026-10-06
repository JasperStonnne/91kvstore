#include<string.h>
#include <limits.h>
#include <stdlib.h>
#include"server.h"

/* 输入、输出缓冲区共用的扩容逻辑；只管理内存，不读写数据。 */
static int ensure_buffer_space(
    char **data,
    int length,
    int *capacity,
    int minimum_free)
{
    if (data == NULL || capacity == NULL || minimum_free <= 0 ||
        length < 0 || *capacity < 0 || length > *capacity) {
        return -1;
    }

    /* 未分配时为 NULL/0；已分配时指针与容量都有效。 */
    if ((*capacity == 0 && *data != NULL) ||
        (*capacity > 0 && *data == NULL)) {
        return -1;
    }

    if (*capacity - length >= minimum_free) return 0;
    if (length > INT_MAX - minimum_free) return -1;

    int required = length + minimum_free;
    int new_capacity = *capacity > 0 ? *capacity : BUFFER_LENGTH;

    /* 倍增容量，减少反复分配；同时避免整数溢出。 */
    while (new_capacity < required) {
        new_capacity = new_capacity > INT_MAX / 2
            ? required : new_capacity * 2;
    }

    char *new_data = realloc(*data, (size_t)new_capacity);
    if (new_data == NULL) return -1;

    *data = new_data;
    *capacity = new_capacity;
    return 0;
}

/* 输入缓冲区保证至少还有 minimum_free 字节可接收。 */
int kvs_input_buffer_ensure_space(
    kvs_input_buffer_t *buffer,
    int minimum_free)
{
    if (buffer == NULL) return -1;

    return ensure_buffer_space(
        &buffer->data,
        buffer->length,
        &buffer->capacity,
        minimum_free
    );
}

/* 连接结束时释放输入缓冲区。 */
void kvs_input_buffer_free(kvs_input_buffer_t *buffer)
{
    if (buffer == NULL) return;

    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

/* 输出缓冲区保证至少还有 minimum_free 字节可写。 */
int kvs_output_buffer_ensure_space(
    kvs_output_buffer_t *buffer,
    int minimum_free)
{
    if (buffer == NULL ||
        buffer->offset < 0 ||
        buffer->offset > buffer->length) {
        return -1;
    }

    return ensure_buffer_space(
        &buffer->data,
        buffer->length,
        &buffer->capacity,
        minimum_free
    );
}

/* 连接结束时释放输出缓冲区。 */
void kvs_output_buffer_free(kvs_output_buffer_t *buffer)
{
    if (buffer == NULL) return;

    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->offset = 0;
    buffer->capacity = 0;
}

//删除请求缓冲区前面已经处理的字节，把末尾尚未处理的数据移动到开头。
int kvs_input_buffer_consume(kvs_input_buffer_t *buffer,int consumed_length){
    if(buffer==NULL||consumed_length>buffer->length||consumed_length<0){
        return -1;
    }
    int remaining_length=buffer->length-consumed_length;
    if(consumed_length>0&&remaining_length>0){
        memmove(buffer->data,buffer->data+consumed_length,remaining_length);
    }
    buffer->length=remaining_length;
    return 0;
}
/*
 * 查找一条文本消息的结尾，同时支持：
 * 1. 网络命令使用的 \r\n
 * 2. AOF 命令使用的 \n
 */
static int kvs_find_line_end(const char *msg,int length,int *delimiter_length)
{
    if(msg==NULL ||
       length<=0 ||
       delimiter_length==NULL){
        return -1;
    }

    *delimiter_length=0;

    for(int i=0;i<length;i++){
        if(msg[i]!='\n'){
            continue;
        }

        if(i>0 && msg[i-1]=='\r'){
            *delimiter_length=2;
            return i-1;
        }

        *delimiter_length=1;
        return i;
    }

    return -1;
}

int kvs_line_batch_protocol(
    int connection_fd,
    char *msg,
    int length,
    char *response,
    int response_capacity,
    int *consumed_length,
    kvs_frame_handler frame_handler)
{
    if (msg == NULL ||
        length <= 0 ||
        response == NULL ||
        response_capacity <= 0 ||
        consumed_length == NULL ||
        frame_handler == NULL) {
        return -1;
    }

    *consumed_length = 0;                          // 默认不消费任何输入

    int request_offset = 0;                        // 当前处理到输入缓冲区的位置
    int response_offset = 0;                       // 当前已经写入多少响应数据

    while (request_offset < length) {
        char *frame_start = msg + request_offset;  // 当前帧的起始位置
        int remaining_length = length - request_offset;

        int delimiter_length=0;

        int frame_end=kvs_find_line_end(
            frame_start,
            remaining_length,
            &delimiter_length
        );

        if (frame_end < 0) {
            break;                                 // 剩余数据不是完整帧，等待下次 recv
        }

        if (frame_end == 0) {
            return -1;                             // 不接受空消息
        }

        frame_start[frame_end] = '\0';              // 临时把这一帧变成独立字符串

        if (response_offset >= response_capacity) {
            return -1;                             // 响应缓冲区已经没有剩余空间
        }

        int remaining_response_capacity =
            response_capacity - response_offset;

        int response_length = frame_handler(
            connection_fd,
            frame_start,
            frame_end,
            response + response_offset,
            remaining_response_capacity
        );

        if (response_length < 0 ||
            response_length > remaining_response_capacity) {
            return -1;
        }

        response_offset += response_length;         // 累计已经生成的响应长度
        request_offset += frame_end + delimiter_length;            // 跳过消息正文和结尾 CRLF
    }

    *consumed_length = request_offset;              // 返回本次共消费多少输入字节

    return response_offset;                         // 返回本次共生成多少响应字节
}
