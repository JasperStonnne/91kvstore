#ifndef KVS_PROTOCOL_H
#define KVS_PROTOCOL_H
#include <stddef.h>

/* 字段只引用原始报文中的数据，不负责分配或释放内存。 */
typedef struct {
    const char *data;  /* 字段第一个字节的位置 */
    size_t length;     /* 字段长度，单位是字节 */
} kvs_slice_t;

/*
 * 解析输入开头的一条命令。
 * 返回 1：命令完整；0：数据还不够；-1：格式错误。
 * 成功时，fields 指向输入中的各字段，parsed_bytes 是这条命令占用的字节数。
 */
int kvs_parse_command(
    const char *input,
    size_t input_length,
    kvs_slice_t *fields,
    size_t field_capacity,
    size_t *field_count,
    size_t *parsed_bytes
);

/*
 * 计算 fields 编码成一条命令所需的字节数。
 * 返回 0：计算成功；-1：参数错误或长度溢出。
 */
int kvs_encoded_command_size(
    const kvs_slice_t *fields,
    size_t field_count,
    size_t *encoded_bytes
);

/* 将字段编码到 output；成功时 encoded_bytes 是实际写入的字节数。 */
int kvs_encode_command(
    const kvs_slice_t *fields,
    size_t field_count,
    char *output,
    size_t output_capacity,
    size_t *encoded_bytes
);

/* 计算一个已存在的 value 编码成响应所需的字节数。 */
int kvs_encoded_value_response_size(
    const kvs_slice_t *value,
    size_t *encoded_bytes
);

/* 将已存在的 value 编码到 output；不写结尾的 '\0'。 */
int kvs_encode_value_response(
    const kvs_slice_t *value,
    char *output,
    size_t output_capacity,
    size_t *encoded_bytes
);

#endif