"""Static checks of the retail-collision route files for lifts and light bridges (no game run).

    python3 tests/test_traversal_routes.py

Reads packages/maps/<section>.route as tools/import_halo_campaign.py writes it and checks that
  * every row parses as `x y z [flags [device@position]]`, and rows with flag 16 (a lift deck)
    or 128 (a light bridge) name their device,
  * each section in LIFT_RIDES that has an .hcol boards its lift on exactly one deck row, with
    the lift's name: the last row when the section ends on the ride (its BSP switch is half way
    along the shaft), or the row just before the first flag-4 row when the route rides on,
  * the deck row is within reach of the row before it (the landing the walker waits on), and
    a section's light-bridge rows come in runs whose first row follows a floor row,
  * the sections in STOCK_SECTIONS have neither an .hcol nor a .route, and the DRIVEN_SECTIONS
    (d40's Warthog run) have an .hcol and no .route,
  * action rows (written while testing was on hold, not yet run): a row with flag 256 (use a
    control) names the machine and the position it waits for, 512 (shoot a damage target) the
    target, the health it shoots it down to and an aim point (eight columns), 1024 (wait at a
    scripted door) the door; each repeats the position of the step row before it. d40 section 5
    uses its four manifold controls (enc5_1_pistons1, _pistonn1, _pistonn3, _pistons3, in that
    order) and shoots the detectors of s1, n1 and n3 (no walkable spot sees s3's); a10 section 5
    waits at tunnel_door_2 and section 6 at cryo_door_2 on rows of their own (DOOR_WAITS),
  * no route row stands inside a vehicle parked when the section starts (the section map's
    PLAYERSTART team 102 placements that no script creates, with their box from
    packages/halo/vehicles.txt as halo_vehicle.cpp sizes it): the importer's planner goes round
    them (VehicleBlocks). Written while testing was on hold: this check has not been run.
Needs the imported campaign maps, not the game.
"""
import gzip
import math
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import DRIVEN_SECTIONS, LIFT_RIDES, MISSIONS, STOCK_SECTIONS  # noqa: E402

maps = root / 'packages/maps'
require(maps, 'run scripts/import-macos-assets.sh')
problems, checked = [], 0


def rows(path):
    result = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        fields = line.split()
        if not fields:
            continue
        if not 3 <= len(fields) <= 8 or len(fields) in (6, 7):
            raise ValueError(f'{path.name}:{number}: {line!r}')
        x, y, z = map(float, fields[:3])
        flags = int(fields[3]) if len(fields) > 3 else 0
        device, value = None, None
        if len(fields) > 4:
            device, _, value = fields[4].rpartition('@')
            value = float(value)
        if flags & (16 | 128 | 256 | 512 | 1024) and not device:
            raise ValueError(f'{path.name}:{number}: flag {flags} without a device')
        if bool(flags & 512) != (len(fields) == 8):
            raise ValueError(f'{path.name}:{number}: a shoot row (512) has an aim point, and only it: {line!r}')
        if flags & (256 | 512 | 1024) and (not result or result[-1][0] != (x, y, z)):
            raise ValueError(f'{path.name}:{number}: action row {flags} not at the step row before it')
        result.append(((x, y, z), flags, device, value))
    return result


for mission in MISSIONS:
    for part in range(len(mission.sections)):
        name = f'halo_{mission.cache}_{part + 1:02}'
        hcol, route = maps / f'{name}.hcol', maps / f'{name}.route'
        if (mission.cache, part) in STOCK_SECTIONS:
            if hcol.exists() or route.exists():
                problems.append(f'{name}: in STOCK_SECTIONS but has a {"hcol" if hcol.exists() else "route"}')
            continue
        if (mission.cache, part) in DRIVEN_SECTIONS:
            # The Warthog run: an .hcol to drive on, no walking route.
            if route.exists():
                problems.append(f'{name}: in DRIVEN_SECTIONS but has a route')
            if not hcol.exists():
                problems.append(f'{name}: in DRIVEN_SECTIONS but has no hcol (re-run import_halo_campaign.py --mission {mission.cache})')
            continue
        if not route.exists():
            continue
        try:
            table = rows(route)
        except ValueError as error:
            problems.append(str(error))
            continue
        checked += 1
        decks = [i for i, (_, flags, _, _) in enumerate(table) if flags & 16]
        lift = LIFT_RIDES.get((mission.cache, part))
        if lift is None:
            if decks:
                problems.append(f'{name}: deck rows {decks} but no LIFT_RIDES entry')
        elif hcol.exists():
            if len(decks) != 1:
                problems.append(f'{name}: {len(decks)} deck rows for {lift}, expected 1')
            else:
                deck = decks[0]
                _, flags, device, value = table[deck]
                rides = [i for i, (_, f, _, _) in enumerate(table) if f & 4]
                expected = rides[0] - 1 if rides else len(table) - 1
                if device != lift:
                    problems.append(f'{name}: deck row names {device}, expected {lift}')
                if value is None or not 0 <= value <= 1:
                    problems.append(f'{name}: deck row position {value}')
                if deck != expected:
                    problems.append(f'{name}: deck row {deck}, expected {expected} ({"before the ride" if rides else "last"})')
                if deck:
                    # The walker boards from the landing row; the c20 platforms are 20 units across.
                    gap = math.dist(table[deck][0][:2], table[deck - 1][0][:2])
                    if gap > 120:
                        problems.append(f'{name}: deck row {gap:.0f} units from the landing row')
        for i, (_, flags, device, _) in enumerate(table):
            if flags & 128 and (not i or not table[i - 1][1] & 128) and (not i or table[i - 1][1] & 16):
                problems.append(f'{name}: light bridge {device} starts at row {i} after a deck row')
        uses = [device for _, flags, device, _ in table if flags & 256]
        shots = [device for _, flags, device, _ in table if flags & 512]
        waits = [device for _, flags, device, _ in table if flags & 1024]
        expected_actions = {
            'halo_d40_05': (['enc5_1_pistons1', 'enc5_1_pistonn1', 'enc5_1_pistonn3', 'enc5_1_pistons3'],
                            ['enc5_1_detector_s1', 'enc5_1_detector_n1', 'enc5_1_detector_n3'], []),
            'halo_a10_05': ([], [], ['tunnel_door_2']),
            'halo_a10_06': ([], [], ['cryo_door_2']),
        }.get(name, ([], [], []))
        if uses != expected_actions[0]:
            problems.append(f'{name}: use rows {uses}, expected {expected_actions[0]}')
        # Either detector of a manifold destroys it (enc5_1_destroyed_poll): s1 or s1b, and so on.
        if [shot[:-1] if shot.endswith('b') else shot for shot in shots] != expected_actions[1]:
            problems.append(f'{name}: shoot rows {shots}, expected {expected_actions[1]} (or their b detectors)')
        if [wait for wait in waits if wait in expected_actions[2]] != expected_actions[2]:
            problems.append(f'{name}: door rows {waits}, expected {expected_actions[2]} among them')



# Parked vehicles: the engine's box (halo_vehicle.cpp dims) at the placement, widened by the
# player's radius, from the placement's height (it settles onto the floor below) to its top.
def vehicle_bounds():
    bounds, index = {}, None
    table_path = root / 'packages/halo/vehicles.txt'
    if not table_path.is_file():
        return bounds
    for line in table_path.read_text().splitlines():
        words = line.split()
        if len(words) >= 3 and words[0] == 'vehicle':
            index = int(words[1])
        elif len(words) == 7 and words[0] == 'bounds' and index is not None:
            bounds[index] = [float(v) for v in words[1:]]
    return bounds


def parked(name, bounds):
    raw = gzip.decompress((maps / f'{name}.cgz').read_bytes())
    count = struct.unpack_from('<4i', raw, 4)[3]
    boxes = []
    for i in range(count):
        x, y, z, yaw, kind, team, vtype, flags, obj, _, scale = struct.unpack_from('<4h4BhbB', raw, 980 + 16 * i)
        if team != 102 or flags & 1 or flags & 14 == 14 or vtype not in bounds or not scale:
            continue
        lx, ly, lz, hx, hy, hz = bounds[vtype]
        s = scale / 20
        angle = math.radians(yaw / 10)
        boxes.append({'at': (x, y, z), 'f': (math.sin(angle), -math.cos(angle)), 'r': (math.cos(angle), math.sin(angle)),
                      'centre': (hx + lx) / 2 * s, 'halfL': max(.2, hx - lx) * s / 2 + 1.1,
                      'halfW': max(.2, hy - ly) * s / 2 + 1.1, 'top': z + max(.2, hz - max(0., lz)) * s,
                      'name': f'type {vtype} object {obj - 1} at {x},{y},{z}'})
    return boxes


bounds, parked_checked = vehicle_bounds(), 0
for route in sorted(maps.glob('halo_*_*.route')):
    if not bounds or not (maps / f'{route.stem}.cgz').is_file():
        continue
    boxes = parked(route.stem, bounds)
    if not boxes:
        continue
    try:
        table = rows(route)
    except ValueError:
        continue  # reported above
    parked_checked += 1
    for number, ((x, y, z), _, _, _) in enumerate(table, 1):
        for box in boxes:
            dx, dy = x - box['at'][0], y - box['at'][1]
            along = dx * box['f'][0] + dy * box['f'][1] - box['centre']
            side = dx * box['r'][0] + dy * box['r'][1]
            if abs(along) < box['halfL'] and abs(side) < box['halfW'] and z < box['top'] - .42 and z + 5.2 > box['at'][2] - 2:
                problems.append(f'{route.name}:{number}: route row inside a parked vehicle ({box["name"]})')
                break
print(f'{parked_checked} routes past parked vehicles checked' if bounds else
      'packages/halo/vehicles.txt missing: parked vehicles not checked')

print(f'{checked} route files checked')
if problems:
    raise SystemExit('traversal routes: ' + '; '.join(problems))
print('Lift and light-bridge route rows are consistent.')
