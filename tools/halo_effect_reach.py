#!/usr/bin/env python3
"""Static reach of the mission scripts' damaging effects (no game run, no cache needed).

Lists every effect_new and effect_new_on_object_marker call in the compiled mission scripts
(packages/halo/<cache>.hscb, tools/import_halo_scripts.py) whose effect carries a jpt! area damage
part (packages/halo/effects.txt `damage` line, tools/halo_effects.py), with:

  - the script that makes the call, its kind and the scripts that wake it;
  - the cutscene flag's (or the marked object's) retail position, its outer damage radius, and the
    scripted squads whose starting positions lie inside it (an ownerless blast hurts actors too);
  - the trigger: the last player volume test (sleep_until (volume_test_objects <v> (players)) ...)
    before the call in its script, or in the script that woke it, and the scripted delay from it to
    the call (the literal sleeps between; "+" when an untimed wait lies between as well);
  - for each engine section of the mission whose planned route (packages/maps/<section>.route, the QA
    walker's rows) comes within reach: the flag projected into the section (the .hscb projections,
    as source/src/halo_script.cpp toWorld does), the radius in the section's units
    (haloscenarioscale), the route rows whose body middle (feet + 2.25, half the player's 4.5 eye
    height, as haloareablast measures) is inside it, the row at which the route enters the trigger
    volume and an estimate of where the walker is when the call fires (the trigger row advanced by
    the delay at the walker's speed: 16 units a second on foot, the vehicle's top speed on a driven
    route, halo_vehicle.cpp topSpeed; door waits and fights are not counted, so it is a lower bound
    of the time taken and an upper bound of the distance covered).

A continuous script's blasts (begin_random ambience) repeat: the estimate is for the first only.

A flag is projected into every section of its mission (Halo's flags carry no BSP, and the engine
places an effect_new with the current section's projection whatever BSP the flag was authored in),
so a section other than the one the script runs in can show rows in reach: read the trigger row.

It ignores walls: haloareablast needs a clear line from the centre to a body outside the AOE core,
so a row listed in reach may be screened. A row out of reach is out of reach.

  python3 tools/halo_effect_reach.py [--packages packages] [--mission d40] [--all] [--json out.json]

The judgement it informs is in docs/macos-build.md (Presentation, Script effects: halo_effectdamage).
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

NIL = 0xffffffff
TICKS = 30
# The engine's section order per cache (tools/import_halo_campaign.py MISSIONS; a30's first engine
# section is the Crash Site, halo_a30, then halo_a30_01 and _02).
SECTIONS = {
    'a10': 7, 'a30': 3, 'a50': 5, 'b30': 3, 'b40': 7, 'c10': 7, 'c20': 4, 'c40': 5, 'd20': 6, 'd40': 8,
}
SCRIPT_KINDS = {0: 'startup', 1: 'dormant', 2: 'continuous', 3: 'static', 4: 'stub'}
BODY_MIDDLE = 2.25      # haloareablast: the body's middle, o - eyeheight / 2, eye height 4.5 above the feet
WALK_SPEED = 16.0       # physics.cpp: maxspeed 16 units a second on foot
ACTOR_MIDDLE = 0.3      # a squad start's point is its feet: an actor's middle about 0.3 wu above it


def section_names(cache):
    count = SECTIONS[cache]
    if cache == 'a30':
        return ['halo_a30'] + [f'halo_a30_{i:02d}' for i in range(1, count)]
    return [f'halo_{cache}_{i:02d}' for i in range(1, count + 1)]


class Reader:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def take(self, fmt):
        values = struct.unpack_from('<' + fmt, self.data, self.offset)
        self.offset += struct.calcsize('<' + fmt)
        return values

    def one(self, fmt):
        return self.take(fmt)[0]

    def text(self):
        size = self.one('H')
        self.offset += size
        return self.data[self.offset - size:self.offset].decode('latin-1')

    def rows(self, row):
        return [row() for _ in range(self.one('I'))]


def read_program(path):
    """The parts of a .hscb (versions 5 to 10, read as Program::load reads them) this report needs."""
    r = Reader(Path(path).read_bytes())
    assert r.data[:4] == b'HSCB', path
    r.offset = 4
    version = r.one('I')
    assert 5 <= version <= 10, (path, version)
    p = {'version': version}
    p['nodes'] = r.rows(lambda: r.take('4H3I'))
    size = r.one('I')
    p['strings'] = r.data[r.offset:r.offset + size]
    r.offset += size
    p['scripts'] = r.rows(lambda: (r.text(), *r.take('HHI')))
    p['globals'] = r.rows(lambda: (r.text(), *r.take('HI')))
    r.rows(lambda: (r.text(), r.take('7f')))                                   # cameras
    p['flags'] = r.rows(lambda: (r.text(), r.take('5f')))
    p['volumes'] = r.rows(lambda: (r.text(), r.one('H'), r.take('12f')))
    r.rows(lambda: (r.text(), r.text(), r.take('3f')))                         # titles
    r.rows(lambda: (r.text(), r.take('H'), r.rows(lambda: r.take('HBH8f'))))   # recordings

    def obj():
        name, model = r.text(), r.text()
        pose = r.take('6fh9f')
        markers = r.rows(lambda: (r.text(), r.take('5f')))
        r.rows(lambda: (r.text(), r.text()))
        for _ in range(3):
            r.text()
        if version >= 8:
            r.take('7fB')
        return {'name': name, 'model': model, 'pose': pose[:6], 'markers': dict(markers)}
    p['objects'] = r.rows(obj)
    r.rows(lambda: (*r.take('HH'), r.text(), r.text(), r.one('H')))            # animations
    r.rows(lambda: (r.take('HII'), r.text()))                                  # sounds
    r.rows(lambda: (r.text(), r.rows(lambda: (r.take('H'), r.rows(lambda: r.take('H'))))))
    r.rows(lambda: (r.take('HB'), r.rows(lambda: r.take('f5i'))))              # loops
    p['projections'] = r.rows(lambda: r.take('H6f'))
    r.rows(lambda: r.take('H'))                                                # worlds
    r.rows(lambda: (r.text(), r.text()))                                       # HUD messages
    r.rows(lambda: r.take('f'))                                                # device groups
    p['encounters'] = r.rows(lambda: (r.text(), r.take('2h'),
                                      r.rows(lambda: (r.text(), r.take('h'), r.rows(lambda: r.take('h4f'))))))
    return p


class Script:
    """A compiled program's node tree (salt, opcode, type, flags, next, text, data), decompiled."""

    def __init__(self, program):
        self.p = program
        self.nodes = program['nodes']

    def node(self, datum):
        if datum == NIL:
            return None
        index = datum & 0xffff
        return self.nodes[index] if index < len(self.nodes) else None

    def text(self, offset):
        s = self.p['strings']
        if offset >= len(s):
            return ''
        end = s.find(b'\0', offset)
        return s[offset:end if end >= 0 else len(s)].decode('latin-1')

    def children(self, datum):
        n = self.node(datum)
        kids, it = [], n[6]
        while it != NIL and len(kids) < 4096:
            kids.append(it)
            it = self.node(it)[4]
        return kids

    def head(self, datum):
        n = self.node(datum)
        if n is None or n[3] & 1:
            return None
        kids = self.children(datum)
        return self.text(self.node(kids[0])[5]) if kids else None

    def args(self, datum):
        return self.children(datum)[1:]

    def number(self, datum):
        """A numeric literal's value (Scenario::literal: 5 boolean, 6 real, 8 long, else short), or None."""
        n = self.node(datum)
        if n is None or not n[3] & 1 or n[3] & 4:
            return None
        kind, data = n[1], n[6]
        if kind == 5:
            return 1 if data & 0xff else 0
        if kind == 6:
            return struct.unpack('<f', struct.pack('<I', data))[0]
        if kind in (8, 17):
            return struct.unpack('<i', struct.pack('<I', data))[0]
        if kind == 7:
            return struct.unpack('<h', struct.pack('<H', data & 0xffff))[0]
        return None

    def literal(self, datum):
        n = self.node(datum)
        if n is None or not n[3] & 1:
            return None
        if n[3] & 4:
            index = n[6] & 0xffff
            if index < len(self.p['globals']):
                name, _, root = self.p['globals'][index]
                return self.literal(root) or name
            return None
        return self.text(n[5])

    def show(self, datum, depth=0, limit=400):
        n = self.node(datum)
        if n is None:
            return '?'
        if n[3] & 1:
            if n[3] & 4:
                index = n[6] & 0xffff
                return self.p['globals'][index][0] if index < len(self.p['globals']) else '?'
            value = self.number(datum)
            if value is not None:
                return f'{value:g}' if isinstance(value, float) else str(value)
            return self.text(n[5]) or '""'
        if depth > 12:
            return '(...)'
        kids = self.children(datum)
        parts = [self.text(self.node(kids[0])[5])] + [self.show(k, depth + 1) for k in kids[1:]]
        out = '(' + ' '.join(parts) + ')'
        return out if len(out) <= limit else out[:limit] + '...'

    def walk(self, visit):
        """visit(datum, script name, ancestors) for every call node of every script."""
        def go(datum, script, stack, depth=0):
            n = self.node(datum)
            if n is None or n[3] & 3 or depth > 64:
                return
            visit(datum, script, stack)
            for kid in self.children(datum)[1:]:
                go(kid, script, stack + [datum], depth + 1)
        for name, kind, _, root in self.p['scripts']:
            go(root, name, [])

    def player_volume(self, datum):
        """The volumes of the player volume tests inside this expression ((players), (player0),
        (list_get (players) n), (player1)), in order (a wait on `(or (volume_test ...) ...)` names
        several: it ends at the first the player enters), or None."""
        found = []

        def go(d, depth=0):
            n = self.node(d)
            if n is None or n[3] & 3 or depth > 12:
                return
            head = self.head(d)
            args = self.args(d)
            if head and head.startswith('volume_test_object') and len(args) > 1 and 'player' in self.show(args[1]):
                found.append(self.literal(args[0]))
            for a in args:
                go(a, depth + 1)
        go(datum)
        return list(dict.fromkeys(v for v in found if v)) or None

    def delay_before(self, root, target):
        """(volumes, ticks, untimed) for the statement sequence of `root` up to the top-level statement
        holding `target`: the volumes of the last player volume wait before it and the literal sleeps
        since. volumes is None when the sequence has no such wait before the target."""
        if self.head(root) != 'begin':
            return None, 0, False
        statements = self.args(root)
        holder = None
        for i, s in enumerate(statements):
            if self.contains(s, target):
                holder = i
                break
        if holder is None:
            return None, 0, False
        ticks, untimed = 0, False
        for s in reversed(statements[:holder]):
            head = self.head(s)
            if head == 'sleep_until':
                volume = self.player_volume(self.args(s)[0]) if self.args(s) else None
                if volume:
                    return volume, ticks, untimed
                untimed = True
            elif head == 'sleep':
                value = self.number(self.args(s)[0]) if self.args(s) else None
                if value is None:
                    untimed = True
                else:
                    ticks += max(0, value)
            elif head in ('if', 'cond', 'begin', 'ai_conversation', 'sound_impulse_start'):
                untimed |= head != 'sound_impulse_start' and 'sleep' in self.show(s, limit=100000)
        return None, ticks, untimed

    def contains(self, datum, target, depth=0):
        if datum == target:
            return True
        n = self.node(datum)
        if n is None or n[3] & 3 or depth > 64:
            return False
        return any(self.contains(k, target, depth + 1) for k in self.args(datum))


def read_effects(path):
    """effects.txt: {effect path: (inner, outer, lower, upper min, upper max, side effect, category, core)}."""
    out, current = {}, None
    for line in Path(path).read_text().splitlines():
        key, _, rest = line.strip().partition(' ')
        if key == 'path':
            current = rest
        elif key == 'damage' and current:
            out[current] = tuple(float(v) for v in rest.split()[:8])
        elif key == 'end':
            current = None
    return out


def read_vehicles(path):
    """vehicles.txt: {name: retail forward speed per tick} (the motion line's second value)."""
    out, current = {}, None
    if not Path(path).exists():
        return out
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if parts[:1] == ['vehicle'] and len(parts) > 2:
            current = parts[2]
        elif parts[:1] == ['motion'] and current and len(parts) > 2:
            out.setdefault(current, float(parts[2]))
    return out


def read_route(path):
    """Route rows: (x, y, z, vehicle name or '')."""
    rows = []
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) >= 3:
            flags = int(parts[3]) if len(parts) > 3 and parts[3].lstrip('-').isdigit() else 0
            vehicle = parts[4] if flags & 4096 and len(parts) > 4 else ''
            rows.append((float(parts[0]), float(parts[1]), float(parts[2]), vehicle))
    return rows


def to_world(projection, point):
    _, ox, oy, oz, xy, z, margin = projection
    return ((point[0] - ox) * xy + margin, (oy - point[1]) * xy + margin, (point[2] - oz) * z)


def to_retail(projection, point):
    _, ox, oy, oz, xy, z, margin = projection
    return (ox + (point[0] - margin) / xy, oy - (point[1] - margin) / xy, oz + point[2] / z)


def inside_volume(volume, point):
    """Scenario insideVolume: kind 0 an axis-aligned box, else an oriented one."""
    _, kind, v = volume
    forward, up, origin, extent = v[0:3], v[3:6], v[6:9], v[9:12]
    if kind == 0:
        return all(origin[i] < point[i] < extent[i] for i in range(3))
    local = [point[i] - origin[i] for i in range(3)]
    side = (up[1] * forward[2] - up[2] * forward[1], up[2] * forward[0] - up[0] * forward[2],
            up[0] * forward[1] - up[1] * forward[0])
    dot = lambda a, b: sum(a[i] * b[i] for i in range(3))
    x, y, zz = dot(local, forward), dot(local, side), dot(local, up)
    return 0 < x < extent[0] and 0 < y < extent[1] and 0 < zz < extent[2]


def marker_position(obj, marker):
    """The object's marker in retail coordinates, as objectMarkerPosition places it (bind pose, the
    object's yaw); the object's origin when the model has no such marker."""
    x, y, z, yaw = obj['pose'][:4]
    m = obj['markers'].get(marker)
    if not m:
        return (x, y, z), False
    mx, my, mz = m[:3]
    c, s = math.cos(yaw), math.sin(yaw)
    return (x + mx * c - my * s, y + mx * s + my * c, z + mz), True


def route_speed(rows, scale, vehicles):
    """(engine, retail) units a second the walker covers, and what moves it: on foot 16 both ways
    (Halo's own run is not modelled), also for a route walked mostly on foot (c40's, with a short
    Banshee leg); driven, halo_vehicle.cpp topSpeed: a car max(36, retail forward * 30 * scale), any
    other vehicle never below 36 x clamp(forward / 0.255, 0.3, 1.5), a third more for a flyer."""
    names = [r[3] for r in rows if r[3]]
    if not names or 2 * len(names) < len(rows):
        return WALK_SPEED, WALK_SPEED, 'on foot'
    name = max(set(names), key=names.count)
    forward = vehicles.get(name, .25)
    retail = forward * TICKS * scale
    floor = 36.0 if name == 'warthog' else 36.0 * min(max(forward / .255, .3), 1.5) * (1.3 if name == 'banshee' else 1.0)
    return max(floor, retail), retail, name


def advance(rows, start, distance):
    """The route row reached `distance` units along the rows from `start`."""
    travelled, i = 0.0, start
    while i + 1 < len(rows) and travelled + math.dist(rows[i][:3], rows[i + 1][:3]) <= distance:
        travelled += math.dist(rows[i][:3], rows[i + 1][:3])
        i += 1
    return i


def analyse(packages, only=None):
    effects = read_effects(packages / 'halo' / 'effects.txt')
    vehicles = read_vehicles(packages / 'halo' / 'vehicles.txt')
    report = []
    for cache in SECTIONS:
        if only and cache not in only:
            continue
        path = packages / 'halo' / f'{cache}.hscb'
        if not path.exists():
            continue
        program = read_program(path)
        script = Script(program)
        flags = {name: pose for name, pose in program['flags']}
        objects = {o['name']: o for o in program['objects']}
        volumes = {v[0]: v for v in program['volumes']}
        roots = {name: root for name, _, _, root in program['scripts']}
        kinds = {name: SCRIPT_KINDS.get(kind, kind) for name, kind, _, _ in program['scripts']}
        wakes = {}

        def collect(datum, name, stack):
            if script.head(datum) == 'wake':
                args = script.args(datum)
                target = script.literal(args[0]) if args else None
                if target:
                    wakes.setdefault(target, []).append((name, datum))
        script.walk(collect)
        calls = []

        def visit(datum, name, stack):
            head = script.head(datum)
            if head in ('effect_new', 'effect_new_on_object_marker'):
                calls.append((datum, name, head, script.args(datum)))
        script.walk(visit)
        routes = {}
        for s, section in enumerate(section_names(cache)):
            route = packages / 'maps' / f'{section}.route'
            routes[s] = (section, read_route(route) if route.exists() else [])
        starts = [(f'{e[0]}/{q[0]}', st[1:4]) for e in program['encounters'] for q in e[2] for st in q[2]]
        seen = {}
        for datum, name, head, args in calls:
            effect = script.literal(args[0]) if args else None
            damage = effects.get(effect or '')
            if not damage:
                continue
            if head == 'effect_new':
                place = script.literal(args[1]) if len(args) > 1 else None
                retail = flags[place][:3] if place in flags else None
                where = f'flag {place}'
            else:
                obj = script.literal(args[1]) if len(args) > 1 else None
                marker = script.literal(args[2]) if len(args) > 2 else ''
                if obj in objects:
                    retail, has = marker_position(objects[obj], marker)
                    where = f'object {obj} marker {marker}' + ('' if has else ' (origin: no such marker)')
                else:
                    retail, where = None, f'object {obj} (the player or an actor: follows it)'
            # The trigger and the delay: this script's last player volume wait before the call, else
            # the waking script's before its wake (one level up), plus this script's sleeps.
            volume, ticks, untimed = script.delay_before(roots[name], datum)
            via = name
            if volume is None:
                for waker, wake_node in wakes.get(name, ()):
                    v2, t2, u2 = script.delay_before(roots[waker], wake_node)
                    if v2:
                        volume, ticks, untimed, via = v2, ticks + t2, untimed or u2, waker
                        break
            key = (cache, name, effect, where)
            if key in seen:
                seen[key]['calls'] += 1
                continue
            entry = {'mission': cache, 'script': name, 'kind': kinds.get(name),
                     'woken_by': sorted({w for w, _ in wakes.get(name, ())}), 'calls': 1,
                     'effect': effect, 'where': where, 'retail': retail,
                     'inner': damage[0], 'outer': damage[1], 'damage': damage[2:5],
                     'trigger': volume[0] if volume else None, 'triggers': volume or [],
                     'trigger_script': via if volume else None,
                     'delay_s': round(ticks / TICKS, 2), 'untimed': untimed, 'actors': [], 'sections': []}
            seen[key] = entry
            if retail is not None:
                near = {}
                for squad, point in starts:
                    if math.dist(retail, (point[0], point[1], point[2] + ACTOR_MIDDLE)) < damage[1]:
                        near[squad] = near.get(squad, 0) + 1
                entry['actors'] = sorted(near.items())
                for s, (section, rows) in routes.items():
                    if s >= len(program['projections']) or not rows:
                        continue
                    projection = program['projections'][s]
                    scale = projection[4]
                    centre = to_world(projection, retail)
                    outer = damage[1] * scale
                    gap = [math.dist(centre, (x, y, z + BODY_MIDDLE)) for x, y, z, _ in rows]
                    reach = [i for i, d in enumerate(gap) if d < outer]
                    # The first row inside any of the wait's volumes (an `or` ends at the first entered).
                    entered = None
                    tested = [volumes[v] for v in volume or () if v in volumes]
                    for i, (x, y, z, _) in enumerate(rows):
                        if tested and any(inside_volume(v, to_retail(projection, (x, y, z + BODY_MIDDLE))) for v in tested):
                            entered = i
                            break
                    if not reach and entered is None:
                        continue
                    speed, retail_speed, mover = route_speed(rows, scale, vehicles)
                    at_fire = None
                    if entered is not None:
                        i = advance(rows, entered, ticks / TICKS * speed)
                        j = advance(rows, entered, ticks / TICKS * retail_speed)
                        at_fire = {'row': i, 'wu': round(gap[i] / scale, 2), 'in_reach': gap[i] < outer,
                                   'retail_row': j, 'retail_wu': round(gap[j] / scale, 2), 'retail_in_reach': gap[j] < outer,
                                   'trigger_row': entered, 'trigger_wu': round(gap[entered] / scale, 2)}
                    entry['sections'].append({'section': section, 'scale': round(scale, 3),
                                              'centre': tuple(round(c, 1) for c in centre),
                                              'outer_units': round(outer, 2), 'rows': len(rows),
                                              'speed': round(speed, 1), 'retail_speed': round(retail_speed, 1), 'mover': mover,
                                              'closest_wu': round(min(gap) / scale, 2), 'in_reach': reach,
                                              'at_fire': at_fire})
            report.append(entry)
    return report


def runs(indices):
    out, start, last = [], None, None
    for i in indices:
        if start is None:
            start = last = i
        elif i == last + 1:
            last = i
        else:
            out.append((start, last))
            start = last = i
    if start is not None:
        out.append((start, last))
    return ', '.join(f'{a}' if a == b else f'{a}-{b}' for a, b in out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--packages', type=Path, default=Path(__file__).resolve().parent.parent / 'packages')
    parser.add_argument('--mission', action='append', help='cache name (a10 ... d40); repeatable')
    parser.add_argument('--all', action='store_true', help='also list calls no route row comes near')
    parser.add_argument('--json', type=Path, help='also write the report as JSON')
    args = parser.parse_args()
    report = analyse(args.packages, args.mission)
    fired_in_reach = retail_in_reach = 0
    for e in report:
        if not args.all and not e['sections'] and not e['actors']:
            continue
        times = f" x{e['calls']}" if e['calls'] > 1 else ''
        print(f"{e['mission']} {e['script']} ({e['kind']}{', woken by ' + ', '.join(e['woken_by']) if e['woken_by'] else ''})"
              f"{times}: {e['effect']} at {e['where']}; {e['damage'][1]:g}-{e['damage'][2]:g} inside {e['inner']:g} wu, "
              f"{e['damage'][0]:g} at {e['outer']:g} wu")
        if e['trigger']:
            print(f"    fires {e['delay_s']:g} s{'+' if e['untimed'] else ''} after the player enters {' or '.join(e['triggers'])}"
                  + (f" (in {e['trigger_script']})" if e['trigger_script'] != e['script'] else ''))
        else:
            print('    no player volume wait before it (a cue, a device, a count or a static call)')
        if e['actors']:
            print('    squad starts in reach: ' + ', '.join(f'{s} ({n})' for s, n in e['actors']))
        if e['retail'] is None:
            print('    position: follows a unit')
        for s in e['sections']:
            line = (f"    {s['section']} (x{s['scale']:g}, {s['mover']} {s['speed']:g} u/s): radius {s['outer_units']:g} units, "
                    f"closest row {s['closest_wu']:g} wu")
            if s['in_reach']:
                line += f"; rows in reach {runs(s['in_reach'])}"
            f = s['at_fire']
            if f:
                line += (f"; enters the trigger at row {f['trigger_row']} ({f['trigger_wu']:g} wu away), at the call "
                         f"near row {f['row']}: {f['wu']:g} wu, {'IN REACH' if f['in_reach'] else 'clear'}")
                if s['retail_speed'] != s['speed']:
                    line += (f" (at the retail {s['retail_speed']:g} u/s near row {f['retail_row']}: {f['retail_wu']:g} wu, "
                             f"{'in reach' if f['retail_in_reach'] else 'clear'})")
                fired_in_reach += f['in_reach']
                retail_in_reach += f['retail_in_reach']
            print(line)
    print(f"{len(report)} damaging scripted effects ({sum(e['calls'] for e in report)} calls); {fired_in_reach} "
          f"with the walker estimated in reach when they fire, {retail_in_reach} at the retail vehicle speeds "
          "(walls, waits and fights not counted)")
    if args.json:
        args.json.write_text(json.dumps(report, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
