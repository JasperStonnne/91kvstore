#include "../src/replication/capture_cache.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int append(struct capture_cache *cache, uint64_t start, const char *bytes, int pinned)
{
    struct kvs_ebpf_event event = { .version = 1, .start = start, .length = strlen(bytes) };
    event.end = start + event.length;
    memcpy(event.payload, bytes, event.length);
    return capture_cache_append(cache, &event, pinned);
}

int main(void)
{
    struct capture_cache cache = {0};
    char data[16] = {0};
    assert(capture_cache_init(&cache, 8, 100) == 0);
    assert(append(&cache, 100, "abcdef", 1) == 1);
    assert(capture_cache_discard(&cache, 104) == 0);
    assert(append(&cache, 106, "ghijkl", 1) == 1);
    assert(capture_cache_read(&cache, 104, data, sizeof(data)) == 8);
    assert(memcmp(data, "efghijkl", 8) == 0);
    assert(append(&cache, 112, "m", 1) == -1 && errno == ENOSPC);
    assert(!cache.valid); /* 未确认的字节不能因写满被覆盖。 */
    capture_cache_reset(&cache, 200);
    assert(append(&cache, 200, "12345678", 0) == 1);
    assert(append(&cache, 208, "90", 0) == 1);
    assert(cache.start == 202 && cache.end == 210);
    assert(capture_cache_read(&cache, 202, data, sizeof(data)) == 8);
    assert(memcmp(data, "34567890", 8) == 0);
    assert(capture_cache_discard(&cache, 201) == -1);
    assert(capture_cache_discard(&cache, 211) == -1);
    assert(capture_cache_discard(&cache, 210) == 0);
    assert(capture_cache_read(&cache, 210, data, sizeof(data)) == 0);
    assert(append(&cache, 212, "gap", 1) == -1 && errno == EPROTO);
    capture_cache_reset(&cache, UINT64_MAX - 2);
    assert(append(&cache, UINT64_MAX - 2, "xy", 1) == 1);
    assert(cache.end == UINT64_MAX);
    assert(capture_cache_read(&cache, UINT64_MAX - 2, data, sizeof(data)) == 2);
    assert(memcmp(data, "xy", 2) == 0);
    capture_cache_destroy(&cache);
    assert(cache.data == NULL);
    puts("[PASS] capture cache: wrap, APPLIED reclaim, pinned overflow, rolling eviction, gap and uint64 edge");
    return 0;
}
