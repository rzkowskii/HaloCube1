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

Batch 5's critic, gaps 8 and 11 (written while testing was on hold, not yet run):
5. Covered riders (gap 8): a frag grenade going off 0.3 units beside a Wraith with an Elite at its
   controls (qa_vehicle_blast) leaves the Elite's health and shield as they were (covered=1, the hull
   covers the invisible seat) and lowers the Wraith's health; the same blast beside a Ghost with an
   Elite on it hurts the Elite (covered=0: the Ghost's seat stays open) and the Ghost.
6. The death explosion (gap 11): a Grunt put 1.3 half-widths out from the Wraith's middle takes the
   death explosion's jpt! damage when the Wraith is destroyed (`death explosion deals its damage`, a
   DAMAGE_TRACE from the player of 55 to 65 on Normal); effects.txt gives the death explosion that
   `damage` line (1.5 2.5 55 55 65) and a modifier of 0 for metal (no chain of explosions).
7. The tilted hit box (gap 11): a level probe across the parked Wraith above its box's top behind the
   middle misses the level box and hits it pitched nose-down 30 degrees (qa_vehicle_tilt,
   qa_vehicle_probe); one above the top at the middle misses level and hits rolled 30 degrees.
8. The wreck (gap 11, halo_vehiclewreck, off by default): with it off the destroyed Wraith of item 2
   leaves no wreck (wreck=0); with it on, a checkpoint, then the Wraith destroyed: its row has wreck=1,
   qa_vehicle_wrecks finds it and it stops the player's body; after a death and revert to the
   checkpoint the Wraith exists again and no wreck is left.

Batch 7, the critic's worry 3 of batch 6 (written while testing was on hold, not yet run):
9. The death explosion is nobody's: a marine on the human team (allied with the player at the start)
   put beside the Wraith as in item 6 takes the explosion when the player destroys the Wraith (a
   DAMAGE_TRACE naming it), and no kill counts towards a betrayal (FACTION_BETRAYALS ... human=0 after
   it, no "the human turn on the player"), whether or not the marine survives it.
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
ended = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_HEALTH', 'end')}
assert ended[wraith].get('wreck') == '0', ('no wreck while halo_vehiclewreck is off (the default)', ended[wraith])
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

# 5. Blasts leave a covered rider to the hull (gap 8); the Ghost's open seat still takes them.
text = run('covered', [
    (300, 'qa_combat_protect 1'), (400, f'qa_vehicle_pilot {wraith} 1 0 driver'), (500, f'qa_vehicle_pilot {ghost} 1 0 driver'),
    (800, f'qa_vehicle_blast {wraith} 0 0.3'), (1100, f'qa_vehicle_blast {ghost} 0 0.3'), (1400, 'quit')])
blasts = {int(f['vehicle']): f for f in (fields(l) for l in text.splitlines() if l.startswith('VEHICLE_QA blast '))}
assert blasts.get(wraith, {}).get('ok') == '1' and blasts[wraith]['pilot'] != '-', ('a frag beside the piloted Wraith', blasts, text[-3000:])
assert blasts[wraith]['covered'] == '1' and blasts[wraith]['alive'] == '1', blasts[wraith]
assert blasts[wraith]['before'] == blasts[wraith]['after'], ('the Wraith\'s driver is left to the hull', blasts[wraith])
after = [r for r in rows(text, 'VEHICLE_HEALTH', 'blast') if int(r['vehicle']) == wraith]
assert float(after[0]['health']) < 1, ('the Wraith\'s hull takes the blast', after[0])
assert blasts.get(ghost, {}).get('ok') == '1' and blasts[ghost]['covered'] == '0', ('the Ghost\'s seat is open', blasts.get(ghost))
hp0, sh0 = (float(x) for x in blasts[ghost]['before'].split(','))
hp1, sh1 = (float(x) for x in blasts[ghost]['after'].split(','))
assert blasts[ghost]['alive'] == '0' or (hp1, sh1) < (hp0, sh0), ('the Ghost\'s rider takes the blast', blasts[ghost])
ghosted = [r for r in rows(text, 'VEHICLE_HEALTH', 'blast') if int(r['vehicle']) == ghost]
assert float(ghosted[-1]['health']) < 1, ('the ridden Ghost takes it too', ghosted[-1])
print(f'blasts: the Wraith\'s driver untouched (hull {float(after[0]["health"]):.2f}); the Ghost\'s rider hurt.')

# 6. The death explosion's jpt! damage (gap 11).
death, metal, current = None, None, None
for line in effects.splitlines():
    if line.startswith('path '):
        current = line[5:]
    elif line.startswith('damage ') and current == records['wraith']['destroyed']:
        death = [float(x) for x in line.split()[1:]]
    elif line.startswith('materials ') and current == records['wraith']['destroyed']:
        metal = float(line.split()[1 + 7])
assert death and death[:5] == [1.5, 2.5, 55, 55, 65], ('effects.txt: the death explosion\'s damage (re-run tools/halo_effects.py)', death)
assert metal == 0, ('metal takes none of it: no chain of vehicle explosions', metal)
text = run('explosion', [
    (300, 'qa_combat_protect 1'), (400, f'qa_vehicle_approach {wraith} 0'), (500, 'qa_actor_spawn 0 0 3 0'),
    (700, f'qa_vehicle_place_actor {wraith} 0 1.3'), (800, f'qa_vehicle_pilot {wraith} 1 0 driver'),
    (1000, f'qa_vehicle_damage {wraith} 2000 1'), (1300, 'quit')])
placed = re.search(r'VEHICLE_QA place actor vehicle=\d+ ok=1 name=(\S+) inside=-1', text)
assert placed, ('a Grunt beside the Wraith, outside its box', text[-3000:])
assert re.search(r"Halo vehicle: the Wraith's death explosion deals its damage", text), text[-3000:]
hits = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TRACE ') and f' target={placed.group(1)} ' in l + ' ']
assert hits and hits[0]['attacker'] == 'player' and 55 - .01 <= float(hits[0]['amount']) <= 65 + .01, ('the Grunt takes the explosion', hits)
print(f'death explosion: {placed.group(1)} beside the Wraith took {float(hits[0]["amount"]):.1f}.')

# 7. The tilted hit box (gap 11).
import math
text = run('tilt', [
    (300, f'qa_vehicle_probe {wraith} 0 0'), (500, 'quit')])
box = fields(next(l for l in text.splitlines() if l.startswith('VEHICLE_QA probe ')))
back, front, halfw, top = (float(box[k]) for k in ('back', 'front', 'halfwidth', 'top'))
t = math.radians(-30)
x, z = .8 * back, .8 * top
along, up = x * math.cos(t) - z * math.sin(t), x * math.sin(t) + z * math.cos(t)
assert up > top and back < along < front, ('the pitch probe is above the level box', along, up, box)
roll_up = 1.15 * top
assert halfw * .5 + top * math.cos(math.radians(30)) > roll_up, ('the roll probe is below the rolled box\'s raised edge', box)
middle = (back + front) / 2
text = run('tilted', [
    (300, f'qa_vehicle_probe {wraith} {along:.3f} {up:.3f}; qa_vehicle_probe {wraith} {middle:.3f} {roll_up:.3f}'),
    (400, f'qa_vehicle_tilt {wraith} -30 0'), (500, f'qa_vehicle_probe {wraith} {along:.3f} {up:.3f}'),
    (600, f'qa_vehicle_tilt {wraith} 0 30'), (700, f'qa_vehicle_probe {wraith} {middle:.3f} {roll_up:.3f}'), (900, 'quit')])
probes = [fields(l) for l in text.splitlines() if l.startswith('VEHICLE_QA probe ')]
assert len(probes) == 4 and all(p['ok'] == '1' for p in probes), probes
assert probes[0]['hit'] == '0' and probes[1]['hit'] == '0', ('the level box is missed above its top', probes)
assert probes[2]['hit'] == '1', ('pitched nose-down, its raised back is hit', probes[2])
assert probes[3]['hit'] == '1', ('rolled, its raised side is hit', probes[3])
print('hit box: pitched and rolled boxes are hit where the level box is not.')

# 8. The wreck (gap 11, halo_vehiclewreck 1), gone again after a revert to before it.
text = run('wreck', [
    (300, 'qa_combat_protect 1; halo_vehiclewreck 1; qa_checkpoint_save'), (500, f'qa_vehicle_pilot {wraith} 1 0 driver'),
    (700, f'qa_vehicle_damage {wraith} 2000 1'), (900, f'qa_vehicle_wrecks {wraith}; qa_vehicle_state wrecked'),
    (1100, 'qa_combat_protect 0; qa_halo_damage 100000'), (2700, 'qa_halo_revert'), (4200, f'qa_vehicle_wrecks {wraith}; qa_vehicle_state reverted'),
    (4400, 'quit')])
assert re.search(r"Halo vehicle: the Wraith leaves its wreck at", text), text[-3000:]
wrecks = [fields(l) for l in text.splitlines() if l.startswith('VEHICLE_QA wrecks ')]
assert len(wrecks) == 2, (wrecks, text[-3000:])
assert wrecks[0]['wreck'] == '1' and wrecks[0]['blocked'] == '1' and int(wrecks[0]['count']) >= 1, ('the wreck is solid', wrecks[0])
wrecked = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_HEALTH', 'wrecked')}
assert wrecked[wraith]['wreck'] == '1' and wrecked[wraith]['destroyed'] == '1', wrecked[wraith]
assert wrecks[1]['wreck'] == '0' and wrecks[1]['count'] == '0', ('the revert goes back to before the wreck', wrecks[1])
reverted = {int(r['vehicle']): r for r in rows(text, 'VEHICLE_QA', 'reverted')[1:]}
assert reverted[wraith]['exists'] == '1', ('the Wraith is back after the revert', reverted[wraith])
print('wreck: left where the Wraith blew up, solid, and gone after a revert to the checkpoint before it.')

# 9. The death explosion is nobody's: no betrayal (the fresh-start batch, the critic's worry 3 of batch 6).
text = run('explosion-ally', [
    (300, 'qa_combat_protect 1; qa_allegiance_state before'), (400, f'qa_vehicle_approach {wraith} 0'), (500, 'qa_actor_spawn 9 1 3 0'),
    (700, f'qa_vehicle_place_actor {wraith} 0 1.3'), (800, f'qa_vehicle_pilot {wraith} 1 0 driver'),
    (1000, f'qa_vehicle_damage {wraith} 2000 1'), (1300, 'qa_allegiance_state after'), (1400, 'quit')])
allied = [l for l in text.splitlines() if l.startswith('FACTION_ALLEGIANCE before ') and ' human-player ' in l + ' ']
assert allied and 'allied=1' in allied[0], ('the marines start allied with the player', allied, text[-3000:])
placed = re.search(r'VEHICLE_QA place actor vehicle=\d+ ok=1 name=(\S+) inside=-1', text)
assert placed, ('a marine beside the Wraith, outside its box', text[-3000:])
assert re.search(r"Halo vehicle: the Wraith's death explosion deals its damage", text), text[-3000:]
hits = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TRACE ') and f' target={placed.group(1)} ' in l + ' ']
assert hits, ('the marine takes the explosion', text[-3000:])
counted = [l for l in text.splitlines() if l.startswith('FACTION_BETRAYALS after human=')]
assert counted == ['FACTION_BETRAYALS after human=0'], ('the explosion is nobody\'s: no betrayal', counted)
assert 'Halo: the human turn on the player' not in text, text[-3000:]
print(f'death explosion: the marine {placed.group(1)} beside the Wraith took {float(hits[0]["amount"]):.1f}; no betrayal counted.')
