#!/bin/bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
# --partial skips the full 55-section asset check (development builds only);
# everything else goes to cmake. HALO_PACKAGES_DIR overrides the asset directory.
partial=0
args=()
for arg in "$@"; do
    if [[ "$arg" == "--partial" ]]; then partial=1; else args+=("$arg"); fi
done
packages="${HALO_PACKAGES_DIR:-$root/packages}"
"$root/scripts/check-no-retail.sh" "$root"
PYTHONPATH="$root/tools" python3 - "$packages" "$partial" <<'PYTHON'
from pathlib import Path
import sys

from import_halo_campaign import MISSIONS, STOCK_SECTIONS, VALIDATED_COLLISION

packages = Path(sys.argv[1])
partial = sys.argv[2] == '1'
missing = []
for asset in ('maps/halo_a30.cgz', 'maps/halo_a30.cfg', 'maps/halo_a30.mesh',
              'textures/halo_a30/cliff.png', 'models/halo/ma5b/tris.md3', 'audio/halo/ma5b-fire.wav'):
    if not (packages / asset).is_file():
        missing.append(asset)
if not partial:
    for table in ('weapons.txt', 'equipment.txt', 'actors.txt', 'vehicles.txt'):
        if not (packages / 'halo' / table).is_file():
            missing.append(f'halo/{table}')
    for mission in MISSIONS:
        if not (packages / f'halo/{mission.cache}.hscb').is_file():
            missing.append(f'halo/{mission.cache}.hscb')
        for section in range(1, len(mission.sections) + 1):
            # Validated missions play on retail collision (except STOCK_SECTIONS); a missing .hcol
            # would silently fall back to stock geometry.
            retail = mission.cache in VALIDATED_COLLISION and (mission.cache, section - 1) not in STOCK_SECTIONS
            for suffix in ('cgz', 'cfg', 'mesh') + (('hcol',) if retail else ()):
                name = f'maps/halo_{mission.cache}_{section:02}.{suffix}'
                if not (packages / name).is_file():
                    missing.append(name)
if missing:
    shown = ', '.join(missing[:8]) + (f' and {len(missing) - 8} more' if len(missing) > 8 else '')
    raise SystemExit(f'Incomplete asset import in {packages}: missing {shown}.\n'
                     'Run scripts/import-macos-assets.sh (docs/macos-build.md), or pass --partial for a development package.')
PYTHON
"$root/scripts/build-macos.sh" ${args[@]+"${args[@]}"}

app="$root/build/Halo in AssaultCube.app"
temp="$(mktemp -d "$root/build/.app.XXXXXX")"
trap 'rm -rf -- "$temp"' EXIT
staged="$temp/Halo in AssaultCube.app"
contents="$staged/Contents"
resources="$contents/Resources"
mkdir -p "$contents/MacOS" "$resources/mod"
cp "$root/build/macos/halo-ac" "$contents/MacOS/halo-ac"
cp -R "$root/build/runtime" "$resources/runtime"
# Make the app relocatable: ship the Homebrew libraries inside the bundle.
python3 "$root/scripts/bundle-dylibs.py" "$contents/MacOS/halo-ac" "$contents/Frameworks"
if otool -L "$contents/MacOS/halo-ac" "$contents"/Frameworks/*.dylib | grep -E '/opt/homebrew|/usr/local|/Cellar/'; then
    printf 'Packaged binary still links against Homebrew paths.\n' >&2
    exit 1
fi
mkdir -p "$resources/licenses"
cp "$root/NOTICE.md" "$resources/licenses/NOTICE.md"
cp "$root/source/README_CUBEENGINE.txt" "$root/source/enet/LICENSE" "$resources/licenses/"
cp -R "$packages" "$resources/mod/packages"
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
exec "$executables/halo-ac" "--home=$profile" --init --halo "--mod=$resources/mod" -ehalo_title "$@"
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
for lib in "$contents"/Frameworks/*.dylib "$contents/MacOS/halo-ac"; do
    codesign --force --sign - "$lib"
done
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
