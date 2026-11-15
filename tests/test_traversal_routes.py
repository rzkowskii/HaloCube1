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
    (d40's Warthog run, c40's flown sections 1 and 4) have an .hcol, and a .route exactly when
    VEHICLE_ROUTES plans one (gap 5, written while testing was on hold, not yet run): vehicle rows
    (4096 drive or fly, 8192 board, 16384 get out) name a vehicles.txt type; a driven section's rows
    are all 4096 Warthog rows, steps of at most one cell but at a jump's landing (flag 2); a flown
    section boards a Banshee before its first flight row and gets out after its last, its flight rows
    at most two flight cells (8 units) apart; every 16384 row follows a 4096 row; its metrics record
    the vehicle route (`vehicle_route`) and c40 section 1's .hcol holds BSP 12 (EXTRA_BSPS), and since the
    route batch (gap 2, not yet run) its route boards and gets out twice, has foot rows between its
    first landing and its second boarding (into pulse_1_trigger and back) and ends in its exit; the Maw's
    planned drive of sections 6 to 8 (each section's metrics `vehicle_route.drive.seconds`, batch 10,
    not yet run) takes at most two thirds of the 180 s escape countdown,
  * action rows (written while testing was on hold, not yet run): a row with flag 256 (use a
    control) names the machine and the position it waits for, 512 (shoot a damage target) the
    target, the health it shoots it down to and an aim point (eight columns), 1024 (wait at a
    scripted door) the door, 2048 (throw frags at a damage target) the target, the health and an aim
    point; each repeats the position of the step row before it. d40 section 5 uses its four manifold
    controls (enc5_1_pistons1, _pistonn1, _pistonn3, _pistons3, in that order), shoots the detectors
    of s1, n1 and n3, and throws frags at s3's (ThrowTarget: no walkable spot sees them) from one
    spot after the s3 control's row, its aims straight after one another (three to five of them since
    the route batch, each going off beyond the frag's outer radius from the thrower, as the metrics'
    `throws` record); a10 section 5 waits at
    tunnel_door_2 and section 6 at cryo_door_2 on rows of their own (DOOR_WAITS), and section 5's
    route passes tunnel_trigger_close before tunnel_door_2's row (the importer's `door_order` in
    halo_a10_05.metrics.json, DOOR_PREREQUISITES),
  * b30's first two sections are walked routes since the route batch (RASTER_ROUTES is empty): both end
    in their exit volumes (their metrics), section 1 has no action rows, section 2 a door row at
    shafta_lock_door before its one use row (holohalo_1, the Cartographer's control; written while
    testing was on hold, not yet run),
  * b30's return (halo_b30_03) is a walked route now (not RASTER_ROUTES): no action rows, and it ends
    within 1.5 retail units of the ledge beside the extraction Pelican's ramp (its end marker),
  * with CRASH_SITE, the Crash Site (halo_a30) has an .hcol and a route of plain, crouch, jump and
    ledge-drop rows only (written while testing was on hold, not yet run),
  * walking assists (ASSISTS in tools/import_halo_campaign.py, written while testing was on hold, not
    yet run): a section's .hcol has an AST1 trailer exactly when ASSISTS lists the section, naming
    its last len(ASSISTS[section]) surfaces, each two-sided (flag 1) and walkable (normal z 0.65 or
    more); its metrics count them (`assists`), and its route has a row on every assist (the route
    uses each one, or it would not be needed),
  * no route row stands inside a vehicle parked when the section starts (the section map's
    PLAYERSTART team 102 placements that no script creates, with their box from
    packages/halo/vehicles.txt as halo_vehicle.cpp sizes it): the importer's planner goes round
    them (VehicleBlocks). Written while testing was on hold: this check has not been run.
Needs the imported campaign maps, not the game.
"""
import gzip
import json
import math
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import (ASSISTS, CRASH_SITE, CRASH_SITE_NAME, DRIVEN_SECTIONS, EXTRA_BSPS, FLIGHT_CELL,  # noqa: E402
                                  FLOWN_SECTIONS, LIFT_RIDES, MISSIONS, ROUTE_BOARD, ROUTE_GET_OUT, ROUTE_VEHICLE, STOCK_SECTIONS,
                                  VEHICLE_ROUTES)

maps = root / 'packages/maps'
require(maps, 'run scripts/import-macos-assets.sh')
problems, checked = [], 0
# The Maw's escape (sections 6 to 8, keys 5 to 7) and its countdown in seconds (halo.cpp escapeTime).
ESCAPE_SECTIONS, ESCAPE_TIME = {('d40', 5), ('d40', 6), ('d40', 7)}, 180
escape_seconds = {}


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
            if '@' in fields[4]:
                device, _, value = fields[4].rpartition('@')
                value = float(value)
            else:
                device = fields[4]        # a vehicle row's vehicles.txt type
        if flags & (ROUTE_VEHICLE | ROUTE_BOARD | ROUTE_GET_OUT) and not device:
            raise ValueError(f'{path.name}:{number}: vehicle row {flags} without a vehicle type')
        if flags & (16 | 128 | 256 | 512 | 1024 | 2048) and not device:
            raise ValueError(f'{path.name}:{number}: flag {flags} without a device')
        if bool(flags & (512 | 2048)) != (len(fields) == 8):
            raise ValueError(f'{path.name}:{number}: a shoot or throw row (512, 2048) has an aim point, and only it: {line!r}')
        if flags & (256 | 512 | 1024 | 2048) and (not result or result[-1][0] != (x, y, z)):
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
            # The Warthog run and the flown sections: an .hcol to drive or fly on, and the planned
            # vehicle route when VEHICLE_ROUTES has one (gap 5).
            key = (mission.cache, part)
            if not hcol.exists():
                problems.append(f'{name}: in DRIVEN_SECTIONS but has no hcol (re-run import_halo_campaign.py --mission {mission.cache})')
            if key not in VEHICLE_ROUTES:
                if route.exists():
                    problems.append(f'{name}: in DRIVEN_SECTIONS with no VEHICLE_ROUTES entry but has a route')
                continue
            if not route.exists():
                problems.append(f'{name}: VEHICLE_ROUTES plans a route but there is no .route (re-run the import)')
                continue
            try:
                table = rows(route)
            except ValueError as error:
                problems.append(str(error))
                continue
            checked += 1
            vehicle = [i for i, (_, f, _, _) in enumerate(table) if f & ROUTE_VEHICLE]
            boards = [i for i, (_, f, _, _) in enumerate(table) if f & ROUTE_BOARD]
            outs = [i for i, (_, f, _, _) in enumerate(table) if f & ROUTE_GET_OUT]
            if not vehicle:
                problems.append(f'{name}: a vehicle route without drive or fly rows')
            for i in outs:
                if not i or not table[i - 1][1] & ROUTE_VEHICLE:
                    problems.append(f'{name}: get-out row {i} does not follow a drive or fly row')
            if key in FLOWN_SECTIONS:
                if not boards or (vehicle and boards[0] > vehicle[0]):
                    problems.append(f'{name}: flown, but no Banshee boarded before the first flight row')
                if any(table[i][2] != 'banshee' for i in vehicle + boards + outs):
                    problems.append(f'{name}: flown with another vehicle than the Banshee')
                for a, b in zip(vehicle, vehicle[1:]):
                    if b == a + 1 and math.dist(table[a][0], table[b][0]) > 2 * FLIGHT_CELL + .5:
                        problems.append(f'{name}: flight rows {a} and {b} {math.dist(table[a][0], table[b][0]):.1f} apart')
                        break
            else:
                if boards or outs or len(vehicle) != len(table) or any(table[i][2] != 'warthog' for i in vehicle):
                    problems.append(f'{name}: driven, but not all Warthog drive rows')
                for (a, fa, _, _), (b, fb, _, _) in zip(table, table[1:]):
                    if math.hypot(b[0] - a[0], b[1] - a[1]) > 1.5 and not fb & 2:
                        problems.append(f'{name}: a step of {math.hypot(b[0] - a[0], b[1] - a[1]):.1f} cells that is not a jump')
                        break
            metrics_path = maps / f'{name}.metrics.json'
            vehicle_metrics = json.loads(metrics_path.read_text()).get('vehicle_route') if metrics_path.is_file() else None
            if not vehicle_metrics or vehicle_metrics.get('rows') != len(table):
                problems.append(f'{name}: metrics do not record its vehicle route (re-run the import)')
            elif list(EXTRA_BSPS.get(key, ())) != vehicle_metrics.get('extra_bsps'):
                problems.append(f'{name}: metrics name extra BSPs {vehicle_metrics.get("extra_bsps")}, EXTRA_BSPS {EXTRA_BSPS.get(key)}')
            elif key in ESCAPE_SECTIONS:
                # The Maw's escape: the drive's planned time (batch 10: `drive` in the metrics).
                drive = vehicle_metrics.get('drive') or {}
                if not drive.get('seconds'):
                    problems.append(f'{name}: metrics do not record the drive\'s time (re-run the import)')
                else:
                    escape_seconds[key] = drive['seconds']
            if key == ('c40', 0) and vehicle_metrics:
                # The route batch (gap 2): both flights and both foot legs between them are planned (into
                # pulse_1_trigger through the doorway BSPs 2 and 12 share, back to the Banshee, then from
                # the c bridge through door_c1), so the route boards and gets out twice and ends in its exit.
                ended = json.loads(metrics_path.read_text()).get('ends_in_exit')
                if (vehicle_metrics.get('boards'), vehicle_metrics.get('get_outs'), ended) != (2, 2, True):
                    problems.append(f'{name}: {vehicle_metrics.get("boards")} boards, {vehicle_metrics.get("get_outs")} '
                                    f'get-outs, ends in exit {ended}; expected 2, 2, True (pulse_1_trigger, then door_c1)')
                elif not any(not f & (ROUTE_VEHICLE | ROUTE_BOARD | ROUTE_GET_OUT) for _, f, _, _ in table[outs[0] + 1:boards[1]]):
                    problems.append(f'{name}: no foot rows between the first landing and the second boarding')
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
        throws = [i for i, (_, flags, _, _) in enumerate(table) if flags & 2048]
        expected_actions = {
            'halo_d40_05': (['enc5_1_pistons1', 'enc5_1_pistonn1', 'enc5_1_pistonn3', 'enc5_1_pistons3'],
                            ['enc5_1_detector_s1', 'enc5_1_detector_n1', 'enc5_1_detector_n3'], [], ['enc5_1_detector_s3']),
            'halo_a10_05': ([], [], ['tunnel_door_2'], []),
            'halo_a10_06': ([], [], ['cryo_door_2'], []),
            # The route batch (gap 7): b30's shaft A descent uses the Cartographer's control (shafta_map_control,
            # read by holohalo_1, which the scene sets to 1) after a door row at shafta_lock_door.
            'halo_b30_02': (['holohalo_1'], [], ['shafta_lock_door'], []),
        }.get(name, ([], [], [], []))
        thrown = sorted({table[i][2][:-1] if table[i][2].endswith('b') else table[i][2] for i in throws})
        if thrown != expected_actions[3]:
            problems.append(f'{name}: throw rows at {thrown}, expected {expected_actions[3]} (or their b detectors)')
        if throws:
            # One spot, its aims one after another, after the control that opens the channel.
            if throws != list(range(throws[0], throws[0] + len(throws))) or len({table[i][0] for i in throws}) != 1:
                problems.append(f'{name}: throw rows {throws} are not consecutive rows at one spot')
            used = [i for i, (_, flags, device, _) in enumerate(table) if flags & 256 and device == 'enc5_1_pistons3']
            if not used or used[0] > throws[0]:
                problems.append(f'{name}: throw rows before enc5_1_pistons3 is opened (use rows {used})')
            for i in throws:
                if table[i][3] is None or not 0 <= table[i][3] < 1:
                    problems.append(f'{name}: throw row {i} health {table[i][3]}')
            # The route batch (gap 9): three to five aims, each going off beyond the frag's outer blast
            # radius from the thrower's body (the importer's metrics, `throws`).
            if not 3 <= len(throws) <= 5:
                problems.append(f'{name}: {len(throws)} throw rows, expected 3 to 5')
            metrics = maps / f'{name}.metrics.json'
            planned = json.loads(metrics.read_text()).get('throws') if metrics.is_file() else None
            if not planned or len(planned) != len(throws):
                problems.append(f'{name}: metrics record {len(planned or ())} planned throws for {len(throws)} throw rows '
                                '(re-import the section)')
            else:
                close = [t for t in planned if t['thrower_distance'] <= t['blast_outer']]
                if close:
                    problems.append(f'{name}: planned throws within the blast radius of the thrower: {close}')
        if uses != expected_actions[0]:
            problems.append(f'{name}: use rows {uses}, expected {expected_actions[0]}')
        # Either detector of a manifold destroys it (enc5_1_destroyed_poll): s1 or s1b, and so on.
        if [shot[:-1] if shot.endswith('b') else shot for shot in shots] != expected_actions[1]:
            problems.append(f'{name}: shoot rows {shots}, expected {expected_actions[1]} (or their b detectors)')
        if [wait for wait in waits if wait in expected_actions[2]] != expected_actions[2]:
            problems.append(f'{name}: door rows {waits}, expected {expected_actions[2]} among them')
        if name == 'halo_a10_05':
            # mission_tunnel powers tunnel_door_2 only after every player has been in tunnel_trigger_close.
            metrics = maps / f'{name}.metrics.json'
            order = json.loads(metrics.read_text()).get('door_order') if metrics.is_file() else None
            entry = next((e for e in order or () if e.get('door') == 'tunnel_door_2'), None)
            if entry is None or 'tunnel_trigger_close' not in entry.get('passed', ()):
                problems.append(f'{name}: the route does not pass tunnel_trigger_close before tunnel_door_2 '
                                f'(door_order {order}; re-import a10 part 5)')
        if name in ('halo_b30_01', 'halo_b30_02'):
            # Planned since the route batch (gap 7; no longer RASTER_ROUTES): walked routes that end in
            # their exit (bsp 0,1; section 2 back in bsp 1,0 after the Cartographer), and section 2's
            # door row comes before its use row (the door is on the way down).
            metrics = maps / f'{name}.metrics.json'
            m = json.loads(metrics.read_text()) if metrics.is_file() else {}
            if m.get('mode') != 'walked' or m.get('ends_in_exit') is not True:
                problems.append(f'{name}: mode {m.get("mode")}, ends in its exit {m.get("ends_in_exit")} '
                                f'(expected a walked route ending in it; re-import b30 part {name[-1]})')
            if name == 'halo_b30_01' and any(flags & (256 | 512 | 1024 | 2048) for _, flags, _, _ in table):
                problems.append(f'{name}: action rows on the beach walk')
            if name == 'halo_b30_02':
                door = [i for i, (_, flags, _, _) in enumerate(table) if flags & 1024]
                use = [i for i, (_, flags, _, _) in enumerate(table) if flags & 256]
                if door and use and door[0] > use[0]:
                    problems.append(f'{name}: door row {door[0]} after the use row {use[0]}')
        if name == 'halo_b30_03':
            if any(flags & (256 | 512 | 1024 | 2048) for _, flags, _, _ in table):
                problems.append(f'{name}: action rows on the walked return')
            # It ends at the ledge beside the Pelican's ramp (SECTION_EXITS, a retail point): the
            # importer's metrics say whether the route's last step is within reach of it.
            metrics = maps / f'{name}.metrics.json'
            m = json.loads(metrics.read_text()) if metrics.is_file() else {}
            if m.get('mode') != 'walked' or m.get('ends_in_exit') is not True:
                problems.append(f'{name}: mode {m.get("mode")}, ends at the Pelican ledge {m.get("ends_in_exit")} '
                                '(expected a walked route ending there; re-import b30 part 3)')



# The Crash Site (CRASH_SITE: imported from the retail BSP, the engine's first a30 section): an .hcol and
# a walked route of plain steps and ledge drops (flag 8, its slides), no lift, bridge or action rows.
if CRASH_SITE:
    hcol, route = maps / f'{CRASH_SITE_NAME}.hcol', maps / f'{CRASH_SITE_NAME}.route'
    if not hcol.is_file() or not route.is_file():
        problems.append(f'{CRASH_SITE_NAME}: no hcol or route (CRASH_SITE: re-run import_halo_campaign.py --mission a30 --part 0)')
    else:
        try:
            table = rows(route)
            checked += 1
            odd = sorted({flags for _, flags, _, _ in table if flags & ~(1 | 2 | 8)})
            if odd:
                problems.append(f'{CRASH_SITE_NAME}: route flags {odd} (only crouch, jump and ledge-drop rows expected)')
        except ValueError as error:
            problems.append(str(error))


# Walking assists: the .hcol's AST1 trailer (halo_collision.write_structure_collision); an SCN1 trailer
# (scenery collision) may stand before it, and its surfaces before the assists (tests/test_retail_scenery.py).
def hcol_surfaces(path):
    data = path.read_bytes()
    if data[:4] != b'HCL1':
        raise ValueError(f'{path.name}: not HCL1')
    count, offset, surfaces = struct.unpack_from('<I', data, 4)[0], 8, []
    for _ in range(count):
        *normal, flags, _, _, points = struct.unpack_from('<3fBBhI', data, offset)
        offset += 20
        surfaces.append((normal, flags, [struct.unpack_from('<3f', data, offset + 12 * k) for k in range(points)]))
        offset += 12 * points
    trailers = {}
    while offset < len(data):
        tag, entries = struct.unpack_from('<4sI', data, offset)
        # SCN1 (scenery collision, SCENERY_COLLISION) has three uints an entry: placement, first surface, count.
        width = 3 if tag == b'SCN1' else 1
        trailers[tag.decode()] = list(struct.unpack_from(f'<{entries * width}I', data, offset + 8))
        offset += 8 + 4 * entries * width
    return surfaces, trailers


def on_polygon(point, normal, points):
    x, y, z = point
    # Above the polygon's plane by at most a step, inside its outline seen from above.
    height = sum((p - q) * n for p, q, n in zip((x, y, z), points[0], normal)) / max(normal[2], 1e-6)
    if not -.6 <= height <= 1.3:
        return False
    sign = 0
    for a, b in zip(points, points[1:] + points[:1]):
        cross = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])
        if abs(cross) < 1e-9:
            continue
        if sign and (cross > 0) != (sign > 0):
            return False
        sign = cross
    return True


assisted = 0
for mission in MISSIONS:
    for part in range(len(mission.sections)):
        name = f'halo_{mission.cache}_{part + 1:02}'
        hcol, route = maps / f'{name}.hcol', maps / f'{name}.route'
        expected = len(ASSISTS.get((mission.cache, part), ()))
        if not hcol.is_file():
            if expected and (mission.cache, part) not in STOCK_SECTIONS:
                problems.append(f'{name}: in ASSISTS but has no hcol (re-run import_halo_campaign.py --mission {mission.cache})')
            continue
        try:
            surfaces, trailers = hcol_surfaces(hcol)
        except (ValueError, struct.error) as error:
            problems.append(f'{name}: unreadable hcol ({error})')
            continue
        listed = trailers.get('AST1', [])
        if len(listed) != expected or listed != list(range(len(surfaces) - expected, len(surfaces))):
            problems.append(f'{name}: AST1 lists surfaces {listed[:4]}..., expected the last {expected} (ASSISTS)')
            continue
        if not expected:
            continue
        assisted += 1
        metrics = maps / f'{name}.metrics.json'
        counted = json.loads(metrics.read_text()).get('assists') if metrics.is_file() else None
        if counted != expected:
            problems.append(f'{name}: metrics count {counted} assists, ASSISTS lists {expected}')
        table = rows(route) if route.is_file() else []
        for index in listed:
            normal, flags, points = surfaces[index]
            if not flags & 1 or normal[2] < .65:
                problems.append(f'{name}: assist surface {index} flags {flags}, normal z {normal[2]:.2f} (two-sided, walkable)')
            if not any(on_polygon(point, normal, points) for point, _, _, _ in table):
                problems.append(f'{name}: no route row on assist surface {index}: the route does not use it')
print(f'{assisted} sections with walking assists checked')


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

# The Maw's escape countdown (180 s from section 6's start, source/src/halo.cpp escapeTime) against the
# planned drive of sections 6 to 8 at each section's planned speed (`drive.speed`: 0.9 of the Warthog's
# top speed, 0.75 in section 8; jumps included): it must leave a third of the time for corners, section loads and the walker easing off.
if len(escape_seconds) == len(ESCAPE_SECTIONS):
    total = sum(escape_seconds.values())
    print(f"the Maw's planned escape drive: {total:.0f} s of the 180 s countdown")
    if total > ESCAPE_TIME * 2 / 3:
        problems.append(f"the Maw's planned drive takes {total:.0f} s, more than two thirds of the {ESCAPE_TIME} s countdown")

print(f'{checked} route files checked')
if problems:
    raise SystemExit('traversal routes: ' + '; '.join(problems))
print('Lift and light-bridge route rows are consistent.')
