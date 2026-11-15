"""Enemy and flying vehicles: the Ghost, Wraith, Scorpion and Banshee as real vehicles, the
player-flown Banshee, actors at the controls, dropship cargo and Pelican boarding, and c40's flown
sections (source/src/halo_vehicle.cpp, source/src/halo_script_ai.inc).

Written while testing was on hold: not yet run.

1. packages/halo/vehicles.txt format 2 (tools/halo_vehicles.py): the Ghost and the Wraith are
   hover craft, the Scorpion a tank, the Banshee a flyer; the player may drive all but the Wraith;
   weapons.txt gives the Banshee gun and the Scorpion cannon a second trigger (fuel rod, machine
   gun). c40 sections 1 and 4 are flown: an .hcol, and the planned vehicle route (VEHICLE_ROUTES, gap 5)
   as their .route.
2. b40 section 3 (retail collision, ten parked Ghosts, two Banshees, a Wraith): the engine reads
   the classes (qa_vehicle_state class=3 hover, 5 flyer); E by a Ghost drives it (prompt
   `E  Drive the Ghost`), the throttle moves it, the trigger fires its bolts, E gets out; the
   Wraith offers no seat to the player.
3. The Banshee: `E  Fly the Banshee`, climbing with the flight input lifts it off the floor and it
   hovers there (no fall recovery, no stall), the trigger fires the bolts and the secondary key a
   fuel rod, E in mid-air keeps the player in, descending lands it, E gets out.
4. A Scorpion spawned ahead of the player: driven forward, the cannon fires at most once every
   1.5 s while held, the machine gun on the secondary key.
5. Actors at the controls: an Elite put in a Ghost drives it and fires at the player (protected);
   an Elite put in a Banshee takes off and makes strafing runs (its phase changes, it fires);
   their own weapons stay quiet (armed=1).
6. a30 section 3: mission_rubble_wave_2 loads its squad into rubble_rock_cship, which flies in
   on its recording drawn in play (CARGO_QA object rows: recording=1, then flown=1); the riders
   ride at the seats and are set down by vehicle_unload (`Halo cargo: ... set down`).
7. a30 section 3: mission_extraction_cliff brings foehammer_cliff in; once the script makes it
   enterable the player boards it with E (`E  Board the Pelican`), the script's seat test passes,
   and its game_won ends the section (`game_won aboard the extraction`).
8. c40 sections 1 and 4 on retail collision: the engine reports them flown; section 1 keeps the
   scenario's fly_away_1 (no Banshee of the engine's own), section 4 gets one beside the player
   (`a Banshee for the flight`).
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
maps = root / 'packages/maps'
table = root / 'packages/halo/vehicles.txt'
require(table, 'run tools/halo_vehicles.py "$maps" (scripts/import-macos-assets.sh)')
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import DRIVEN_SECTIONS, FLOWN_SECTIONS, STOCK_SECTIONS, VEHICLE_ROUTES  # noqa: E402

HOVER, TANK, FLYER = '3', '4', '5'

# 1. The tables.
text = table.read_text()
assert text.startswith('format 2'), 'vehicles.txt format 2: re-run tools/halo_vehicles.py'
records, current = {}, None
for line in text.splitlines():
    if not line or line.startswith('#'):
        continue
    key, _, rest = line.partition(' ')
    if key == 'vehicle':
        current = records.setdefault(rest.split()[1], {})
    elif current is not None and key in ('class', 'player', 'weapon'):
        current[key] = rest
want = {'warthog': ('car', '1'), 'turret': ('turret', '1'), 'ghost': ('hover', '1'), 'scorpion': ('tank', '1'),
        'banshee': ('flyer', '1'), 'wraith': ('hover', '0')}
for name, (kind, player) in want.items():
    assert records[name]['class'] == kind and records[name]['player'] == player, (name, records[name])
weapons = (root / 'packages/halo/weapons.txt').read_text().split('\nweapon ')
def triggers(path):
    block = next(b for b in weapons if f'path {path}\n' in b)
    return [line for line in block.splitlines() if line.startswith('projectile ')]
assert [p.split()[-1] for p in triggers(r'vehicles\banshee\banshee gun')][-2:] == ['bolt', 'rod'], 'Banshee bolts and fuel rod'
assert len(triggers(r'vehicles\scorpion\scorpion cannon')) == 2, 'Scorpion cannon and machine gun'
assert FLOWN_SECTIONS <= DRIVEN_SECTIONS and not FLOWN_SECTIONS & STOCK_SECTIONS, FLOWN_SECTIONS
for part in (0, 3):
    name = f'halo_c40_0{part + 1}'
    assert (maps / f'{name}.hcol').is_file() and (maps / f'{name}.route').exists() == (('c40', part) in VEHICLE_ROUTES), \
        f'{name}: flown, an .hcol and its planned vehicle route (import_halo_campaign.py "$maps" --mission c40)'
print('tables: hover, tank and flyer classes, the Wraith enemy-only, two-trigger vehicle guns, c40 flown sections.')


def run(name, start, schedule, timeout=240):
    profile = scratch(f'enemyveh-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_vehicletrace 1; halo_scripttrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace')


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def state(text, label):
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


def distance(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** .5


# 2. b40 section 3: the parked Ghosts, Banshees and Wraith.
text = run('b40-find', 'halo_section 5 3', [(300, 'qa_vehicle_state find'), (600, 'quit')])
head, rows = state(text, 'find')
ghost = first(rows, 'ghost', exists='1')
banshee = first(rows, 'banshee', exists='1')
wraith = first(rows, 'wraith', exists='1')
assert rows[ghost]['class'] == HOVER and rows[banshee]['class'] == FLYER and rows[wraith]['class'] == HOVER, rows
scale = float(head['scale'])
text = run('b40-ghost', 'halo_section 5 3', [
    (300, 'qa_combat_protect 1'), (400, f'qa_vehicle_approach {ghost} 0'), (600, 'qa_vehicle_use'),
    (800, 'qa_vehicle_state seated'), (900, 'qa_vehicle_input 1 0 2500'), (3600, 'qa_vehicle_state driven'),
    (3700, 'qa_halo_fire 1'), (4700, 'qa_halo_fire 0; qa_vehicle_state fired'), (4900, 'qa_vehicle_use'),
    (5300, 'qa_vehicle_state out'), (5500, f'qa_vehicle_approach {wraith} 0'), (5700, 'quit')])
assert re.search(rf'VEHICLE_QA approach vehicle={ghost} found=1 prompt=E  Drive the Ghost', text), text[-3000:]
head, rows = state(text, 'seated')
assert head['seated'] == '1' and head['role'] == '0' and head['gun'] != '-1', ('the Ghost driver works its guns', head)
start = position(rows[ghost])
head, rows = state(text, 'driven')
moved = distance(position(rows[ghost]), start)
assert moved > 6 and rows[ghost]['flipped'] == '0', ('the Ghost drives', moved, rows[ghost])
head, _ = state(text, 'fired')
assert int(head['shots']) > 3, ('the Ghost bolts fire', head)
head, _ = state(text, 'out')
assert head['seated'] == '0' and head['alive'] == '1', head
assert not re.search(rf'VEHICLE_QA approach vehicle={wraith} found=1 prompt=E  Drive the Wraith', text), 'the Wraith is the Covenant\'s'
print(f'b40: Ghost driven {moved:.1f} units with its bolts; the Wraith offers the player no seat.')

# 3. The Banshee.
text = run('b40-banshee', 'halo_section 5 3', [
    (300, 'qa_combat_protect 1'), (400, f'qa_vehicle_approach {banshee} 0'), (600, 'qa_vehicle_use'),
    (800, 'qa_vehicle_state landed'), (900, 'qa_vehicle_look 0 0; qa_vehicle_flight 0 0 1 2500'),
    (3600, 'qa_vehicle_state up'), (5600, 'qa_vehicle_state hover'),
    (5700, 'qa_halo_fire 1'), (6700, 'qa_halo_fire 0; qa_vehicle_secondary 1'), (6900, 'qa_vehicle_secondary 0; qa_vehicle_state fired'),
    (7000, 'qa_vehicle_use'), (7300, 'qa_vehicle_state midair'),
    (7400, 'qa_vehicle_flight 0 0 -1 6000'), (13600, 'qa_vehicle_state down'), (13800, 'qa_vehicle_use'),
    (14200, 'qa_vehicle_state out'), (14400, 'quit')])
assert re.search(rf'VEHICLE_QA approach vehicle={banshee} found=1 prompt=E  Fly the Banshee', text), text[-3000:]
_, rows = state(text, 'landed')
ground = position(rows[banshee])[2]
_, rows = state(text, 'up')
assert position(rows[banshee])[2] > ground + 3 * scale, ('climbing lifts the Banshee', ground, rows[banshee])
_, hover = state(text, 'hover')
assert abs(position(hover[banshee])[2] - position(rows[banshee])[2]) < 1.5 * scale, ('a piloted Banshee hovers', rows[banshee], hover[banshee])
assert 'Halo vehicle recovery' not in text, ('flying is neither a fall nor a stall', text[-3000:])
head, _ = state(text, 'fired')
assert int(head['shots']) > 3, ('bolts and the fuel rod', head)
head, _ = state(text, 'midair')
assert head['seated'] == '1', ('no getting out in mid-air', head)
_, rows = state(text, 'down')
assert position(rows[banshee])[2] < ground + 1.5 * scale, ('descending lands it', rows[banshee])
head, _ = state(text, 'out')
assert head['seated'] == '0' and head['alive'] == '1', head
print('b40: Banshee flown up, hovered, fired bolts and a fuel rod, kept the player in mid-air, landed, left.')

# 4. A Scorpion spawned ahead of the player (no scenario Scorpion stands in a campaign section).
text = run('b40-scorpion', 'halo_section 5 3', [
    (300, 'qa_combat_protect 1'), (400, 'qa_vehicle_spawn 3 14'), (700, 'qa_vehicle_state spawned'), (800, 'quit')])
spawned = re.search(r'VEHICLE_QA spawn type=3 vehicle=(\d+)', text)
assert spawned, text[-3000:]
tank = int(spawned.group(1))
text = run('b40-tank', 'halo_section 5 3', [
    (300, 'qa_combat_protect 1'), (400, 'qa_vehicle_spawn 3 14'), (700, f'qa_vehicle_enter {tank} 0'),
    (900, 'qa_vehicle_state seated'), (1000, 'qa_vehicle_input 1 0 3000'), (4200, 'qa_vehicle_state driven'),
    (4300, 'qa_halo_fire 1'), (7300, 'qa_halo_fire 0; qa_vehicle_state cannon'),
    (7400, 'qa_vehicle_secondary 1'), (8400, 'qa_vehicle_secondary 0; qa_vehicle_state gun'), (8600, 'quit')])
head, rows = state(text, 'seated')
assert head['seated'] == '1' and rows[tank]['class'] == TANK, (head, rows[tank])
start = position(rows[tank])
_, rows = state(text, 'driven')
assert distance(position(rows[tank]), start) > 4, ('the Scorpion drives', rows[tank])
head, _ = state(text, 'cannon')
cannon = int(head['shots'])
assert 1 <= cannon <= 3, ('the cannon holds 1.5 s between rounds', cannon)
head, _ = state(text, 'gun')
assert int(head['shots']) > cannon + 3, ('the machine gun on the secondary key', head)
print(f'b40: Scorpion driven, {cannon} cannon rounds in 3 s, machine gun on the secondary key.')

# 5. Actors at the controls.
text = run('b40-pilots', 'halo_section 5 3', [
    (300, 'qa_combat_protect 1; qa_vehicle_state before'), (500, f'qa_vehicle_pilot {ghost} 1 0 ""'),
    (700, f'qa_vehicle_pilot {banshee} 1 0 ""')] +
    [(1000 + 1000 * i, f'qa_vehicle_state t{i}') for i in range(14)] + [(15500, 'quit')])
pilots = [fields(line) for line in text.splitlines() if line.startswith('VEHICLE_QA pilot ')]
assert len(pilots) == 2 and all(p['ok'] == '1' and p['armed'] == '1' for p in pilots), pilots
_, before = state(text, 'before')
samples = [state(text, f't{i}')[1] for i in range(14)]
ghost_moved = max(distance(position(s[ghost]), position(before[ghost])) for s in samples)
assert ghost_moved > 4 and samples[-1][ghost]['pilot'] != '-', ('an Elite drives the Ghost', ghost_moved, samples[-1][ghost])
heights = [position(s[banshee])[2] for s in samples]
assert max(heights) > position(before[banshee])[2] + 3 * scale, ('an Elite flies the Banshee', heights)
phases = {s[banshee]['phase'] for s in samples}
assert len(phases) > 1, ('strafing runs: approach, dive, pull out', phases)
assert any(s[ghost]['shots'] == '1' for s in samples) and any(s[banshee]['shots'] == '1' for s in samples), \
    ('the vehicles fire for their pilots', samples[-1][ghost], samples[-1][banshee])
print(f'b40: an Elite drove a Ghost {ghost_moved:.1f} units and flew a Banshee through phases {sorted(phases)}, both firing.')


def cargo_rows(text, label, kind):
    return [fields(line) for line in text.splitlines() if line.startswith(f'CARGO_QA {label} {kind}=')]


# 6. a30 section 3: the rubble dropship.
schedule = [(300, 'qa_combat_protect 1; qa_hs_script mission_rubble_wave_2 0')]
schedule += [(1000 + 2000 * i, f'qa_cargo_state c{i}') for i in range(30)] + [(62000, 'quit')]
text = run('a30-dropship', 'halo_section 2 3', schedule, timeout=300)
riding = [r for i in range(30) for r in cargo_rows(text, f'c{i}', 'rider') if r['vehicle'] == 'rubble_rock_cship' and r['walking'] == '0']
assert riding, ('the squad rides in the dropship', text[-3000:])
flying = [r for i in range(30) for r in cargo_rows(text, f'c{i}', 'object') if r['object'] == 'rubble_rock_cship']
assert any(r['recording'] == '1' for r in flying) and any(r['flown'] == '1' for r in flying), ('drawn on its recording', flying[:3])
assert 'set down by rubble_rock_cship' in text, ('vehicle_unload sets the riders down', text[-3000:])
print(f'a30: rubble_rock_cship flew its squad in ({len(riding)} rider samples) and set them down.')

# 7. a30 section 3: the cliff extraction Pelican.
schedule = [(300, 'qa_combat_protect 1; qa_hs_script mission_extraction_cliff 0')]
schedule += [(2000 + 3000 * i, f'qa_cargo_state p{i}') for i in range(20)]
schedule += [(62000, 'qa_cargo_approach foehammer_cliff'), (62300, 'qa_cargo_board'), (62600, 'qa_cargo_state aboard'),
             (80000, 'qa_campaign_state'), (80500, 'quit')]
text = run('a30-extraction', 'halo_section 2 3', schedule, timeout=300)
pelican = [r for i in range(20) for r in cargo_rows(text, f'p{i}', 'object') if r['object'] == 'foehammer_cliff']
assert pelican and pelican[-1]['boardable'] == '1' and pelican[-1]['recording'] == '0', ('landed and enterable', pelican[-3:])
assert re.search(r'CARGO_QA approach object=foehammer_cliff ok=1 prompt=E  Board the Pelican', text), text[-3000:]
assert re.search(r'CARGO_QA board prompt=\S.* ok=1 boarded=1 driving=1', text), text[-3000:]
assert 'Halo: player aboard foehammer_cliff' in text, text[-3000:]
assert 'game_won aboard the extraction' in text, ('boarding lets the extraction script end the mission', text[-3000:])
print('a30: foehammer_cliff landed, the player boarded it with E and the extraction ended the section.')

# 8. c40's flown sections.
text = run('c40-1', 'halo_section 8 1', [(300, 'qa_vehicle_flown'), (3500, 'qa_vehicle_state flight'), (4000, 'quit')])
assert 'retail collision surfaces for halo_c40_01' in text, 'c40 section 1 plays on retail collision'
assert 'VEHICLE_QA flown mission=8 section=1 flown=1' in text, text[-3000:]
_, rows = state(text, 'flight')
assert any(r['type'] == 'banshee' and r['exists'] == '1' and r['object'] != '-1' for r in rows), ('fly_away_1 stands in the canyon', rows)
assert 'a Banshee for the flight' not in text, 'the scenario\'s Banshee serves: no second one'
text = run('c40-4', 'halo_section 8 4', [(300, 'qa_vehicle_flown'), (3500, 'qa_vehicle_state flight'), (4000, 'quit')])
assert 'retail collision surfaces for halo_c40_04' in text and 'flown=1' in text, text[-3000:]
assert 'a Banshee for the flight' in text, text[-3000:]
_, rows = state(text, 'flight')
assert any(r['type'] == 'banshee' and r['exists'] == '1' and r['object'] == '-1' for r in rows), rows
print('c40: sections 1 and 4 on retail collision are flown; section 1 keeps fly_away_1, section 4 gets a Banshee.')
