#define _GNU_SOURCE
#include "capture_server.h"
#include "capture_cache.h"
#include "ebpf_capture.h"
#include "protocol.h"
#include "replication_capture_protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum output_kind { OUTPUT_NONE, OUTPUT_BEGIN, OUTPUT_DATA, OUTPUT_ONLINE, OUTPUT_ERROR };

struct capture_peer {
    int fd;
    int connecting;
    int handshake;
    int requested;
    int active;
    int online;
    int closing;
    char primary_id[41];
    char sync_id[41];
    uint64_t snapshot;
    uint64_t generation;
    uint64_t target;
    uint64_t sent;
    uint64_t applied;
    uint64_t accepted_ms;
    uint64_t ack_ms;
    uint64_t lag_ms;
    uint64_t state_wait_ms;
    char input[KVS_CAPTURE_LINE_BYTES];
    size_t input_length;
    char output[KVS_CAPTURE_DATA_BYTES + KVS_CAPTURE_LINE_BYTES];
    size_t output_length;
    size_t output_offset;
    uint64_t output_end;
    enum output_kind output_kind;
};

struct capture_server {
    int listener;
    int push_mode, have_ready, push_failed;
    kvs_replica_ready_event_t ready;
    uint64_t resume_applied, resume_sent, retry_ms;
    unsigned int retry_delay;
    char pending_error[32];
    struct capture_cache cache;
    struct kvs_ebpf_state state;
    int state_result;
    uint64_t generation;
    uint64_t retired_generation;
    unsigned char primary_id[KVS_EBPF_ID_BYTES];
    uint64_t observed_stream_end;
    int source_changed;
    struct capture_peer peer;
};

static uint64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        abort();
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int parse_offset(const char *text, uint64_t *value)
{
    if (*text < '0' || *text > '9' || strlen(text) > 20)
        return -1;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || *end)
        return -1;
    *value = parsed;
    return 0;
}

static int listen_endpoint(const char *endpoint)
{
    char host[INET_ADDRSTRLEN], port_text[21];
    int consumed = 0;
    uint64_t port;
    if (sscanf(endpoint, "%15[0-9.]:%20[0-9]%n", host, port_text, &consumed) != 2 ||
        endpoint[consumed] || parse_offset(port_text, &port) < 0 || port == 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 16) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* 绑定 PID 的启动时间；PID 被复用也不能继续转发旧进程的数据。 */
static unsigned long long process_start(pid_t pid)
{
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return 0;
    char *read = fgets(line, sizeof(line), file);
    fclose(file);
    char *tail = read ? strrchr(line, ')') : NULL;
    if (tail == NULL)
        return 0;
    char *save;
    char *token = strtok_r(tail + 1, " ", &save);
    if (token != NULL && (*token == 'Z' || *token == 'X'))
        return 0;
    for (int field = 3; token != NULL && field < 22; field++)
        token = strtok_r(NULL, " ", &save);
    return token ? strtoull(token, NULL, 10) : 0;
}

static void close_peer(struct capture_server *server)
{
    struct capture_peer *peer = &server->peer;
    if (server->push_mode) {
        if (peer->fd >= 0) {
            server->resume_applied = peer->applied;
            server->resume_sent = peer->sent;
        }
        server->retry_delay = server->retry_delay ? server->retry_delay * 2 : 25;
        if (server->retry_delay > 500) server->retry_delay = 500;
        server->retry_ms = now_ms() + server->retry_delay;
    } else if (peer->active && peer->generation > server->retired_generation)
        server->retired_generation = peer->generation;
    if (peer->fd >= 0)
        close(peer->fd);
    *peer = (struct capture_peer){ .fd = -1 };
}

static void fail_peer(struct capture_server *server, const char *reason)
{
    struct capture_peer *peer = &server->peer;
    if (server->push_mode && !peer->active && !peer->closing) {
        snprintf(server->pending_error, sizeof(server->pending_error), "%s", reason);
        return; /* BEGIN / APPLIED 身份校验后才发送 ERROR。 */
    }
    if (server->push_mode) server->push_failed = 1;
    if (peer->fd < 0 || peer->closing)
        return;
    fprintf(stderr, "capture TCP error=%s sent=%" PRIu64 " applied=%" PRIu64 "\n",
            reason, peer->sent, peer->applied);
    if (peer->active && peer->generation > server->retired_generation)
        server->retired_generation = peer->generation;
    peer->active = 0;
    /* 半个 DATA 已在网络中时只能断开，不能把 ERROR 插进它的 payload。 */
    if (peer->output_offset != 0) {
        close_peer(server);
        return;
    }
    peer->closing = 1;
    peer->output_kind = OUTPUT_ERROR;
    peer->output_offset = 0;
    peer->output_length = (size_t)snprintf(peer->output, sizeof(peer->output),
                                         "ERROR %s\r\n", reason);
    peer->state_wait_ms = now_ms();
}

static void observe_state(struct capture_server *server)
{
    server->state_result = kvs_ebpf_read_state(&server->state);
    if (server->state_result < 0)
        return;
    const struct kvs_ebpf_snapshot_boundary *boundary = &server->state.boundary;
    /* 旧缓存/内核队列不能跨 Primary 实例复用，即便 PID 或字节区间相同。 */
    if (server->generation &&
        (memcmp(server->primary_id, boundary->primary_id, KVS_EBPF_ID_BYTES) ||
         server->state.stream_end < server->observed_stream_end)) {
        server->source_changed = 1;
        fail_peer(server, "SOURCE_CHANGED");
        return;
    }
    memcpy(server->primary_id, boundary->primary_id, KVS_EBPF_ID_BYTES);
    server->observed_stream_end = server->state.stream_end;
    if (boundary->generation == server->generation)
        return;
    if (server->peer.active)
        fail_peer(server, "BOUNDARY_CHANGED");
    server->generation = boundary->generation;
    /* 这是内核观察到的 S，不是 BEGIN 声称的 offset。旧事件稍后直接跳过。 */
    if (server->cache.end <= boundary->stream_offset)
        capture_cache_reset(&server->cache, boundary->stream_offset);
    else if (server->cache.valid && server->cache.start <= boundary->stream_offset)
        capture_cache_discard(&server->cache, boundary->stream_offset);
    fprintf(stderr, "capture TCP boundary generation=%" PRIu64 " S=%" PRIu64 "\n",
            server->generation, (uint64_t)boundary->stream_offset);
}

static int collect_events(struct capture_server *server)
{
    for (unsigned int count = 0; count < 256; count++) {
        struct kvs_ebpf_event event;
        int result = kvs_ebpf_read(&event);
        if (result <= 0)
            return result;
        uint64_t floor = server->generation ? server->state.boundary.stream_offset : 0;
        if (event.end <= floor)
            continue;
        if (event.start < floor) {
            server->cache.valid = 0;
            fail_peer(server, "CACHE_GAP");
            continue;
        }
        result = capture_cache_append(&server->cache, &event, server->peer.active);
        if (result < 0)
            fail_peer(server, errno == ENOSPC ? "CACHE_OVERFLOW" : "CACHE_GAP");
        else if (result > 0)
            server->peer.lag_ms = 0;
    }
    return 1;
}

static void try_begin(struct capture_server *server)
{
    struct capture_peer *peer = &server->peer;
    if (!peer->requested || peer->active || peer->closing || server->state_result == -EAGAIN)
        return;
    if (server->state_result < 0) {
        fail_peer(server, "STATE_UNAVAILABLE");
        return;
    }
    int result = kvs_ebpf_match_boundary(&server->state,
        peer->primary_id, peer->sync_id, peer->snapshot);
    if (result < 0) {
        fail_peer(server, result == -ENODATA ? "CAPTURE_LOSS" : "STALE_BOUNDARY");
        return;
    }
    if (server->generation <= server->retired_generation) {
        fail_peer(server, "NEED_FULLSYNC");
        return;
    }
    if (!server->cache.valid || peer->snapshot < server->cache.start) {
        fail_peer(server, "CACHE_EVICTED");
        return;
    }
    if (peer->snapshot > server->cache.end) {
        fail_peer(server, "CACHE_GAP");
        return;
    }
    capture_cache_discard(&server->cache, peer->snapshot);
    peer->generation = server->generation;
    peer->sent = peer->applied = peer->snapshot;
    peer->target = server->state.stream_end;
    peer->active = 1;
    peer->ack_ms = now_ms();
    fprintf(stderr, "capture TCP BEGIN S=%" PRIu64 " target=%" PRIu64 "\n",
            peer->snapshot, peer->target);
}

/* 复用现有 RESP 解析器，防止 APPLIED 落在命令内部；不另存逐条索引。 */
static int valid_applied(const struct capture_cache *cache, uint64_t from, uint64_t to)
{
    while (from < to) {
        char command[KVS_EBPF_MAX_PAYLOAD];
        size_t capacity = to - from < sizeof(command) ? (size_t)(to - from) : sizeof(command);
        int length = capture_cache_read(cache, from, command, capacity);
        kvs_slice_t fields[16];
        size_t field_count, parsed;
        if (length <= 0 || kvs_parse_command_limited(command, (size_t)length, fields,
                16, &field_count, &parsed, sizeof(command)) != 1)
            return 0;
        from += parsed;
    }
    return from == to;
}

static void push_applied(struct capture_server *server, const char *line)
{
    struct capture_peer *peer = &server->peer;
    uint64_t offset;
    if (!peer->handshake || strncmp(line, "APPLIED ", 8) || parse_offset(line + 8, &offset) < 0 ||
        offset < peer->applied || offset > peer->sent) {
        peer->active = 1;
        fail_peer(server, "INVALID_APPLIED");
        return;
    }
    peer->active = 1;
    int match = server->state_result == 0 ? kvs_ebpf_match_boundary(&server->state,
        peer->primary_id, peer->sync_id, peer->snapshot) : -ESTALE;
    if (match < 0 || server->pending_error[0] || !server->cache.valid ||
        peer->applied < server->cache.start || offset > server->cache.end ||
        !valid_applied(&server->cache, peer->applied, offset)) {
        fail_peer(server, server->pending_error[0] ? server->pending_error :
            match == -ENODATA ? "CAPTURE_LOSS" : "NEED_FULLSYNC");
        return;
    }
    capture_cache_discard(&server->cache, offset);
    peer->sent = peer->applied = offset;
    peer->target = server->state.stream_end;
    peer->generation = server->generation;
    peer->ack_ms = now_ms();
    server->retry_delay = 0;
    fprintf(stderr, "capture TCP BEGIN S=%" PRIu64 " resume=%" PRIu64 " target=%" PRIu64 "\n",
            peer->snapshot, offset, peer->target);
}

static void control_line(struct capture_server *server, const char *line)
{
    struct capture_peer *peer = &server->peer;
    if (server->push_mode && !peer->active) {
        push_applied(server, line);
        return;
    }
    if (!peer->requested) {
        char offset[21];
        int consumed = 0;
        if (strlen(line) < 89 || strncmp(line, "BEGIN ", 6) || line[46] != ' ' || line[87] != ' ' ||
            sscanf(line, "BEGIN %40[0-9a-f] %40[0-9a-f] %20[0-9]%n",
                peer->primary_id, peer->sync_id, offset, &consumed) != 3 ||
            line[consumed] || strlen(peer->primary_id) != 40 || strlen(peer->sync_id) != 40 ||
            parse_offset(offset, &peer->snapshot) < 0) {
            fail_peer(server, "PROTOCOL");
            return;
        }
        peer->requested = 1;
        observe_state(server); /* BEGIN 到达前，Primary 可能刚发布了新边界。 */
        try_begin(server);
        return;
    }
    uint64_t offset;
    if (!peer->active || strncmp(line, "APPLIED ", 8) ||
        parse_offset(line + 8, &offset) < 0 || offset < peer->applied || offset > peer->sent ||
        !valid_applied(&server->cache, peer->applied, offset)) {
        fail_peer(server, "INVALID_APPLIED");
        return;
    }
    if (offset != peer->applied) {
        capture_cache_discard(&server->cache, offset);
        peer->applied = offset;
        peer->ack_ms = now_ms();
    }
}

static void read_controls(struct capture_server *server)
{
    struct capture_peer *peer = &server->peer;
    for (unsigned int budget = 0; budget < 16 && peer->fd >= 0 && !peer->closing; budget++) {
        ssize_t bytes = recv(peer->fd, peer->input + peer->input_length,
                             sizeof(peer->input) - peer->input_length, MSG_DONTWAIT);
        if (bytes == 0) {
            close_peer(server);
            return;
        }
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            close_peer(server);
            return;
        }
        peer->input_length += (size_t)bytes;
        char *newline;
        while (!peer->closing && peer->fd >= 0 &&
               (newline = memchr(peer->input, '\n', peer->input_length)) != NULL) {
            size_t line_bytes = (size_t)(newline - peer->input) + 1;
            if (line_bytes < 2 || newline[-1] != '\r' ||
                memchr(peer->input, '\0', line_bytes) != NULL) {
                fail_peer(server, "PROTOCOL");
                return;
            }
            newline[-1] = '\0';
            control_line(server, peer->input);
            if (peer->closing || peer->fd < 0)
                return;
            memmove(peer->input, peer->input + line_bytes, peer->input_length - line_bytes);
            peer->input_length -= line_bytes;
        }
        if (peer->input_length == sizeof(peer->input)) {
            fail_peer(server, "PROTOCOL");
            return;
        }
    }
}

static void check_session(struct capture_server *server, uint64_t now)
{
    struct capture_peer *peer = &server->peer;
    if (peer->fd < 0)
        return;
    if (peer->closing) {
        if (now - peer->state_wait_ms >= KVS_CAPTURE_TIMEOUT_MS)
            close_peer(server);
        return;
    }
    if (!peer->active) {
        if (!server->push_mode) try_begin(server);
        if (!peer->active && now - peer->accepted_ms >= KVS_CAPTURE_TIMEOUT_MS) {
            if (server->push_mode) {
                server->push_failed = 1;
                close_peer(server); /* 未完成身份确认，不能插入 ERROR。 */
            } else fail_peer(server, "NO_BOUNDARY");
        }
        return;
    }
    if (server->state_result < 0) {
        if (!peer->state_wait_ms)
            peer->state_wait_ms = now;
        if (server->state_result != -EAGAIN || now - peer->state_wait_ms >= KVS_CAPTURE_TIMEOUT_MS)
            fail_peer(server, "STATE_UNAVAILABLE");
        return;
    }
    peer->state_wait_ms = 0;
    int result = kvs_ebpf_match_boundary(&server->state,
        peer->primary_id, peer->sync_id, peer->snapshot);
    if (result < 0 || !server->cache.valid) {
        fail_peer(server, result == -ENODATA ? "CAPTURE_LOSS" : "BOUNDARY_CHANGED");
        return;
    }
    if (server->state.stream_end > server->cache.end) {
        if (!peer->lag_ms)
            peer->lag_ms = now;
        if (now - peer->lag_ms >= KVS_CAPTURE_TIMEOUT_MS)
            fail_peer(server, "CAPTURE_LAG");
    } else {
        peer->lag_ms = 0;
    }
    if (peer->active && peer->sent > peer->applied &&
        now - peer->ack_ms >= KVS_CAPTURE_TIMEOUT_MS)
        fail_peer(server, "APPLIED_TIMEOUT");
}

static int prepare_output(struct capture_server *server)
{
    struct capture_peer *peer = &server->peer;
    if (!peer->active || server->state_result < 0)
        return 0;
    uint64_t end = server->cache.end;
    if (!peer->online && end > peer->target)
        end = peer->target;
    if (peer->sent < end) {
        size_t bytes = end - peer->sent < KVS_CAPTURE_DATA_BYTES ?
                       (size_t)(end - peer->sent) : KVS_CAPTURE_DATA_BYTES;
        int header = snprintf(peer->output, KVS_CAPTURE_LINE_BYTES,
                              "DATA %" PRIu64 " %zu\r\n", peer->sent, bytes);
        if (capture_cache_read(&server->cache, peer->sent, peer->output + header, bytes) != (int)bytes) {
            fail_peer(server, "CACHE_GAP");
            return 0;
        }
        peer->output_length = (size_t)header + bytes;
        peer->output_kind = OUTPUT_DATA;
        peer->output_end = peer->sent + bytes;
        return 1;
    }
    if (!peer->online && peer->sent == peer->target && peer->applied == peer->target) {
        peer->output_length = (size_t)snprintf(peer->output, sizeof(peer->output),
                                              "ONLINE %" PRIu64 "\r\n", peer->target);
        peer->output_kind = OUTPUT_ONLINE;
        return 1;
    }
    return 0;
}

static void write_frames(struct capture_server *server)
{
    struct capture_peer *peer = &server->peer;
    if (peer->connecting) {
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(peer->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
            close_peer(server);
            return;
        }
        peer->connecting = 0;
    }
    /* 有上限的非阻塞发送；下轮必须回去继续消费 BPF。 */
    for (unsigned int budget = 0; budget < 16 && peer->fd >= 0; budget++) {
        if (!peer->output_length && !prepare_output(server))
            return;
        ssize_t bytes = send(peer->fd, peer->output + peer->output_offset,
                              peer->output_length - peer->output_offset,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_peer(server);
            return;
        }
        if (bytes == 0) {
            close_peer(server);
            return;
        }
        peer->output_offset += (size_t)bytes;
        if (peer->output_offset != peer->output_length)
            continue;
        if (peer->output_kind == OUTPUT_ERROR) {
            close_peer(server);
            return;
        }
        if (peer->output_kind == OUTPUT_BEGIN) peer->handshake = 1;
        if (peer->output_kind == OUTPUT_DATA) {
            if (peer->sent == peer->applied)
                peer->ack_ms = now_ms();
            peer->sent = peer->output_end;
        } else if (peer->output_kind == OUTPUT_ONLINE) {
            peer->online = 1;
            fprintf(stderr, "capture TCP ONLINE offset=%" PRIu64 "\n", peer->target);
        }
        peer->output_length = peer->output_offset = 0;
        peer->output_kind = OUTPUT_NONE;
    }
}

static void accept_peers(struct capture_server *server)
{
    for (unsigned int budget = 0; budget < 8; budget++) {
        int fd = accept4(server->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        if (server->peer.fd >= 0) {
            (void)send(fd, "ERROR BUSY\r\n", 12, MSG_DONTWAIT | MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        int enabled = 1, idle = 5, interval = 1, count = 3;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) < 0 ||
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0 ||
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0 ||
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
            close(fd);
            continue;
        }
        server->peer = (struct capture_peer){ .fd = fd, .accepted_ms = now_ms() };
    }
}

static void report_stats(const struct capture_server *server)
{
    fprintf(stderr, "capture TCP stats cache_start=%" PRIu64 " cache_end=%" PRIu64
                    " valid=%d sent=%" PRIu64 " applied=%" PRIu64
                    " stream_end=%" PRIu64 " oversize=%" PRIu64 " ring_full=%" PRIu64
                    " read_error=%" PRIu64 " map_result=%d\n",
            server->cache.start, server->cache.end, server->cache.valid,
            server->peer.sent, server->peer.applied, server->state.stream_end,
            (uint64_t)server->state.failures.oversize, (uint64_t)server->state.failures.ring_full,
            (uint64_t)server->state.failures.read_error, server->state_result);
}

static void push_ready(struct capture_server *server)
{
    for (unsigned int budget = 0; budget < 16; budget++) {
        kvs_replica_ready_event_t event;
        int result = kvs_ebpf_read_ready(&event);
        if (result <= 0) {
            if (result < 0) { server->source_changed = 1; fail_peer(server, "READY_SOURCE_ERROR"); }
            return;
        }
        struct in_addr address;
        if (event.version != KVS_EBPF_EVENT_VERSION || !event.replica_port ||
            !memchr(event.replica_ip, '\0', sizeof(event.replica_ip)) ||
            inet_pton(AF_INET, event.replica_ip, &address) != 1)
            continue;
        char primary[41], sync[41];
        memcpy(primary, event.primary_id, 40); primary[40] = '\0';
        memcpy(sync, event.full_sync_id, 40); sync[40] = '\0';
        if (strspn(primary, "0123456789abcdef") != 40 || strspn(sync, "0123456789abcdef") != 40)
            continue;
        if (server->state_result == 0 &&
            kvs_ebpf_match_boundary(&server->state, primary, sync, event.stream_offset) == -ESTALE)
            continue;
        if (server->have_ready && !memcmp(&event, &server->ready, sizeof(event))) continue;
        close_peer(server);
        server->ready = event;
        server->have_ready = 1;
        server->push_failed = 0;
        server->pending_error[0] = '\0';
        server->resume_applied = server->resume_sent = event.stream_offset;
        server->retry_delay = 0;
        server->retry_ms = 0;
        fprintf(stderr, "capture push ready %s:%u S=%" PRIu64 "\n", event.replica_ip,
                event.replica_port, (uint64_t)event.stream_offset);
    }
}

static void push_connect(struct capture_server *server)
{
    if (!server->have_ready || server->push_failed || server->peer.fd >= 0 ||
        now_ms() < server->retry_ms || server->state_result != 0)
        return;
    const kvs_replica_ready_event_t *event = &server->ready;
    char primary[41], sync[41];
    memcpy(primary, event->primary_id, 40); primary[40] = '\0';
    memcpy(sync, event->full_sync_id, 40); sync[40] = '\0';
    int match = kvs_ebpf_match_boundary(&server->state, primary, sync, event->stream_offset);
    if (match < 0 && match != -ENODATA) return;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(event->replica_port)};
    if (inet_pton(AF_INET, event->replica_ip, &address.sin_addr) != 1) return;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { server->retry_ms = now_ms() + 500; return; }
    int enabled = 1, idle = 5, interval = 1, count = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
        close(fd); server->retry_ms = now_ms() + 500; return;
    }
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd); server->retry_ms = now_ms() + 100; return;
    }
    struct capture_peer *peer = &server->peer;
    *peer = (struct capture_peer){.fd = fd, .requested = 1, .connecting = result < 0,
        .snapshot = event->stream_offset, .sent = server->resume_sent,
        .applied = server->resume_applied, .accepted_ms = now_ms()};
    memcpy(peer->primary_id, event->primary_id, 40);
    memcpy(peer->sync_id, event->full_sync_id, 40);
    peer->output_kind = OUTPUT_BEGIN;
    peer->output_length = (size_t)snprintf(peer->output, sizeof(peer->output),
        "BEGIN %s %s %" PRIu64 "\r\n", peer->primary_id, peer->sync_id, peer->applied);
}

static int capture_run(const char *endpoint, pid_t primary_pid,
                        const volatile sig_atomic_t *stopping)
{
    struct capture_server server = { .listener = -1, .peer = { .fd = -1 } };
    if (capture_cache_init(&server.cache, KVS_EBPF_CACHE_BYTES, 0) < 0)
        return -1;
    server.push_mode = endpoint == NULL;
    if (endpoint) server.listener = listen_endpoint(endpoint);
    if (endpoint && server.listener < 0) {
        int saved = errno;
        capture_cache_destroy(&server.cache);
        errno = saved;
        return -1;
    }
    unsigned long long source_start = process_start(primary_pid);
    if (source_start == 0) {
        close(server.listener);
        capture_cache_destroy(&server.cache);
        errno = ESRCH;
        return -1;
    }
    if (endpoint) fprintf(stderr, "capture TCP listening %s\n", endpoint);
    else fprintf(stderr, "capture push client ready\n");
    uint64_t reported_ms = 0;
    int status = 0, saved_errno = 0;
    while (!*stopping) {
        observe_state(&server);
        if (server.source_changed) {
            write_frames(&server);
            status = -1;
            saved_errno = ESTALE;
            break;
        }
        int events = collect_events(&server);
        if (events < 0) {
            fail_peer(&server, "EVENT_SOURCE_ERROR");
            write_frames(&server);
            status = -1;
            saved_errno = -events;
            break;
        }
        if (server.push_mode) { push_ready(&server); push_connect(&server); }
        uint64_t now = now_ms();
        check_session(&server, now);
        if (now - reported_ms >= 1000) {
            report_stats(&server);
            reported_ms = now;
            if (process_start(primary_pid) != source_start) {
                fail_peer(&server, "SOURCE_GONE");
                write_frames(&server);
                break;
            }
        }
        struct pollfd fds[2] = {
            { .fd = server.listener, .events = POLLIN },
            { .fd = server.peer.fd, .events = server.peer.closing ? 0 : POLLIN }
        };
        uint64_t send_end = server.peer.online || server.cache.end < server.peer.target ?
                            server.cache.end : server.peer.target;
        int can_send = server.peer.active && server.state_result == 0;
        if ((server.peer.closing && server.peer.output_length) || (can_send &&
            (server.peer.output_length || server.peer.sent < send_end ||
             (!server.peer.online && server.peer.sent == server.peer.target &&
              server.peer.applied == server.peer.target))))
            fds[1].events |= POLLOUT;
        if (server.push_mode && server.peer.output_length) fds[1].events |= POLLOUT;
        /* 无数据时睡 1ms；读/写/采集都有预算，慢客户端不能阻塞采集。 */
        int result = poll(fds, 2, 1);
        if (result < 0 && errno != EINTR) {
            status = -1;
            saved_errno = errno;
            break;
        }
        if (fds[0].revents & POLLIN)
            accept_peers(&server);
        if (fds[1].revents & (POLLIN | POLLHUP))
            read_controls(&server);
        if (fds[1].revents & (POLLERR | POLLNVAL))
            close_peer(&server);
        if (server.peer.fd >= 0 && (fds[1].revents & POLLOUT))
            write_frames(&server);
    }
    report_stats(&server);
    close_peer(&server);
    close(server.listener);
    capture_cache_destroy(&server.cache);
    if (status < 0)
        errno = saved_errno;
    return status;
}

int capture_server_run(const char *endpoint, pid_t pid, const volatile sig_atomic_t *stopping)
{
    return capture_run(endpoint, pid, stopping);
}
int capture_client_run(pid_t pid, const volatile sig_atomic_t *stopping)
{
    return capture_run(NULL, pid, stopping);
}
