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
