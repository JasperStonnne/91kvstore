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
# Replica 启动前先向 Primary 写入数据。
# 这条数据只能通过 Snapshot 全量同步到 Replica。
#
primary_response=$(
    send_command "$PRIMARY_PORT" \
        "HSET fullsync_test before_sync"
)

if [ "$primary_response" != "OK" ]; then
    fail "Primary returned unexpected response: $primary_response"
fi

echo "[PASS] Initial data written to Primary"

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
# 验证 Snapshot 全量同步。
#
wait_for_value \
    "$REPLICA_PORT" \
    "HGET fullsync_test" \
    "before_sync" ||
    fail "Snapshot data was not installed on Replica"

echo "[PASS] Snapshot full synchronization"

#
# ONLINE 后再向 Primary 写入。
# 这条数据必须通过持续 AOF 复制到 Replica。
#
online_response=$(
    send_command "$PRIMARY_PORT" \
        "HSET online_test after_online"
)

if [ "$online_response" != "OK" ]; then
    fail "Primary ONLINE write failed: $online_response"
fi

wait_for_value \
    "$REPLICA_PORT" \
    "HGET online_test" \
    "after_online" ||
    fail "ONLINE data was not replicated"

echo "[PASS] ONLINE incremental synchronization"

echo
echo "replication integration test passed"