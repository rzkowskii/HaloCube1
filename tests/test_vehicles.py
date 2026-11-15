"""Vehicles: the imported definitions and placements, getting in and out, driving, turrets, flips
and the mission script's seat commands (source/src/halo_vehicle.cpp).

Written while testing was on hold: not yet run.

1. packages/halo/vehicles.txt (tools/halo_vehicles.py): six vehicles, the Warthog a car with
   driver, passenger and gunner seats and the warthog gun, the turret one gunner seat, models.
2. The section maps carry the scenario's vehicle placements (PLAYERSTART team 102): d40's
   escape Warthogs in section 6, the enc7_7 turrets in section 8, a50's turrets, b30's unnamed
   Warthog; every placement has a known type and the section scale.
3. b30 section 1 on retail collision: the unnamed Warthog stands on the beach; E beside its
   driver door gets in, throttle drives it, E gets out onto the floor; a flipped Warthog throws
   the player out and E flips it back; the gunner seat fires the warthog gun.
4. a50 section 1: a placed turret; E mans it, the trigger fires the turret gun, E gets out.
5. b30's script commands on its jeep: object_create makes it, vehicle_load_magic seats the
   player, vehicle_test_seat_list and unit_in_vehicle see it, unit_exit_vehicle lets them out.
6. A Warthog spawned on stock geometry (d40 section 3) drives and stops at a wall.
7. Bodies inside vehicles (b30 section 1, a50 section 1): the player put part way into the
   beach Warthog, or into its middle, is pushed out within a frame (`player pushed out of the
   Warthog`) and stands beside it; a Warthog rolling into the player never has the body inside
   it (it shoves the player or stops); the script creating the jeep on top of the player pushes
   them out; an actor put inside a turret is pushed out alive.
8. Stuck vehicles (b30 section 1): at the wheel of the beach Warthog, a drop 20 units below the
   mesh puts it back on its last safe spot with the player still at the wheel, alive; sunk half
   into the beach it is recovered again; thrown out of it upside down with no room for the body,
   the player's fall recovery puts them back in its seat, the Warthog on its wheels
   (`player put back in the Warthog`).
9. Vehicles on lift decks (c20 section 1): a Warthog parked on platform1 rises with it when the
   platform is raised (it does not sleep on a device).
Items 7 to 9 were added for the vehicle fixes and have not been run either.
"""
import gzip
import re
import struct
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
maps = root / 'packages/maps'
table = root / 'packages/halo/vehicles.txt'
require(table, 'run tools/halo_vehicles.py "$maps" (scripts/import-macos-assets.sh)')
NAMES = ['warthog', 'turret', 'ghost', 'scorpion', 'banshee', 'wraith']

# 1. The definitions.
records, current = {}, None
for line in table.read_text().splitlines():
    if not line or line.startswith('#'):
        continue
    key, _, rest = line.partition(' ')
    if key == 'vehicle':
        index, name = rest.split()
        current = records.setdefault(name, {'index': int(index), 'seats': []})
    elif key == 'seat':
        current['seats'].append(rest.split()[0])
    elif key in ('class', 'model', 'weapon', 'motion', 'bounds'):
        current[key] = rest
assert [records[n]['index'] for n in NAMES] == list(range(6)), records
assert records['warthog']['class'] == 'car' and records['turret']['class'] == 'turret', records
assert records['warthog']['seats'] == ['W-driver', 'W-passenger', 'W-gunner'], records['warthog']
assert records['turret']['seats'] == ['GT-gunner'], records['turret']
assert records['warthog']['weapon'] == r'vehicles\warthog\warthog gun', records['warthog']
assert records['warthog']['motion'].split()[:2] == ['1', '0.255'], records['warthog']
for name in NAMES:
    model = records[name]['model'].split()[0]
    assert (root / 'packages/models' / model / 'tris.md3').is_file(), (name, model)
print('vehicles.txt: six vehicles, Warthog seats and gun, turret gunner seat, models exported.')


# 2. The placements in the section maps.
def placements(name):
    raw = gzip.decompress((maps / f'{name}.cgz').read_bytes())
    count = struct.unpack_from('<4i', raw, 4)[3]
    rows = []
    for i in range(count):
        x, y, z, yaw, kind, team, vtype, flags, obj, _, scale = struct.unpack_from('<4h4BhbB', raw, 980 + 16 * i)
        if team == 102:
            rows.append({'type': NAMES[vtype] if vtype < len(NAMES) else vtype, 'flags': flags, 'object': obj - 1,
                         'scale': scale / 20, 'at': (x, y, z)})
    return rows


every = {p.stem: placements(p.stem) for p in sorted(maps.glob('halo_*_*.cgz'))}
for name, rows in every.items():
    for row in rows:
        assert row['type'] in NAMES and row['scale'] > 0, (name, row)
d40_06 = [r for r in every['halo_d40_06'] if r['type'] == 'warthog']
assert len(d40_06) >= 3 and all(r['flags'] & 1 for r in d40_06), ('the parked escape Warthogs are script-created', d40_06)
assert not [r for r in every.get('halo_d40_05', []) if r['type'] == 'warthog'], 'the escape Warthogs belong to BSP 5, not the engine room'
assert sum(r['type'] == 'turret' for r in every['halo_d40_08']) == 2, every['halo_d40_08']
assert sum(r['type'] == 'turret' for r in every['halo_a50_01']) == 14, every['halo_a50_01']
assert any(r['type'] == 'warthog' and r['object'] < 0 and not r['flags'] for r in every['halo_b30_01']), every['halo_b30_01']
print(f'placements: {sum(map(len, every.values()))} vehicles in {sum(bool(v) for v in every.values())} sections, types and scales set.')


def run(name, start, schedule, timeout=180):
    profile = scratch(f'vehicles-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_vehicletrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace')


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def state(text, label):
    """The header and per-vehicle rows of a qa_vehicle_state dump."""
    rows = [fields(line) for line in text.splitlines() if line.startswith(f'VEHICLE_QA {label} ')]
    assert rows, (label, text[-3000:])
    return rows[0], rows[1:]


def first(rows, kind, **want):
    for row in rows:
        if row.get('type') == kind and all(row.get(k) == v for k, v in want.items()):
            return int(row['vehicle'])
    raise AssertionError((kind, want, rows))


def position(row):
    return tuple(map(float, row['pos'].split(',')))


# 3. b30 on retail collision: the beach Warthog. Find it first, then a second run uses its index.
text = run('b30-find', 'halo_section 4 1', [(300, 'qa_vehicle_state find'), (600, 'quit')])
head, rows = state(text, 'find')
hog = first(rows, 'warthog', object='-1', exists='1')
start = position(rows[hog])
text = run('b30-drive', 'halo_section 4 1', [
    (300, f'qa_vehicle_approach {hog} 0'), (500, 'qa_vehicle_use'), (700, 'qa_vehicle_state seated'),
    (800, 'qa_vehicle_input 1 0 2500'), (3600, 'qa_vehicle_state driven'),
    (3800, 'qa_vehicle_use'), (4300, 'qa_vehicle_state out; qa_halo_status out'),
    (4500, f'qa_vehicle_flip {hog}'), (4700, f'qa_vehicle_approach {hog} 0'), (4900, 'qa_vehicle_use'),
    (5400, 'qa_vehicle_state flippedback'),
    (5600, f'qa_vehicle_approach {hog} 1'), (5800, 'qa_vehicle_use'), (6000, 'qa_halo_fire 1'),
    (7000, 'qa_halo_fire 0; qa_vehicle_state gunner'), (7200, 'qa_vehicle_use'),
    (7400, f'qa_vehicle_enter {hog} 0'), (7600, f'qa_vehicle_flip {hog}'), (8800, 'qa_vehicle_state ejected'),
    (9000, 'quit')])
assert re.search(r'VEHICLE_QA approach vehicle=\d+ found=1 prompt=E  Drive the Warthog', text), text[-3000:]
head, rows = state(text, 'seated')
assert head['seated'] == '1' and head['role'] == '0', head
head, rows = state(text, 'driven')
moved = sum((a - b) ** 2 for a, b in zip(position(rows[hog]), start)) ** .5
assert moved > 8 and rows[hog]['flipped'] == '0', ('throttle drives the Warthog', moved, rows[hog])
head, rows = state(text, 'out')
assert head['seated'] == '0' and head['alive'] == '1', head
assert 'Halo vehicle: player out of the Warthog' in text, text[-3000:]
assert 'Halo recovery' not in text.split('VEHICLE_QA out')[1].split('VEHICLE_QA flippedback')[0], 'getting out lands on floor'
assert 'prompt=E  Flip the Warthog' in text and 'Warthog flipped back' in text, text[-3000:]
head, rows = state(text, 'flippedback')
assert rows[hog]['flipped'] == '0', rows[hog]
head, rows = state(text, 'gunner')
assert head['seated'] == '1' and head['role'] == '1' and int(head['shots']) > 3, ('the gunner fires the warthog gun', head)
head, rows = state(text, 'ejected')
assert head['seated'] == '0' and rows[hog]['flipped'] == '1' and 'player thrown out' in text, ('a flip throws the driver out', head)
print(f'b30: Warthog entered with E, driven {moved:.1f} units, left onto the floor, flipped back, gunner fired, flip ejects.')

# 4. a50: a placed turret.
text = run('a50-turret', 'halo_section 3 1', [(300, 'qa_vehicle_state find'), (600, 'quit')])
head, rows = state(text, 'find')
turret = first(rows, 'turret', exists='1')
text = run('a50-man', 'halo_section 3 1', [
    (300, f'qa_vehicle_approach {turret} 1'), (500, 'qa_vehicle_use'), (700, 'qa_halo_fire 1'),
    (2200, 'qa_halo_fire 0; qa_vehicle_state manned'), (2400, 'qa_vehicle_use'), (2800, 'qa_vehicle_state left'),
    (3000, 'quit')])
assert 'prompt=E  Man the Covenant Turret' in text, text[-3000:]
head, rows = state(text, 'manned')
assert head['seated'] == '1' and head['role'] == '1' and int(head['shots']) > 2 and head['gun'] != '-1', head
head, rows = state(text, 'left')
assert head['seated'] == '0' and head['alive'] == '1', head
print('a50: turret manned with E, fired its gun, left.')

# 5. b30's jeep through the script's seat commands (the mission script runs on b30's retail sections).
text = run('b30-script', 'halo_section 4 1', [
    (300, 'qa_hs_cmd object_create "s:jeep"'), (500, 'qa_hs_cmd vehicle_load_magic "s:jeep s:w-driver -2"'),
    (700, 'qa_hs_cmd vehicle_test_seat_list "s:jeep s:w-driver -3"'), (750, 'qa_hs_cmd unit_in_vehicle "-2"'),
    (800, 'qa_hs_cmd vehicle_test_seat_list "s:jeep s:w-gunner -3"'), (900, 'qa_vehicle_state loaded'),
    (1000, 'qa_hs_cmd unit_exit_vehicle "-2"'), (1300, 'qa_hs_cmd vehicle_test_seat_list "s:jeep s:w-driver -3"'),
    (1400, 'qa_vehicle_state unloaded'), (1600, 'quit')])
calls = [line.split()[1:] for line in text.splitlines() if line.startswith('HS_CMD')]
seat = [float(v) for c, v in calls if c == 'vehicle_test_seat_list']
assert seat == [1.0, 0.0, 0.0], ('seat tests: driver yes, gunner no, after exit no', calls)
assert [float(v) for c, v in calls if c == 'unit_in_vehicle'] == [1.0], calls
assert 'created by the script' in text, text[-3000:]
head, _ = state(text, 'loaded')
assert head['seated'] == '1', head
head, _ = state(text, 'unloaded')
assert head['seated'] == '0', head
print('b30: object_create, vehicle_load_magic, vehicle_test_seat_list, unit_in_vehicle and unit_exit_vehicle act on the jeep.')

# 6. Stock geometry: a Warthog spawned ahead of the player in d40 section 3 (no placements there,
#    so it is vehicle 0).
text = run('stock-drive', 'halo_section 10 3', [
    (300, 'qa_vehicle_spawn 0 8'), (600, 'qa_vehicle_enter 0 0'), (800, 'qa_vehicle_state before'),
    (900, 'qa_vehicle_input 1 0 6000'), (7200, 'qa_vehicle_state after'), (7400, 'quit')])
assert 'retail collision surfaces for halo_d40_03' not in text, 'd40 section 3 plays on stock geometry'
assert 'VEHICLE_QA spawn type=0 vehicle=0' in text and 'enter vehicle=0 role=0 ok=1' in text, text[-3000:]
_, before = state(text, 'before')
_, after = state(text, 'after')
moved = sum((a - b) ** 2 for a, b in zip(position(after[0]), position(before[0]))) ** .5
assert moved > 5 and after[0]['grounded'] == '1', ('the Warthog drives on stock geometry', moved, after[0])
print(f'd40 section 3 (stock): a spawned Warthog drove {moved:.1f} units and stayed on the ground.')


def inside(text, label):
    found = [fields(line) for line in text.splitlines() if line.startswith(f'VEHICLE_QA inside {label} ')]
    assert found, (label, text[-3000:])
    return found[0]


# 7. Bodies inside vehicles. The beach Warthog (`hog`, `start` from step 3).
text = run('b30-inside', 'halo_section 4 1', [
    (300, f'qa_vehicle_place_player {hog} 0.5 0.3'), (450, 'qa_vehicle_inside partway; qa_vehicle_state partway'),
    (600, f'qa_vehicle_place_player {hog} 0 0'), (750, 'qa_vehicle_inside middle'),
    (900, f'qa_vehicle_place_player {hog} 2.2 0'), (1000, 'qa_vehicle_state ahead; qa_vehicle_watch 1800'),
    (1050, f'qa_vehicle_push {hog} 14'), (3000, 'qa_vehicle_state rolled; qa_vehicle_inside rolled'), (3200, 'quit')])
assert text.count('Halo vehicle: player pushed out of the Warthog') >= 2, ('pushed out of the box', text[-3000:])
for label in ('partway', 'middle', 'rolled'):
    row = inside(text, label)
    assert row['player'] == '-1', ('the body is not left inside the Warthog', label, row)
head, rows = state(text, 'partway')
assert head['alive'] == '1' and head['trapped'] == '0', head
watch = [fields(line) for line in text.splitlines() if line.startswith('VEHICLE_QA watch ')]
assert watch and int(watch[0]['samples']) > 20 and watch[0]['inside'] == '0', ('a rolling Warthog shoves or stops', watch)
_, ahead = state(text, 'ahead')
_, rolled = state(text, 'rolled')
print(f'b30: a body part way in or in the middle of the Warthog is pushed out; rolled into the player it '
      f'moved {sum((a - b) ** 2 for a, b in zip(position(rolled[hog]), position(ahead[hog]))) ** .5:.1f} units '
      f'with the body never inside.')

# The script creates the jeep on top of the player.
text = run('b30-create', 'halo_section 4 1', [(400, 'qa_hs_cmd object_create "s:jeep"'), (600, 'qa_vehicle_state made'), (800, 'quit')])
created = re.search(r'Halo vehicle (\d+) created by the script', text)
assert created, text[-3000:]
jeep = int(created.group(1))
text = run('b30-createover', 'halo_section 4 1', [
    (300, f'qa_vehicle_place_player {jeep} 0 0'), (500, 'qa_hs_cmd object_create "s:jeep"'),
    (800, 'qa_vehicle_inside created; qa_vehicle_state created'), (1000, 'quit')])
after = text.split('HS_CMD object_create')[1]
assert 'Halo vehicle: player pushed out of the Warthog' in after, ('object_create over the player pushes them out', text[-3000:])
assert inside(text, 'created')['player'] == '-1', text[-3000:]
print('b30: object_create jeep over the player pushes them out of it.')

# An actor put inside a turret (a50 section 1, `turret` from step 4).
text = run('a50-actor', 'halo_section 3 1', [
    (300, f'qa_vehicle_place_actor {turret} 0 0'), (500, 'qa_vehicle_inside actor'), (700, 'quit')])
placed = [fields(line) for line in text.splitlines() if line.startswith('VEHICLE_QA place actor')]
assert placed and placed[0]['ok'] == '1', ('an actor to place', placed, text[-3000:])
row = inside(text, 'actor')
assert row['actor'] == '-1' and row['actoralive'] == '1', ('the actor is pushed out of the turret alive', row)
print(f'a50: actor {placed[0]["name"]} put inside a turret is pushed out.')

# 8. Stuck vehicles: at the wheel of the beach Warthog.
text = run('b30-stuck', 'halo_section 4 1', [
    (300, f'qa_vehicle_enter {hog} 0'), (1500, 'qa_vehicle_state parked'), (1600, f'qa_vehicle_stuck {hog} fall'),
    (2000, 'qa_vehicle_state fell'), (3200, f'qa_vehicle_stuck {hog} sink'), (5500, 'qa_vehicle_state sunk'),
    (5700, f'qa_vehicle_throw {hog}'), (5900, 'qa_vehicle_state thrown'), (6000, 'qa_retail_recover fall'),
    (6400, 'qa_vehicle_state reclaimed'), (6600, 'quit')])
head, rows = state(text, 'parked')
assert head['seated'] == '1' and int(rows[hog]['safe']) >= 1, ('a safe spot is recorded at the wheel', head, rows[hog])
parked = position(rows[hog])
assert 'Halo vehicle recovery: fell out of the world, the Warthog put back at' in text, text[-3000:]
head, rows = state(text, 'fell')
assert head['seated'] == '1' and head['alive'] == '1', ('the driver stays at the wheel, alive', head)
back = sum((a - b) ** 2 for a, b in zip(position(rows[hog]), parked)) ** .5
assert back < 2 * float(rows[hog]['length']), ('back on its last safe spot', back, rows[hog])
recoveries = text.count('Halo vehicle recovery:')
assert recoveries >= 2, ('the sunk Warthog is recovered as well', text[-3000:])
head, rows = state(text, 'sunk')
assert head['seated'] == '1' and rows[hog]['grounded'] == '1', (head, rows[hog])
assert 'VEHICLE_QA throw vehicle=%d ok=1' % hog in text and 'player thrown clear of the Warthog' in text, text[-3000:]
head, _ = state(text, 'thrown')
assert head['seated'] == '0', head
assert 'player put back in the Warthog' in text, ('the thrown-out driver is put back in its seat', text[-3000:])
head, rows = state(text, 'reclaimed')
assert head['seated'] == '1' and rows[hog]['flipped'] == '0' and head['alive'] == '1', (head, rows[hog])
assert 'player left in place' not in text and 'reverting to the checkpoint' not in text, text[-3000:]
print(f'b30: the Warthog fallen out of the world is back {back:.1f} units from its safe spot, sunk it is recovered, '
      'and the thrown-out driver is put back in its seat.')

# 9. A Warthog on c20's platform1 (the deck row at its centre) rises with it.
deck = next(line.split() for line in (maps / 'halo_c20_01.route').read_text().splitlines() if 'platform1@' in line)
landing = (maps / 'halo_c20_01.route').read_text().splitlines()[-2].split()
dx, dy, dz = map(float, deck[:3])
text = run('c20-platform', 'halo_section 7 1', [
    (300, f'qa_halo_feet {dx} {dy} {dz + .2}'), (400, 'qa_vehicle_spawn 0 0'),
    (600, f'qa_halo_feet {landing[0]} {landing[1]} {float(landing[2]) + .2}'), (1800, 'qa_vehicle_state before'),
    (2000, 'qa_hs_cmd device_set_position "s:platform1 1"'), (6000, 'qa_vehicle_state rising'), (6200, 'quit')])
spawned = re.search(r'VEHICLE_QA spawn type=0 vehicle=(\d+)', text)
assert spawned and spawned.group(1) != '-1', text[-3000:]
car = int(spawned.group(1))
_, before = state(text, 'before')
_, rising = state(text, 'rising')
rise = position(rising[car])[2] - position(before[car])[2]
assert rise > 3 and rising[car]['grounded'] == '1', ('the Warthog rides the platform up', rise, before[car], rising[car])
print(f'c20: a Warthog on platform1 rose {rise:.1f} units with it.')
