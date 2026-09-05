#!/usr/bin/env bash
set -euo pipefail

preset="${1:-debug}"
binary_dir="build/${preset}"

for program in novacache-server novacache-cli novacache-benchmark; do
    output="$("${binary_dir}/${program}" --version)"
    if [[ "${output}" != "${program} 0.1.0" ]]; then
        echo "Unexpected ${program} version output: ${output}" >&2
        exit 1
    fi
done

server_log="$(mktemp)"
data_dir="$(mktemp -d)"
server_pid=""

cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill "${server_pid}"
        wait "${server_pid}" 2>/dev/null || true
    fi
    rm -f "${server_log}"
    rm -rf "${data_dir}"
}
trap cleanup EXIT

start_server() {
    : >"${server_log}"
    "${binary_dir}/novacache-server" --host 127.0.0.1 --port 0 \
        --data-dir "${data_dir}" --fsync always --snapshot-interval 0 \
        >"${server_log}" 2>&1 &
    server_pid=$!

    port=""
    for _ in {1..100}; do
        port="$(awk '/NovaCache listening on port/ { print $NF; exit }' "${server_log}")"
        if [[ -n "${port}" ]]; then
            break
        fi
        if ! kill -0 "${server_pid}" 2>/dev/null; then
            echo "NovaCache server exited during startup:" >&2
            awk '{ print }' "${server_log}" >&2
            exit 1
        fi
        sleep 0.05
    done

    if [[ -z "${port}" ]]; then
        echo "Timed out waiting for NovaCache server startup." >&2
        exit 1
    fi
}

start_server

[[ "$("${binary_dir}/novacache-cli" -p "${port}" PING)" == "PONG" ]]
[[ "$("${binary_dir}/novacache-cli" -p "${port}" SET smoke value)" == "OK" ]]
[[ "$("${binary_dir}/novacache-cli" -p "${port}" GET smoke)" == "value" ]]
[[ "$("${binary_dir}/novacache-cli" -p "${port}" DEL smoke)" == "1" ]]
[[ "$("${binary_dir}/novacache-cli" -p "${port}" SET durable keep-me)" == "OK" ]]
[[ "$("${binary_dir}/novacache-cli" -p "${port}" INFO)" == *"total_commands_processed:"* ]]

kill "${server_pid}"
wait "${server_pid}" 2>/dev/null || true
server_pid=""

start_server
[[ "$("${binary_dir}/novacache-cli" -p "${port}" GET durable)" == "keep-me" ]]

echo "Phase 3 smoke test passed (${preset}, port ${port})."
