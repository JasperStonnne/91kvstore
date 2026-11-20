#include "ebpf_state.h"
#include <errno.h>
#include <string.h>

static int valid_id(const unsigned char *id)
{
    for (size_t i = 0; i < KVS_EBPF_ID_BYTES; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f')))
            return 0;
    }
    return 1;
}

int kvs_ebpf_validate_state(const struct kvs_ebpf_state *state)
{
    if (state == NULL)
        return -EINVAL;
    const struct kvs_ebpf_snapshot_boundary *b = &state->boundary;
    if (b->generation == 0 || (b->generation & 1))
        return -EAGAIN;
    if (!valid_id(b->primary_id) || !valid_id(b->full_sync_id) ||
        state->stream_end < b->stream_offset ||
        state->failures.oversize < b->failures.oversize ||
        state->failures.ring_full < b->failures.ring_full ||
        state->failures.read_error < b->failures.read_error)
        return -EPROTO;
    return 0;
}

static int check_losses(const struct kvs_ebpf_state *state)
{
    int result = kvs_ebpf_validate_state(state);
    if (result < 0)
        return result;
    const struct kvs_ebpf_failures *before = &state->boundary.failures;
    const struct kvs_ebpf_failures *now = &state->failures;
    return before->oversize != now->oversize ||
           before->ring_full != now->ring_full ||
           before->read_error != now->read_error ? -ENODATA : 0;
}

int kvs_ebpf_match_boundary(const struct kvs_ebpf_state *state,
                          const char *primary_id, const char *full_sync_id,
                          uint64_t snapshot_stream_offset)
{
    if (primary_id == NULL || full_sync_id == NULL ||
        strlen(primary_id) != KVS_EBPF_ID_BYTES ||
        strlen(full_sync_id) != KVS_EBPF_ID_BYTES)
        return -EINVAL;
    int result = kvs_ebpf_validate_state(state);
    if (result < 0)
        return result;
    if (memcmp(state->boundary.primary_id, primary_id, KVS_EBPF_ID_BYTES) ||
        memcmp(state->boundary.full_sync_id, full_sync_id, KVS_EBPF_ID_BYTES) ||
        state->boundary.stream_offset != snapshot_stream_offset)
        return -ESTALE;
    return check_losses(state);
}

int kvs_ebpf_check_progress(const struct kvs_ebpf_state *state,
                            uint64_t cached_end)
{
    int result = check_losses(state);
    if (result < 0)
        return result;
    if (cached_end < state->boundary.stream_offset || cached_end > state->stream_end)
        return -ERANGE;
    return cached_end == state->stream_end ? 0 : -EAGAIN;
}
