"""Grenades, equipment, powerups and weapon audio in the QA client.

Not yet run: written while testing was on hold (see docs/macos-build.md, "Grenades, equipment
and weapon audio").

Tables (no game): packages/halo/equipment.txt has the frag and plasma grenade types (the plasma
grenade sticks, its fuse starts at rest), the campaign eqip tags (ammo packs tied to their
weapons, the health pack, overshield and active camouflage); weapons.txt names a fire sound
for eight of the nine player weapons; the section maps carry the scenario equipment placements
(team 103) and drop the old stand-in ammo, health and grenade boxes where they do.

On a30 section 2 (no enemies): every sound the tables name was decoded; the grenade switch
needs grenades of the other type; a thrown plasma grenade leaves the count, flies, rests and
explodes; the last grenade of a type switches to the other; dropped plasma grenade, health
pack, overshield, active camouflage and assault rifle ammo are picked up with their effects
(the overshield triples the shield, takes the next hit and drains; camouflage counts down);
a revert puts the loadout back (a30's starting profile: no frags, no plasma; no powerups).

On a30 section 3 (Covenant in the second wave): the Covenant throw plasma grenades, a plasma
grenade thrown at an actor sticks to it and kills it, killed actors drop the grenades they
carried, and under active camouflage the actors lose track of the player.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

EQUIPMENT = root / 'packages/halo/equipment.txt'
WEAPONS = root / 'packages/halo/weapons.txt'
require(EQUIPMENT, 'run python3 tools/halo_weapons.py with the expanded caches')
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

# ---- Tables
grenades, equipment, block = {}, [], None
for line in EQUIPMENT.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'grenade':
        values = rest.split()
        block = grenades.setdefault(int(values[0]), {'maximum': int(values[1]), 'sticks': values[2] == '1',
                                                     'starts': int(values[3]), 'fuse': float(values[5]), 'sounds': {}})
    elif key == 'equipment' and rest.isdigit():
        block = {'index': int(rest), 'ammo': [], 'sounds': {}}
        equipment.append(block)
    elif key == 'end':
        block = None
    elif block is not None and key == 'path':
        block['path'] = rest
    elif block is not None and key == 'kind':
        block['powerup'], block['grenade'], block['time'] = int(rest.split()[0]), int(rest.split()[1]), float(rest.split()[2])
    elif block is not None and key == 'ammo':
        block['ammo'].append(tuple(map(int, rest.split())))
    elif block is not None and key == 'blast':
        block['blast'] = [float(x) for x in rest.split()]
    elif block is not None and key == 'sound':
        role, _, tag = rest.partition(' ')
        block['sounds'][role] = tag
assert set(grenades) == {0, 1}, grenades
frag, plasma = grenades[0], grenades[1]
assert not frag['sticks'] and plasma['sticks'] and plasma['starts'] == 2, (frag, plasma)
# blast: inner, outer radius, damage lower, upper min, upper max (the jpt! layout), side effect, category
assert frag['blast'][2:5] == [80, 120, 120] and plasma['blast'][2:5] == [80, 120, 120], (frag, plasma)
assert frag['blast'][5] == 0 and plasma['blast'][5] == 3, 'the plasma blast is EMP'
assert {'explode', 'throw'} <= set(frag['sounds']) and 'explode' in plasma['sounds'], grenades
by_path = {e['path']: e for e in equipment}
powerups = {e['powerup'] for e in equipment}
assert {2, 3, 5, 6} <= powerups, ('overshield, camouflage, health pack and grenades', powerups)
assert by_path[r'powerups\assault rifle ammo\assault rifle ammo']['ammo'], 'AR ammo names its weapon'
assert {e['grenade'] for e in equipment if e['powerup'] == 6} == {0, 1}

fire = {}
current = None
for line in WEAPONS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'path':
        current = rest
    elif key == 'sound' and rest.startswith('fire '):
        fire[current] = rest[5:]
players = ['assault rifle', 'pistol', 'shotgun', 'sniper rifle', 'rocket launcher', 'plasma pistol', 'plasma rifle', 'needler']
missing = [p for p in players if rf'weapons\{p}\{p}' not in fire]
assert not missing, ('player weapons without a fire sound', missing)

# Scenario equipment placements in the section maps (PLAYERSTART, team 103).
import gzip
import struct


def entities(name):
    data = gzip.decompress((root / f'packages/maps/{name}.cgz').read_bytes())
    count = struct.unpack_from('<4s4i', data)[4]
    # Header: magic, version, headersize, sfactor, numents, then the rest of the header.
    headersize = struct.unpack_from('<i', data, 8)[0]
    out = []
    for i in range(count):
        out.append(struct.unpack_from('<4h4BhbB', data, headersize + i * 16))
    return out


placed = {}
for name in ('halo_a10_02', 'halo_a30_01', 'halo_a30_02', 'halo_b30_01', 'halo_d40_04'):
    ents = entities(name)
    placed[name] = sum(e[4] == 2 and e[5] == 103 for e in ents)
    stand_ins = sum(e[4] in (4, 5, 6) for e in ents)
    assert not (placed[name] and stand_ins), (name, 'stand-in boxes beside retail equipment', placed[name], stand_ins)
assert sum(placed.values()) > 0, placed
print(f'Tables: 2 grenade types, {len(equipment)} equipment definitions, {len(fire)} weapons with fire sounds; '
      f'placements {placed}.')


def run(name, section, schedule, timeout=120):
    profile = scratch(f'combat-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-eqa_campaign_resume 2 {section}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    text = log.read_text(errors='replace')
    states = {}
    for line in text.splitlines():
        if line.startswith('COMBAT_QA '):
            fields = line.split()
            states.setdefault(fields[1], {}).update(dict(f.split('=', 1) for f in fields[2:] if '=' in f))
    return text, states, log


# ---- Player: sounds, grenades, pickups, powerups, revert (a30 section 2)
text, s, log = run('player', 2, [
    (100, 'botsshoot 0; qa_sound_table; qa_combat_state start; qa_combat_items start'),
    # a30's starting profile carries no grenades: two frags first, so the switch and auto-switch
    # back to frag below have frags to go to.
    (300, 'qa_grenade_give 0 2; qa_grenade_switch switch-empty'),   # no plasma grenades: stays on frag
    (500, 'qa_grenade_give 1 2; qa_grenade_switch switch'),   # now plasma
    (800, 'qa_grenade_throw'),
    (1000, 'qa_grenade_state flying'),
    (5000, 'qa_grenade_state later; qa_combat_state after-plasma'),
    (5200, 'qa_grenade_throw'),                           # the last plasma grenade: back to frag
    (5400, 'qa_combat_state last-plasma'),
    (9000, 'qa_item_drop 6 1 1; qa_combat_state drop-plasma'),
    (9300, 'qa_combat_state picked-plasma'),
    (9500, 'qa_halo_damage 110'),
    (9700, 'qa_combat_state hurt; qa_item_drop 5 0 1'),
    (10000, 'qa_combat_state healed'),
    (10200, 'qa_item_drop 2 0 1'),
    (12500, 'qa_combat_state overshield'),
    (12600, 'qa_halo_damage 60'),
    (12700, 'qa_combat_state overshield-hit'),
    (22000, 'qa_combat_state overshield-drain'),
    (22200, 'qa_item_drop 3 0 1'),
    (22500, 'qa_combat_state camo'),
    (22600, 'qa_weapon_fire 1500'),
    (24500, 'qa_weapon_state fired; qa_item_drop 0 0 1'),
    (24800, 'qa_weapon_state ammo'),
    (25000, 'qa_halo_damage 1000'),
    (26200, 'qa_halo_revert'),
    (27200, 'qa_combat_state reverted'),
    (27400, 'quit'),
])
need = ['start', 'switch-empty', 'switch', 'after-plasma', 'last-plasma', 'picked-plasma', 'hurt', 'healed', 'overshield',
        'overshield-hit', 'overshield-drain', 'camo', 'reverted', 'flying', 'later']
missing = [n for n in need if n not in s]
assert not missing, (missing, log)
i = lambda n, k: int(s[n][k])
f = lambda n, k: float(s[n][k])

sounds = re.findall(r'^SOUND_QA (\S+) (\S+) (\S+) (\d)', text, re.M)
assert len(sounds) >= 40 and all(present == '1' for *_, present in sounds), \
    ([row for row in sounds if row[3] != '1'], log)
# a30's player starting profile (.hscb version 9) gives no grenades; an older .hscb the default two.
START_FRAGS = 0 if read_hscb(root / 'packages/halo/a30.hscb')['version'] >= 9 else 2
assert i('start', 'frag') == START_FRAGS and i('start', 'plasma') == 0 and i('start', 'type') == 0, s['start']
assert i('switch-empty', 'type') == 0, ('no switch to a type with no grenades', s['switch-empty'])
assert i('switch', 'type') == 1 and i('switch', 'plasma') == 2, s['switch']
assert i('flying', 'grenades') == 1 and re.search(r'^GRENADE flying type=1 ', text, re.M), log
assert i('later', 'grenades') == 0, ('the plasma grenade exploded within 4 s', log)
assert i('after-plasma', 'plasma') == 1, s['after-plasma']
assert i('last-plasma', 'plasma') == 0 and i('last-plasma', 'type') == 0, ('empty plasma switches to frag', s['last-plasma'])
assert i('picked-plasma', 'plasma') == 1, ('a dropped plasma grenade is picked up', s['picked-plasma'])
assert i('hurt', 'hp') < 75 and i('healed', 'hp') == 75, (s['hurt'], s['healed'])
assert f('overshield', 'overshield') >= 140, ('the overshield charges two more layers', s['overshield'])
assert f('overshield-hit', 'overshield') < f('overshield', 'overshield') and i('overshield-hit', 'hp') == 75 \
    and f('overshield-hit', 'shield') >= 74, ('the overshield takes the hit first', s['overshield-hit'])
assert f('overshield-drain', 'overshield') < f('overshield-hit', 'overshield'), 'the overshield drains'
assert f('camo', 'camo') > 0.9, s['camo']
weapon = {}
for line in text.splitlines():
    if line.startswith('WEAPON_QA '):
        fields = line.split()
        weapon[fields[1]] = dict(x.split('=', 1) for x in fields[2:] if '=' in x)
assert int(weapon['ammo']['ammo']) > int(weapon['fired']['ammo']), ('AR ammo pack refills the reserve', weapon)
assert i('reverted', 'frag') == START_FRAGS and i('reverted', 'plasma') == 0 and f('reverted', 'overshield') == 0 \
    and f('reverted', 'camo') == 0, s['reverted']
print('Player: sounds decoded, grenade switch, plasma throw and fuse, auto-switch, pickups, overshield, camouflage, revert.')

# ---- Actors: plasma grenades, sticking, drops, camouflage (a30 section 3)
text, s, log = run('actors', 3, [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (200, 'qa_campaign_reach'),
    (2200, 'qa_campaign_reach'),
    (4500, 'botsshoot 0; qa_actor_grenades spawned; qa_enemy_throw'),
    (8000, 'qa_actor_state seen'),
    (8100, 'qa_camo_give 60000'),
    (15000, 'qa_actor_state cloaked'),
    (15200, 'qa_grenade_give 1 1; qa_grenade_select 1; qa_grenade_face_actor 4'),
    (15300, 'qa_grenade_throw'),
    (15700, 'qa_grenade_state stuck'),
    (19000, 'qa_actor_count after-stick; qa_combat_items before-clear'),
    (19200, 'qa_halo_clear'),
    (20200, 'qa_combat_items after-clear'),
    (20400, 'quit'),
])
covenant = re.findall(r'^COMBAT_ACTOR spawned name=\S+ kind=(\d+) team=0 grenade_type=(-?\d+) grenades=(\d+)', text, re.M)
assert covenant and all(kind in ('6', '7') or t == '1' for kind, t, _ in covenant), ('the Covenant throw plasma', covenant, log)
assert re.search(r'^COMBAT_QA enemy-throw actor=\S+ type=1 thrown=1', text, re.M), log
assert re.search(r'^GRENADE stuck type=1 .* stuck=(?!-)\S+', text, re.M), ('a plasma grenade sticks to an actor', log)


def view(label):
    rows = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) > 2 and fields[0] in ('ACTOR_QA', 'ACTOR_VIEW') and fields[1] == label:
            data = dict(x.split('=', 1) for x in fields[2:] if '=' in x)
            rows.setdefault(data['cn'], {}).update(data)
    return rows


seen, cloaked = view('seen'), view('cloaked')
hunting = [cn for cn, a in seen.items() if a.get('target') == 'player' and a.get('alive') == '1']
assert hunting, ('actors target the player before the camouflage', log)


def lost_track(a):
    # Dead, no longer fighting the player, or close enough to see through the camouflage
    # (within 18% of its vision range: the tracking reach under full camouflage).
    return a is None or a.get('alive') != '1' or a.get('target') != 'player' or a.get('awareness') != 'combat' or \
        float(a.get('distance', 0)) <= float(a.get('range', 0)) * .18


still = [cn for cn in hunting if not lost_track(cloaked.get(cn))]
assert not still, ('actors lose track of the camouflaged player beyond close range', still, cloaked, log)
# Every grenade item, picked up or not: the list only grows.
before = [l for l in text.splitlines() if l.startswith('ITEM_QA before-clear') and ' powerup=6 ' in l]
after = [l for l in text.splitlines() if l.startswith('ITEM_QA after-clear') and ' powerup=6 ' in l]
assert len(after) > len(before), ('killed actors drop their grenades', before, after, log)
print(f'Actors: {len(covenant)} Covenant with plasma grenades, throw, stick, drops, camouflage hides the player.')
