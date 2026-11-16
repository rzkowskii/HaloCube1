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
4. The general material model: by default (halo_actormaterials 1, since the headshot and actor
   material work) a sniper hit on an Elite logs material 14 at scale 1 (its indirect damage
   material, body multiplier 1) and the Elite's shield takes the sniper's elite-energy-shield
   modifier (2: twice the shield of the same hit with halo_actormaterials 0, which logs no
   DAMAGE_MATERIAL), and a Flood combat form takes 0.05 of a sniper hit (material 19).
5. The needle supercombine (qa_damage_needles): 6 needles at once set off nothing, 7 set off one
   super detonation (SUPERCOMBINE needles=7), and with halo_supercombine 0, 7 set off none.

Batch 5's critic, gap 11 and the melee review (written while testing was on hold, not yet run):
6. Allies are spared the kill from behind (a judgement call, docs "Damage fidelity"): the player's melee
   from behind on a calm marine (qa_damage_melee_friend) logs DAMAGE_UNSUSPECTING_SPARED (team 1,
   attached 0) and no DAMAGE_UNSUSPECTING line; its DAMAGE_TRACE amount is the melee's own, not the
   marine's whole health and shield. (a50's attached Keyes: tests/test_ai_attach.py item 11.)
7. The Hunter's belly window opens from its cannon fire only: a frontal hit at belly height just after
   a smash (qa_damage_hunter_act 0: lastaction set, as the smash does) lands on the shield arm (32);
   just after a cannon shot (qa_damage_hunter_act 1: haloactorfired) on the skin (13).

Per-material shield leak and per-unit shield timing (written while testing was on hold, not yet run;
docs "Factions and special enemies", Material damage, and "Difficulty", Shield recharge):
8. The Jackal's hand-held shield (`jackal minor plasma pistol`, qa_variant_spawn, Normal: body 50,
   shield 200; DAMAGE_SHIELD lines): a frontal plasma rifle hit of 10 lands on material 3 (type 16,
   leak 0) and takes 10 off the shield, none off the body; the pistol's frontal 10 lands there too
   and does nothing (its modifier for the jackal energy shield is 0); from behind (material 0,
   leak 1), the plasma rifle at the head from the front (material 1; the pistol there would be a
   headshot, item 11) and as a rocket blast (the indirect material, 0) the body loses 10 (the
   modifiers for the jackal, 15, are 1) and the shield stays full; a frontal 250 empties the shield and leaves the
   body whole (body multiplier 0); with the shield down (qa_damage_keep 1) a frontal pistol hit
   reaches the body (material 0).
9. Shield timing (qa_shield_timing): the Jackal stuns 15000 ms and recharges in 33, the Elite minor
   12000 and 10000, the marine 8000 and 1000 (Normal's recharge values are 1), the crewman and
   the stealth Elite have no shield (shield_max 0: an actv that sets a body vitality and no shield);
   all on the Covenant team, so no one fights (Normal's enemy and friend recharge are both 1).
   A Jackal shield hit for 50 is still 150 a second later and full 15.5 s after the hit.
10. The marine's leak (marine assault rifle on the Covenant team, so the player's hits are hostile):
   a pistol hit of 10 from behind at full shield has leak 0.2 and share 0.8 (8 to the shield, 2
   passed); a second hit (qa_damage_keep 1) shares 0.8 x (0.8 x shield/shield_max + 0.2), the
   failure curve (threshold 1, maximum 0.2).

Headshots and the actors' material modifiers (written while testing was on hold, not yet run; docs
"Damage fidelity", Material damage and Headshots):
11. Tables: weapons.txt's `jptflags` lines give the pistol and sniper bullets bit 0x2 (can cause
   headshots) and no other projectile; DAMAGE_FLAGS headshots=1 for exactly those two. An `elite
   minor plasma rifle` (shield 100, body 100 on Normal): a pistol 10 at the head with the shield up
   only drains the shield (alive, no DAMAGE_HEADSHOT); after a plasma rifle 50 at the body empties it
   (2 x 50 = 100, no excess: health 100), the pistol 10 at the body takes 10 (alive) and, with
   halo_headshots 0, at the head another 10 (alive, health 80); with halo_headshots 1 the pistol 10
   at the head kills it (DAMAGE_HEADSHOT kind=1 material=14). A Hunter: the sniper's 5 at the side
   (armour) leaves it standing, at its back (skin, head-flagged) kills it (DAMAGE_HEADSHOT kind=3
   material=13). A Flood combat form takes the pistol at head height without a headshot (no head
   material: alive). A `sentinel_shielded` (shield 60, body 60) takes needles on its shield only:
   the needler's 40 empties 40 of the shield (its modifier for the elite energy shield, the coll's
   shield type 30, is 1) and leaves the body whole (0 for the sentinel material), then a second 40
   (qa_damage_keep 1) empties the rest and its excess does nothing to the body either.
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

# ---- 4. The general material model (halo_actormaterials, on by default)
s = index[sniper['path']]
text, log = run('materials', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_actor_spawn 1 0 8 0'),
    (1000, f'qa_damage_region 1 0 0.5 {s} 10 on'),
    (1300, f'halo_actormaterials 0; qa_damage_region 1 0 0.5 {s} 10 off'),
    (1600, 'halo_actormaterials 1; qa_actor_spawn 4 2 14 4'),
    (2300, f'qa_damage_region 4 0 0.5 {s} 100 flood'),
    (2600, 'quit'),
])
r = regions(text)
for label in ('off', 'on', 'flood'):
    assert label in r and 'name' in r[label], (label, log)
assert r['off']['material_line'] is None, ('halo_actormaterials 0: no material for an Elite', r['off'])
on = r['on']['material_line']
assert on and on['kind'] == '1' and on['material'] == '14' and abs(float(on['scale']) - 1) < 1e-3, on
lost_off = float(r['off']['shield_max']) - float(r['off']['shield'])
lost_on = float(r['on']['shield_max']) - float(r['on']['shield'])
if float(r['off']['shield_max']) >= 20:
    assert abs(lost_on - 2 * lost_off) < .1, ('the sniper hits an Elite shield twice as hard with the model on', lost_off, lost_on)
flood = r['flood']['material_line']
assert flood and flood['kind'] == '4' and flood['material'] == '19' and abs(float(flood['scale']) - .05) < 1e-3, flood
print(f'a30 section 2: material model on by default: an Elite shield lost {lost_on:.1f} to the sniper (off {lost_off:.1f}), '
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

# ---- 6. Allies are spared the kill from behind (batch 5's critic; normal melee damage still applies)
text, log = run('ally', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_actor_spawn 9 1 6 0'),
    (1000, 'qa_damage_melee_friend ally'),
    (1400, 'quit'),
])
r = regions(text)
assert 'ally' in r and 'name' in r['ally'], ('a marine to strike', log)
spared = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_UNSUSPECTING_SPARED ')]
assert spared and spared[0]['team'] == '1' and spared[0]['attached'] == '0', ('the ally is spared and logged', spared, log)
assert 'DAMAGE_UNSUSPECTING ' not in text, ('no kill from behind on an ally', log)
traces = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_TRACE ') and l.split('target=')[1].split()[0] == r['ally']['name']]
assert traces, ('the melee still lands on the ally', log)
print(f'a30 section 2: a marine struck from behind is spared the kill (melee {traces[0]["amount"]}, alive={r["ally"]["alive"]}).')

# ---- 7. The Hunter's belly window opens after a cannon shot, not after a smash
text, log = run('belly', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_actor_spawn 3 0 10 0'),
    (1000, f'qa_damage_hunter_act 0; qa_damage_region 3 0 0.45 {p} 5 smashed'),
    (1800, f'qa_damage_hunter_act 1; qa_damage_region 3 0 0.45 {p} 5 fired'),
    (2200, 'quit'),
])
r = regions(text)
for label, material in {'smashed': '32', 'fired': '13'}.items():
    assert label in r and r[label].get('material_line'), (label, log)
    got = r[label]['material_line']
    assert got['kind'] == '3' and got['material'] == material, (label, material, got)
print('a30 section 2: the Hunter\'s belly is open after its cannon shot (skin), shut after its smash (shield arm).')

# ---- 8-10. Per-material shield leak and per-unit shield timing
ACTORS = root / 'packages/halo/actors.txt'
require(ACTORS, 'run python3 tools/halo_actors.py with the expanded caches')
variant = {}
for line in ACTORS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'actor':
        current = int(rest)
    elif key == 'path':
        variant[rest] = current
assert any(l.startswith('shieldtime ') for l in ACTORS.read_text().splitlines()), 'rerun tools/halo_actors.py: no shieldtime lines'
JACKAL = variant[r'characters\jackal\jackal minor plasma pistol']
ELITE = variant[r'characters\elite\elite minor\elite minor plasma rifle']
MARINE = variant[r'characters\marine\marine assault rifle']
CREWMAN = variant[r'characters\crewman\crewman pistol']
STEALTH = variant[r'characters\elite\elite stealth\stealth elite plasma rifle']
pr, rl = index[weapon('plasma rifle')['path']], index[rocket['path']]


def shielded(text):
    """Each DAMAGE_REGION line by label, with the last DAMAGE_SHIELD line logged before it (or None)."""
    out, shield = {}, None
    for line in text.splitlines():
        if line.startswith('DAMAGE_SHIELD '):
            shield = fields(line)
        elif line.startswith('DAMAGE_REGION '):
            row = fields(line)
            row['shield_line'] = shield
            out.setdefault(line.split()[1], row)
            shield = None
    return out


text, log = run('jackal', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_variant_spawn "jackal minor plasma pistol" 0 8 0'),
    (1000, f'qa_damage_region 2 0 0.5 {pr} 10 front'),
    (1300, f'qa_damage_region 2 0 0.5 {p} 10 frontpistol'),
    (1600, f'qa_damage_region 2 180 0.5 {p} 10 back'),
    (1900, f'qa_damage_region 2 0 0.9 {pr} 10 head'),
    (2200, f'qa_damage_blast 2 {rl} 10 blast'),
    (2500, f'qa_damage_region 2 0 0.5 {pr} 250 break'),
    (2800, f'qa_damage_keep 1; qa_damage_region 2 0 0.5 {p} 10 downed'),
    (3100, 'qa_damage_keep 0; quit'),
])
r = shielded(text)
want = {  # label: (material index, shield left, health left)
    'front': ('3', 190, 50), 'frontpistol': ('3', 200, 50), 'back': ('0', 200, 40), 'head': ('1', 200, 40),
    'blast': ('0', 200, 40), 'break': ('3', 0, 50), 'downed': ('0', 0, 40),
}
for label, (material, shield, health) in want.items():
    assert label in r and 'name' in r[label], (label, log)
    got = r[label]
    assert got['shield_line'] and got['shield_line']['material'] == material, (label, material, got)
    assert abs(float(got['shield']) - shield) < .01 and int(got['health']) == health, (label, shield, health, got)
assert float(r['front']['shield_line']['leak']) == 0 and r['front']['shield_line']['type'] == '16', r['front']
assert float(r['back']['shield_line']['leak']) == 1, r['back']
print('a30 section 2: the Jackal\'s shield covers its hand-held shield only; body, head and blast hits reach its body.')

text, log = run('shieldtime', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_variant_spawn "jackal minor plasma pistol" 0 8 0; qa_variant_spawn "elite minor plasma rifle" 0 10 6; '
        'qa_variant_spawn "stealth elite plasma rifle" 0 10 -6'),
    (400, 'qa_variant_spawn "marine\\marine assault rifle" 0 -4 3; qa_variant_spawn "crewman pistol" 0 -4 -3'),
    (1000, 'qa_shield_timing'),
    (1200, f'qa_damage_region 2 0 0.5 {pr} 50 hit'),
    (2200, 'qa_damage_state 2 stunned'),
    (16700, 'qa_damage_state 2 recharged'),
    (16900, 'quit'),
], timeout=180)
timing = {int(f['variant']): f for f in (fields(l) for l in text.splitlines() if l.startswith('SHIELD_TIMING '))}
for v, (delay, recharge) in {JACKAL: (15000, 33), ELITE: (12000, 10000), MARINE: (8000, 1000)}.items():
    assert v in timing and timing[v]['timed'] == '1', (v, timing, log)
    assert int(timing[v]['delay']) == delay and int(timing[v]['recharge']) == recharge and int(timing[v]['scaled']) == recharge, (v, timing[v])
for v in (CREWMAN, STEALTH):
    assert v in timing and float(timing[v]['shield_max']) == 0, ('no shield: the actv sets a body and no shield', v, timing.get(v))
r = shielded(text)
assert abs(float(r['hit']['shield']) - 150) < .01, r['hit']
assert abs(float(r['stunned']['shield']) - 150) < .01, ('still stunned a second later', r['stunned'])
assert abs(float(r['recharged']['shield']) - 200) < .01, ('full once the 15 s stun ends', r['recharged'])
print('a30 section 2: shield stun and recharge from each coll (Jackal 15 s and a tick, Elite 12 s and 10 s, marine 8 s and 1 s).')

text, log = run('marine', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_variant_spawn "marine\\marine assault rifle" 0 6 0'),
    (1000, f'qa_damage_region 9 180 0.5 {p} 10 full'),
    (1300, f'qa_damage_keep 1; qa_damage_region 9 180 0.5 {p} 4 half'),
    (1600, 'qa_damage_keep 0; quit'),
])
r = shielded(text)
full = r['full']['shield_line']
assert full and abs(float(full['leak']) - .2) < 1e-3 and abs(float(full['share']) - .8) < 1e-3, r['full']
assert abs(float(full['shield_part']) - 8) < 1e-2 and abs(float(full['passed']) - 2) < 1e-2, full
fraction = float(r['full']['shield']) / float(r['full']['shield_max'])
half = r['half']['shield_line']
assert half and abs(float(half['share']) - .8 * (.8 * fraction + .2)) < 1e-3, (fraction, half)
print(f'a30 section 2: a marine\'s shield leaks 0.2 at full strength and {1 - float(half["share"]):.2f} at {fraction:.2f}.')

# ---- 11. Headshots and the actors' material modifiers
flags = {}
for line in WEAPONS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'weapon':
        current = int(rest)
        last = None
    elif key in ('melee', 'projectile'):
        last = key if rest != '-' else None
        if key == 'projectile' and last:
            flags.setdefault(current, []).append(None)
    elif key == 'jptflags' and last == 'projectile':
        flags[current][-1] = int(rest)
headshot = {weapons[i]['path'] for i, f in flags.items() if any(v is not None and v & 2 for v in f)}
assert headshot == {pistol['path'], sniper['path']}, ('only the pistol and sniper bullets can cause headshots', headshot)
text, log = run('headshot', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1; qa_damage_table'),
    (300, 'qa_variant_spawn "elite minor plasma rifle" 0 8 0; qa_actor_spawn 3 0 14 6; qa_actor_spawn 4 2 14 -6'),
    (400, 'qa_variant_spawn "sentinel_shielded" 0 18 0'),
    (1000, f'qa_damage_region 1 0 0.9 {p} 10 shielded'),
    (1300, f'qa_damage_region 1 0 0.5 {pr} 50 drained'),
    (1600, f'qa_damage_keep 1; qa_damage_region 1 0 0.5 {p} 10 bodyshot'),
    (1900, f'halo_headshots 0; qa_damage_region 1 0 0.9 {p} 10 nohead'),
    (2200, f'halo_headshots 1; qa_damage_region 1 0 0.9 {p} 10 headshot'),
    (2500, f'qa_damage_keep 0; qa_damage_region 3 90 0.5 {s} 5 hunterside'),
    (2800, f'qa_damage_region 3 180 0.5 {s} 5 hunterback'),
    (3100, f'qa_damage_region 4 0 0.9 {p} 10 floodhead'),
    (3400, f'qa_damage_region 8 0 0.5 {index[needler["path"]]} 40 needled'),
    (3700, f'qa_damage_keep 1; qa_damage_region 8 0 0.5 {index[needler["path"]]} 40 needled2'),
    (4000, 'qa_damage_keep 0; quit'),
])
table = {(r['weapon'], r['trigger']): r for r in (fields(l) for l in text.splitlines() if l.startswith('DAMAGE_FLAGS '))}
parsed = {weapons[int(w)]['path'] for (w, t), r in table.items() if r['headshots'] == '1'}
assert parsed == {pistol['path'], sniper['path']}, ('the engine reads jptflags', parsed, log)
r = regions(text)
for label in ('shielded', 'drained', 'bodyshot', 'nohead', 'headshot', 'hunterside', 'hunterback', 'floodhead', 'needled', 'needled2'):
    assert label in r and 'name' in r[label], (label, log)
heads = [fields(l) for l in text.splitlines() if l.startswith('DAMAGE_HEADSHOT ')]
assert r['shielded']['alive'] == '1' and abs(float(r['shielded']['shield']) - (float(r['shielded']['shield_max']) - 8)) < .01, \
    ('a head hit the shield takes whole is no headshot: 10 x 0.8 off the shield', r['shielded'])
assert float(r['drained']['shield']) == 0 and int(r['drained']['health']) == 100, ('the plasma rifle 50 at 2 empties 100, no excess', r['drained'])
assert r['bodyshot']['alive'] == '1' and int(r['bodyshot']['health']) == 90, r['bodyshot']
assert r['nohead']['alive'] == '1' and int(r['nohead']['health']) == 80, ('halo_headshots 0', r['nohead'])
assert r['headshot']['alive'] == '0', ('the pistol at the head of an unshielded Elite kills it', r['headshot'], log)
assert r['hunterside']['alive'] == '1', ('the sniper on a Hunter\'s armour', r['hunterside'])
assert r['hunterback']['alive'] == '0', ('the sniper on a Hunter\'s back skin kills it', r['hunterback'], log)
assert r['floodhead']['alive'] == '1', ('the Flood have no head material', r['floodhead'])
kinds = sorted((h['kind'], h['material']) for h in heads)
assert kinds == [('1', '14'), ('3', '13')], ('two headshots: the Elite\'s head and the Hunter\'s skin', heads, log)
needled, needled2 = r['needled'], r['needled2']
assert abs(float(needled['shield']) - (float(needled['shield_max']) - 40)) < .01 and int(needled['health']) == 60, \
    ('needles drain a shielded Sentinel\'s shield at 1 and leave its body', needled)
assert int(needled2['health']) == 60 and needled2['alive'] == '1', ('the sentinel material takes 0 from needles', needled2)
print('a30 section 2: headshots kill an unshielded Elite and a Hunter from behind, not a shielded Elite, a Flood form or with '
      'halo_headshots 0; needles drain a shielded Sentinel\'s shield only.')
