"""Scenery collision on retail collision (SCENERY_COLLISION in tools/import_halo_campaign.py; the .hcol's SCN1 trailer).

    python3 tests/test_retail_scenery.py

For each section SCENERY_COLLISION lists that is on retail collision (every section on retail
collision since the batch after the c10 section 5 batch: the Crash Site and all 53 others, walked,
driven and flown, and d40 section 3 since the d40 section 3 batch put it on retail collision: all 55;
c40 section 1's SCN1 also lists the scenery of BSP 12, EXTRA_BSPS, which the d40 section 3 batch
scans too: one door blinker with no collision model, so none), it checks, without the game, that
  * the section's .hcol ends in an SCN1 trailer (after DEV1, before AST1) whose placements match the
    importer's metrics (`scenery` in packages/maps/<section>.metrics.json), whose surface ranges ascend
    without overlapping and fall inside the file's surfaces;
  * a section not in the table has no SCN1 trailer (its .hcol is what the importer wrote before);
  * the Crash Site's mapmodel angles (import_halo_campaign.mapmodel_angles) draw a placement as Halo
    turns it: over a sweep of yaw, pitch and roll, the rotation AssaultCube builds from the entity's
    attr1, attr5 and attr6 (md3.h's quaternion path, re-implemented here) is within 0.6 degrees of
    halo_collision.euler_rows seen in the campaign frame (y reflected), and attr6 fits its signed byte;
and then starts each section with scenery placements (a section whose window holds none, b40
sections 4 and 6, c40 section 5 and d40 section 6, has no SCN1 and is only checked statically) as
campaign play and checks that
  * the engine loaded the .hcol with that many scenery objects ("Halo: loaded N retail collision
    surfaces for <map> (...), S scenery objects (K surfaces)");
  * for at least 90% of the placements (qa_retail_scenery in tests/native/retail_qa.inc), a ray down
    onto the top of the placement's bounds stops on a scenery surface (shots and sight lines hit
    scenery, as in Halo), so does a grenade's sweep and a body's capsule. A thin placement (a tree's
    trunk below its crown's bounds, a tilted plank) may miss the probe at its centre; every miss is
    listed.
The routes over and round the scenery are walked by tests/test_retail_collision.py (every walk
re-planned with scenery is pending). Written while testing was on hold: not yet run.
"""
import json
import math
import random
import re
import struct
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import (CRASH_SITE, CRASH_SITE_KEY, CRASH_SITE_NAME, MISSIONS, SCENERY_COLLISION,  # noqa: E402
                                  STOCK_SECTIONS, VALIDATED_COLLISION, mapmodel_angles)
from halo_collision import euler_rows  # noqa: E402

maps = root / 'packages/maps'


def trailers(path):
    """The .hcol's trailers in file order: [(tag, entries, raw uints)], and its surface count."""
    data = path.read_bytes()
    signature, count = struct.unpack_from('<4sI', data, 0)
    if signature != b'HCL1':
        raise ValueError(f'{path.name}: not an HCL1 file')
    offset = 8
    for _ in range(count):
        points, = struct.unpack_from('<I', data, offset + 16)
        offset += 20 + 12 * points
    found = []
    while offset < len(data):
        tag, entries = struct.unpack_from('<4sI', data, offset)
        width = 3 if tag == b'SCN1' else 1
        values = struct.unpack_from(f'<{entries * width}I', data, offset + 8)
        found.append((tag.decode(), entries, values))
        offset += 8 + 4 * entries * width
    if offset != len(data):
        raise ValueError(f'{path.name}: trailing bytes')
    return found, count


def name_of(key):
    return CRASH_SITE_NAME if key == CRASH_SITE_KEY else f'halo_{key[0]}_{key[1] + 1:02}'


def drawn_rotation(attr1, attr5, attr6):
    """The rotation AssaultCube draws a mapmodel with, from its entity's attr1 (yaw, tenths), attr5
    (pitch, tenths) and attr6 (roll, degrees): entities.cpp rendermapmodels, md3.h render's
    quat(-yaw - 180, pitch).roll(roll), glmatrixf::fromquat and glMultMatrixf (column-major), as rows."""
    yaw, pitch, roll = attr1 / 10, attr5 / 10, attr6
    a, b = math.radians(-yaw - 180) / 2, math.radians(pitch) / 2
    x, y, z, w = math.sin(b) * math.sin(a), math.sin(b) * math.cos(a), math.cos(b) * math.sin(a), math.cos(b) * math.cos(a)
    if roll:
        px, pw = math.sin(math.radians(roll) / 2), math.cos(math.radians(roll) / 2)
        x, y, z, w = pw * x + px * w, pw * y - px * z, pw * z + px * y, pw * w - px * x
    v = [1 - 2*(y*y + z*z), 2*(x*y - z*w), 2*(x*z + y*w), 0,
         2*(x*y + z*w), 1 - 2*(x*x + z*z), 2*(y*z - x*w), 0,
         2*(x*z - y*w), 2*(y*z + x*w), 1 - 2*(x*x + y*y)]
    return [[v[c * 4 + r] for c in range(3)] for r in range(3)]


problems, sections = [], []
# Static: the Crash Site's mapmodels turn as Halo turns their placements (mapmodel_angles).
generator, worst = random.Random(7), 0.0
for _ in range(5000):
    angles = (generator.uniform(-math.pi, math.pi), generator.uniform(-1.55, 1.55), generator.uniform(-math.pi, math.pi))
    attr1, attr5, attr6 = mapmodel_angles(*angles)
    if not (0 <= attr1 < 3600 and -1800 <= attr5 < 1800 and -128 <= attr6 <= 127):
        problems.append(f'mapmodel_angles{angles}: attributes {attr1}, {attr5}, {attr6} out of range')
        break
    halo = euler_rows(*angles)
    want = [[halo[i][j] * (-1 if (i == 1) != (j == 1) else 1) for j in range(3)] for i in range(3)]
    got = drawn_rotation(attr1, attr5, attr6)
    trace = sum(got[i][k] * want[i][k] for i in range(3) for k in range(3))
    worst = max(worst, math.degrees(math.acos(max(-1.0, min(1.0, (trace - 1) / 2)))))
if worst > 0.6:
    problems.append(f'mapmodel_angles: a mapmodel drawn {worst:.2f} degrees from Halo\'s rotation (at most 0.6)')
print(f'Crash Site mapmodel angles: at most {worst:.2f} degrees from Halo\'s rotation over 5000 placements.')
# Static: the trailer and the metrics agree; sections outside the table have none.
for hcol in sorted(maps.glob('*.hcol')):
    found, count = trailers(hcol)
    tags = [tag for tag, _, _ in found]
    keyed = [key for key in SCENERY_COLLISION if name_of(key) == hcol.stem]
    if 'SCN1' in tags and not keyed:
        problems.append(f'{hcol.name}: SCN1 trailer, but the section is not in SCENERY_COLLISION (re-import it)')
    if tags != sorted(tags, key=['DEV1', 'SCN1', 'AST1'].index):
        problems.append(f'{hcol.name}: trailers out of order: {tags}')
    if not keyed:
        continue
    metrics = json.loads((maps / f'{hcol.stem}.metrics.json').read_text())
    scn = next(((entries, values) for tag, entries, values in found if tag == 'SCN1'), (0, ()))
    if scn[0] != metrics.get('scenery'):
        problems.append(f'{hcol.name}: SCN1 lists {scn[0]} placements, the metrics {metrics.get("scenery")} (re-import it)')
    end = 0
    for i in range(scn[0]):
        placement, first, surfaces = scn[1][3*i:3*i + 3]
        if surfaces == 0 or first < end or first + surfaces > count:
            problems.append(f'{hcol.name}: SCN1 entry {i} (placement {placement}) covers {first}+{surfaces} of {count}')
            break
        end = first + surfaces
    sections.append((keyed[0], hcol.stem, scn[0]))
for key in sorted(SCENERY_COLLISION, key=str):
    cache = key[0]
    if key in STOCK_SECTIONS or cache not in VALIDATED_COLLISION or (key == CRASH_SITE_KEY and not CRASH_SITE):
        continue
    if not (maps / f'{name_of(key)}.hcol').is_file():
        problems.append(f'{name_of(key)}: no .hcol (run tools/import_halo_campaign.py)')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
checked = 0
for key, name, expected in sections:
    if not expected:
        # No placement in the section's window (no SCN1, so the load line has no scenery part).
        print(f'{name}: no scenery placements (checked statically).')
        continue
    cache = key[0]
    mission = next(i for i, m in enumerate(MISSIONS) if m.cache == cache) + 1
    # The engine's a30 has the Crash Site as its first section.
    section = 1 if key == CRASH_SITE_KEY else key[1] + 1 + (cache == 'a30')
    script = (f'halo_difficulty 2; qa_campaign_play {mission} {section}; '
              'qa_after_gameplay [sleep 500 [qa_retail_scenery; quit] 1]')
    profile = scratch(f'retail-scenery-{name}-')
    logfile = SCRATCH / f'retail-scenery-{name}.log'
    with logfile.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480', '-e' + script],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=240, check=True)
    log = logfile.read_text(errors='replace')
    loaded = re.search(rf'loaded \d+ retail collision surfaces for {name} \([^)]*\), (\d+) scenery objects \((\d+) surfaces\)', log)
    if not loaded or int(loaded.group(1)) != expected:
        problems.append(f'{name}: loaded {loaded.group(1) if loaded else "no"} scenery objects, the .hcol lists {expected} ({logfile})')
        continue
    found = re.findall(r'RETAIL_QA scenery (\d+) placement=(\d+) low=\S+ high=\S+ ray_hits=(\d) bounce_hits=(\d) body_hits=(\d)', log)
    if len(found) != expected:
        problems.append(f'{name}: {len(found)} scenery placements reported, expected {expected} ({logfile})')
        continue
    misses = [f'{placement} (ray {ray}, grenade {bounce}, body {body})' for _, placement, ray, bounce, body in found
              if (ray, bounce, body) != ('1', '1', '1')]
    if len(misses) > expected // 10:
        problems.append(f'{name}: {len(misses)} of {expected} scenery placements not hit from above: {", ".join(misses)} ({logfile})')
    elif misses:
        print(f'{name}: thin placements the centre probe missed: {", ".join(misses)}')
    checked += 1
    print(f'{name}: {expected} scenery placements load and stop rays, grenades and bodies.')

if problems:
    raise SystemExit('scenery collision: ' + '; '.join(problems))
print(f'{checked} sections with scenery collision checked.')
