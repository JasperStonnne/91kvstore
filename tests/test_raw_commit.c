/* Exercise the real parser/executor; only intercept the commit boundary and codec calls. */
#define main kvs_server_main
#include "../src/kvstore.c"
#undef main
#include <assert.h>

static size_t commits, size_calls, encode_calls;
static const char *committed_pointer;
static size_t committed_length;
static char committed_bytes[8192];

int __wrap_kvs_replication_advance_stream(const char *data, size_t length)
{
    assert(length <= sizeof(committed_bytes));
    committed_pointer = data;
    committed_length = length;
    memcpy(committed_bytes, data, length);
    ++commits;
    return 0;
}
int __real_kvs_encoded_command_size(const kvs_slice_t *, size_t, size_t *);
int __wrap_kvs_encoded_command_size(const kvs_slice_t *fields, size_t count, size_t *bytes)
{
    ++size_calls;
    return __real_kvs_encoded_command_size(fields, count, bytes);
}
int __real_kvs_encode_command(const kvs_slice_t *, size_t, char *, size_t, size_t *);
int __wrap_kvs_encode_command(const kvs_slice_t *fields, size_t count,
                              char *out, size_t capacity, size_t *bytes)
{
    ++encode_calls;
    return __real_kvs_encode_command(fields, count, out, capacity, bytes);
}

int main(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_SYSTEM) == 0);
    assert(init_kvengine() == 0);
    kvs_server_role = KVS_ROLE_PRIMARY;
    char replies[256];
    kvs_output_buffer_t output = {.data = replies, .capacity = sizeof(replies)};
    char first[] = "*03\r\n$04\r\nHSET\r\n$03\r\nkey\r\n$05\r\nfirst\r\n";
    char second[] = "*3\r\n$4\r\nHMOD\r\n$3\r\nkey\r\n$6\r\nsecond\r\n";
    char pipeline[sizeof(first) + sizeof(second)] = {0};
    memcpy(pipeline, first, sizeof(first) - 1);
    memcpy(pipeline + sizeof(first) - 1, second, sizeof(second));
    char original[sizeof(pipeline)];
    memcpy(original, pipeline, sizeof(pipeline));
    int consumed = -1;
    assert(kvs_network_protocol(7, pipeline, sizeof(first) - 2, &output, &consumed) == 0);
    assert(consumed == 0 && commits == 0);
    assert(kvs_network_protocol(7, pipeline, sizeof(first) + sizeof(second) - 2,
                                &output, &consumed) == 5);
    assert(consumed == sizeof(first) - 1 && commits == 1);
    assert(committed_pointer == pipeline && committed_length == sizeof(first) - 1);
    assert(memcmp(committed_bytes, first, committed_length) == 0);
    assert(size_calls == 0 && encode_calls == 0);
    assert(kvs_network_protocol(7, pipeline + consumed, sizeof(second) - 1,
                                &output, &consumed) == 5);
    assert(commits == 2 && committed_pointer == pipeline + sizeof(first) - 1);
    assert(committed_length == sizeof(second) - 1 && size_calls == 0 && encode_calls == 0);
    assert(memcmp(pipeline, original, sizeof(pipeline)) == 0);
    /* Missing-key MOD fails: neither the stream nor codec may advance. */
    char failed[] = "*3\r\n$4\r\nHMOD\r\n$7\r\nmissing\r\n$1\r\nx\r\n";
    assert(kvs_network_protocol(7, failed, sizeof(failed) - 1, &output, &consumed) > 0);
    assert(commits == 2 && size_calls == 0 && encode_calls == 0);

    kvs_slice_t fields[] = {{"HMOD",4},{"key",3},{"fallback",8}};
    kvs_exec_status_t status;
    assert(kvs_execute_fields(fields, 3, &output, KVS_COMMAND_SOURCE_CLIENT,
                              (kvs_slice_t){0}, &status) > 0 && status == KVS_EXEC_OK);
    assert(commits == 3 && size_calls == 1 && encode_calls == 1);
    assert(committed_bytes[0] == '*');
    static const char inline_command[] = "HMOD key fallback\r\n";
    assert(kvs_execute_fields(fields, 3, &output, KVS_COMMAND_SOURCE_CLIENT,
                              (kvs_slice_t){inline_command, sizeof(inline_command)-1}, &status) > 0);
    assert(commits == 4 && size_calls == 2 && encode_calls == 2 && committed_bytes[0] == '*');
    assert(kvs_execute_fields(fields, 3, &output, KVS_COMMAND_SOURCE_RECOVERY,
                              (kvs_slice_t){first,sizeof(first)-1}, &status) > 0);
    assert(kvs_execute_fields(fields, 3, &output, KVS_COMMAND_SOURCE_REPLICATION,
                              (kvs_slice_t){first,sizeof(first)-1}, &status) > 0);
    assert(commits == 4 && size_calls == 2 && encode_calls == 2);

    char large[5200];
    int header = snprintf(large, sizeof(large), "*3\r\n$4\r\nHSET\r\n$5\r\nlarge\r\n$5000\r\n");
    memset(large + header, 'x', 5000);
    memcpy(large + header + 5000, "\r\n", 2);
    assert(kvs_network_protocol(7, large, header + 5002, &output, &consumed) == 5);
    assert(commits == 5 && committed_pointer == large && committed_length == (size_t)header + 5002);
    assert(size_calls == 2 && encode_calls == 2);
    dest_kvengine();
    assert(kvs_allocator_destroy() == 0);
    puts("[PASS] raw commit: exact pointer/bytes, one-frame pipeline/partial input, no codec calls, failed writes excluded, missing/inline fallback, recovery/replication excluded, large value");
    return 0;
}
