#!/usr/bin/env bash
set -euo pipefail

cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan -j
ctest --preset tsan --output-on-failure
