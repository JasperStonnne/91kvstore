#ifndef KVS_CAPTURE_CACHE_H
#define KVS_CAPTURE_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include "ebpf_event.h"

#define KVS_EBPF_CACHE_BYTES (8u * 1024u * 1024u)

struct capture_cache {
    unsigned char *data;
    size_t capacity;
    uint64_t start;
    uint64_t end;
    int valid;
};

int capture_cache_init(struct capture_cache *cache, size_t capacity, uint64_t offset);
void capture_cache_destroy(struct capture_cache *cache);
/* 只能由已确认的快照边界初始化；不能用客户端任意请求填补缺口。 */
void capture_cache_reset(struct capture_cache *cache, uint64_t offset);
int capture_cache_append(struct capture_cache *cache,
                         const struct kvs_ebpf_event *event, int pinned);
int capture_cache_read(const struct capture_cache *cache, uint64_t offset,
                       void *output, size_t capacity);
int capture_cache_discard(struct capture_cache *cache, uint64_t offset);
#endif
