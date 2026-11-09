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

    /* 从第一条命令之后继续解析，验证粘包中的第二条命令。 */
    size_t second_bytes = 0;
    field_count = 0;
    result = kvs_parse_command(
        input + parsed_bytes,
        sizeof(input) - 1 - parsed_bytes,
        fields,
        3,
        &field_count,
        &second_bytes
    );

    assert(result == 1);
    assert(field_count == 2);
    assert(second_bytes == sizeof(input) - 1 - parsed_bytes);
    assert(fields[0].length == 3);
    assert(memcmp(fields[0].data, "GET", 3) == 0);
    assert(fields[1].length == 3);
    assert(memcmp(fields[1].data, "key", 3) == 0);
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

/* 空 Bulk String 是合法字段；是否允许空 key 由命令层决定。 */
static void test_empty_bulk_string(void)
{
    static const char command[] =
        "*2\r\n"
        "$3\r\nGET\r\n"
        "$0\r\n\r\n";

    kvs_slice_t fields[2] = {0};
    size_t field_count = 0;
    size_t parsed_bytes = 0;

    int result = kvs_parse_command(
        command, sizeof(command) - 1,
        fields, 2, &field_count, &parsed_bytes
    );

    assert(result == 1);
    assert(field_count == 2);
    assert(parsed_bytes == sizeof(command) - 1);
    assert(fields[1].length == 0);
}

static void test_limited_parser_rejects_large_header(void)
{
    static const char command[] =
        "*2\r\n"
        "$3\r\nGET\r\n"
        "$1000000000\r\n"; /* 故意不发送字段内容。 */

    kvs_slice_t fields[2] = {0};
    size_t field_count = 0;
    size_t parsed_bytes = 0;

    int result = kvs_parse_command_limited(
        command, sizeof(command) - 1,
        fields, 2, &field_count, &parsed_bytes,
        1024 /* 本测试限定一条命令最多 1024 字节。 */
    );

    assert(result == -1);
}

/* 对同一条合法命令逐字节截断，确认任何半包都不会提前成功。 */
static void test_every_incomplete_prefix(void)
{
    static const char input[] =
        "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\na\0b\r\n\r\n";
    kvs_slice_t fields[3];
    for (size_t length = 0; length < sizeof(input) - 1; length++) {
        size_t count = 99, used = 99;
        assert(kvs_parse_command(input, length, fields, 3, &count, &used) == 0);
        assert(count == 0 && used == 0);
    }
    size_t count = 0, used = 0;
    assert(kvs_parse_command(input, sizeof(input) - 1, fields, 3, &count, &used) == 1);
    assert(count == 3 && used == sizeof(input) - 1);
    assert(fields[2].length == 5 && memcmp(fields[2].data, "a\0b\r\n", 5) == 0);
    puts("protocol every-prefix and binary field tests passed");
}

static void test_invalid_numeric_and_terminator_table(void)
{
    static const char *inputs[] = {
        "*0\r\n", "*-1\r\n", "*1\r\n$-1\r\n",
        "*184467440737095516160\r\n",
        "*1\r\n$184467440737095516160\r\n",
        "*1\r\n$1\r\nx\n\n", "*1\r\n$1\r\nx\rX", "*1\n$0\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        kvs_slice_t fields[3];
        size_t count = 99, used = 99;
        assert(kvs_parse_command(inputs[i], strlen(inputs[i]), fields, 3, &count, &used) == -1);
        assert(count == 0 && used == 0);
    }
    puts("protocol invalid length/count/terminator table passed");
}

static void test_exact_limit_and_pipeline(void)
{
    /* 4 字节数组头 + 7 字节长度头 + 4083 字节内容 + 2 字节结尾 = 4096。 */
    const size_t length = 4096;
    char *input = malloc(length * 2);
    assert(input != NULL);
    memcpy(input, "*1\r\n$4083\r\n", 11);
    memset(input + 11, 'v', 4083);
    memcpy(input + length - 2, "\r\n", 2);
    memcpy(input + length, input, length);
    kvs_slice_t fields[1];
    size_t count = 0, used = 0;
    assert(kvs_parse_command_limited(input, length, fields, 1, &count, &used, length - 1) == -1);
    assert(count == 0 && used == 0);
    assert(kvs_parse_command_limited(input, length, fields, 1, &count, &used, length) == 1);
    assert(used == length && count == 1 && fields[0].length == 4083);
    /* 后续粘包不占用当前命令的长度限额。 */
    assert(kvs_parse_command_limited(input, length * 2, fields, 1, &count, &used, length) == 1);
    assert(used == length);
    assert(kvs_parse_command_limited(input + used, length, fields, 1, &count, &used, length) == 1);
    assert(used == length);
    free(input);
    puts("protocol exact size limit and pipeline tests passed");
}

static void test_field_capacity_and_small_output(void)
{
    char input[6 + 128 * 6];
    memcpy(input, "*128\r\n", 6);
    for (size_t i = 0; i < 128; i++)
        memcpy(input + 6 + i * 6, "$0\r\n\r\n", 6);
    kvs_slice_t fields[128];
    size_t count = 0, used = 0;
    assert(kvs_parse_command(input, sizeof(input), fields, 127, &count, &used) == -1);
    assert(kvs_parse_command(input, sizeof(input), fields, 128, &count, &used) == 1);
    assert(count == 128 && used == sizeof(input));

    /* 编码容量不足时，不能写坏调用方缓冲区。 */
    unsigned char output[16], before[16];
    memset(output, 0x5a, sizeof(output));
    memcpy(before, output, sizeof(output));
    size_t written = 99;
    assert(kvs_encode_command(fields, count, (char *)output, sizeof(output), &written) == -1);
    assert(written == 0 && memcmp(output, before, sizeof(output)) == 0);
    puts("protocol field capacity and short output tests passed");
}

int main(void)
{
    test_parse_command_with_crlf_value();
    test_incomplete_command();
    test_invalid_field_length();
    test_large_value();
    test_empty_bulk_string();
    test_limited_parser_rejects_large_header();
    test_every_incomplete_prefix();
    test_invalid_numeric_and_terminator_table();
    test_exact_limit_and_pipeline();
    test_field_capacity_and_small_output();
    puts("protocol parser tests passed");
    return 0;
}