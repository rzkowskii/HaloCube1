#!/bin/bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
client="$root/build/macos/halo-ac"
if [[ ! -x "$client" ]]; then
    "$root/scripts/build-macos.sh"
else
    "$root/scripts/stage-macos-assets.sh"
fi

if [[ "${1:-}" == "--stock" ]]; then
    shift
    profile="${HALO_AC_PROFILE:-$root/build/stock-profile}"
else
    profile="${HALO_AC_PROFILE:-$HOME/Library/Application Support/Halo in AssaultCube}"
    set -- --halo "--mod=$root" -ehalo_campaign "$@"
fi
mkdir -p -- "$profile"
profile="$(cd -- "$profile" && pwd -P)"
cd -- "$root/build/runtime"
exec "$client" "--home=$profile" --init "$@"
