#include "capture_cache.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void capture_cache_reset(struct capture_cache *cache, uint64_t offset)
{
    cache->start = cache->end = offset;
    cache->valid = 1;
}

int capture_cache_init(struct capture_cache *cache, size_t capacity, uint64_t offset)
{
    if (capacity == 0 || capacity > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    unsigned char *data = malloc(capacity);
    if (data == NULL)
        return -1;
    *cache = (struct capture_cache){ .data = data, .capacity = capacity };
    capture_cache_reset(cache, offset);
    return 0;
}

void capture_cache_destroy(struct capture_cache *cache)
{
    free(cache->data);
    *cache = (struct capture_cache){0};
}

int capture_cache_append(struct capture_cache *cache,
                         const struct kvs_ebpf_event *event, int pinned)
{
    if (cache->data == NULL || event->version != KVS_EBPF_EVENT_VERSION ||
        event->length == 0 || event->length > sizeof(event->payload) ||
        event->length > cache->capacity || event->end < event->start ||
        event->end - event->start != event->length) {
        cache->valid = 0;
        errno = EPROTO;
        return -1;
    }
    if (!pinned) {
        if (!cache->valid || event->start != cache->end)
            capture_cache_reset(cache, event->start);
        if (event->end - cache->start > cache->capacity)
            cache->start = event->end - cache->capacity;
    } else {
        if (!cache->valid || event->end <= cache->start)
            return 0;
        if (event->start != cache->end) {
            cache->valid = 0;
            errno = EPROTO;
            return -1;
        }
        if (event->length > cache->capacity - (cache->end - cache->start)) {
            cache->valid = 0;
            errno = ENOSPC;
            return -1;
        }
    }
    size_t index = cache->end % cache->capacity;
    size_t first = cache->capacity - index;
    if (first > event->length)
        first = event->length;
    memcpy(cache->data + index, event->payload, first);
    memcpy(cache->data, event->payload + first, event->length - first);
    cache->end = event->end;
    return 1;
}

int capture_cache_read(const struct capture_cache *cache, uint64_t offset,
                       void *output, size_t capacity)
{
    if (!cache->valid || offset < cache->start || offset > cache->end) {
        errno = ERANGE;
        return -1;
    }
    size_t bytes = cache->end - offset < capacity ? (size_t)(cache->end - offset) : capacity;
    size_t index = offset % cache->capacity;
    size_t first = cache->capacity - index;
    if (first > bytes)
        first = bytes;
    memcpy(output, cache->data + index, first);
    memcpy((unsigned char *)output + first, cache->data, bytes - first);
    return (int)bytes;
}

int capture_cache_discard(struct capture_cache *cache, uint64_t offset)
{
    if (!cache->valid || offset < cache->start || offset > cache->end) {
        errno = ERANGE;
        return -1;
    }
    cache->start = offset;
    return 0;
}
