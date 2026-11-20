#ifndef KVS_EBPF_STATE_H
#define KVS_EBPF_STATE_H
#include <stdint.h>
#include "ebpf_event.h"

struct kvs_ebpf_state {
    struct kvs_ebpf_snapshot_boundary boundary;
    uint64_t stream_end;
    struct kvs_ebpf_failures failures;
};

/* 只核对元数据结构；尚未观察到完整边界返回 -EAGAIN。 */
int kvs_ebpf_validate_state(const struct kvs_ebpf_state *state);
/* 匹配本轮身份/S，并拒绝边界之后已知丢失的会话。 */
int kvs_ebpf_match_boundary(const struct kvs_ebpf_state *state,
                          const char *primary_id, const char *full_sync_id,
                          uint64_t snapshot_stream_offset);
/* 调用方先 match_boundary，并保证缓存连续；末尾等于水位才返回 0。 */
int kvs_ebpf_check_progress(const struct kvs_ebpf_state *state,
                            uint64_t cached_end);
#endif
