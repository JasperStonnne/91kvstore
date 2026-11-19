/* 独立 RESP ECHO 基线，复用 kvstore 的 NtyCo、协议解析和缓冲实现。 */
#include "server.h"
#include "protocol.h"
#include "kvs_allocator.h"
#include "log.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kvs_malloc(size_t size)
{
    return kvs_allocator_alloc(size);
}

void kvs_free(void *pointer)
{
    kvs_allocator_free(pointer);
}

static int echo_request(int fd, char *input, int length,
                        kvs_output_buffer_t *output, int *consumed)
{
    (void)fd;
    kvs_slice_t fields[128];
    size_t count = 0, parsed = 0;
    *consumed = 0;
    int result = kvs_parse_command_limited(input, (size_t)length,
                                          fields, 128, &count, &parsed, 4096);
    if (result <= 0) {
        return result;
    }
    *consumed = (int)parsed;
    if (count != 2 || fields[0].length != 4 || memcmp(fields[0].data, "ECHO", 4) != 0) {
        const char error[] = "-ERR expected ECHO payload\r\n";
        if (kvs_output_buffer_ensure_space(output, (int)sizeof(error) - 1) < 0) {
            return -1;
        }
        memcpy(output->data + output->length, error, sizeof(error) - 1);
        return (int)sizeof(error) - 1;
    }
    size_t required;
    if (kvs_encoded_value_response_size(&fields[1], &required) < 0 || required > INT_MAX ||
        kvs_output_buffer_ensure_space(output, (int)required) < 0) {
        return -1;
    }
    size_t written;
    if (kvs_encode_value_response(&fields[1], output->data + output->length,
                                  (size_t)(output->capacity - output->length), &written) < 0) {
        return -1;
    }
    return (int)written;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s BIND_IP PORT\n", argv[0]);
        return 2;
    }
    char *end;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno || *end || port < 1 || port > 65535) {
        return 2;
    }
    if (kvs_allocator_init(KVS_ALLOCATOR_POOL) < 0) {
        return 1;
    }
    kvs_log_set_level(KVS_LOG_ERROR);
    kvs_listener_config_t listener = {
        .bind_ip = argv[1], .port = (unsigned short)port, .handler = echo_request
    };
    return ntyco_start_listeners(&listener, 1) < 0 ? 1 : 0;
}
