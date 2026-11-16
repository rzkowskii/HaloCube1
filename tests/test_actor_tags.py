"""Retail actor behaviour tables: importer output and the engine's parsed copy agree.

Checks packages/halo/actors.txt (tools/halo_actors.py) against values read from
the Xbox actr and actv tags, and that halo-ac-qa parses the same table.

The shield leak and shield timing lines (NOT YET RUN: written while testing was on hold): every
`material` line carries a fifth field, the coll material's shield_leak_fraction (+40), and every
variant with a coll a `resistance` line (indirect material +4, maximum body +8 and shield +204,
shield material type +210, failure function +236, threshold +240, maximum +244) and a
`shieldtime` line (minimum stun damage +264, stun +268, recharge +272, runtime velocity +448).
The Jackal's materials leak 1, 1, 1 and 0 (its hand-held `shield`, type 16, body multiplier 0),
its indirect material is 0, its stun 15 s with recharge velocity 1; the marines leak 0.2 with
failure 0, 1, 0.2 and stun 8 s, recharge 1 s; the Elites leak 0 with 12 s and 10 s. With the caches
(HALO_SOURCE) every variant is compared with the coll bytes. The engine's ACTOR_SHIELD lines
(qa_actor_table) match the table.

The melee delay and grenade timeout (NOT YET RUN): every variant with an actr has a fourth `berserk`
field, the actr's melee_attack_delay_timer (+888, ../Halo3DS ai/actor_definitions.h:222), and every
variant a seventh `grenade` field, the actv's encounter_grenade_timeout (+424, :375): the Hunters
0.1 s, Elite minors 5, majors 4, commanders 2; Grunt minors 5 s, marines 20; with the caches every
variant is compared with the tag bytes, and the engine's ACTOR_TIMER lines match.
"""
import math
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

TABLE = root / 'packages/halo/actors.txt'
require(TABLE, 'run python3 tools/halo_actors.py with the expanded caches')


def records():
    actors, current = [], None
    for line in TABLE.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'actor':
            current = {'index': int(rest)}
            actors.append(current)
        elif current is None or key.startswith('#') or key in ('format', 'end'):
            continue
        elif key in ('path', 'major', 'destroyed'):
            # format 3: `major <actv path>`, the variant's major upgrade; `destroyed <effe path>`
            # (the carrier-squad change), the body destroyed effect a unit destroyed after dying sets off
            current[key] = rest
        elif key == 'material':
            current.setdefault('materials', []).append([float(x) for x in rest.split()])
        elif key == 'family':
            family, kind, kind_type, *_ = rest.split()
            current.update(family=family, kind=int(kind), type=int(kind_type))
        else:
            current[key] = [float(x) for x in rest.split()]
    return actors


actors = records()
by_path = {a['path']: a for a in actors}
assert [a['index'] for a in actors] == list(range(len(actors)))
close = lambda a, b: abs(a - b) <= 1e-3 * max(1.0, abs(b))

# fire: max range, rate, error, first delay min/max, combat range min/max, lead, damage scale
marine = by_path[r'characters\marine\marine assault rifle']
assert marine['kind'] == 9 and marine['fire'][:2] == [13, 10] and marine['fire'][5:7] == [3, 6], marine['fire']
assert marine['burst'][:2] == [0.6, 0.8] and marine['burst'][2:] == [1, 1.5], marine['burst']
grunt = by_path[r'characters\grunt\grunt minor plasma pistol']
assert grunt['fire'][1] == 5 and close(math.degrees(grunt['fire'][2]), 3) and grunt['morale'][1] == 1.0, grunt
assert grunt['vision'][0] == 40 and grunt['vitality'][0] == 30
elite = by_path[r'characters\elite\elite minor\elite minor plasma rifle']
assert elite['defense'][0] == 0.5 and elite['vitality'] == [100, 100] and elite['weapon'][0] >= 0, elite
assert sum(a['kind'] >= 0 for a in actors) >= 70, 'most variants have engine bodies'

# The shield leak, the coll's resistance values and its shield timing.
jackal = by_path[r'characters\jackal\jackal minor plasma pistol']
assert [m[4] for m in jackal['materials']] == [1, 1, 1, 0], jackal['materials']
assert jackal['materials'][3][:4] == [16, 0, 0, 1], jackal['materials']
assert jackal['resistance'] == [0, 60, 100, 16, 4, 0, 0] and jackal['shieldtime'] == [0, 15, 0, 1], (jackal['resistance'], jackal['shieldtime'])
assert all(m[4] == 0.2 for m in marine['materials']) and marine['resistance'][3:] == [23, 0, 1, 0.2], marine
assert marine['shieldtime'][:3] == [0, 8, 1] and close(marine['shieldtime'][3], 1 / 30), marine['shieldtime']
assert all(m[4] == 0 for m in elite['materials']) and elite['resistance'][3] == 30, elite
assert elite['shieldtime'][:3] == [0, 12, 10] and close(elite['shieldtime'][3], 1 / 300), elite['shieldtime']
for a in actors:
    assert all(len(m) == 5 and 0 <= m[4] <= 1 for m in a.get('materials', [])), a['path']
    assert ('resistance' in a) == ('shieldtime' in a) and (not a.get('materials') or 'resistance' in a), a['path']
print('Shield leaks, resistance and shield timing are exported (Jackal hand shield, marine leak, Elite timing).')

# The actr melee delay and the actv encounter grenade timeout (optional last fields).
hunter = by_path[r'characters\hunter\hunter']
assert close(hunter['berserk'][3], 0.1) and elite['berserk'][3] == 5 and grunt['grenade'][6] == 5, (hunter, elite, grunt)
assert by_path[r'characters\elite\elite major\elite major plasma rifle']['berserk'][3] == 4
assert by_path[r'characters\elite\elite commander\elite commander energy sword']['berserk'][3] == 2
assert marine['grenade'][6] == 20 and marine['berserk'][3] == 0, marine
for a in actors:
    assert len(a['grenade']) == 7 and a['grenade'][6] >= 0, a['path']
    assert len(a['berserk']) in (3, 4) and (len(a['berserk']) == 3 or a['berserk'][3] >= 0), a['path']
print('Melee delays and encounter grenade timeouts are exported (Hunter 0.1 s, Elite minor 5 s, Grunt 5 s, marine 20 s).')

if (HALO_SOURCE / 'a10.map').is_file():
    import mmap
    import struct
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    checked, timers, seen = 0, 0, set()
    for cache in ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40'):
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            t = halo.tags
            for tag in halo.directory:
                if tag.group != 'actv' or tag.name in seen:
                    continue
                seen.add(tag.name)
                a = by_path[tag.name]
                # actv +424 encounter_grenade_timeout; actr +888 melee_attack_delay_timer.
                assert close(a['grenade'][6], t.read('f', tag.address + 424)[0]), (tag.name, a['grenade'])
                actr = halo.reference(t, tag.address + 4)
                if actr and actr.group == 'actr':
                    assert close(a['berserk'][3], t.read('f', actr.address + 888)[0]), (tag.name, a['berserk'])
                    timers += 1
                unit = halo.reference(t, tag.address + 20)
                coll = halo.reference(t, unit.address + 112) if unit and unit.group in ('bipd', 'vehi') else None
                if not coll or coll.group != 'coll':
                    assert 'resistance' not in a, tag.name
                    continue
                raw = bytes(t.read('452s', coll.address)[0])
                want = (struct.unpack_from('<h', raw, 4)[0], *struct.unpack_from('<f', raw, 8), *struct.unpack_from('<f', raw, 204),
                        struct.unpack_from('<h', raw, 210)[0], struct.unpack_from('<h', raw, 236)[0], *struct.unpack_from('<2f', raw, 240))
                assert all(close(x, y) for x, y in zip(a['resistance'], want)), (tag.name, a['resistance'], want)
                want = (*struct.unpack_from('<3f', raw, 264), *struct.unpack_from('<f', raw, 448))
                assert all(close(x, y) for x, y in zip(a['shieldtime'], want)), (tag.name, a['shieldtime'], want)
                for m, got in zip(t.block(coll.address + 564, 72), a['materials']):
                    leak, = struct.unpack_from('<f', bytes(t.read('72s', m)[0]), 40)
                    assert close(got[4], leak), (tag.name, got, leak)
                checked += 1
    print(f'{checked} variants\' leaks, resistance and shield timing match the coll bytes.')
    print(f'{len(seen)} variants\' grenade timeouts and {timers} actr melee delays match the tag bytes.')

profile = scratch('actor-table-')
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480',
                    '-eqa_actor_table; quit'], cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT,
                   timeout=60, check=True)
rows = [line.split()[1:] for line in log.read_text(errors='replace').splitlines() if line.startswith('ACTOR_TABLE')]
assert len(rows) == len(actors), (len(rows), len(actors), log)
for row, a in zip(rows, actors):
    index, family, kind, weapon, rate, burst, vision = row
    assert (int(index), family, int(kind), int(weapon)) == (a['index'], a['family'], a['kind'], int(a['weapon'][0])), (row, a['path'])
    assert close(float(rate), a['fire'][1]) and close(float(burst), a['burst'][0]) and close(float(vision), a['vision'][0]), row
shields = [line.split()[1:] for line in log.read_text(errors='replace').splitlines() if line.startswith('ACTOR_SHIELD ')]
assert len(shields) == len(actors), (len(shields), len(actors), log)
for row, a in zip(shields, actors):
    resistance, shieldtime = a.get('resistance'), a.get('shieldtime')
    assert int(row[1]) == bool(resistance) and int(row[8]) == bool(shieldtime), (row, a['path'])
    if resistance:
        assert int(row[2]) == resistance[0] and close(float(row[3]), resistance[2]) and int(row[4]) == resistance[3], (row, a['path'])
        assert int(row[5]) == resistance[4] and close(float(row[6]), resistance[5]) and close(float(row[7]), resistance[6]), (row, a['path'])
    if shieldtime:
        assert all(close(float(x), y) for x, y in zip(row[9:13], shieldtime)), (row, a['path'])
    leaks = [] if row[13] == '-' else [float(x) for x in row[13].split(',')]
    assert all(close(x, m[4]) for x, m in zip(leaks, a.get('materials', []))) and len(leaks) == len(a.get('materials', [])), (row, a['path'])
timer_rows = [line.split()[1:] for line in log.read_text(errors='replace').splitlines() if line.startswith('ACTOR_TIMER ')]
assert len(timer_rows) == len(actors), (len(timer_rows), len(actors), log)
for row, a in zip(timer_rows, actors):
    melee = a['berserk'][3] if len(a['berserk']) > 3 else -1
    assert int(row[0]) == a['index'] and close(float(row[1]), melee) and close(float(row[2]), a['grenade'][6]), (row, a['path'])
print(f'{len(actors)} retail actor variants imported, engine table matches (shield model included).')
