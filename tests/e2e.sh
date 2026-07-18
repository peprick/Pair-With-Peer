#!/usr/bin/env bash
set -euo pipefail

repository_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tracker_binary="${PWP_TRACKER_BIN:-$repository_dir/bin/pwp-tracker}"
peer_binary="${PWP_PEER_BIN:-$repository_dir/bin/pwp-peer}"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/pwp-e2e.XXXXXX")"

# Give adjacent concurrent test processes non-overlapping four-port blocks.
base_port=$((20000 + (($$ % 10000) * 4)))
tracker_port=$base_port
alice_port=$((base_port + 1))
bob_port=$((base_port + 2))
alice_restart_port=$((base_port + 3))
tracker_pid=""
alice_pid=""
bob_pid=""

terminate_process() {
    local process_id="$1"
    if [[ -z "$process_id" ]] || ! kill -0 "$process_id" 2>/dev/null; then
        return
    fi
    kill "$process_id" 2>/dev/null || true
    for _ in {1..40}; do
        if ! kill -0 "$process_id" 2>/dev/null; then
            wait "$process_id" 2>/dev/null || true
            return
        fi
        sleep 0.05
    done
    kill -KILL "$process_id" 2>/dev/null || true
    wait "$process_id" 2>/dev/null || true
}

cleanup() {
    local status=$?
    trap - EXIT
    exec 3>&- 4>&- || true

    if [[ $status -ne 0 ]]; then
        echo "End-to-end test logs:" >&2
        for log_file in tracker alice alice-restarted bob; do
            echo "--- $log_file ---" >&2
            sed -n '1,200p' "$test_dir/$log_file.log" >&2 2>/dev/null || true
        done
    fi

    terminate_process "$alice_pid" 2>/dev/null
    terminate_process "$bob_pid" 2>/dev/null
    terminate_process "$tracker_pid" 2>/dev/null
    rm -rf "$test_dir"
    exit "$status"
}
trap cleanup EXIT

wait_for_log() {
    local pattern="$1"
    local log_file="$2"
    local process_id="$3"
    for _ in {1..300}; do
        if grep -Fq -- "$pattern" "$log_file" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$process_id" 2>/dev/null; then
            echo "Process exited while waiting for '$pattern' in $log_file" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "Timed out waiting for '$pattern' in $log_file" >&2
    return 1
}

wait_for_file() {
    local file="$1"
    local process_id="$2"
    for _ in {1..600}; do
        if [[ -f "$file" ]]; then
            return 0
        fi
        if ! kill -0 "$process_id" 2>/dev/null; then
            echo "Process exited while waiting for $file" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "Timed out waiting for $file" >&2
    return 1
}

wait_for_exit() {
    local process_id="$1"
    local name="$2"
    for _ in {1..200}; do
        if ! kill -0 "$process_id" 2>/dev/null; then
            wait "$process_id"
            return
        fi
        sleep 0.05
    done
    echo "$name did not exit cleanly" >&2
    return 1
}

mkdir -p "$test_dir/alice/public" "$test_dir/alice/private" "$test_dir/bob"
printf 'Hello from the Pair with Peer end-to-end test.\n' > "$test_dir/alice/public/hello.txt"
printf 'This file was sent directly to Bob.\n' > "$test_dir/alice/private/direct.txt"
mkfifo "$test_dir/alice.stdin" "$test_dir/bob.stdin"
exec 3<>"$test_dir/alice.stdin"
exec 4<>"$test_dir/bob.stdin"

"$tracker_binary" --listen "127.0.0.1:$tracker_port" >"$test_dir/tracker.log" 2>&1 &
tracker_pid=$!
wait_for_log "Listening on" "$test_dir/tracker.log" "$tracker_pid"

"$peer_binary" \
    --name alice \
    --listen "127.0.0.1:$alice_port" \
    --tracker "127.0.0.1:$tracker_port" \
    --data-dir "$test_dir/alice" \
    <"$test_dir/alice.stdin" >"$test_dir/alice.log" 2>&1 &
alice_pid=$!
wait_for_log "Connected as alice" "$test_dir/alice.log" "$alice_pid"

"$peer_binary" \
    --name bob \
    --listen "127.0.0.1:$bob_port" \
    --tracker "127.0.0.1:$tracker_port" \
    --data-dir "$test_dir/bob" \
    --accept-direct \
    <"$test_dir/bob.stdin" >"$test_dir/bob.log" 2>&1 &
bob_pid=$!
wait_for_log "Connected as bob" "$test_dir/bob.log" "$bob_pid"

printf 'list\n' >&4
wait_for_log "alice (1)" "$test_dir/bob.log" "$bob_pid"
wait_for_log "- hello.txt" "$test_dir/bob.log" "$bob_pid"

printf 'get hello.txt\n' >&4
wait_for_file "$test_dir/bob/downloads/hello.txt" "$bob_pid"
cmp "$test_dir/alice/public/hello.txt" "$test_dir/bob/downloads/hello.txt"

printf 'send bob direct.txt\n' >&3
wait_for_file "$test_dir/bob/inbox/direct.txt" "$bob_pid"
cmp "$test_dir/alice/private/direct.txt" "$test_dir/bob/inbox/direct.txt"

# Simulate a crash, then verify that a stale username can be reclaimed.
terminate_process "$alice_pid" 2>/dev/null
alice_pid=""
"$peer_binary" \
    --name alice \
    --listen "127.0.0.1:$alice_restart_port" \
    --tracker "127.0.0.1:$tracker_port" \
    --data-dir "$test_dir/alice" \
    <"$test_dir/alice.stdin" >"$test_dir/alice-restarted.log" 2>&1 &
alice_pid=$!
wait_for_log "Connected as alice" "$test_dir/alice-restarted.log" "$alice_pid"
wait_for_log "~ alice" "$test_dir/tracker.log" "$tracker_pid"

printf 'quit\n' >&4
printf 'quit\n' >&3
wait_for_exit "$bob_pid" "Bob"
bob_pid=""
wait_for_exit "$alice_pid" "Alice"
alice_pid=""
wait_for_log "- bob" "$test_dir/tracker.log" "$tracker_pid"
wait_for_log "- alice" "$test_dir/tracker.log" "$tracker_pid"

echo "End-to-end test passed."
