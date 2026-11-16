"""Content currency: the map and scene-texture checks compare content, not compressed bytes.

    python3 tests/test_content_currency.py

No game, no caches. The gzip and PNG bytes the importers write come from the running Python's zlib
(Python 3.13's and 3.15's deflate streams differ for the same map), so the currency checks compare
what the engine reads:

  * `cgz_disk_map` (tools/import_halo_campaign.py, what `cgz_current` compares): a .cgz gzipped at
    another compression level, or with another header mtime, gives back the same map; a truncated
    file, a flipped byte in its deflate stream or CRC, a non-gzip file and a missing file give None
    (stale).
  * `png_content` (tools/halo_map_art.py, what `stale_scene_meshes` compares textures by): the same
    image recompressed at another level, its IDAT split in two, gives equal content; a truncated PNG,
    a bad chunk CRC, a missing IEND and a damaged deflate stream give None.
  * `write_png` leaves a file with the same content untouched (its mtime and its bytes, even when
    another zlib wrote them) and rewrites one whose pixels or size changed.
  * The metrics on disk (when there are any): every `packages/maps/*.metrics.json` has `cgz_raw_sha1`
    beside `cgz_sha1`, and where `cgz_current` is true the gunzipped .cgz on disk hashes to it.

Written while testing was on hold: not yet run.
"""
import gzip
import hashlib
import json
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / 'tools'))
from halo_map_art import PNG_SIGNATURE, png_content, write_png  # noqa: E402
from import_halo_campaign import cgz_disk_map  # noqa: E402

problems = []


def check(ok, message):
    if not ok:
        problems.append(message)


def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))


with tempfile.TemporaryDirectory(prefix='halo-currency-') as scratch:
    scratch = Path(scratch)

    # Maps: equal content whatever the compression, damaged files stale.
    raw = bytes(range(256)) * 400 + b'halo' * 1000
    cgz = scratch / 'map.cgz'
    for level, mtime in ((9, 0), (1, 0), (6, 12345)):
        cgz.write_bytes(gzip.compress(raw, compresslevel=level, mtime=mtime))
        check(cgz_disk_map(cgz) == raw, f'cgz_disk_map: level {level}, mtime {mtime} did not give the map back')
    good = gzip.compress(raw, mtime=0)
    damaged = bytearray(good)
    damaged[len(damaged) // 2] ^= 0x55
    crc = bytearray(good)
    crc[-6] ^= 0x01
    for label, data in (('truncated', good[:-20]), ('damaged stream', bytes(damaged)), ('bad CRC', bytes(crc)),
                        ('not gzip', b'not a map at all'), ('empty', b'')):
        cgz.write_bytes(data)
        check(cgz_disk_map(cgz) is None, f'cgz_disk_map: a {label} file is not stale')
    check(cgz_disk_map(scratch / 'missing.cgz') is None, 'cgz_disk_map: a missing file is not stale')

    # Textures: content, not bytes.
    width, height = 48, 20
    pixels = bytearray(os.urandom(width * height * 4))
    png = scratch / 'skin.png'
    write_png(png, width, height, pixels)
    written = png.read_bytes()
    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    scanlines = b''.join(b'\0' + pixels[i:i + width * 4] for i in range(0, len(pixels), width * 4))
    check(png_content(written) == (header, scanlines), 'png_content: write_png output does not decode to its pixels')
    stream = zlib.compress(scanlines, 1)
    other = (PNG_SIGNATURE + chunk(b'IHDR', header) + chunk(b'IDAT', stream[:len(stream) // 3])
             + chunk(b'IDAT', stream[len(stream) // 3:]) + chunk(b'IEND', b''))
    check(other != written and png_content(other) == png_content(written),
          'png_content: the same image at another level with a split IDAT differs')
    bad_crc = bytearray(written)
    bad_crc[30] ^= 0x01  # inside IHDR's CRC or data
    bad_stream = PNG_SIGNATURE + chunk(b'IHDR', header) + chunk(b'IDAT', stream[:-8]) + chunk(b'IEND', b'')
    no_end = PNG_SIGNATURE + chunk(b'IHDR', header) + chunk(b'IDAT', stream)
    for label, data in (('truncated', written[:-7]), ('bad CRC', bytes(bad_crc)), ('cut stream', bad_stream),
                        ('no IEND', no_end), ('not a PNG', b'GIF89a')):
        check(png_content(data) is None, f'png_content: a {label} PNG decodes')

    # write_png skips unchanged content, rewrites changed content.
    os.utime(png, ns=(1, 1))
    write_png(png, width, height, pixels)
    check(png.stat().st_mtime_ns == 1 and png.read_bytes() == written, 'write_png rewrote an identical PNG')
    png.write_bytes(other)
    os.utime(png, ns=(1, 1))
    write_png(png, width, height, pixels)
    check(png.stat().st_mtime_ns == 1 and png.read_bytes() == other,
          'write_png rewrote a PNG with the same content from another zlib')
    pixels[5] ^= 0x80
    write_png(png, width, height, pixels)
    check(png_content(png.read_bytes()) == (header, b''.join(b'\0' + pixels[i:i + width * 4]
                                                             for i in range(0, len(pixels), width * 4))),
          'write_png did not rewrite a PNG whose pixels changed')
    write_png(png, width // 2, height * 2, pixels)
    check(png_content(png.read_bytes())[0] == struct.pack('>IIBBBBB', width // 2, height * 2, 8, 6, 0, 0, 0),
          'write_png did not rewrite a PNG whose size changed')
    # A damaged file with the same pixels (bad_crc is `written`, the original pixels, with a flipped
    # IHDR CRC byte) is rewritten, not skipped as current.
    pixels[5] ^= 0x80
    png.write_bytes(bytes(bad_crc))
    write_png(png, width, height, pixels)
    check(png_content(png.read_bytes()) == (header, scanlines), 'write_png kept a damaged PNG')

# The metrics on disk.
maps = root / 'packages/maps'
metrics = sorted(maps.glob('*.metrics.json')) if maps.is_dir() else []
for path in metrics:
    m = json.loads(path.read_text())
    name = path.name[:-len('.metrics.json')]
    if 'cgz_raw_sha1' not in m:
        problems.append(f'{name}: no cgz_raw_sha1 (re-run tools/import_halo_campaign.py "$maps" --metrics-only)')
        continue
    check('cgz_sha1' in m, f'{name}: no cgz_sha1')
    if m.get('cgz_current'):
        disk = cgz_disk_map(maps / f'{name}.cgz')
        check(disk is not None and hashlib.sha1(disk).hexdigest() == m['cgz_raw_sha1'],
              f'{name}: cgz_current, but the gunzipped .cgz does not hash to cgz_raw_sha1')

for problem in problems:
    print(problem)
if problems:
    raise SystemExit(f'{len(problems)} content-currency problems')
print(f'Map and texture currency compare content; {len(metrics)} metrics files carry cgz_raw_sha1.')
