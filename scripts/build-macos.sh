#!/bin/bash
# Build the game (build/macos). Arguments go to cmake, for example -DHALO_BUILD_QA=ON.
#   --asan   build the ASan/UBSan QA client in build/macos-asan instead (HALO_SANITIZE, for
#            tests/test_soak.py --asan and tests/test_asset_robustness.py --asan); never packaged.
# HALO_BUILD_DIR overrides the build directory.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build="${HALO_BUILD_DIR:-$root/build/macos}"
type=Release
args=()
for arg in "$@"; do
    if [[ "$arg" == "--asan" ]]; then
        build="${HALO_BUILD_DIR:-$root/build/macos-asan}"
        type=RelWithDebInfo
        args+=(-DHALO_BUILD_QA=ON -DHALO_SANITIZE=ON)
    else
        args+=("$arg")
    fi
done
prefix="${CMAKE_PREFIX_PATH:-}"
if [[ -z "$prefix" ]] && command -v brew >/dev/null; then
    prefix="$(brew --prefix)"
fi
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE="$type" -DCMAKE_PREFIX_PATH="$prefix" ${args[@]+"${args[@]}"}
cmake --build "$build" --parallel 4
"$root/scripts/stage-macos-assets.sh"
