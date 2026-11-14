#!/bin/bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
archive="${1:-${AC_ASSET_ARCHIVE:-$HOME/Downloads/AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2}}"
runtime="$root/build/runtime"
release_sha256=9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55
marker="$runtime/.assaultcube-assets"

if [[ -f "$marker" && $(<"$marker") == "$release_sha256" && -f "$runtime/config/font.cfg" && -f "$runtime/packages/misc/notexture.jpg" ]]; then
    exit 0
fi
if [[ -e "$runtime" || -L "$runtime" ]]; then
    printf 'Move the incomplete asset directory before staging: %s\n' "$runtime" >&2
    exit 1
fi
if [[ ! -f "$archive" ]]; then
    printf 'Set AC_ASSET_ARCHIVE to the stock AssaultCube 1.3.0.2 Lockdown Edition RC1 Linux archive.\n' >&2
    exit 1
fi
actual="$(shasum -a 256 < "$archive")"
if [[ "${actual%% *}" != "$release_sha256" ]]; then
    printf 'Unexpected AssaultCube archive SHA-256: %s\n' "${actual%% *}" >&2
    exit 1
fi

mkdir -p "$root/build"
staged="$(mktemp -d "$root/build/.runtime.XXXXXX")"
trap 'rm -rf -- "$staged"' EXIT
tar -xjf "$archive" -C "$staged" ./bot ./config ./demos ./docs ./mods ./packages
for required in config/font.cfg config/configtemplates.zip packages/misc/notexture.jpg docs/license.html; do
    [[ -f "$staged/$required" ]] || { printf 'Missing stock asset: %s\n' "$required" >&2; exit 1; }
done
printf '%s\n' "$release_sha256" > "$staged/.assaultcube-assets"
python3 - "$staged" "$runtime" <<'PY'
import errno
import os
from pathlib import Path
import sys

staged, runtime = map(Path, sys.argv[1:])
try:
    os.rename(staged, runtime)
except OSError as error:
    if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
        raise
    if (runtime / ".assaultcube-assets").read_bytes() != (staged / ".assaultcube-assets").read_bytes():
        raise
PY
