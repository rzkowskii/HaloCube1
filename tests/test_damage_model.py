"""Damage fidelity: jpt! range and blast falloff, actors' grenades and shots, EMP.

Not yet run: written while testing was on hold (see docs/macos-build.md, "Retail weapons",
"Damage fidelity").

Tables (no game): weapons.txt is format 3 with each projectile's area inner radius, air damage
range and side effect; the grenade blasts carry their lower bound; and the time an actor's gun
takes to kill the player on Normal (shots from full shield and health, the cadence unchanged)
stays within 16% of the integer slot damage it replaced at close range, for every actor variant
and weapon in actors.txt (the Sentinel beam by its 1-point floor).

a30 section 2 (no enemies): the engine's damage table (qa_damage_table) falls from the upper
bound to the lower across a projectile's damage range and a grenade's radius bounds, actors'
grenade rolls cover [lower, upper max] while the player's stay at the retail roll; exact hits
carry their fraction to the next hit; EMP drains the shield and the overshield.

d40 section 3 (stock geometry): an EMP stuns a Warthog the player drives (the throttle does
nothing until it wears off).

a30 section 3 (Covenant): with halo_damagetrace, every shot an actor lands on the (protected)
player is an exact hit within its weapon's [lower, upper max] times the variant's modifier
(at least 1), some of them fractional; an actor's grenade blast deals at least the lower bound;
the player's EMP hit drains an Elite's shield.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

WEAPONS = root / 'packages/halo/weapons.txt'
EQUIPMENT = root / 'packages/halo/equipment.txt'
ACTORS = root / 'packages/halo/actors.txt'
for path in (WEAPONS, EQUIPMENT, ACTORS):
    require(path, 'run python3 tools/halo_weapons.py and tools/halo_actors.py with the expanded caches')
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
GUN_SUBGUN, GUN_GRENADE = 4, 7


def close(a, b, tolerance=1e-3):
    return abs(a - b) <= tolerance * max(1.0, abs(b))


# ---- Tables
assert WEAPONS.read_text().startswith('format 3\n'), 're-run tools/halo_weapons.py (format 3)'
weapons, current = {}, None
for line in WEAPONS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'weapon':
        current = {'index': int(rest), 'triggers': []}
        weapons[current['index']] = current
    elif current is None:
        continue
    elif key == 'path':
        current['path'] = rest
    elif key == 'trigger':
        current['triggers'].append({'values': [float(x) for x in rest.split()], 'p': None})
    elif key == 'projectile' and rest != '-':
        f = rest.split(' ', 15)
        current['triggers'][-1]['p'] = dict(speed=float(f[0]), guided=float(f[4]), lower=float(f[5]), umin=float(f[6]),
                                            umax=float(f[7]), cat=int(float(f[8])), outer=float(f[9]), inner=float(f[10]),
                                            start=float(f[11]), end=float(f[12]), side=int(float(f[13])), path=f[15])
by_path = {w['path']: w for w in weapons.values()}
pellet = by_path[r'weapons\shotgun\shotgun']['triggers'][0]['p']
assert (pellet['start'], pellet['end'], pellet['lower'], pellet['umin']) == (1.5, 3, 8, 18), pellet
charged = by_path[r'weapons\plasma pistol\plasma pistol']['triggers'][1]['p']
assert charged['side'] == 3, ('the charged plasma bolt is EMP', charged)
rocket = by_path[r'weapons\rocket launcher\rocket launcher']['triggers'][0]['p']
assert (rocket['inner'], rocket['outer'], rocket['lower']) == (0.5, 2, 80), rocket

blasts = {}
for line in EQUIPMENT.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'grenade':
        grenade = int(rest.split()[0])
    elif key == 'blast':
        blasts[grenade] = [float(x) for x in rest.split()]
assert blasts[0][2:5] == [80, 120, 120] and blasts[1][2:5] == [80, 120, 120] and blasts[1][5] == 3, blasts

actors, current = [], None
for line in ACTORS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'actor':
        current = {}
        actors.append(current)
    elif current is None:
        continue
    elif key == 'path':
        current['path'] = rest
    elif key == 'weapon':
        current['weapon'] = int(rest)
    elif key == 'family':
        current['kind'] = int(rest.split()[1])
    elif key == 'fire':
        current['fire'] = [float(x) for x in rest.split()]


def shots_to_kill(per_shot, factor, pellets, carry_fraction):
    """Shots from 75 shield and 75 health on Normal, as halodamage takes them: the shield absorbs
    `factor` times the damage (2 for plasma and needles), the rest goes to health (the old code
    rounded it up; the exact code carries the fraction)."""
    shield, health, carry, shots = 75.0, 75.0, 0.0, 0
    while health > 0:
        shots += 1
        for _ in range(pellets):
            absorbed = min(shield, per_shot * factor)
            shield -= absorbed
            rest = max(0.0, per_shot - absorbed / factor)
            if carry_fraction:
                rest += carry
                whole = int(rest)
                carry = rest - whole
            else:
                whole = max(0, -int(-(rest - 0.001) // 1))
            health -= whole
            if health <= 0:
                break
    return shots


rows = []
for a in actors:
    w = weapons.get(a.get('weapon', -1))
    p = w['triggers'][0]['p'] if w and w['triggers'] else None
    if not p or a.get('kind') == 9:       # marines are allies
        continue
    scale = a['fire'][8] if a['fire'][8] > 0 else 0.5
    pellets = max(1, int(w['triggers'][0]['values'][5]))
    factor = 2 if (p['cat'] == 10 or p['guided'] > 0) and p['outer'] <= 0 else 1
    old = max(1, int(p['umin'] * scale + .5))
    mean = (p['umin'] + p['umax']) / 2
    new = max(1.0, mean * scale)
    before, after = shots_to_kill(old, factor, pellets, False), shots_to_kill(new, factor, pellets, True)
    rows.append((a['path'], w['path'], scale, old, new, before, after))
    assert abs(after - before) <= .16 * before + 1e-9, ('time to kill on Normal moved by more than 16%', rows[-1])
print(f'Tables: format 3 projectiles, grenade lower bounds, {len(rows)} actor variants within 16% of their old time to kill.')


def run(name, start, schedule, timeout=120):
    profile = scratch(f'damage-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_damagetrace 1; halo_vehicletrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def labelled(text, prefix):
    found = {}
    for line in text.splitlines():
        if line.startswith(prefix + ' '):
            parts = line.split()
            found.setdefault(parts[1], fields(line))
    return found


# ---- a30 section 2: the damage table, exact hits, EMP on the player
text, log = run('table', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_damage_table'),
    (300, 'qa_damage_vitals 75 0'),
    (400, 'qa_damage_hit 0.5 0 0 0'), (450, 'qa_damage_hit 0.5 0 0 0'), (500, 'qa_damage_hit 0.5 0 0 0'),
    (600, 'qa_damage_vitals 75 75'),
    (700, 'qa_damage_hit 10 0 0 0'),
    (800, 'qa_damage_hit 1 3 0 0'),
    (900, 'qa_damage_hit 30 0 0 0'),
    (1000, 'qa_damage_vitals 75 75; qa_item_drop 2 0 1'),
    (3000, 'qa_combat_state overshield'),
    (3100, 'qa_damage_hit 1 3 0 0'),
    (3300, 'quit'),
])
table = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TABLE ')]
assert len(table) >= len(weapons), (len(table), log)
index = {w['path']: str(w['index']) for w in weapons.values()}
row = next(r for r in table if r['weapon'] == index[r'weapons\shotgun\shotgun'] and r['trigger'] == '0')
assert close(float(row['at0']), 18) and close(float(row['atStart']), 18) and close(float(row['atMid']), 13) \
    and close(float(row['atEnd']), 8) and close(float(row['atFar']), 8), ('pellets fall from 18 to 8 over 1.5-3', row)
row = next(r for r in table if r['weapon'] == index[r'weapons\plasma rifle\plasma rifle'] and r['trigger'] == '0')
assert close(float(row['atStart']), 12) and close(float(row['atMid']), 11) and close(float(row['atFar']), 10), row
row = next(r for r in table if r['weapon'] == index[r'weapons\pistol\pistol'] and r['trigger'] == '0')
assert close(float(row['atFar']), 25), ('a flat jpt! (lower = upper) has no falloff', row)
falloff = re.findall(r'^DAMAGE_QA falloff (\S+) (\S+) (\S+) = (\S+)', text, re.M)
assert [float(f[3]) for f in falloff] == [1, 1, .5, 0, 0, 1, 1, 0], falloff
grenades = {int(r['type']): r for r in (fields(l) for l in text.splitlines() if l.startswith('DAMAGE_GRENADE '))}
for t, g in grenades.items():
    assert close(float(g['centre']), 120) and close(float(g['mid']), 100) and close(float(g['edge']), 80), \
        ('blasts fall from 120 to the 80 lower bound at the outer radius', g)
    assert close(float(g['player_min']), 120) and close(float(g['player_max']), 120), ('the player keeps the retail roll', g)
    assert float(g['actor_min']) >= 80 - 1e-3 and float(g['actor_max']) <= 120 + 1e-3 and \
        float(g['actor_min']) < 90 and float(g['actor_max']) > 110, ('actors roll [lower, upper max]', g)
assert grenades[1]['side'] == '3' and float(grenades[1]['attached_actor_min']) < 90, grenades
vitals = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_QA hit ')]
assert len(vitals) == 7, (vitals, log)
assert vitals[2]['hp'] == '74' and close(float(vitals[2]['carry']), .5), ('three half points: one taken, a half carried', vitals[:3])
assert vitals[3]['hp'] == '75' and close(float(vitals[3]['shield']), 65), ('the shield takes a hit first', vitals[3])
assert vitals[4]['hp'] == '75' and float(vitals[4]['shield']) == 0, ('EMP drains the shield', vitals[4])
assert vitals[5]['hp'] == '45', ('with no shield the hit reaches health', vitals[5])
over = labelled(text, 'COMBAT_QA')['overshield']
assert float(over['overshield']) > 100, ('the overshield charged', over)
assert float(vitals[6]['overshield']) == 0 and float(vitals[6]['shield']) == 0 and vitals[6]['hp'] == '75', \
    ('EMP drains the overshield and the shield', vitals[6])
print('a30 section 2: range and blast falloff to the lower bound, actor grenade rolls, exact hits, EMP on the player.')

# ---- d40 section 3 (stock): EMP stuns the Warthog the player drives
text, log = run('vehicle', 'halo_section 10 3', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_vehicle_spawn 0 8'), (600, 'qa_vehicle_enter 0 0'), (700, 'qa_vehicle_state before'),
    (800, 'qa_damage_emp_vehicle 0 3000; qa_vehicle_state stunned'),
    (900, 'qa_vehicle_input 1 0 2400'), (3500, 'qa_vehicle_state stalled'),
    (4000, 'qa_vehicle_input 1 0 4000'), (8200, 'qa_vehicle_state driven'), (8400, 'quit'),
])
assert 'DAMAGE_QA emp vehicle=0 stunned=1' in text, log


def vehicle(label):
    rows = [fields(l) for l in text.splitlines() if l.startswith(f'VEHICLE_QA {label} vehicle=0 ')]
    assert rows, (label, log)
    return rows[0]


def moved(a, b):
    pa, pb = (tuple(map(float, r['pos'].split(','))) for r in (a, b))
    return sum((x - y) ** 2 for x, y in zip(pa, pb)) ** .5


before, stunned, stalled, driven = (vehicle(n) for n in ('before', 'stunned', 'stalled', 'driven'))
assert int(stunned['stunned']) > 2500, stunned
assert moved(before, stalled) < 1.5, ('a stunned Warthog does not drive', before, stalled)
assert int(driven['stunned']) == 0 and moved(stalled, driven) > 5, ('it drives once the stun wears off', stalled, driven)
print(f'd40 section 3: EMP held the Warthog ({moved(before, stalled):.1f} units), then it drove {moved(stalled, driven):.1f}.')

# ---- a30 section 3: actors' exact shots and grenades on the player, EMP on an Elite
text, log = run('actors', 'qa_campaign_resume 2 3', [
    (100, 'qa_combat_protect 1'),
    (200, 'qa_campaign_reach'),
    (2200, 'qa_campaign_reach'),
    (4500, 'qa_damage_actors; qa_enemy_throw'),
    (14000, 'qa_damage_actors; qa_damage_actor 0 0 before-emp'),
    (14100, 'qa_damage_actor 1 3 emp'),
    (14300, 'quit'),
], timeout=150)
armed = {}
for line in text.splitlines():
    if line.startswith('DAMAGE_ACTOR weapon '):
        f = fields(line)
        name = line.split(' name=', 1)[1].split(' kind=', 1)[0]
        armed[name] = (int(f['weapon']), float(f['scale']))
# Actor names have a space ("Elite 3"): the trace's attacker field is the name up to it, so a
# shot is checked against every actor whose name starts with it.
trace = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TRACE ') and ' target=player ' in l]
shots = [t for t in trace if t['attacker'] not in ('player', '-') and t['exact'] == '1' and int(t['gun']) != GUN_GRENADE]
assert shots, ('actors land shots on the protected player', log)
fractional = 0
for t in shots:
    kinds = {v for name, v in armed.items() if name.startswith(t['attacker'])}
    bounds = []
    for index, scale in kinds:
        w = weapons.get(index)
        p = w['triggers'][0]['p'] if w and w['triggers'] else None
        if p:
            bounds.append((max(1.0, min(p['lower'], p['umin']) * scale), max(1.0, p['umax'] * scale)))
    amount = float(t['amount'])
    assert not bounds or any(lo - 1e-3 <= amount <= hi + 1e-3 for lo, hi in bounds), ('shot outside its weapon range', t, bounds)
    fractional += abs(amount - round(amount)) > .01
assert fractional, ('actor shots keep their fraction', shots[:10])
blasts = [t for t in trace if t['attacker'] not in ('player', '-') and int(t['gun']) == GUN_GRENADE and t['exact'] == '1']
for t in blasts:
    assert 80 - 1e-3 <= float(t['amount']) <= 240 + 1e-3, ('an actor grenade deals at least its lower bound', t)
emp = labelled(text, 'DAMAGE_ACTOR')
assert 'shield' in emp.get('before-emp', {}) and 'shield' in emp.get('emp', {}), ('a shielded actor to hit', emp, log)
assert float(emp['before-emp']['shield']) > 1 and float(emp['emp']['shield']) == 0, ('EMP drains an Elite shield', emp)
print(f'a30 section 3: {len(shots)} exact actor shots ({fractional} fractional), {len(blasts)} actor grenade blasts, '
      'EMP drained an Elite shield.')
