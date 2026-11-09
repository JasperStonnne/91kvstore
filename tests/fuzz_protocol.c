/* 独立覆盖引导 fuzz：仅真实 codec，无网络、引擎或第三方协程。 */
#include "protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;
    kvs_slice_t fields[128], decoded[128];
    size_t count = 0, used = 0;
    int result = kvs_parse_command_limited((const char *)data, size,
                                           fields, 128, &count, &used, 4096);
    assert(result >= -1 && result <= 1);
    kvs_connection_kind_t kind = kvs_identify_connection((const char *)data, size);
    assert(kind >= KVS_CONNECTION_NEED_MORE && kind <= KVS_CONNECTION_INVALID);
    if (result != 1) {
        assert(count == 0 && used == 0);
        return 0;
    }
    assert(count > 0 && count <= 128 && used > 0 && used <= size && used <= 4096);
    for (size_t i = 0; i < count; i++) {
        uintptr_t base = (uintptr_t)data, address = (uintptr_t)fields[i].data;
        assert(address >= base && address - base <= used);
        assert(fields[i].length <= used - (address - base));
    }

    /* 少最后一个字节时仍是半包；完整命令不能提前执行。 */
    size_t short_count = 0, short_used = 0;
    assert(kvs_parse_command_limited((const char *)data, used - 1, decoded, 128,
                                     &short_count, &short_used, 4096) == 0);
    assert(short_count == 0 && short_used == 0);

    /* 允许数字头有前导零，因此比较字段语义，不强求重编码逐字节等于原头。 */
    size_t required = 0, encoded = 0;
    assert(kvs_encoded_command_size(fields, count, &required) == 0);
    assert(required > 0 && required <= used);
    char *canonical = malloc(required * 2);
    assert(canonical != NULL);
    assert(kvs_encode_command(fields, count, canonical, required, &encoded) == 0);
    assert(encoded == required);
    memcpy(canonical + required, canonical, required);
    size_t decoded_count = 0, decoded_used = 0;
    assert(kvs_parse_command_limited(canonical, required * 2, decoded, 128,
                                     &decoded_count, &decoded_used, 4096) == 1);
    /* 粘包时只消费第一条，不吞掉下一条。 */
    assert(decoded_used == required && decoded_count == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i].length == fields[i].length);
        assert(memcmp(decoded[i].data, fields[i].data, fields[i].length) == 0);
    }
    free(canonical);
    return 0;
}
