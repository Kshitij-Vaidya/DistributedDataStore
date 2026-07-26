#!/usr/bin/env bash
set -euo pipefail

cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure

if [[ "$(uname -s)" == "Linux" ]]; then
    cmake --preset tsan
    cmake --build --preset tsan -j
    ctest --preset tsan --output-on-failure
else
    echo "Skipping TSan: NovaCache validates this preset on Linux."
fi
