#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
KVSTORE_BIN="$PROJECT_DIR/bin/kvstore"

PRIMARY_PORT=29000
REPLICA_PORT=29001
REPLICATION_PORT=29100

TEST_DIR=$(mktemp -d /tmp/91kv-replication-test.XXXXXX)
PRIMARY_DIR="$TEST_DIR/primary"
REPLICA_DIR="$TEST_DIR/replica"

PRIMARY_LOG="$TEST_DIR/primary.log"
REPLICA_LOG="$TEST_DIR/replica.log"

PRIMARY_PID=""
REPLICA_PID=""

mkdir -p "$PRIMARY_DIR" "$REPLICA_DIR"

cleanup()
{
    if [ -n "$REPLICA_PID" ]; then
        kill "$REPLICA_PID" 2>/dev/null || true
        wait "$REPLICA_PID" 2>/dev/null || true
    fi

    if [ -n "$PRIMARY_PID" ]; then
        kill "$PRIMARY_PID" 2>/dev/null || true
        wait "$PRIMARY_PID" 2>/dev/null || true
    fi

    if [[ "$TEST_DIR" == /tmp/91kv-replication-test.* ]]; then
        rm -rf -- "$TEST_DIR"
    fi
}

trap cleanup EXIT INT TERM

fail()
{
    echo "replication test failed: $1" >&2

    echo "----- primary log -----" >&2
    cat "$PRIMARY_LOG" >&2 || true

    echo "----- replica log -----" >&2
    cat "$REPLICA_LOG" >&2 || true

    exit 1
}

wait_for_log()
{
    local log_path=$1
    local expected=$2

    for ((attempt = 0; attempt < 200; attempt++)); do
        if grep -Fq "$expected" "$log_path" 2>/dev/null; then
            return 0
        fi

        sleep 0.1
    done

    return 1
}

send_command()
{
    local port=$1
    local command=$2

    printf '%s\r\n' "$command" |
        timeout 3 nc -N 127.0.0.1 "$port" |
        tr -d '\r\n'
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

# 用 RESP 编码并发送 HSET，字段长度由脚本按字节内容计算。
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

# 用 RESP 编码并发送 HGET。
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

wait_for_value()
{
    local port=$1
    local command=$2
    local expected=$3
    local response=""

    for ((attempt = 0; attempt < 100; attempt++)); do
        if response=$(send_command "$port" "$command" 2>/dev/null) &&
           [ "$response" = "$expected" ]; then
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

for required_command in nc timeout stdbuf; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        fail "$required_command is required"
    fi
done

#
# 启动 Primary。
#
(
    cd "$PRIMARY_DIR"

    exec stdbuf -oL "$KVSTORE_BIN" \
        primary "$PRIMARY_PORT" "$REPLICATION_PORT"
) >"$PRIMARY_LOG" 2>&1 &

PRIMARY_PID=$!

wait_for_log "$PRIMARY_LOG" "listen port : $PRIMARY_PORT" ||
    fail "Primary service port did not start"

wait_for_log "$PRIMARY_LOG" "listen port : $REPLICATION_PORT" ||
    fail "Primary replication port did not start"

echo "[PASS] Primary started"

#
# Replica 启动前，用 RESP 写入含空格的 key/value。
# 这条数据应通过 Snapshot 全量同步到 Replica。
#
primary_response=$(
    send_resp_frame "$PRIMARY_PORT" \
        '*3\r\n$4\r\nHSET\r\n$13\r\nfullsync_test\r\n$17\r\nbefore sync value\r\n'
)

if [ "$primary_response" != "OK" ]; then
    fail "Primary returned unexpected RESP response: $primary_response"
fi

echo "[PASS] Initial RESP data written to Primary"


# 准备一个超过旧固定缓冲区的 value。
large_value=$(printf '%2048s' '' | tr ' ' 'v')

large_write_response=$(
    send_resp_hset "$PRIMARY_PORT" "large_fullsync" "$large_value"
)

if [ "$large_write_response" != "OK" ]; then
    fail "Primary rejected the 2048-byte value: $large_write_response"
fi

echo "[PASS] 2048-byte value written to Primary"

#
# 启动 Replica。
#
(
    cd "$REPLICA_DIR"

    exec stdbuf -oL "$KVSTORE_BIN" \
        replica "$REPLICA_PORT" \
        127.0.0.1 "$REPLICATION_PORT"
) >"$REPLICA_LOG" 2>&1 &

REPLICA_PID=$!

wait_for_log "$REPLICA_LOG" "listen port : $REPLICA_PORT" ||
    fail "Replica service port did not start"

wait_for_log "$REPLICA_LOG" "replication: enter ONLINE" ||
    fail "Replica did not enter ONLINE"

echo "[PASS] Replica entered ONLINE"

#
# 验证 Snapshot 全量同步保留了 value 中的空格。
#
wait_for_value \
    "$REPLICA_PORT" \
    "HGET fullsync_test" \
    "before sync value" ||
    fail "Snapshot did not preserve the value containing spaces"

echo "[PASS] Snapshot full synchronization with spaces"


# RESP bulk string 响应去掉 CR/LF 后，应为 $长度 加上完整 value。
large_expected_response="\$2048${large_value}"

large_read_response=$(
    send_resp_hget "$REPLICA_PORT" "large_fullsync"
)

if [ "$large_read_response" != "$large_expected_response" ]; then
    fail "Snapshot replication did not preserve the 2048-byte value"
fi

echo "[PASS] Snapshot full synchronization with a 2048-byte value"

#
# Replica 进入 ONLINE 后，再通过 RESP 写入含空格的数据。
# 这条数据应通过 AOF 增量复制到 Replica。
#
online_response=$(
    send_resp_frame "$PRIMARY_PORT" \
        '*3\r\n$4\r\nHSET\r\n$11\r\nonline_test\r\n$18\r\nafter online value\r\n'

)

if [ "$online_response" != "OK" ]; then
    fail "Primary returned unexpected online RESP response: $online_response"
fi

wait_for_value \
    "$REPLICA_PORT" \
    "HGET online_test" \
    "after online value" ||
    fail "ONLINE replication did not preserve the value containing spaces"

echo "[PASS] ONLINE incremental synchronization with spaces"

echo
echo "replication integration test passed"