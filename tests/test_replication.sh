#!/usr/bin/env bash

# -e：未处理的命令失败时退出
# -u：使用未定义变量时报错
# pipefail：管道中任一命令失败，整条管道算失败
set -euo pipefail

PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
KVSTORE_BIN="$PROJECT_DIR/bin/kvstore"

# 默认端口与原脚本相同；运行时可以用环境变量覆盖。
PRIMARY_PORT=${PRIMARY_PORT:-29000}
REPLICA_PORT=${REPLICA_PORT:-29001}

# 两个服务在不同目录运行，各自的持久化文件不会落在仓库里。
TEST_DIR=$(mktemp -d /tmp/91kv-replication-test.XXXXXX)
PRIMARY_DIR="$TEST_DIR/primary"
REPLICA_DIR="$TEST_DIR/replica"
PRIMARY_LOG="$TEST_DIR/primary.log"
PRIMARY_CONFIG="$PRIMARY_DIR/primary.conf"
REPLICA_CONFIG="$REPLICA_DIR/replica.conf"
REPLICA_LOG="$TEST_DIR/replica.log"

# 这里只会保存本脚本亲自启动的进程号。
PRIMARY_PID=""
REPLICA_PID=""

mkdir -p "$PRIMARY_DIR" "$REPLICA_DIR"

cleanup()
{
    local status=$?
    trap - EXIT INT TERM

    # 停止本测试启动的服务。未启动时 PID 为空，不会执行 kill。
    if [ -n "$REPLICA_PID" ]; then
        kill "$REPLICA_PID" 2>/dev/null || true
        wait "$REPLICA_PID" 2>/dev/null || true
    fi

    if [ -n "$PRIMARY_PID" ]; then
        kill "$PRIMARY_PID" 2>/dev/null || true
        wait "$PRIMARY_PID" 2>/dev/null || true
    fi

if [ "$status" -eq 0 ] ||
   { [ -z "$PRIMARY_PID" ] && [ -z "$REPLICA_PID" ]; }; then
        # 成功，或尚未启动服务就失败：删除本测试创建的目录。
        if [[ "$TEST_DIR" == /tmp/91kv-replication-test.* ]]; then
            rm -rf -- "$TEST_DIR"
        fi
    else
        # 失败时保留数据和日志，便于排查。
        printf 'rerun: PRIMARY_PORT=%q REPLICA_PORT=%q bash %q\n' \
    "$PRIMARY_PORT" "$REPLICA_PORT" \
    "$PROJECT_DIR/tests/test_replication.sh" >&2
    fi
}

# 正常退出、Ctrl+C 或终止信号都会触发清理。
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    echo "replication test failed: $1" >&2

    # 启动前失败时日志文件可能尚不存在，不打印无关的 cat 错误。
    if [ -f "$PRIMARY_LOG" ]; then
        echo "----- primary log -----" >&2
        cat "$PRIMARY_LOG" >&2
    fi

    if [ -f "$REPLICA_LOG" ]; then
        echo "----- replica log -----" >&2
        cat "$REPLICA_LOG" >&2
    fi

    exit 1
}

# 最多等待约 20 秒；服务提前退出时立即报告失败。
wait_for_log()
{
    local log_path=$1
    local expected=$2
    local pid=$3
    local attempt

    for ((attempt = 0; attempt < 200; attempt++)); do
        if grep -Fq "$expected" "$log_path" 2>/dev/null; then
            return 0
        fi

        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi

        sleep 0.1
    done

    return 1
}

# 发送已经编码好的 RESP 字节帧。
send_resp_frame()
{
    local port=$1
    local frame=$2

    printf '%b' "$frame" |
        timeout 3 nc -N 127.0.0.1 "$port" |
        tr -d '\r\n'
}

# 用 RESP 编码发送 HSET。这里的 key/value 都是 ASCII，字符数等于字节数。
send_resp_hset()
{
    local port=$1
    local key=$2
    local value=$3

    {
        printf '*3\r\n$4\r\nHSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n' \
            "${#key}" "$key" "${#value}" "$value"
    } |
        timeout 3 nc -N 127.0.0.1 "$port" |
        tr -d '\r\n'
}

# 用 RESP 编码发送 HGET。
send_resp_hget()
{
    local port=$1
    local key=$2

    {
        printf '*2\r\n$4\r\nHGET\r\n$%d\r\n%s\r\n' \
            "${#key}" "$key"
    } |
        timeout 3 nc -N 127.0.0.1 "$port" |
        tr -d '\r\n'
}

# 复制是异步的：在限定时间内反复读取，直到得到准确的值。
wait_for_value()
{
    local port=$1
    local key=$2
    local expected=$3
    local expected_frame
    printf -v expected_frame '$%d%s' "${#expected}" "$expected"
    local response=""
    local deadline=$((SECONDS + 15))

    while (( SECONDS < deadline )); do
        if response=$(send_resp_hget "$port" "$key" 2>/dev/null) &&
           [ "$response" = "$expected_frame" ]; then
            return 0
        fi

        sleep 0.1
    done

    echo "last response: $response" >&2
    return 1
}

if [ ! -x "$KVSTORE_BIN" ]; then
    fail "bin/kvstore does not exist; run make first"
fi

for required_command in nc timeout stdbuf ss; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        fail "$required_command is required"
    fi
done

# 本测试中两个进程都监听本机，因此服务端口必须不同。
if [ "$PRIMARY_PORT" = "$REPLICA_PORT" ]; then
    fail "test ports must be different"
fi

# 启动服务前检查端口格式、范围和现有监听者。
# 检查与实际启动之间仍可能有极短的竞争；下面的启动检查也会捕获失败。
for port in "$PRIMARY_PORT" "$REPLICA_PORT"; do
    if [[ ! "$port" =~ ^[0-9]+$ ]] ||
       [ "$port" -lt 1 ] ||
       [ "$port" -gt 65535 ]; then
        fail "invalid test port: $port"
    fi

    listeners=$(ss -Hlt "( sport = :$port )") ||
        fail "could not inspect test port: $port"
    if [ -n "$listeners" ]; then
        fail "test port $port is already in use"
    fi
done

# 配置文件也放在测试临时目录中，不写入仓库。
# Primary 保持现有的双端口：客户端端口 + Replica 专用复制端口。
cat >"$PRIMARY_CONFIG" <<EOF
role = primary
bind_ip = 127.0.0.1
service_port = $PRIMARY_PORT
log_level = info
persistence_mode = legacy_hybrid
EOF

# 启动 Primary。工作目录是临时目录，PID 是本脚本启动的进程。
(
    cd "$PRIMARY_DIR"
    exec stdbuf -oL "$KVSTORE_BIN" "$PRIMARY_CONFIG"
) </dev/null >"$PRIMARY_LOG" 2>&1 &
PRIMARY_PID=$!

wait_for_log "$PRIMARY_LOG" "listen port : $PRIMARY_PORT" "$PRIMARY_PID" ||
    fail "Primary service port did not start"
echo "[PASS] Primary started"

python3 - "$PRIMARY_PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])

def receive_exact(conn, length):
    result = b""
    while len(result) < length:
        chunk = conn.recv(length - len(result))
        if not chunk:
            raise RuntimeError("connection closed before complete reply")
        result += chunk
    return result

with socket.create_connection(("127.0.0.1", port), timeout=3) as replica:
    replica.sendall(b"PI")

    # 复制握手未完成时，普通客户端仍应正常工作。
    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
        client.sendall(
            b"*3\r\n$4\r\nHSET\r\n"
            b"$11\r\nroute_probe\r\n$2\r\nok\r\n"
        )
        assert receive_exact(client, 5) == b"+OK\r\n"

    # 补齐同一条连接上的握手。
    replica.sendall(b"NG\r\n")
    assert receive_exact(replica, 6) == b"PONG\r\n"

print("[PASS] fragmented replication handshake and concurrent client")
for invalid in (b"PINK\r\n", b"PINGX\r\n", b"HELLO\r\n"):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as conn:
        conn.sendall(invalid)
        try:
            reply = conn.recv(1)
        except ConnectionResetError:
            reply = b""

        assert reply == b"", (
            f"invalid prefix received an unexpected reply: {invalid!r}, {reply!r}"
        )

print("[PASS] invalid connection prefixes rejected")
import time

with socket.create_connection(("127.0.0.1", port), timeout=7) as conn:
    started = time.monotonic()

    # 不发送任何字节，等待 Primary 主动关闭。
    try:
        reply = conn.recv(1)
    except ConnectionResetError:
        reply = b""
    except socket.timeout:
        raise AssertionError("unidentified connection was not closed") from None

    elapsed = time.monotonic() - started
    assert reply == b"", "silent connection received unexpected data"
    assert 4.5 <= elapsed < 7, (
        f"unexpected identification timeout: {elapsed:.3f}s"
    )

print("[PASS] silent connection identification timeout")
with socket.create_connection(("127.0.0.1", port), timeout=7) as conn:
    started = time.monotonic()
    conn.sendall(b"P")

    time.sleep(3)
    conn.sendall(b"I")

    # 总计最多等到第 7 秒，不能因为收到 I 续期到第 8 秒。
    conn.settimeout(4)

    try:
        reply = conn.recv(1)
    except ConnectionResetError:
        reply = b""
    except socket.timeout:
        raise AssertionError("partial handshake renewed the deadline") from None

    elapsed = time.monotonic() - started
    assert reply == b"", "partial handshake received unexpected data"
    assert 4.5 <= elapsed < 7, (
        f"unexpected partial handshake timeout: {elapsed:.3f}s"
    )

print("[PASS] partial handshake does not renew identification deadline")
with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
    # 首字节已确定客户端路径，但 RESP 命令尚不完整。
    client.sendall(b"*")
    time.sleep(6)

    client.sendall(
        b"3\r\n$4\r\nHSET\r\n"
        b"$11\r\nlate_client\r\n$2\r\nok\r\n"
    )
    assert receive_exact(client, 5) == b"+OK\r\n"

print("[PASS] identified client preserves existing partial-request behavior")
PY

primary_listener_count=$(
    ss -Hltpn |
        awk -v marker="pid=$PRIMARY_PID," \
            'index($0, marker) { count++ } END { print count + 0 }'
)

if [ "$primary_listener_count" -ne 1 ]; then
    fail "Primary must have exactly one listening socket; found $primary_listener_count"
fi

# Replica 启动前，写入带空格的数据；它应通过 Snapshot 全量同步。
primary_response=$(
    send_resp_frame "$PRIMARY_PORT" \
        '*3\r\n$4\r\nHSET\r\n$13\r\nfullsync_test\r\n$17\r\nbefore sync value\r\n'
) || fail "Initial RESP HSET request failed or timed out"

if [ "$primary_response" != "+OK" ]; then
    fail "Primary returned unexpected RESP response: $primary_response"
fi
echo "[PASS] Initial RESP data written to Primary"

# 2048 字节值用于检查全量同步不截断较大的数据。
large_value=$(printf '%2048s' '' | tr ' ' 'v')
large_write_response=$(
    send_resp_hset "$PRIMARY_PORT" "large_fullsync" "$large_value"
) || fail "2048-byte HSET request failed or timed out"

if [ "$large_write_response" != "+OK" ]; then
    fail "Primary rejected the 2048-byte value: $large_write_response"
fi
echo "[PASS] 2048-byte value written to Primary"

# Replica 的客户端监听端口属于自己；primary_host/primary_port
# 指向它要连接的 Primary 复制端口。
cat >"$REPLICA_CONFIG" <<EOF
role = replica
bind_ip = 127.0.0.1
service_port = $REPLICA_PORT
primary_host = 127.0.0.1
primary_port = $PRIMARY_PORT
log_level = info
persistence_mode = legacy_hybrid
EOF

# 启动 Replica，它的数据文件位于另一个临时目录。
(
    cd "$REPLICA_DIR"
    exec stdbuf -oL "$KVSTORE_BIN" "$REPLICA_CONFIG"
) </dev/null >"$REPLICA_LOG" 2>&1 &
REPLICA_PID=$!

wait_for_log "$REPLICA_LOG" "listen port : $REPLICA_PORT" "$REPLICA_PID" ||
    fail "Replica service port did not start"
wait_for_log "$REPLICA_LOG" "replication: enter ONLINE" "$REPLICA_PID" ||
    fail "Replica did not enter ONLINE"
echo "[PASS] Replica entered ONLINE"

wait_for_value "$REPLICA_PORT" "fullsync_test" "before sync value"||
    fail "Snapshot did not preserve the value containing spaces"
echo "[PASS] Snapshot full synchronization with spaces"

# 这里保留原测试对 RESP 大值回复的准确内容检查。
large_expected_response="\$2048${large_value}"
large_read_response=$(
    send_resp_hget "$REPLICA_PORT" "large_fullsync"
) || fail "2048-byte HGET request failed or timed out"

if [ "$large_read_response" != "$large_expected_response" ]; then
    fail "Snapshot replication did not preserve the 2048-byte value"
fi
echo "[PASS] Snapshot full synchronization with a 2048-byte value"

# Replica ONLINE 后再写入，验证 AOF 增量复制。
online_response=$(
    send_resp_frame "$PRIMARY_PORT" \
        '*3\r\n$4\r\nHSET\r\n$11\r\nonline_test\r\n$18\r\nafter online value\r\n'
) || fail "ONLINE RESP HSET request failed or timed out"

if [ "$online_response" != "+OK" ]; then
    fail "Primary returned unexpected online RESP response: $online_response"
fi

wait_for_value "$REPLICA_PORT" "online_test" "after online value" ||
    fail "ONLINE replication did not preserve the value containing spaces"
echo "[PASS] ONLINE incremental synchronization with spaces"



echo
# Replica 已完成同步，现在停止它，检查 Primary 的空闲连接清理。
kill "$REPLICA_PID"
wait "$REPLICA_PID" || true
REPLICA_PID=""

python3 - "$PRIMARY_PID" <<'PY'
import os
import sys
import time

fd_dir = f"/proc/{int(sys.argv[1])}/fd"
deadline = time.monotonic() + 3

while True:
    sockets = []

    for name in os.listdir(fd_dir):
        try:
            target = os.readlink(f"{fd_dir}/{name}")
        except FileNotFoundError:
            continue

        if target.startswith("socket:["):
            sockets.append(name)

    # 当前测试没有其他活动连接，只应剩下服务监听 socket。
    if len(sockets) == 1:
        break

    if time.monotonic() >= deadline:
        raise AssertionError(
            f"Primary retained sockets after Replica disconnected: {sockets}"
        )

    time.sleep(0.05)

print("[PASS] Primary releases idle disconnected Replica socket")
PY
echo "replication integration test passed"