#include "ebpf_state.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char primary_id[] = "0123456789012345678901234567890123456789";
static const char sync_id[] = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";

int main(void)
{
    struct kvs_ebpf_state state = {0};
    assert(kvs_ebpf_validate_state(NULL) == -EINVAL);
    assert(kvs_ebpf_validate_state(&state) == -EAGAIN);
    state.boundary.generation = 1;
    assert(kvs_ebpf_validate_state(&state) == -EAGAIN);
    state.boundary.generation = 2;
    state.boundary.stream_offset = state.stream_end = 100;
    memcpy(state.boundary.primary_id, primary_id, KVS_EBPF_ID_BYTES);
    memcpy(state.boundary.full_sync_id, sync_id, KVS_EBPF_ID_BYTES);
    /* 快照覆盖边界前的丢失；历史累计计数不能毒化新的全量。 */
    state.boundary.failures.ring_full = state.failures.ring_full = 7;
    assert(kvs_ebpf_match_boundary(&state, primary_id, sync_id, 100) == 0);
    assert(kvs_ebpf_check_progress(&state, 100) == 0);
    struct kvs_ebpf_state empty = state;
    empty.boundary.stream_offset = empty.stream_end = 0;
    assert(kvs_ebpf_match_boundary(&empty, primary_id, sync_id, 0) == 0);
    assert(kvs_ebpf_check_progress(&empty, 0) == 0); /* 合法空流不同于未知边界。 */
    assert(kvs_ebpf_match_boundary(&state, sync_id, sync_id, 100) == -ESTALE);
    assert(kvs_ebpf_match_boundary(&state, primary_id, primary_id, 100) == -ESTALE);
    assert(kvs_ebpf_match_boundary(&state, primary_id, sync_id, 99) == -ESTALE);
    assert(kvs_ebpf_match_boundary(&state, NULL, sync_id, 100) == -EINVAL);
    assert(kvs_ebpf_match_boundary(&state, "short", sync_id, 100) == -EINVAL);

    state.stream_end = 130;
    assert(kvs_ebpf_check_progress(&state, 100) == -EAGAIN);
    assert(kvs_ebpf_check_progress(&state, 130) == 0);
    assert(kvs_ebpf_check_progress(&state, 99) == -ERANGE);
    assert(kvs_ebpf_check_progress(&state, 131) == -ERANGE);
    /* 即使末尾碰巧相等，任何新增的采集失败也禁止宣称追平。 */
    state.failures.oversize++;
    assert(kvs_ebpf_check_progress(&state, 130) == -ENODATA);
    state.failures.oversize--;
    state.failures.ring_full++;
    assert(kvs_ebpf_match_boundary(&state, primary_id, sync_id, 100) == -ENODATA);
    state.failures.ring_full--;
    state.failures.read_error++;
    assert(kvs_ebpf_check_progress(&state, 130) == -ENODATA);
    state.failures.read_error--;
    state.failures.ring_full = 6;
    assert(kvs_ebpf_validate_state(&state) == -EPROTO);
    state.failures.ring_full = 7;
    state.stream_end = 99;
    assert(kvs_ebpf_validate_state(&state) == -EPROTO);
    state.stream_end = 130;
    state.boundary.primary_id[0] = 'z';
    assert(kvs_ebpf_validate_state(&state) == -EPROTO);
    puts("[PASS] eBPF state: unknown/updating boundary, IDs/S, lag, range and all loss counters");
    return 0;
}
