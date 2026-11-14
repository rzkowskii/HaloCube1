#!/bin/bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
prefix="${CMAKE_PREFIX_PATH:-}"
if [[ -z "$prefix" ]] && command -v brew >/dev/null; then
    prefix="$(brew --prefix)"
fi
cmake -S "$root" -B "$root/build/macos" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" "$@"
cmake --build "$root/build/macos" --parallel 4
"$root/scripts/stage-macos-assets.sh"
