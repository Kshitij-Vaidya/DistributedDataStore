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

echo "Phase 0 smoke test passed (${preset})."
