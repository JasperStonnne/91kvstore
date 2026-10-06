#include "protocol.h"  // 使用头文件中的声明
#include <stdint.h>     // 使用 SIZE_MAX 检查数字溢出
#include <string.h>     // 使用 memchr 检查 NUL 字节

/* 只读取“数字\r\n”，不读取 key 或 value。 */
static int parse_number(
    const char *input,      // 收到的字节
    size_t input_length,    // 目前收到多少字节
    size_t *offset,         // 当前读取位置；成功后更新
    size_t *number)         // 成功后写入读出的数字
{
    size_t pos = *offset;   // 先用局部位置；没读完就不改 offset
    size_t value = 0;      // 用来累积数字

    if (pos >= input_length) {
        return 0;           // 当前没有字节，继续等
    }
    if (input[pos] < '0' || input[pos] > '9') {
        return -1;          // 数字应该从这里开始，却读到了别的字符
    }

    while (pos < input_length &&
           input[pos] >= '0' && input[pos] <= '9') {
        size_t digit = (size_t)(input[pos] - '0'); // '3' 变成 3
        if (value > (SIZE_MAX - digit) / 10) {
            return -1;      // 下一次计算 value * 10 + digit 会溢出
        }
        value = value * 10 + digit;
        pos++;              // 去看下一个字节
    }

    if (pos >= input_length) return 0;         // 数字后还没收到 \r
    if (input[pos] != '\r') return -1;         // 数字后不是 \r
    if (pos + 1 >= input_length) return 0;     // 收到 \r，还没收到 \n
    if (input[pos + 1] != '\n') return -1;     // \r 后不是 \n

    *number = value;        // 把读出的数字交给调用方
    *offset = pos + 2;      // 跳过 \r\n，指向后面的内容
    return 1;               // “数字\r\n”读完整了
}

/* 解析一个字段；只有字段完整时才更新 offset 和 field。 */
static int parse_field(
    const char *input,
    size_t input_length,
    size_t *offset,
    kvs_slice_t *field)
{
    size_t pos = *offset;

    if (pos >= input_length) return 0;
    if (input[pos] != '$') return -1;
    pos++;

    size_t length = 0;
    int result = parse_number(input, input_length, &pos, &length);
    if (result <= 0) return result;

    /* 先检查剩余字节，避免用超长 length 计算位置时溢出。 */
    size_t remaining = input_length - pos;
    if (length > remaining || remaining - length < 2) return 0;

    if (input[pos + length] != '\r' ||
        input[pos + length + 1] != '\n') {
        return -1;
    }

    field->data = input + pos;
    field->length = length;
    *offset = pos + length + 2;
    return 1;
}

/* 解析输入开头的一条完整命令，不修改输入，也不执行命令。 */
int kvs_parse_command(
    const char *input,
    size_t input_length,
    kvs_slice_t *fields,
    size_t field_capacity,
    size_t *field_count,
    size_t *parsed_bytes)
{
    if (input == NULL || fields == NULL ||
        field_count == NULL || parsed_bytes == NULL ||
        field_capacity == 0) {
        return -1;
    }

    *field_count = 0;
    *parsed_bytes = 0;

    if (input_length == 0) return 0;
    if (input[0] != '*') return -1;

    size_t offset = 1;  /* 跳过 * */
    size_t count = 0;
    int result = parse_number(input, input_length, &offset, &count);
    if (result <= 0) return result;
    if (count == 0 || count > field_capacity) return -1;

    for (size_t i = 0; i < count; i++) {
        result = parse_field(input, input_length, &offset, &fields[i]);
        if (result <= 0) return result;
    }

    *field_count = count;
    *parsed_bytes = offset;
    return 1;
}

/* 计算十进制数字需要几个字符，例如 11 需要 2 个。 */
static size_t decimal_digits(size_t value)
{
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits;
}

/* 计算一个“$长度\r\n内容\r\n”字段编码后的字节数。 */
static int encoded_field_size(size_t content_length, size_t *encoded_size)
{
    if (encoded_size == NULL) return -1;

    size_t overhead = 1 + decimal_digits(content_length) + 4;
    if (content_length > SIZE_MAX - overhead) return -1;

    *encoded_size = overhead + content_length;
    return 0;
}

int kvs_encoded_command_size(
    const kvs_slice_t *fields,
    size_t field_count,
    size_t *encoded_bytes)
{
    if (fields == NULL || field_count == 0 || encoded_bytes == NULL) {
        return -1;
    }

    /* 命令开头：*字段数\r\n */
    size_t total = 1 + decimal_digits(field_count) + 2;

    for (size_t i = 0; i < field_count; i++) {
        if (fields[i].data == NULL) return -1;

        /* 单个字段的大小交给共用函数计算。 */
        size_t field_size = 0;
        if (encoded_field_size(fields[i].length, &field_size) != 0 ||
            total > SIZE_MAX - field_size) {
            return -1;
        }
        total += field_size;
    }

    *encoded_bytes = total;
    return 0;
}

/* 将十进制数字写入 output，不写字符串结束符。 */
static size_t write_decimal(char *output, size_t value)
{
    size_t digits = decimal_digits(value);

    for (size_t i = digits; i > 0; i--) {
        output[i - 1] = (char)('0' + value % 10);
        value /= 10;
    }
    return digits;
}

int kvs_encode_command(
    const kvs_slice_t *fields,
    size_t field_count,
    char *output,
    size_t output_capacity,
    size_t *encoded_bytes)
{
    if (output == NULL || encoded_bytes == NULL) return -1;
    *encoded_bytes = 0;

    size_t required = 0;
    if (kvs_encoded_command_size(fields, field_count, &required) != 0 ||
        output_capacity < required) {
        return -1;
    }

    size_t pos = 0;
    output[pos++] = '*';
    pos += write_decimal(output + pos, field_count);
    output[pos++] = '\r';
    output[pos++] = '\n';

    for (size_t i = 0; i < field_count; i++) {
        output[pos++] = '$';
        pos += write_decimal(output + pos, fields[i].length);
        output[pos++] = '\r';
        output[pos++] = '\n';

        memcpy(output + pos, fields[i].data, fields[i].length);
        pos += fields[i].length;

        output[pos++] = '\r';
        output[pos++] = '\n';
    }

    *encoded_bytes = pos;
    return 0;
}

int kvs_encoded_value_response_size(
    const kvs_slice_t *value,
    size_t *encoded_bytes)
{
    if (value == NULL || value->data == NULL) return -1;

    /* value 响应就是一个“$长度\r\n内容\r\n”字段。 */
    return encoded_field_size(value->length, encoded_bytes);
}

int kvs_encode_value_response(
    const kvs_slice_t *value,
    char *output,
    size_t output_capacity,
    size_t *encoded_bytes)
{
    if (output == NULL || encoded_bytes == NULL) return -1;
    *encoded_bytes = 0;

    size_t required = 0;
    if (kvs_encoded_value_response_size(value, &required) != 0 ||
        output_capacity < required) {
        return -1;
    }

    size_t pos = 0;
    output[pos++] = '$';
    pos += write_decimal(output + pos, value->length);
    output[pos++] = '\r';
    output[pos++] = '\n';

    memcpy(output + pos, value->data, value->length);
    pos += value->length;

    output[pos++] = '\r';
    output[pos++] = '\n';

    *encoded_bytes = pos;
    return 0;
}