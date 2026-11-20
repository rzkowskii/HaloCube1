# Notice

**Halo in AssaultCube** is an unofficial, non-commercial fan project. It is not
affiliated with, endorsed by, or sponsored by Microsoft, Xbox Game Studios,
343 Industries, Bungie, or the AssaultCube project. "Halo" and related names,
characters and artwork are trademarks and property of their owners.

## Bring your own assets

This repository contains no Halo game data. Maps, textures, models, audio,
scripts and tables are generated locally by the importers in `tools/` from a copy
of *Halo: Combat Evolved* (Xbox) that you own, and are git-ignored, the route
metrics written beside the maps included. Do not commit or redistribute the
generated `packages/` directory or an app built from it (`scripts/check-no-retail.sh`
guards the repository; `scripts/package-macos.sh` and `scripts/ci-macos.sh` run it).
An app built this way is for local use only. AssaultCube stock data is
read from the AssaultCube release you provide and is covered by its own license
(`build/runtime/docs/license.html`).

The screenshots in `docs/screenshots/` show the game running with locally imported
assets. They are included to illustrate the project; the artwork they show belongs
to its owners.

## Engine and libraries

- **AssaultCube 1.3.0.2** (Lockdown Edition) engine source, in `source/`.
  AssaultCube is based on the Cube engine and uses the same zlib-like license;
  it also contains code from pCube and Sauerbraten. See `source/README.txt` and
  `source/README_CUBEENGINE.txt` (copied into the app at `Resources/licenses/`).
- **Cube engine** (c) 2001-2004 Wouter van Oortmerssen, Lee Salzman, Mike Dysart,
  Robert Pointon, Quinton Reeves. Permission is granted to use, alter and
  redistribute the code, provided the origin is not misrepresented, altered
  versions are marked as such, and this notice is not removed.
- **ENet** (c) 2002-2012 Lee Salzman, MIT license; see `source/enet/LICENSE`.
- **zlib**, used for compression, under the zlib license.
- **SDL2 / sdl2-compat, SDL2_image, libvorbis, libogg** and their image-format
  dependencies (libpng, libjpeg-turbo, libtiff, libwebp, libavif, libjxl) are
  bundled in `Contents/Frameworks` of a packaged app under their own permissive
  licenses (zlib, BSD, libpng). Install them from Homebrew to read the full texts.
- The optional ASan/UBSan verification build (`scripts/build-macos.sh --asan`,
  `build/macos-asan`) links the sanitizer runtime from Apple's Xcode toolchain. It is
  a local test tool: `scripts/package-macos.sh` refuses to package a sanitized build.

## Scope

Single-player campaign only. Co-op, multiplayer and splitscreen are out of scope.
