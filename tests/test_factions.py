"""Factions and special enemies.

Written while testing was on hold: this test has not been run yet.

1. Tables (no game): actors.txt format 2 carries each variant's unit team, its unit melee and
   its collision materials (Hunter shield arm 0, Monitor 0); weapons.txt and equipment.txt carry
   the jpt! material modifiers (assault rifle 0.5 on Hunter armour and 0 on its shield, pistol
   0.2 on armour and 1 on skin, plasma rifle 2 on Sentinels, frag 0.25 on armour).
2. The engine parses the modifiers (qa_weapon_materials).
3. Three sides fight each other: a Covenant Elite, a Flood combat form and a Sentinel spawned
   together away from the player pick each other as targets, and somebody dies.
4. ai_allegiance / ai_allegiance_remove / ai_allegiance_broken on a30 section 2: the matrix
   follows the commands, allied teams stop targeting each other, and killing two marines breaks
   the player-human allegiance (marines then fight the player).
5. Hunters: the shield arm stops an assault rifle hit from the front, the back takes a pistol hit
   in full (skin), the side a fifth of it (armour), a blast reaches the armour whatever the
   facing; two Hunters pair off, the survivor of a pair goes berserk, a close Hunter smashes.
6. Stealth Elites: the actv camouflage flag cloaks them (drawn translucent), a hit makes the cloak
   flicker weaker; plain Elites are opaque.
7. Sentinels and the Monitor fly: they rise off the floor and hold their height; the shielded
   Sentinel has its shield; the Monitor is passive, cannot be hurt and nobody targets it; Captain
   Keyes and the Monitor draw their own models; Engineers float and never fire.
8. Campaign: c20 places 343 Guilty Spark (bsp0_monitor) as a flying, passive actor on its
   command lists; c10's mission script allies the player and the humans with the Sentinels.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

ACTORS = root / 'packages/halo/actors.txt'
WEAPONS = root / 'packages/halo/weapons.txt'
EQUIPMENT = root / 'packages/halo/equipment.txt'
for table in (ACTORS, WEAPONS, EQUIPMENT):
    require(table, 'run python3 tools/halo_weapons.py and tools/halo_actors.py with the expanded caches')

ARMOR, SKIN, SENTINEL, MONITOR, SHIELD = 12, 13, 25, 26, 32
HUNTER, ELITE, SENTINEL_KIND, MARINE, MONITOR_KIND, ENGINEER_KIND = 3, 1, 8, 9, 10, 11


def actor_table():
    actors, current = {}, None
    text = ACTORS.read_text()
    assert text.startswith('format 2'), 'actors.txt is format 2 (re-run tools/halo_actors.py)'
    for line in text.splitlines():
        key, _, rest = line.partition(' ')
        if key == 'actor':
            current = {'materials': []}
        elif current is None or key.startswith('#'):
            continue
        elif key == 'path':
            current['path'] = rest
            actors[rest] = current
        elif key == 'family':
            family, kind, kind_type, actr, actv = rest.split()
            current.update(family=family, kind=int(kind), type=int(kind_type), actr=int(actr), actv=int(actv))
        elif key == 'team':
            current['team'] = int(rest)
        elif key == 'unitmelee':
            current['unitmelee'] = [float(x) for x in rest.split()]
        elif key == 'material':
            kind, flags, body, shield = rest.split()
            current['materials'].append((int(kind), int(flags), float(body), float(shield)))
    return actors


def materials_after(path, key, owner_key, owner):
    """The materials line after the first `key` line inside the block whose `owner_key` line is `owner`."""
    lines = path.read_text().splitlines()
    inside = False
    for i, line in enumerate(lines):
        if line.startswith(owner_key + ' '):
            inside = line.split(' ', 1)[1] == owner
        elif inside and line.startswith(key + ' ') and i + 1 < len(lines) and lines[i + 1].startswith('materials '):
            return [float(x) for x in lines[i + 1].split()[1:]]
    return None


def weapon_index(tag):
    current = None
    for line in WEAPONS.read_text().splitlines():
        if line.startswith('weapon '):
            current = int(line.split()[1])
        elif line == f'path {tag}':
            return current
    raise AssertionError(f'{tag} missing from weapons.txt')


# 1. Tables.
actors = actor_table()
hunter = actors[r'characters\hunter\hunter']
assert hunter['team'] == 3 and hunter['unitmelee'][1:] == [35, 45], hunter
assert (SHIELD, 0, 0.0, 0.0) in hunter['materials'] and any(m[0] == SKIN and m[1] & 1 for m in hunter['materials']), hunter
monitor = actors[r'characters\monitor\monitor']
assert monitor['kind'] == MONITOR_KIND and monitor['team'] == 5 and monitor['materials'][0][:3] == (MONITOR, 0, 0.0), monitor
assert monitor['actr'] >> 21 & 1, 'the Monitor flies'
assert actors[r'characters\sentinel\sentinel']['team'] == 5 and actors[r'characters\sentinel\sentinel']['actr'] >> 21 & 1
assert actors[r'characters\flood_infection\flood_infection']['team'] == 4
assert actors[r'characters\marine\marine assault rifle']['team'] == 2
assert actors[r'characters\engineer\engineer major plasma pistol']['kind'] == ENGINEER_KIND
stealth = actors[r'characters\elite\elite stealth\stealth elite plasma rifle']
assert stealth['actv'] >> 4 & 1 and not actors[r'characters\elite\elite minor\elite minor plasma rifle']['actv'] >> 4 & 1, stealth
assert actors[r'characters\elite\elite stealth\stealth elite major energy sword']['actv'] >> 6 & 1, 'sword Elites cannot use ranged weapons'
ar = materials_after(WEAPONS, 'projectile', 'path', r'weapons\assault rifle\assault rifle')
pistol = materials_after(WEAPONS, 'projectile', 'path', r'weapons\pistol\pistol')
plasma = materials_after(WEAPONS, 'projectile', 'path', r'weapons\plasma rifle\plasma rifle')
frag = materials_after(EQUIPMENT, 'blast', 'path', r'weapons\frag grenade\frag grenade')
assert ar and len(ar) == 33 and ar[ARMOR] == 0.5 and ar[SHIELD] == 0, ar
assert pistol and pistol[ARMOR] == 0.2 and pistol[SKIN] == 1, pistol
assert plasma and plasma[SENTINEL] == 2, plasma
assert frag and frag[ARMOR] == 0.25, frag
print('Tables: unit teams, unit melee, collision materials and jpt! material modifiers are exported.')

AR, PISTOL, PLASMA = weapon_index(r'weapons\assault rifle\assault rifle'), weapon_index(r'weapons\pistol\pistol'), \
    weapon_index(r'weapons\plasma rifle\plasma rifle')


def run(name, start, schedule, timeout=120):
    profile = scratch(f'faction-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def rows(text, tag, label=None):
    found = []
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] == tag and (label is None or (len(parts) > 1 and parts[1] == label)):
            found.append(dict(f.split('=', 1) for f in re.findall(r'(\w+=\S+)', line)))
    return found


def allegiance(text, label):
    found = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[0] == 'FACTION_ALLEGIANCE' and parts[1] == label:
            found[parts[2]] = (parts[3] == 'allied=1', parts[4] == 'broken=1')
    return found


require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

# 2. The engine's parsed modifiers.
text, log = run('materials', 'halo_preview', [(0, f'qa_weapon_materials {AR}; qa_weapon_materials {PISTOL}; qa_weapon_materials {PLASMA}'),
                                               (300, 'quit')])
parsed = {r[0]: r for r in (line.split() for line in text.splitlines() if line.startswith('FACTION_MATERIALS'))}
assert f'armor=0.50' in ' '.join(parsed[f'{AR}'][1:]) and 'shield=0.00' in ' '.join(parsed[f'{AR}']), (parsed, log)
assert 'skin=1.00' in ' '.join(parsed[f'{PISTOL}']) and 'sentinel=2.00' in ' '.join(parsed[f'{PLASMA}']), (parsed, log)
print('Engine: the weapon table\'s material modifiers are parsed.')

# 3. Covenant, Flood and Sentinels fight each other, away from a protected player.
text, log = run('three-way', 'halo_preview', [
    (0, 'qa_combat_protect 1; qa_actor_spawn 1 0 18 -3; qa_actor_spawn 4 2 18 3; qa_variant_spawn "sentinel\\sentinel" -1 22 0'),
    (6000, 'qa_actor_state fight'),
    (12000, 'qa_actor_state later; qa_actor_count later'),
    (12500, 'quit'),
])
fight = rows(text, 'ACTOR_QA', 'fight')
targets = {a['family']: a['target'] for a in fight}
assert targets.get('elite', '-') not in ('-', 'player') or targets.get('floodcombat_elite', '-') not in ('-', 'player'), \
    ('the Covenant and the Flood target each other or the Sentinel', targets, log)
sides = {a['side'] for a in rows(text, 'ACTOR_FACTION', 'fight')}
assert {'covenant', 'flood', 'sentinel'} <= sides, (sides, log)
print(f'Three sides: targets {targets}.')

# 4. Allegiance commands and betrayal on a30 section 2 (the script's own ai_allegiance player human).
text, log = run('allegiance', 'qa_campaign_resume 2 2', [
    (0, 'qa_allegiance_state start'),
    (200, 'qa_hs_call ai_allegiance 3 4; qa_allegiance_state allied'),
    (400, 'qa_hs_call ai_allegiance_remove 3 4; qa_allegiance_state removed'),
    (600, 'qa_combat_protect 1; qa_actor_spawn 9 1 4 2; qa_actor_spawn 9 1 4 -2; qa_actor_spawn 9 1 5 0'),
    (1200, 'qa_hs_call ai_allegiance_broken 1 2; qa_actor_kill_one 9; qa_actor_kill_one 9'),
    (1500, 'qa_hs_call ai_allegiance_broken 1 2; qa_allegiance_state broken'),
    (5000, 'qa_actor_state turned'),
    (5300, 'qa_hs_call ai_allegiance 1 2; qa_allegiance_state renewed'),
    (5600, 'quit'),
])
start, allied, removed = allegiance(text, 'start'), allegiance(text, 'allied'), allegiance(text, 'removed')
broken, renewed = allegiance(text, 'broken'), allegiance(text, 'renewed')
assert start['human-player'] == (True, False) and not start['covenant-flood'][0], (start, log)
assert allied['covenant-flood'][0] and not removed['covenant-flood'][0], (allied, removed, log)
calls = re.findall(r'^HS_CALL ai_allegiance_broken (\S+)', text, re.M)
assert calls[:2] == ['0.0000', '1.0000'], ('two killed marines break the allegiance', calls, log)
assert broken['human-player'] == (False, True) and renewed['human-player'] == (True, False), (broken, renewed, log)
assert 'the human turn on the player' in text, log
turned = [a for a in rows(text, 'ACTOR_FACTION', 'turned') if a['side'] == 'human']
assert turned and all(a['friendly'] == '0' for a in turned), ('the surviving marines are hostile now', turned, log)
print('Allegiances: ai_allegiance and _remove change the matrix; two kills break player-human; ai_allegiance renews it.')

# 5. Hunters: armour by where the hit lands, pairs, rage and the smash.
text, log = run('hunter', 'halo_preview', [
    (0, 'qa_combat_protect 1; qa_actor_spawn 3 0 8 0'),
    (300, f'qa_actor_turn 3 0; qa_actor_hit 3 {AR} 10'),
    (500, f'qa_actor_turn 3 180; qa_actor_hit 3 {PISTOL} 25'),
    (700, f'qa_actor_turn 3 90; qa_actor_hit 3 {PISTOL} 25'),
    (900, f'qa_actor_turn 3 0; qa_actor_blast 3 {AR} 10'),
    (1100, 'quit'),
])
hits = rows(text, 'FACTION_HIT')
loss = [int(h['before']) - int(h['after']) for h in hits]
assert loss == [0, 25, 5, 5], ('shield arm 0, skin x1, armour x0.2 (pistol), blast on armour x0.5 (rifle)', hits, log)
text, log = run('hunter-pair', 'halo_preview', [
    (0, 'qa_combat_protect 1; qa_actor_spawn 3 0 10 -2; qa_actor_spawn 3 0 10 2'),
    (1500, 'qa_actor_state paired'),
    (1800, 'qa_actor_kill_one 3'),
    (2300, 'qa_actor_state rage'),
    (2400, 'quit'),
])
paired = rows(text, 'ACTOR_FACTION', 'paired')
assert len(paired) == 2 and all(p['partner'] != '-' for p in paired), ('two Hunters pair off', paired, log)
rage = [a for a in rows(text, 'ACTOR_QA', 'rage') if a['family'] == 'hunter' and a['alive'] == '1']
assert rage and rage[0]['morale'] == 'berserk', ('the surviving Hunter goes berserk', rage, log)
text, log = run('hunter-smash', 'halo_preview', [
    (0, 'qa_combat_protect 1; qa_actor_spawn 3 0 1.2 0'),
    (5000, 'qa_actor_state smash'),
    (5300, 'quit'),
])
smash = rows(text, 'ACTOR_FACTION', 'smash')
assert smash and int(smash[0]['smashes']) >= 1, ('a Hunter at arm\'s length smashes', smash, log)
print(f'Hunters: armour losses {loss}, pair and rage, {smash[0]["smashes"]} smash(es).')

# 6. Stealth Elites: cloaked, translucent, flickering when hit.
text, log = run('stealth', 'halo_preview', [
    (0, 'qa_combat_protect 1; qa_variant_spawn "stealth elite plasma rifle" -1 10 0; qa_variant_spawn "elite minor plasma rifle" -1 10 4'),
    (300, 'qa_actor_flag 1 1'),
    (1500, 'qa_actor_cloak'),
    (1700, f'qa_actor_hit 1 {AR} 1; qa_actor_cloak'),
    (3200, 'qa_actor_cloak'),
    (3300, 'quit'),
])
cloaks = rows(text, 'FACTION_CLOAK')
camo = [c for c in cloaks if c['camo'] == '1']
plain = [c for c in cloaks if c['camo'] == '0']
assert len(camo) == 3 and plain and all(float(c['alpha']) < 0 for c in plain), (cloaks, log)
calm, hit, later = (float(c['cloak']) for c in camo)
assert calm >= .85 and 0 < float(camo[0]['alpha']) < .35, ('a calm stealth Elite is a faint shimmer', camo, log)
assert hit < calm * .5 and later > hit, ('a hit makes the cloak flicker, then it recovers', camo, log)
print(f'Stealth: cloak {calm:.2f}, after a hit {hit:.2f}, a second and a half later {later:.2f}.')

# 7. Flyers, the shielded Sentinel, the Monitor, Engineers and Keyes.
text, log = run('flyers', 'halo_preview', [
    (0, 'qa_combat_protect 1; qa_variant_spawn "sentinel\\sentinel_shielded" -1 12 0; qa_variant_spawn "monitor\\monitor" -1 6 4; '
        'qa_variant_spawn "engineer" -1 8 -4; qa_variant_spawn "captain\\captain" 1 3 2; qa_actor_spawn 1 0 14 4'),
    (4000, 'qa_actor_state up; qa_actor_model'),
    (4300, f'qa_actor_hit {MONITOR_KIND} {AR} 50'),
    (8000, 'qa_actor_state held'),
    (8300, 'quit'),
])
spawns = {line.split()[1]: line for line in text.splitlines() if line.startswith('FACTION_SPAWN')}
assert any('shield=1.00' in line and 'kind=8' in line for line in spawns.values()), ('the shielded Sentinel has its shield', spawns, log)
flyers = [a for a in rows(text, 'ACTOR_FACTION', 'up') if a['flying'] == '1']
assert len(flyers) == 3 and all(float(a['altitude']) > .5 for a in flyers), ('flyers rise off the floor', flyers, log)
held = [a for a in rows(text, 'ACTOR_FACTION', 'held') if a['flying'] == '1']
assert all(float(a['altitude']) > .5 for a in held), ('and hold their height', held, log)
assert sum(a['passive'] == '1' for a in held) == 2, ('the Monitor and the Engineer are passive', held, log)
hit = rows(text, 'FACTION_HIT')
assert hit and hit[0]['before'] == hit[0]['after'], ('nothing hurts the Monitor', hit, log)
qa = rows(text, 'ACTOR_QA', 'held')
assert all(not a['target'].startswith('343') for a in qa) and not any(a['family'] == 'engineer' and int(a['shots']) for a in qa), (qa, log)
models = {m['kind']: m['model'] for m in rows(text, 'FACTION_MODEL')}
assert models.get(str(MONITOR_KIND)) in ('halo/monitor', 'halo/sentinel') and 'halo/keyes' in models.values(), (models, log)
print(f'Flyers: altitudes {[a["altitude"] for a in held]}; Monitor unhurt; models {models}.')

# 8. Campaign: c20's Monitor and c10's allegiances.
text, log = run('c20-monitor', 'qa_campaign_resume 7 1', [(6000, 'qa_actor_state c20'), (6300, 'quit')], timeout=150)
monitors = [a for a in rows(text, 'ACTOR_QA', 'c20') if a['family'] == 'monitor']
assert monitors and monitors[0]['flags'] != '0', ('init_monitor_bsp0 places a disregarded, undamageable Monitor', monitors, log)
spark = [a for a in rows(text, 'ACTOR_FACTION', 'c20') if a['side'] == 'sentinel' and a['passive'] == '1']
assert spark and spark[0]['flying'] == '1', (spark, log)
text, log = run('c10-allies', 'qa_campaign_resume 6 1', [(4000, 'qa_allegiance_state c10'), (4300, 'quit')], timeout=150)
c10 = allegiance(text, 'c10')
assert c10['sentinel-player'][0] and c10['human-sentinel'][0] and c10['human-player'][0], (c10, log)
print('Campaign: c20 places 343 Guilty Spark as a flying passive actor; c10 allies the player and humans with the Sentinels.')
