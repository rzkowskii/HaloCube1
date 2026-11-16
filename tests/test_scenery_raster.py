"""The cube-raster stamp: baked scenery in the native cube raster (scenery_raster in tools/halo_collision.py).

    python3 tests/test_scenery_raster.py

Every section's .cgz holds AssaultCube's cube raster, which the engine's flood waypoints, AssaultCube's
sight checks on stock geometry and the stock fallback read (on retail collision the player, actors,
shots and sight lines use the .hcol). Since the cube-raster stamp it holds the scenery the .hcol bakes
(SCN1): tall scenery makes its cubes solid (with vdelta RASTER_SCENERY_VDELTA, which the engine's light
passes), a low walkable top raises the floor onto it. Without the game, for every section it checks that
  * the metrics' `raster` record is there and its `scenery_solid` is the number of marked solid cubes
    in the .cgz; a section with no SCN1 has none (and nothing raised or kept);
  * no route row stands in a solid cube except rows with no native floor at their cube (a lift's deck,
    a flight over a drop, an exit fall's last step, a vehicle leg off the raster's floor), never in a
    scenery cube, and the count by reason matches the metrics' `route_rows_solid`;
  * no actor, weapon, equipment, start, route marker, light, item or mapmodel entity is in a solid cube
    (a vehicle may be, where its retail point's cube has no floor, and in a driven or flown section a
    start, marker or light on a vehicle route cell with none), and the counts match `entities_solid`;
  * independently of the importer's code, the stamp covers the scenery: every scenery polygon of the
    .hcol's SCN1 that is not a walkable top (normal z under 0.65), sampled at its centroid and at its
    vertices pulled a fifth of the way to it, misses if its cube is open space and the sample is 2 to
    5.2 units over the cube's floor (the body's column, less the floor's rounding to whole units), unless
    a route row or an entity stands in the cube (the cubes the stamp keeps); at most 0.5% of the
    samples of a section may miss, and the misses are listed;
  * the .hcol and the route are what the importer writes (the metrics' `route_current`; the stamp does
    not touch them).
Then, in the game (qa_raster_waypoints, tests/native/retail_qa.inc), for a sample of sections with a lot
of scenery played on retail collision, once AssaultCube has flooded the map with waypoints:
  * the engine read the marked cubes (`marked` equals the metrics' `scenery_solid`) and lets light through
    them (`lightpass=1`);
  * no waypoint is in a solid cube, and at most 1% of them (or 2) have scenery in the body's column at
    their cube's centre (a scenery top or underside 2 to 5.2 units over the cube's floor).
Written while testing was on hold: not yet run.
"""
import gzip
import json
import re
import struct
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import (CRASH_SITE, CRASH_SITE_NAME, EQUIPMENT_PLACEMENT, MISSIONS, RASTER_SCENERY_VDELTA,  # noqa: E402
                                  VEHICLE_PLACEMENT, WEAPON_PLACEMENT)

maps = root / 'packages/maps'
PLAYERSTART, LIGHT, MAPMODEL = 2, 1, 10
LABELS = {0: 'actor', 1: 'start', 100: 'marker', WEAPON_PLACEMENT: 'weapon', VEHICLE_PLACEMENT: 'vehicle',
          EQUIPMENT_PLACEMENT: 'equipment'}
# Sections played in the game: the Crash Site's trees and boulders, b30 section 1's beach rocks and palms,
# a30 section 3's (halo_a30_02's) pines, a50 section 3's hangar crates, c10 section 3's channel crates, d20 section 2's
# rocks and shards, d40 section 3's fallen computer banks. (game mission, section)
GAME_SECTIONS = {'halo_a30': (2, 1), 'halo_b30_01': (4, 1), 'halo_a30_02': (2, 3), 'halo_a50_03': (3, 3),
                 'halo_c10_03': (6, 3), 'halo_d20_02': (9, 2), 'halo_d40_03': (10, 3)}


def decode(path):
    """The .cgz's entities and cubes: [(x, y, z, attr1, type, attr2, ...)], [(type, floor or wtex, ceil or
    vdelta)] for size * size cubes, row by row."""
    raw = gzip.decompress(path.read_bytes())
    _, _, headersize, factor, count = struct.unpack_from('<4s4i', raw)
    offset, entities = headersize, []
    for _ in range(count):
        entities.append(struct.unpack_from('<4h4BhbB', raw, offset))
        offset += 16
    size = 1 << factor
    cubes, previous = [], None
    while len(cubes) < size * size:
        kind = raw[offset]
        if kind == 255:
            cubes.extend([previous] * raw[offset + 1])
            offset += 2
            continue
        if kind == 0:
            previous = (0, raw[offset + 1], raw[offset + 2])
            offset += 3
        else:
            previous = (kind, struct.unpack_from('b', raw, offset + 1)[0], struct.unpack_from('b', raw, offset + 2)[0])
            offset += 9
        cubes.append(previous)
    return size, entities, cubes


def scenery_polygons(path):
    """The .hcol's scenery polygons (its SCN1 ranges): [(normal, points)] in campaign coordinates."""
    data = path.read_bytes()
    _, count = struct.unpack_from('<4sI', data, 0)
    offset, surfaces = 8, []
    for _ in range(count):
        nx, ny, nz, _, _, _, points = struct.unpack_from('<3fBBhI', data, offset)
        offset += 20
        surfaces.append(((nx, ny, nz), [struct.unpack_from('<3f', data, offset + 12 * i) for i in range(points)]))
        offset += 12 * points
    found = []
    while offset < len(data):
        tag, entries = struct.unpack_from('<4sI', data, offset)
        width = 3 if tag == b'SCN1' else 1
        values = struct.unpack_from(f'<{entries * width}I', data, offset + 8)
        if tag == b'SCN1':
            for i in range(entries):
                _, first, number = values[3*i:3*i + 3]
                found.extend(surfaces[first:first + number])
        offset += 8 + 4 * entries * width
    return found


names = ([CRASH_SITE_NAME] if CRASH_SITE else []) + \
    [f'halo_{m.cache}_{part + 1:02}' for m in MISSIONS for part in range(len(m.sections))]
problems, metrics_of = [], {}
for name in names:
    cgz, hcol, metrics_path = maps / f'{name}.cgz', maps / f'{name}.hcol', maps / f'{name}.metrics.json'
    if not cgz.is_file() or not metrics_path.is_file():
        problems.append(f'{name}: no map or metrics (import the campaign, docs/macos-build.md)')
        continue
    metrics = json.loads(metrics_path.read_text())
    metrics_of[name] = metrics
    raster = metrics.get('raster')
    if raster is None:
        problems.append(f'{name}: no `raster` in the metrics (re-run tools/import_halo_campaign.py --metrics-only)')
        continue
    if not metrics.get('cgz_current') or not metrics.get('route_current'):
        problems.append(f'{name}: the map or route on disk is not what the importer writes (re-import the section)')
    size, entities, cubes = decode(cgz)

    def solid(x, y):
        return not (0 <= x < size and 0 <= y < size) or cubes[y * size + x][0] == 0

    marked = {(i % size, i // size) for i, c in enumerate(cubes) if c[0] == 0 and c[2] == RASTER_SCENERY_VDELTA}
    if len(marked) != raster['scenery_solid']:
        problems.append(f'{name}: {len(marked)} scenery cubes in the .cgz, the metrics say {raster["scenery_solid"]}')
    polygons = scenery_polygons(hcol) if hcol.is_file() else []
    if not polygons and (marked or raster['scenery_raised'] or raster['scenery_kept']):
        problems.append(f'{name}: no scenery in the .hcol, but the raster has {len(marked)} scenery cubes, '
                        f'{raster["scenery_raised"]} raised and {raster["scenery_kept"]} kept')
    # Route rows: in a solid cube only where their cube has no native floor; never in a scenery cube.
    occupied, rows_solid = set(), {}
    route = maps / f'{name}.route'
    for number, line in enumerate(route.read_text().splitlines() if route.is_file() else (), 1):
        x, y = (int(v) for v in line.split()[:2])
        occupied.add((x, y))
        if (x, y) in marked:
            problems.append(f'{name}.route:{number}: route row in a scenery cube ({x},{y})')
        if solid(x, y):
            rows_solid['scenery' if (x, y) in marked else 'other'] = rows_solid.get('scenery' if (x, y) in marked else 'other', 0) + 1
    expected = raster['route_rows_solid']
    if set(expected) - {'no floor'} or sum(rows_solid.values()) != sum(expected.values()):
        problems.append(f'{name}: route rows in solid cubes {rows_solid}, the metrics say {expected} (only rows with no floor may be)')
    # Entities: none in a solid cube, but a vehicle at a retail point with no floor under it.
    entities_solid = {}
    for e in entities:
        occupied.add((e[0], e[1]))
        if solid(e[0], e[1]):
            label = LABELS.get(e[5], 'start') if e[4] == PLAYERSTART else {LIGHT: 'light', MAPMODEL: 'mapmodel'}.get(e[4], 'item')
            entities_solid[label] = entities_solid.get(label, 0) + 1
    # A driven or flown section's starts, markers and lights stand on its vehicle route's cells, which
    # may have no native floor (flights, legs off the raster's floor), as its route rows may.
    allowed = {'vehicle'} | ({'start', 'marker', 'light'} if metrics.get('mode') in ('driven', 'flown') else set())
    if set(entities_solid) - allowed or entities_solid != raster['entities_solid']:
        problems.append(f'{name}: entities in solid cubes {entities_solid} (metrics {raster["entities_solid"]}; '
                        f'only {", ".join(sorted(allowed))} may be)')
    # Independently of scenery_raster: the scenery's sides and undersides are inside solid cubes.
    samples, misses = 0, []
    for (nx, ny, nz), points in polygons:
        if nz >= .65 or not points:
            continue
        cx, cy, cz = (sum(p[i] for p in points) / len(points) for i in range(3))
        for px, py, pz in [(cx, cy, cz)] + [(p[0] + (cx - p[0]) / 5, p[1] + (cy - p[1]) / 5, p[2] + (cz - p[2]) / 5) for p in points]:
            x, y = int(px // 1), int(py // 1)
            samples += 1
            if solid(x, y) or (x, y) in occupied:
                continue
            level = cubes[y * size + x][1]
            if level + 2.0 < pz < level + 5.2:
                misses.append(f'{px:.1f},{py:.1f},{pz:.1f} over cube floor {level}')
    if samples and len(misses) > samples * .005:
        problems.append(f'{name}: {len(misses)} of {samples} scenery wall samples in open cubes in the body\'s column: '
                        f'{"; ".join(misses[:8])}')
    elif misses:
        print(f'{name}: {len(misses)} of {samples} scenery wall samples in open cubes (within the 0.5%): {"; ".join(misses[:4])}')
    print(f'{name}: {len(marked)} scenery cubes, {raster["scenery_raised"]} raised, {raster["scenery_kept"]} kept '
          f'({raster.get("scenery_capped", 0)} capped); {sum(rows_solid.values())} route rows and '
          f'{sum(entities_solid.values())} vehicles in solid cubes; {samples} wall samples, {len(misses)} in open cubes.')

if problems:
    raise SystemExit('cube-raster stamp (static): ' + '; '.join(problems))

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
for name, (mission, section) in GAME_SECTIONS.items():
    metrics = metrics_of.get(name)
    if metrics is None or not metrics['raster']['scenery_solid']:
        continue
    script = (f'halo_difficulty 2; qa_campaign_play {mission} {section}; '
              'qa_after_gameplay [sleep 6000 [qa_raster_waypoints; quit] 1]')
    profile = scratch(f'scenery-raster-{name}-')
    logfile = SCRATCH / f'scenery-raster-{name}.log'
    with logfile.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480', '-e' + script],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=300, check=True)
    log = logfile.read_text(errors='replace')
    found = re.search(rf'RETAIL_QA raster map=\S*{name} collision=(\d) waypoints=(\d+) solid=(\d+) scenery=(\d+) '
                      r'marked=(\d+) lightpass=(\d)', log)
    if not found:
        problems.append(f'{name}: no qa_raster_waypoints line ({logfile})')
        continue
    collision, waypoints, in_solid, in_scenery, marked, lightpass = map(int, found.groups())
    if collision != 1 or marked != metrics['raster']['scenery_solid'] or lightpass != 1:
        problems.append(f'{name}: collision={collision}, {marked} marked cubes loaded (metrics '
                        f'{metrics["raster"]["scenery_solid"]}), lightpass={lightpass} ({logfile})')
    if waypoints == 0 or in_solid or in_scenery > max(2, waypoints // 100):
        listed = re.findall(r'RETAIL_QA raster waypoint at=(\S+) top=(\d) under=(\d)', log)
        problems.append(f'{name}: {waypoints} flood waypoints, {in_solid} in solid cubes, {in_scenery} with scenery in the '
                        f'body\'s column (at most {max(2, waypoints // 100)}): {listed[:6]} ({logfile})')
        continue
    print(f'{name}: {waypoints} flood waypoints, none in a solid cube, {in_scenery} beside scenery in the column; '
          f'{marked} scenery cubes loaded, light passes them.')

if problems:
    raise SystemExit('cube-raster stamp: ' + '; '.join(problems))
print('The cube raster holds the baked scenery; route rows, entities and flood waypoints stand clear of it.')
