#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_client_prefix(void)
{
    /* 这里只选择客户端路径，完整性由 RESP 解析器检查。 */
    assert(kvs_identify_connection("*", 1) == KVS_CONNECTION_CLIENT);
}

static void test_replica_handshake(void)
{
    static const char handshake[] = "PING\r\n";

    /* 空输入及每一种半包，都应该继续等待。 */
    for (size_t length = 0; length < sizeof(handshake) - 1; ++length) {
        assert(kvs_identify_connection(handshake, length)
               == KVS_CONNECTION_NEED_MORE);
    }

    assert(kvs_identify_connection(handshake, sizeof(handshake) - 1)
           == KVS_CONNECTION_REPLICA);
}

static void test_coalesced_handshake(void)
{
    char input[] = "PING\r\nPSYNC ? -1\r\n";
    char original[sizeof(input)];
    memcpy(original, input, sizeof(input));

    assert(kvs_identify_connection(input, sizeof(input) - 1)
           == KVS_CONNECTION_REPLICA);

    /* 识别不能修改握手及其后面的数据。 */
    assert(memcmp(input, original, sizeof(input)) == 0);
}

static void test_invalid_prefix(void)
{
    static const char *cases[] = {
        "HELLO\r\n",
        "PINK",
        "PINGX",
        "PING\n",
        "PING\rX"
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(kvs_identify_connection(cases[i], strlen(cases[i]))
               == KVS_CONNECTION_INVALID);
    }
}

int main(void)
{
    test_client_prefix();
    test_replica_handshake();
    test_coalesced_handshake();
    test_invalid_prefix();
    puts("connection protocol tests passed");
    return 0;
}