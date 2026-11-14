#!/bin/bash
# Compile and link-check every target on Linux without a Mac.
#
# Fetches public SDL2, SDL_image, OpenAL, Ogg and Vorbis headers into a cache,
# configures a copy of CMakeLists.txt with the macOS frameworks stubbed, builds
# halo-engine, halo-ac and halo-ac-qa, and fails on any compile error or any
# unresolved symbol that is not provided by those external libraries.
# Needs git, curl, g++ and cmake (pip install --user cmake).
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cache="${HALO_CHECK_CACHE:-$HOME/.cache/halo-ac-check}"
cmake="$(command -v cmake || echo "$HOME/.local/bin/cmake")"
raw=https://raw.githubusercontent.com
inc="$cache/include"
mkdir -p "$inc/AL" "$inc/vorbis" "$inc/ogg"
[[ -d "$cache/sdl2" ]] || git clone -q --depth 1 -b release-2.30.x https://github.com/libsdl-org/SDL.git "$cache/sdl2"
[[ -f "$inc/SDL_image.h" ]] || curl -sfL "$raw/libsdl-org/SDL_image/SDL2/include/SDL_image.h" -o "$inc/SDL_image.h"
for f in al.h alc.h alext.h efx.h; do
    [[ -f "$inc/AL/$f" ]] || curl -sfL "$raw/kcat/openal-soft/master/include/AL/$f" -o "$inc/AL/$f"
done
for f in codec.h vorbisfile.h; do
    [[ -f "$inc/vorbis/$f" ]] || curl -sfL "$raw/xiph/vorbis/master/include/vorbis/$f" -o "$inc/vorbis/$f"
done
for f in ogg.h os_types.h; do
    [[ -f "$inc/ogg/$f" ]] || curl -sfL "$raw/xiph/ogg/master/include/ogg/$f" -o "$inc/ogg/$f"
done
cat > "$inc/ogg/config_types.h" <<'H'
#pragma once
#include <stdint.h>
typedef int16_t ogg_int16_t; typedef uint16_t ogg_uint16_t; typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t; typedef int64_t ogg_int64_t; typedef uint64_t ogg_uint64_t;
H

work="$cache/cmake"
rm -rf "$work"; mkdir -p "$work"
ln -s "$root/source" "$work/source"
ln -s "$root/tests" "$work/tests"
python3 - "$root/CMakeLists.txt" "$cache" > "$work/CMakeLists.txt" <<'PY'
import re, sys
s = open(sys.argv[1]).read(); cache = sys.argv[2]
s = re.sub(r'if\(NOT APPLE\).*?endif\(\)\n', '', s, flags=re.S)
s = re.sub(r'find_package\(.*?\)\n|find_library\(.*?\)\n|find_path\(.*?\)\n', '', s)
stub = f'''foreach(t SDL2::SDL2 SDL2_image::SDL2_image ZLIB::ZLIB)
  add_library(${{t}} INTERFACE IMPORTED)
endforeach()
target_include_directories(SDL2::SDL2 INTERFACE {cache}/sdl2/include {cache}/include)
set(VORBIS_INCLUDE_DIR {cache}/include)
set(OGG_INCLUDE_DIR {cache}/include)
set(OPENGL_FRAMEWORK z)
set(OPENAL_FRAMEWORK z)
set(VORBISFILE_LIBRARY z)
'''
print(s.replace('set(ENET_DIR', stub + 'set(ENET_DIR', 1))
PY
"$cmake" -S "$work" -B "$work/b" -DHALO_BUILD_QA=ON -DCMAKE_C_FLAGS="-DHAS_SOCKLEN_T -w" -DCMAKE_CXX_FLAGS=-w >/dev/null
log="$work/build.log"
"$cmake" --build "$work/b" -j1 -- -k >"$log" 2>&1 || true
if grep -qE ":[0-9]+:[0-9]+: (fatal )?error:" "$log"; then grep -E -A3 ":[0-9]+:[0-9]+: (fatal )?error:" "$log" | head -60; exit 1; fi
bad=$(grep -oE "undefined reference to \`[^']+'|multiple definition of \`[^']+'" "$log" |
      sed -E "s/^(undefined reference to|multiple definition of) .//; s/'$//" |
      grep -vE "^(SDL_|IMG_|gl[A-Z]|al[A-Z]|alc[A-Z]|ov_)" | sort -u || true)
if [[ -n "$bad" ]]; then printf 'Unresolved or duplicate symbols:\n%s\n' "$bad"; exit 1; fi
echo "Compile and link check passed: halo-engine, halo-ac, halo-ac-qa."
