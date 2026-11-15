"""Melee from behind, Hunter regions, the general material model, the needle supercombine, the AOE core.

Not yet run: written while testing was on hold (batch 9; see docs/macos-build.md, "Damage fidelity"
and "Factions and special enemies").

Tables (no game): weapons.txt's melee lines carry the jpt! side effect (2, lethal to the
unsuspecting, for every player weapon), the needle's projectile has its `supercombine` line (timer
0.75 s, 0.5-1.0 world units, 60 down to 40, EMP) and sound, the rocket its `core 0.6` line, both
grenade blasts in equipment.txt `core 0.4`; b30's Engineer is baked with its animation clips
(packages/models/halo/engineer/md3.cfg has md3anim lines; it used to be the bind pose alone).

a30 section 2 (no enemies), with Elites, a Hunter and a Flood combat form spawned (halo_damagetrace 1):
1. The engine parses them: DAMAGE_TABLE gives the rocket core 0.60, the needle super=1 with side
   effect 3 and the player weapons melee_side=2; DAMAGE_GRENADE core 0.40.
2. Melee (qa_damage_melee): from the front on a calm Elite it survives; from behind on an Elite in
   combat with the player it survives; from behind on a calm Elite with halo_meleeunsuspecting 0 it
   survives; from behind on a calm Elite it dies, with a DAMAGE_UNSUSPECTING line.
3. Hunter regions (qa_damage_region with the pistol's jpt!): from behind hunter skin (13); from the
   side at head height skin, at belly height armour (12); from the front the shield arm (32) at
   both heights (it is not firing: botsshoot 0); with halo_hunterregions 0 the side head hit is
   armour again.
4. The general material model: with halo_actormaterials 0 (the default) a sniper hit on an Elite
   logs no DAMAGE_MATERIAL; with 1 it logs material 14 at scale 1, the Elite's shield takes the
   sniper's elite-energy-shield modifier (2: twice the shield of the same hit with it off), and a
   Flood combat form takes 0.05 of a sniper hit (material 19).
5. The needle supercombine (qa_damage_needles): 6 needles at once set off nothing, 7 set off one
   super detonation (SUPERCOMBINE needles=7), and with halo_supercombine 0, 7 set off none.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

WEAPONS = root / 'packages/halo/weapons.txt'
EQUIPMENT = root / 'packages/halo/equipment.txt'
ENGINEER = root / 'packages/models/halo/engineer/md3.cfg'
for path in (WEAPONS, EQUIPMENT):
    require(path, 'run python3 tools/halo_weapons.py with the expanded caches')
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

# ---- Tables
weapons, current = {}, None
for line in WEAPONS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'weapon':
        current = {'index': int(rest), 'sounds': {}, 'core': [], 'super': None}
        weapons[current['index']] = current
    elif current is None or key.startswith('#'):
        continue
    elif key == 'path':
        current['path'] = rest
    elif key == 'label':
        current['class'] = rest.split()[1]
    elif key == 'melee':
        current['melee'] = [float(x) for x in rest.split()]
    elif key == 'core':
        current['core'].append(float(rest))
    elif key == 'supercombine':
        current['super'] = [float(x) for x in rest.split()]
    elif key == 'sound':
        role, _, tag = rest.partition(' ')
        current['sounds'][role] = tag
by_path = {w['path']: w for w in weapons.values()}


def weapon(name):
    return by_path[rf'weapons\{name}\{name}']


players = [w for w in weapons.values() if w['class'] in ('assault', 'pistol', 'shotgun', 'sniper', 'projectile')]
assert len(players) == 9, [w['path'] for w in players]
for w in players:
    assert len(w['melee']) == 4 and w['melee'][3] == 2, ('every player weapon melee is lethal to the unsuspecting', w['path'], w['melee'])
needler, rocket, pistol, sniper = weapon('needler'), weapon('rocket launcher'), weapon('pistol'), weapon('sniper rifle')
assert needler['super'] and needler['super'][:8] == [0.75, 0.5, 1, 40, 60, 60, 3, 3], needler['super']
assert needler['sounds'].get('supercombine', '').endswith('needler_super_expl'), needler['sounds']
assert rocket['core'] == [0.6] and not pistol['core'], (rocket['core'], pistol['core'])
cores = [float(l.split()[1]) for l in EQUIPMENT.read_text().splitlines() if l.startswith('core ')]
assert cores == [0.4, 0.4], ('both grenade blasts carry the 0.4 core radius', cores)
require(ENGINEER, 'run python3 tools/import_halo_campaign_actors.py')
anims = [l for l in ENGINEER.read_text().splitlines() if l.startswith('md3anim ')]
assert any('"idle"' in a for a in anims) and any('"run"' in a for a in anims), ('the Engineer is animated', anims)
print('Tables: melee side effect 2 on the nine player weapons, the needle supercombine, the core radii, the animated Engineer.')


def run(name, start, schedule, timeout=150):
    profile = scratch(f'melee-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_damagetrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def regions(text):
    """Each DAMAGE_REGION line by label, with the last DAMAGE_MATERIAL line logged before it (or None)."""
    out, material = {}, None
    for line in text.splitlines():
        if line.startswith('DAMAGE_MATERIAL '):
            material = fields(line)
        elif line.startswith('DAMAGE_REGION '):
            label = line.split()[1]
            row = fields(line)
            row['material_line'] = material
            out.setdefault(label, row)
            material = None
    return out


# ---- 1-2. The parsed table, and the melee from behind (Elites: a 50-60 blow from the front leaves them standing)
text, log = run('melee', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1; qa_damage_table'),
    (300, 'qa_actor_spawn 1 0 8 0'),
    (1000, 'qa_damage_melee 0 0 front'),
    (2400, 'qa_damage_melee 1 1 aware'),
    (3800, 'halo_meleeunsuspecting 0; qa_damage_melee 1 0 off'),
    (5200, 'halo_meleeunsuspecting 1; qa_damage_melee 1 0 behind'),
    (5600, 'quit'),
])
table = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TABLE ')]
index = {w['path']: str(w['index']) for w in weapons.values()}
row = next(r for r in table if r['weapon'] == index[rocket['path']] and r['trigger'] == '0')
assert abs(float(row['core']) - .6) < 1e-3, ('the engine reads the rocket core', row)
row = next(r for r in table if r['weapon'] == index[needler['path']] and r['trigger'] == '0')
assert row['super'] == '1' and row['super_side'] == '3' and abs(float(row['super_timer']) - .75) < 1e-3, row
assert all(r['melee_side'] == '2' for r in table if r['weapon'] in {index[w['path']] for w in players}), 'melee side effect 2 parsed'
grenades = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_GRENADE ')]
assert grenades and all(abs(float(g['core']) - .4) < 1e-3 for g in grenades), grenades
r = regions(text)
for label in ('front', 'aware', 'off', 'behind'):
    assert label in r and 'name' in r[label], (label, log)
assert r['front']['alive'] == '1', ('a blow from the front leaves a calm Elite standing', r['front'])
assert r['aware']['alive'] == '1', ('an Elite fighting the player is not unsuspecting', r['aware'])
assert r['off']['alive'] == '1', ('halo_meleeunsuspecting 0 turns the kill off', r['off'])
assert r['behind']['alive'] == '0', ('a blow from behind kills a calm Elite', r['behind'], log)
assert re.search(r'DAMAGE_UNSUSPECTING target=\S+ kind=1 status=[0-4] facing=-', text), ('the kill from behind is logged', log)
assert text.count('DAMAGE_UNSUSPECTING ') == 1, ('only the calm blow from behind is lethal', log)
print('a30 section 2: the melee from behind kills a calm Elite; from the front, on an aware one or switched off it does not.')

# ---- 3. Hunter regions (the pistol: armour 0.2, skin 1, shield 0)
p = index[pistol['path']]
text, log = run('hunter', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_actor_spawn 3 0 10 0'),
    (1000, f'qa_damage_region 3 180 0.5 {p} 5 back'),
    (1300, f'qa_damage_region 3 90 0.9 {p} 5 sidehead'),
    (1600, f'qa_damage_region 3 90 0.5 {p} 5 sidebelly'),
    (1900, f'qa_damage_region 3 0 0.5 {p} 5 frontbelly'),
    (2200, f'qa_damage_region 3 0 0.9 {p} 5 fronthead'),
    (2500, f'halo_hunterregions 0; qa_damage_region 3 90 0.9 {p} 5 offhead'),
    (2800, 'quit'),
])
r = regions(text)
want = {'back': '13', 'sidehead': '13', 'sidebelly': '12', 'frontbelly': '32', 'fronthead': '32', 'offhead': '12'}
for label, material in want.items():
    assert label in r and r[label].get('material_line'), (label, log)
    got = r[label]['material_line']
    assert got['kind'] == '3' and got['material'] == material, (label, material, got)
print('a30 section 2: Hunter skin from behind and at the head from the side, armour at the side belly, the shield arm in front.')

# ---- 4. The general material model (behind halo_actormaterials, off by default)
s = index[sniper['path']]
text, log = run('materials', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_actor_spawn 1 0 8 0'),
    (1000, f'qa_damage_region 1 0 0.5 {s} 10 off'),
    (1300, f'halo_actormaterials 1; qa_damage_region 1 0 0.5 {s} 10 on'),
    (1600, 'qa_actor_spawn 4 2 14 4'),
    (2300, f'qa_damage_region 4 0 0.5 {s} 100 flood'),
    (2600, 'quit'),
])
r = regions(text)
for label in ('off', 'on', 'flood'):
    assert label in r and 'name' in r[label], (label, log)
assert r['off']['material_line'] is None, ('the model is off by default: no material for an Elite', r['off'])
on = r['on']['material_line']
assert on and on['kind'] == '1' and on['material'] == '14' and abs(float(on['scale']) - 1) < 1e-3, on
lost_off = float(r['off']['shield_max']) - float(r['off']['shield'])
lost_on = float(r['on']['shield_max']) - float(r['on']['shield'])
if float(r['off']['shield_max']) >= 20:
    assert abs(lost_on - 2 * lost_off) < .1, ('the sniper hits an Elite shield twice as hard with the model on', lost_off, lost_on)
flood = r['flood']['material_line']
assert flood and flood['kind'] == '4' and flood['material'] == '19' and abs(float(flood['scale']) - .05) < 1e-3, flood
print(f'a30 section 2: material model off by default; on, an Elite shield lost {lost_on:.1f} to the sniper (off {lost_off:.1f}), '
      'a Flood combat form took 0.05 of it.')

# ---- 5. The needle supercombine (several Elites: a supercombine can kill the one it strikes)
text, log = run('needles', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_actor_spawn 1 0 8 0; qa_actor_spawn 1 0 10 4; qa_actor_spawn 1 0 12 -4'),
    (1000, 'qa_damage_needles 6 six'),
    (2200, 'qa_damage_needles 7 seven'),
    (3400, 'halo_supercombine 0; qa_damage_needles 7 off'),
    (3800, 'quit'),
])
needles = {l.split()[1]: fields(l) for l in text.splitlines() if l.startswith('DAMAGE_NEEDLES ')}
for label in ('six', 'seven', 'off'):
    assert label in needles and 'detonations' in needles[label], (label, log)
assert needles['six']['detonations'] == '0', ('six needles do not combine', needles['six'])
assert needles['seven']['detonations'] == '1', ('seven needles super-detonate', needles['seven'], log)
assert needles['off']['detonations'] == '0', ('halo_supercombine 0 turns it off', needles['off'])
assert re.search(r'SUPERCOMBINE target=\S+ needles=7 damage=60', text), ('the super detonation is logged', log)
print('a30 section 2: six stuck needles do nothing, seven super-detonate, halo_supercombine 0 stops it.')
