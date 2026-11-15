"""Vehicle destruction and the vehicle actor variants (source/src/halo_vehicle.cpp halovehiclehit,
halovehicletrace, variantOf; source/src/weapon.cpp; tools/halo_vehicles.py `vitality`, `destroyed`,
`variant` lines and the seat flags).

Written while testing was on hold: not yet run.

1. packages/halo/vehicles.txt: the collision models' body vitality and flags as the retail tags have
   them (Wraith 450, flags 0; Ghost 160 and Banshee 100, both "only damaged while occupied", the
   Banshee "takes body damage for its children"; the Warthog, Scorpion and turret 0, indestructible),
   the body depleted effect for the three that can blow up (in effects.txt too), the Wraith driver's
   invisible seat (flag 1) and a `variant` line naming an actors.txt variant for every vehicle.
2. b40 section 3 (retail collision; parked Ghosts, Banshees and a Wraith): every VEHICLE_HEALTH row
   starts whole with the variant's damage modifier and error from actors.txt; an empty Ghost takes
   no damage (taken=0); the Wraith takes 100 points (health about 1 - 100/450 on Normal) and
   `unit_get_health` of its scenario object follows; with an Elite at its controls a 2000-point blast
   destroys it: `the Wraith is destroyed`, its row exists=0 destroyed=1, the Elite is dead,
   `unit_get_health` reads -1 and the body depleted effect played (`halo_fxtrace`).
3. The vehicle actor variant (gap 13): a Grunt put at the gun of a spawned turret fires at the
   protected player with the turret gunner variant's modifier (0.333), not the Grunt's (0.5): every
   DAMAGE_TRACE amount it deals is at most the turret bolt's upper bound times 0.333.
4. The player's own weapon: facing the Wraith (qa_vehicle_aim) and firing hits its body (its health
   drops, `takes ... from the player` with halo_vehicletrace 1), with qa_vehicle_protect 1 nothing
   is hurt and shots pass.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
table = root / 'packages/halo/vehicles.txt'
require(table, 'run tools/halo_vehicles.py "$maps" (scripts/import-macos-assets.sh)')

# 1. The tables.
records, current = {}, None
for line in table.read_text().splitlines():
    if not line or line.startswith('#'):
        continue
    key, _, rest = line.partition(' ')
    if key == 'vehicle':
        current = records.setdefault(rest.split()[1], {'seats': []})
    elif current is not None and key == 'seat':
        current['seats'].append(rest.split())
    elif current is not None and key in ('vitality', 'destroyed', 'variant', 'path'):
        current[key] = rest
want = {'warthog': (0, None), 'turret': (0, None), 'scorpion': (0, None), 'ghost': (160, 64), 'banshee': (100, 64),
        'wraith': (450, 0)}
for name, (vitality, flag) in want.items():
    assert 'vitality' in records[name], (f'{name}: no vitality line: re-run tools/halo_vehicles.py', records[name])
    body, flags, material, multiplier = records[name]['vitality'].split()
    assert abs(float(body) - vitality) < .01, (name, body)
    if flag is not None:
        assert (int(flags) & 64) == flag, (name, flags)
        assert 'destroyed' in records[name], (name, 'a body depleted effect')
    assert records[name].get('variant', '').lower().startswith(records[name]['path'].lower() + ' '), (name, records[name].get('variant'))
assert int(records['banshee']['vitality'].split()[1]) & 2, 'the Banshee takes its pilot\'s damage'
assert int(records['wraith']['seats'][0][8]) & 1, 'the Wraith driver sits inside the hull (invisible seat)'
effects = (root / 'packages/halo/effects.txt').read_text()
assert f'path {records["wraith"]["destroyed"]}\n' in effects, 'effects.txt has the Wraith\'s death explosion (re-run tools/halo_effects.py)'
actors = (root / 'packages/halo/actors.txt').read_text()
for name in want:
    assert f'path {records[name]["variant"]}\n' in actors, (name, 'variant in actors.txt')
print('tables: body vitality and flags, death effects, the Wraith\'s covered seat, a variant per vehicle.')


def run(name, schedule, timeout=240):
    profile = scratch(f'vehdamage-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_vehicletrace 1; halo_fxtrace 1; halo_damagetrace 1; halo_section 5 3; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace')


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def rows(text, prefix, label):
    found = [fields(line) for line in text.splitlines() if line.startswith(f'{prefix} {label} ')]
    assert found, (prefix, label, text[-3000:])
    return found


def first(vehicles, kind):
    for row in vehicles:
        if row.get('type') == kind and row.get('exists') == '1':
            return int(row['vehicle'])
    raise AssertionError((kind, vehicles))


# 2. Health rows, the empty Ghost, the Wraith damaged and destroyed with its pilot.
text = run('find', [(300, 'qa_combat_protect 1; qa_vehicle_state find'), (600, 'quit')])
vehicles = rows(text, 'VEHICLE_QA', 'find')[1:]
health = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_HEALTH', 'find')}
for index, row in health.items():
    assert row['health'] == '1.000' and row['destroyed'] == '0', row
    assert int(row['variant']) >= 0, ('every vehicle names its actor variant', row)
ghost, wraith = first(vehicles, 'ghost'), first(vehicles, 'wraith')
count = len(vehicles)
name = health[wraith]['name']
assert name != '-', ('the Wraith is a scenario object', health[wraith])
text = run('wraith', [
    (300, 'qa_combat_protect 1'), (400, f'qa_vehicle_damage {ghost} 100 0'),
    (500, f'qa_vehicle_damage {wraith} 100 0'), (600, f'qa_hs_cmd unit_get_health "obj:{name}"'),
    (700, f'qa_vehicle_pilot {wraith} 1 0 driver'), (900, 'qa_actor_state piloted'),
    (1000, f'qa_vehicle_damage {wraith} 2000 1'), (1300, f'qa_hs_cmd unit_get_health "obj:{name}"; qa_actor_state after'),
    (1500, 'qa_vehicle_state end'), (1700, 'quit')])
taken = [fields(l) for l in text.splitlines() if l.startswith('VEHICLE_QA damage ')]
assert float(taken[0]['taken']) == 0, ('an empty Ghost takes no damage', taken[0])
assert float(taken[1]['taken']) > 0, ('the Wraith takes damage', taken[1])
damaged = [r for r in rows(text, 'VEHICLE_HEALTH', 'damage') if int(r['vehicle']) == wraith]
assert 0.6 < float(damaged[0]['health']) < 0.9, ('100 of 450 points', damaged[0])
cmds = [float(l.split()[-1]) for l in text.splitlines() if l.startswith('HS_CMD unit_get_health ')]
assert abs(cmds[0] - float(damaged[0]['health'])) < .01, ('unit_get_health follows the body health', cmds)
assert 'Halo vehicle: the Wraith is destroyed' in text, text[-3000:]
assert cmds[-1] == -1, ('a destroyed vehicle no longer exists for the scripts', cmds)
end = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_QA', 'end')[1:]}
assert end[wraith]['exists'] == '0', end[wraith]
assert re.search(r'death.explosion|death_explosion', text), ('the body depleted effect played', text[-3000:])
pilot = re.search(rf'VEHICLE_QA pilot vehicle={wraith} ok=1 name=(\S+)', text)
assert pilot, text[-3000:]
after = [fields(l) for l in text.splitlines() if l.startswith('ACTOR_QA after ') and f'name={pilot.group(1)} ' in l]
assert not after or after[0].get('alive') == '0', ('the pilot dies with the Wraith', after)
print(f'b40: an empty Ghost unhurt, the Wraith at {float(damaged[0]["health"]):.2f} after 100 points, destroyed with its pilot.')

# 3. A Grunt at a spawned turret's gun fires with the turret gunner variant's modifier.
modifier = None
for block in actors.split('\nactor '):
    if f'path {records["turret"]["variant"]}\n' in block:
        modifier = float(next(l for l in block.splitlines() if l.startswith('fire ')).split()[9])
assert modifier and modifier < .5, ('the turret gunner variant has its own modifier', modifier)
weapons = (root / 'packages/halo/weapons.txt').read_text()
bolt = next(l for l in weapons.splitlines() if l.startswith('projectile ') and l.endswith(r'vehicles\c gun turret\c gun turret'))
upper = float(bolt.split()[8])
text = run('turret', [
    (300, 'qa_combat_protect 1'), (400, 'qa_vehicle_spawn 1 8'), (600, f'qa_vehicle_pilot {count} 0 0 gunner'),
    (900, f'qa_vehicle_state gun'), (9000, 'quit')], timeout=300)
gunner = re.search(rf'VEHICLE_QA pilot vehicle={count} ok=1 name=(\S+) armed=1', text)
assert gunner, ('a Grunt at the turret\'s gun', text[-3000:])
amounts = [float(f['amount']) for f in (fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TRACE '))
           if f.get('attacker') == gunner.group(1) and f.get('target') == 'player']
assert amounts, ('the turret fired at the player', text[-3000:])
assert max(amounts) <= upper * modifier + .01, ('turret gunner modifier, not the Grunt\'s', max(amounts), upper, modifier)
print(f'turret: {len(amounts)} hits on the player, at most {max(amounts):.2f} ({upper} x {modifier}).')

# 4. The player's rifle on the Wraith, then with every vehicle protected.
text = run('rifle', [
    (300, 'qa_combat_protect 1'), (400, f'qa_vehicle_approach {wraith} 0'), (500, f'qa_vehicle_pilot {wraith} 1 0 driver'),
    (600, f'qa_vehicle_aim {wraith}'), (700, 'qa_halo_fire 1'), (2700, 'qa_halo_fire 0; qa_vehicle_state shot'),
    (2800, 'qa_vehicle_protect 1'), (2900, f'qa_vehicle_aim {wraith}; qa_halo_fire 1'),
    (4900, 'qa_halo_fire 0; qa_vehicle_state protected'), (5100, 'quit')])
shot = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_HEALTH', 'shot')}
protected = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_HEALTH', 'protected')}
assert float(shot[wraith]['health']) < 1, ('the rifle hits the Wraith\'s body', shot[wraith])
assert re.search(r'Halo vehicle \d+ \(Wraith\) takes [\d.]+ from the player', text), text[-3000:]
assert protected[wraith]['health'] == shot[wraith]['health'], ('qa_vehicle_protect stops vehicle damage', protected[wraith])
print('rifle: the player\'s shots hit the Wraith\'s body; protected vehicles take none.')
