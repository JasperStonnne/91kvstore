#include<string.h>
#include"server.h"
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
static int kvs_find_crlf(const char *msg, int length)
{
    if (msg == NULL || length < 2) {
        return -1;
    }

    for (int i = 0; i + 1 < length; i++) {
        if (msg[i] == '\r' && msg[i + 1] == '\n') {
            return i;                              // 返回 CRLF 中 \r 的位置
        }
    }

    return -1;                                     // 当前还没有完整消息
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

        int frame_end = kvs_find_crlf(
            frame_start,
            remaining_length
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
        request_offset += frame_end + 2;            // 跳过消息正文和结尾 CRLF
    }

    *consumed_length = request_offset;              // 返回本次共消费多少输入字节

    return response_offset;                         // 返回本次共生成多少响应字节
}
