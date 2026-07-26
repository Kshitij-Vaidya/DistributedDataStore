#!/usr/bin/env bash
set -euo pipefail

formatter="${CLANG_FORMAT:-clang-format}"
if ! command -v "${formatter}" >/dev/null 2>&1; then
    echo "${formatter} was not found; install clang-format or set CLANG_FORMAT." >&2
    exit 1
fi

files=()
while IFS= read -r file; do
    files+=("${file}")
done < <(git ls-files --cached --others --exclude-standard -- '*.cpp' '*.hpp')

if [[ ${#files[@]} -eq 0 ]]; then
    echo "No tracked C++ files found."
    exit 0
fi

"${formatter}" --dry-run --Werror "${files[@]}"
