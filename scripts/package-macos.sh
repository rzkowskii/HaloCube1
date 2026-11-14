#!/bin/bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
for asset in packages/maps/halo_a30.cgz packages/maps/halo_a30.cfg packages/maps/halo_a30.mesh \
    packages/textures/halo_a30/cliff.png packages/models/halo/ma5b/tris.md3 \
    packages/audio/halo/ma5b-fire.wav; do
    if [[ ! -f "$root/$asset" ]]; then
        printf 'Missing %s. Import Halo A30 content as described in docs/macos-build.md.\n' "$asset" >&2
        exit 1
    fi
done
if [[ -f "$root/packages/maps/halo_a10_01.cgz" ]]; then
    PYTHONPATH="$root/tools" python3 - "$root" <<'PYTHON'
from pathlib import Path
import sys

from import_halo_campaign import MISSIONS

root = Path(sys.argv[1])
for section in (1, 2):
    collision = root / f'packages/maps/halo_b30_{section:02}.hcol'
    if not collision.is_file():
        raise SystemExit(f'Missing retail collision import: {collision}. Reimport b30 as described in docs/macos-build.md.')
for mission in MISSIONS:
    program = root / f'packages/halo/{mission.cache}.hscb'
    if not program.is_file():
        raise SystemExit(f'Missing retail script import: {program}. See docs/macos-build.md.')
    for section in range(1, len(mission.sections) + 1):
        for suffix in ('cgz', 'cfg', 'mesh'):
            path = root / f'packages/maps/halo_{mission.cache}_{section:02}.{suffix}'
            if not path.is_file():
                raise SystemExit(f'Missing campaign asset: {path}')
PYTHON
fi
"$root/scripts/build-macos.sh" "$@"

app="$root/build/Halo in AssaultCube.app"
temp="$(mktemp -d "$root/build/.app.XXXXXX")"
trap 'rm -rf -- "$temp"' EXIT
staged="$temp/Halo in AssaultCube.app"
contents="$staged/Contents"
resources="$contents/Resources"
mkdir -p "$contents/MacOS" "$resources/mod"
cp "$root/build/macos/halo-ac" "$contents/MacOS/halo-ac"
cp -R "$root/build/runtime" "$resources/runtime"
cp -R "$root/packages" "$resources/mod/packages"
if [[ -d "$root/config" ]]; then
    cp -R "$root/config" "$resources/mod/config"
fi
cat > "$contents/MacOS/Halo in AssaultCube" <<'SH'
#!/bin/bash
set -euo pipefail
executables="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
resources="$(cd "$executables/../Resources" && pwd -P)"
profile="${HALO_AC_PROFILE:-$HOME/Library/Application Support/Halo in AssaultCube}"
mkdir -p -- "$profile"
profile="$(cd -- "$profile" && pwd -P)"
cd -- "$resources/runtime"
exec "$executables/halo-ac" "--home=$profile" --init --halo "--mod=$resources/mod" -ehalo_campaign "$@"
SH
chmod +x "$contents/MacOS/Halo in AssaultCube"
cat > "$contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDisplayName</key>
    <string>Halo in AssaultCube</string>
    <key>CFBundleName</key>
    <string>Halo in AssaultCube</string>
    <key>CFBundleIdentifier</key>
    <string>local.haloinassaultcube</string>
    <key>CFBundleExecutable</key>
    <string>Halo in AssaultCube</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
plutil -lint "$contents/Info.plist"
codesign --force --sign - "$staged"
codesign --verify --deep --strict "$staged"
if [[ -e "$app" ]]; then
    mv -- "$app" "$temp/previous.app"
fi
if ! mv -- "$staged" "$app"; then
    if [[ -e "$temp/previous.app" ]]; then
        mv -- "$temp/previous.app" "$app"
    fi
    exit 1
fi
printf '%s\n' "$app"
