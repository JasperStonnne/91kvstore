#include "protocol.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 验证解析器能解析带 CRLF 内容的字段，并只消费第一条命令。 */
static void test_parse_command_with_crlf_value(void)
{
    static const char first_command[] =
        "*3\r\n"
        "$3\r\nSET\r\n"
        "$3\r\nkey\r\n"
        "$4\r\na\r\nb\r\n";

    /* 两条命令连续到达时，第一次解析只应得到第一条。 */
    static const char input[] =
        "*3\r\n"
        "$3\r\nSET\r\n"
        "$3\r\nkey\r\n"
        "$4\r\na\r\nb\r\n"
        "*2\r\n"
        "$3\r\nGET\r\n"
        "$3\r\nkey\r\n";

    kvs_slice_t fields[3] = {0};
    size_t field_count = 0;
    size_t parsed_bytes = 0;

    int result = kvs_parse_command(
        input,
        sizeof(input) - 1,
        fields,
        3,
        &field_count,
        &parsed_bytes
    );

    assert(result == 1);
    assert(field_count == 3);
    assert(parsed_bytes == sizeof(first_command) - 1);

    assert(fields[0].length == 3);
    assert(memcmp(fields[0].data, "SET", 3) == 0);

    assert(fields[1].length == 3);
    assert(memcmp(fields[1].data, "key", 3) == 0);

    /* value 中的 CRLF 是字段内容，不是命令结束符。 */
    assert(fields[2].length == 4);
    assert(memcmp(fields[2].data, "a\r\nb", 4) == 0);
}

/* 验证半条命令会被识别为“还需要更多输入”。 */
static void test_incomplete_command(void)
{
    static const char command[] =
        "*2\r\n"
        "$3\r\nGET\r\n"
        "$3\r\nkey\r\n";

    kvs_slice_t fields[2] = {0};
    size_t field_count = 0;
    size_t parsed_bytes = 0;

    int result = kvs_parse_command(
        command,
        sizeof(command) - 2, /* 故意少给最后一个字节 */
        fields,
        2,
        &field_count,
        &parsed_bytes
    );

    assert(result == 0);
}

/* 验证字段长度不是十进制数字时会被拒绝。 */
static void test_invalid_field_length(void)
{
    static const char command[] = "*1\r\n$X\r\n";

    kvs_slice_t fields[1] = {0};
    size_t field_count = 0;
    size_t parsed_bytes = 0;

    int result = kvs_parse_command(
        command,
        sizeof(command) - 1,
        fields,
        1,
        &field_count,
        &parsed_bytes
    );

    assert(result == -1);
}
/* 验证长度超过原固定缓冲区时，字段仍能完整编码和解析。 */
static void test_large_value(void)
{
    const size_t value_length = 2048;
    char *value = malloc(value_length);
    assert(value != NULL);
    memset(value, 'v', value_length);

    kvs_slice_t input_fields[2] = {
        { .data = "GET", .length = 3 },
        { .data = value, .length = value_length }
    };

    size_t encoded_capacity = 0;
    assert(kvs_encoded_command_size(
        input_fields, 2, &encoded_capacity) == 0);

    char *encoded = malloc(encoded_capacity);
    assert(encoded != NULL);

    size_t encoded_length = 0;
    assert(kvs_encode_command(
        input_fields, 2, encoded, encoded_capacity, &encoded_length) == 0);

    kvs_slice_t parsed_fields[2] = {0};
    size_t field_count = 0;
    size_t parsed_bytes = 0;

    int result = kvs_parse_command(
        encoded,
        encoded_length,
        parsed_fields,
        2,
        &field_count,
        &parsed_bytes
    );

    assert(result == 1);
    assert(field_count == 2);
    assert(parsed_bytes == encoded_length);
    assert(parsed_fields[1].length == value_length);
    assert(memcmp(parsed_fields[1].data, value, value_length) == 0);

    free(encoded);
    free(value);
}
int main(void)
{
    test_parse_command_with_crlf_value();
    test_incomplete_command();
    test_invalid_field_length();
    test_large_value();
    puts("protocol parser tests passed");
    return 0;
}