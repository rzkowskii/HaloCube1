#!/usr/bin/env python3
"""Static scan of every planned route drop against Halo's falling damage.

Reads the generated packages/maps/<section>.route files (their step rows: ledge drops, flag 8;
jumps, flag 2; and any other step down between consecutive rows), the section's .hcol floors
(the landing floor under each drop) and the section's projection from packages/halo/<cache>.hscb,
and lists for each drop:

- the drop in world units (the engine's AssaultCube units) from the take-off floor, or from the
  engine's jump apex for a jump, to the landing floor;
- the impact speed the engine's own movement gives that drop (source/src/physics.cpp moveplayer and
  retailmove, simulated tick by tick at the 5 ms physics frame: the fall rate (19 + timeinair/15) *
  curtime / 2000 units a tick, and the jump's vel.z of 2 at 16 units a second decaying by
  (fpsfric - 1) / fpsfric), converted to Halo units a tick with the section's vertical projection
  scale (world units / scale / 30);
- the Halo-equivalent impact speed of a free fall of the same height in Halo units,
  sqrt(2 g d) with Halo's gravity (../Halo3DS physics/physics.c:279, 0.0035651792 units a tick
  squared; the globals' runtime velocities, 0.1463, 0.2068 and 0.3430, are exactly sqrt(2 g d) of
  their 3, 6 and 16.5 unit distances);
- the falling damage each speed would deal by biped_falling_damage (../Halo3DS units/bipeds.c:
  792-845): nothing at or under the minimum damage velocity, else globals\\falling's 10 + 145 x
  scale with scale = (v - min) / (max - min) pinned to 0..1 (damage.c:1375-1382), against the
  player's 75 shield and 75 body (falling damage to the player ignores difficulty, damage.c:820-825
  and 1142-1147).

The thresholds come from packages/halo/player.txt's `falling` line when it is there
(tools/halo_actors.py), else the retail values (the same in all ten caches).

Usage: tools/halo_fall_scan.py [--json out.json] [--all] [sections...]
Exit status 1 when a planned on-foot drop is harmful (over the lower distance bound) or lethal under
the distance model the engine uses (source/src/halo.cpp updateFalling), else 0.
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from qa_common import read_hscb  # noqa: E402  (the one full .hscb reader; tests/qa_common.py)

MISSIONS = (('a10', 7), ('a30', 3), ('a50', 5), ('b30', 3), ('b40', 7), ('c10', 7), ('c20', 4), ('c40', 5),
            ('d20', 6), ('d40', 8))
HALO_GRAVITY = 0.0035651792            # ../Halo3DS source/physics/physics.c:279, units a tick squared
RETAIL = {'lower': 3.0, 'upper': 6.0, 'maximum': 16.5, 'max_falling': 0.3430028, 'min_damage': 0.1462569,
          'max_damage': 0.2068385, 'falling': (10.0, 155.0, 155.0), 'distance': (105.0, 105.0, 105.0)}
PLAYER_VITALITY = 150.0                # the cyborg coll's 75 shield + 75 body (player.txt `resistance`)
# Route flags (tools/import_halo_campaign.py): 2 jump, 4 first step after a lift ride, 8 ledge drop,
# 16 lift deck row; action rows (256, 512, 1024, 2048) repeat the step before them; 4096 drive or
# fly, 8192 board, 16384 get out.
JUMP, RIDE, DROP, DECK, ACTIONS, DRIVE, BOARD, LEAVE = 2, 4, 8, 16, 256 | 512 | 1024 | 2048, 4096, 8192, 16384
PHYS_MS = 5                            # source/src/physics.cpp PHYSFPS 200
MAXSPEED = 16.0                        # the player's maxspeed (physics.cpp moveplayer: min(maxspeed, 16))


def section_names():
    for cache, count in MISSIONS:
        for s in range(count):
            name = 'halo_a30' if cache == 'a30' and s == 0 else f'halo_{cache}_{s + (0 if cache == "a30" else 1):02}'
            yield cache, s, name


def thresholds():
    """The falling block from player.txt's `falling` line, else the retail values."""
    values = dict(RETAIL)
    path = ROOT / 'packages/halo/player.txt'
    if path.is_file():
        for line in path.read_text().splitlines():
            key, _, rest = line.partition(' ')
            if key == 'falling':
                f = [float(v) for v in rest.split()]
                if len(f) >= 12:
                    values.update(lower=f[0], upper=f[1], maximum=f[2], max_falling=f[3], min_damage=f[4],
                                  max_damage=f[5], falling=tuple(f[6:9]), distance=tuple(f[9:12]))
                    values['source'] = 'player.txt'
    values.setdefault('source', 'retail defaults')
    return values


def engine_fall(height, jump=False):
    """The engine player's fall from rest (a walk off a ledge) or from a standing jump on level
    floor, down to `height` units below the take-off: (apex above take-off, impact speed in units a
    second, seconds in the air). physics.cpp moveplayer/retailmove at the 5 ms frame."""
    z, vz, t_air, onfloor, apex, elapsed = 0.0, 0.0, 0, True, 0.0, 0
    speed = PHYS_MS / 1000.0 * MAXSPEED
    first = True
    while True:
        if onfloor:
            if jump and first:
                vz = 2.0
            t_air = 0
        else:
            t_air += PHYS_MS
        friction = 6.0 if onfloor else 30.0
        fpsfric = max(friction / PHYS_MS * 20.0, 1.0)
        vz = vz * (fpsfric - 1.0) / fpsfric
        gravity = (19.0 + t_air / 15.0) * PHYS_MS / 2000.0
        rising = vz > 0.05
        dz = vz * speed
        if not onfloor or rising or first:
            dz -= gravity
        first = False
        z += dz
        elapsed += PHYS_MS
        apex = max(apex, z)
        onfloor = False
        if z <= -height and dz < 0:
            return apex, -dz / (PHYS_MS / 1000.0), elapsed / 1000.0
        if elapsed > 20000:
            return apex, -dz / (PHYS_MS / 1000.0), elapsed / 1000.0


def falling_damage(v, t):
    """biped_falling_damage's landing damage at impact speed v (Halo units a tick)."""
    if v <= t['min_damage']:
        return 0.0
    s = min(max((v - t['min_damage']) / (t['max_damage'] - t['min_damage']), 0.0), 1.0)
    minimum, lower, upper = t['falling']
    return (1 - s) * minimum + s * (lower + upper) / 2


def hcol_floors(path):
    """Floor polygons (normal z >= 0.65) of an .hcol, indexed by 8-unit cell."""
    data = path.read_bytes()
    if data[:4] != b'HCL1':
        raise ValueError(f'{path.name}: not HCL1')
    count, offset = struct.unpack_from('<I', data, 4)[0], 8
    grid = {}
    for _ in range(count):
        nx, ny, nz, flags, _, _, points = struct.unpack_from('<3fBBhI', data, offset)
        offset += 20
        pts = [struct.unpack_from('<3f', data, offset + 12 * k) for k in range(points)]
        offset += 12 * points
        if nz < .65:
            continue
        poly = ((nx, ny, nz), pts)
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        for cx in range(int(math.floor(min(xs) / 8)), int(math.floor(max(xs) / 8)) + 1):
            for cy in range(int(math.floor(min(ys) / 8)), int(math.floor(max(ys) / 8)) + 1):
                grid.setdefault((cx, cy), []).append(poly)
    return grid


def floor_below(grid, x, y, z, slack=.5):
    """The highest floor under (x, y) at or below z + slack, or None."""
    best = None
    for normal, pts in grid.get((int(math.floor(x / 8)), int(math.floor(y / 8))), ()):
        sign, inside = 0, True
        for a, b in zip(pts, pts[1:] + pts[:1]):
            cross = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])
            if abs(cross) < 1e-9:
                continue
            if sign and (cross > 0) != (sign > 0):
                inside = False
                break
            sign = cross
        if not inside:
            continue
        p = pts[0]
        height = p[2] - ((x - p[0]) * normal[0] + (y - p[1]) * normal[1]) / normal[2]
        if height <= z + slack and (best is None or height > best):
            best = height
    return best


def route_steps(path):
    """The on-foot step rows of a route: (index, x, y, z, flags), without action rows, lift deck
    rows, the step after a ride, or the driven and flown legs. A get-out row (16384) stands in as
    the take-off of the first step after it (the drop from the seat: kind 'vehicle exit'); boarding
    ends the on-foot leg."""
    steps, seated = [], False
    for i, line in enumerate(path.read_text().splitlines()):
        f = line.split()
        if len(f) < 3:
            continue
        flags = int(f[3]) if len(f) > 3 and f[3].lstrip('-').isdigit() else 0
        if flags & BOARD:
            seated = True
            steps.append(None)
            continue
        if flags & LEAVE:
            seated = False
            steps.append((i + 1, float(f[0]), float(f[1]), float(f[2]), LEAVE))
            continue
        if seated or flags & (ACTIONS | DRIVE):
            continue
        steps.append((i + 1, float(f[0]), float(f[1]), float(f[2]), flags))
    return steps


def scan(names=None, everything=False):
    t = thresholds()
    jump_apex = engine_fall(0.0, jump=True)[0]
    sections, drops, programs = [], [], {}
    for cache, index, name in section_names():
        if names and name not in names:
            continue
        route, hcol = ROOT / f'packages/maps/{name}.route', ROOT / f'packages/maps/{name}.hcol'
        hscb = ROOT / f'packages/halo/{cache}.hscb'
        if not route.is_file() or not hscb.is_file():
            continue
        if cache not in programs:
            programs[cache] = read_hscb(hscb)['projections']
        projection = programs[cache][index] if index < len(programs[cache]) else None
        if not projection:
            continue
        zscale = projection[5]
        grid = hcol_floors(hcol) if hcol.is_file() else {}
        steps = route_steps(route)
        section = {'name': name, 'z_scale': round(zscale, 4), 'jump_apex_units': round(jump_apex, 3),
                   'jump_apex_halo': round(jump_apex / zscale, 3), 'steps': sum(s is not None for s in steps), 'drops': 0}
        sections.append(section)
        for before, step in zip(steps, steps[1:]):
            if before is None or step is None or step[4] & LEAVE:
                continue
            (_, _, _, z0, f0), (row, x, y, z, flags) = before, step
            if f0 & (RIDE | DECK) or flags & (RIDE | DECK):
                continue
            jump = bool(flags & JUMP)
            descent = z0 - z
            if not (flags & DROP or jump or descent >= 1.0 or f0 & LEAVE):
                continue
            landing = floor_below(grid, x, y, z) if grid else None
            land_z = landing if landing is not None and landing > z - 1.0 else z
            height = z0 - land_z
            apex = jump_apex if jump else 0.0
            fall = height + apex
            if fall <= 0:
                continue
            _, impact, seconds = engine_fall(max(height, -apex + 1e-3), jump)
            engine_v = impact / zscale / 30.0
            halo_d = fall / zscale
            halo_v = math.sqrt(2 * HALO_GRAVITY * halo_d)
            drop = {'section': name, 'row': row, 'x': x, 'y': y, 'from_z': z0, 'to_z': round(land_z, 3),
                    'kind': 'jump' if jump else 'ledge drop' if flags & DROP else 'seat exit' if f0 & LEAVE else 'step down',
                    'floor_found': landing is not None, 'fall_units': round(fall, 3), 'fall_halo': round(halo_d, 3),
                    'engine_seconds': round(seconds, 3), 'engine_v': round(engine_v, 4), 'halo_v': round(halo_v, 4),
                    'engine_damage': round(falling_damage(engine_v, t), 1),
                    'halo_damage': round(falling_damage(halo_v, t), 1)}
            drop['engine_lethal'] = drop['engine_damage'] >= PLAYER_VITALITY
            drop['halo_lethal'] = drop['halo_damage'] >= PLAYER_VITALITY or halo_d > t['maximum']
            section['drops'] += 1
            if everything or flags & (DROP | JUMP) or f0 & LEAVE or halo_d >= t['lower'] * .5 or drop['engine_damage'] > 0:
                drops.append(drop)
    return t, sections, drops


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sections', nargs='*')
    parser.add_argument('--json', type=Path)
    parser.add_argument('--all', action='store_true', help='list every step down of 1 unit or more')
    args = parser.parse_args()
    t, sections, drops = scan(set(args.sections) or None, args.all)
    print(f'thresholds ({t["source"]}): harmless to {t["lower"]} Halo units ({t["min_damage"]:.4f} a tick), '
          f'lethal from {t["upper"]} ({t["max_damage"]:.4f}), distance damage past {t["maximum"]} ({t["max_falling"]:.4f})')
    scales = sorted({s['z_scale'] for s in sections})
    if sections:
        apex = sections[0]['jump_apex_units']
        print(f'{len(sections)} sections, vertical scales {scales[0]} to {scales[-1]}; the engine\'s standing jump apex '
              f'{apex} units = {apex / scales[-1]:.3f} to {apex / scales[0]:.3f} Halo units (under {t["lower"]} everywhere: '
              f'{all(s["jump_apex_halo"] < t["lower"] for s in sections)})')
    print(f'{"section":14} {"row":>5} {"kind":10} {"fall":>6} {"halo":>6} {"eng s":>6} {"eng v":>7} {"eng dmg":>7} '
          f'{"halo v":>7} {"halo dmg":>8} floor')
    for d in drops:
        print(f'{d["section"]:14} {d["row"]:5} {d["kind"]:10} {d["fall_units"]:6.2f} {d["fall_halo"]:6.2f} '
              f'{d["engine_seconds"]:6.2f} {d["engine_v"]:7.4f} {d["engine_damage"]:7.1f}{"!" if d["engine_lethal"] else " "}'
              f'{d["halo_v"]:7.4f} {d["halo_damage"]:8.1f}{"!" if d["halo_lethal"] else " "} {"hcol" if d["floor_found"] else "row"}')
    engine_lethal = [d for d in drops if d['engine_lethal']]
    engine_hurt = [d for d in drops if d['engine_damage'] > 0]
    halo_hurt = [d for d in drops if d['halo_damage'] > 0]
    halo_lethal = [d for d in drops if d['halo_lethal']]
    print(f'{len(drops)} drops listed: engine-velocity model hurts {len(engine_hurt)} and kills {len(engine_lethal)}; '
          f'Halo distance model hurts {len(halo_hurt)} and kills {len(halo_lethal)}')
    if args.json:
        args.json.write_text(json.dumps({'thresholds': t, 'sections': sections, 'drops': drops}, indent=1))
    return 1 if halo_hurt or halo_lethal else 0


if __name__ == '__main__':
    sys.exit(main())
