"""Falling damage: Halo's biped_falling_damage for the player and the actors.

Written while testing was on hold, NOT YET RUN.

Static (no game): player.txt's `falling` line is the globals' falling_damage block of every cache
(matg +392, tools/halo_actors.py falling_record: 3 and 6 units, 16.5, runtime velocities 0.3430,
0.1463 and 0.2068, globals\\falling 10/155/155 and globals\\distance 105, category 1), and those
velocities are sqrt(2 g d) of the distances with Halo's gravity (../Halo3DS physics/physics.c:279).
The planned-drop scan (tools/halo_fall_scan.py) finds every section's standing jump apex under the
3-unit lower bound, and no planned on-foot drop, jump or seat exit that the engine's distance model
(halo.cpp updateFalling) would hurt; it also records that the engine's own landing speed would hurt
or kill on some of them (why the engine goes by distance).

Game, a30 section 2 (qa_campaign_resume 2 2, actors not shooting): `qa_fall_def` reports the block;
the player lifted 2 Halo units and dropped takes nothing; 4 units deals 10 + 145 x scale, scale =
(v - 0.1463) / (0.2068 - 0.1463) with v = sqrt(2 g 4) times the landing floor's normal (0.9 to 1 on
the Crash Site's ground), to the shield first, and the same on Legendary (falling damage to the
player ignores difficulty, damage.c:820-825 and 1142-1147); with halo_fallingdamage 0 a 7-unit drop
does nothing; 7 units kills (scale 1, 155 against 150); 20 units deals globals\\distance mid-air
(FALL_DAMAGE kind=distance) and kills. A placed lz_search actor dropped 2 units takes nothing and
dropped 7 units takes 155 (kind=landing, scale 1). Every FALL_DAMAGE hit is ownerless: its
DAMAGE_TRACE has no difficulty line.
"""
import math
import mmap
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))

require(root / 'packages/halo/player.txt', 'run tools/halo_actors.py')
import halo_fall_scan  # noqa: E402

GRAVITY = 0.0035651792
line = next((l.split()[1:] for l in (root / 'packages/halo/player.txt').read_text().splitlines() if l.startswith('falling ')), None)
assert line, 'player.txt has no falling line; rerun tools/halo_actors.py'
values = [float(v) for v in line]
lower, upper, maximum, max_falling, min_damage, max_damage = values[:6]
falling, distance = values[6:9], values[9:12]
assert (lower, upper, maximum) == (3, 6, 16.5), values
assert falling == [10, 155, 155] and distance == [105, 105, 105] and values[12:14] == [1, 1], values
for d, v in ((lower, min_damage), (upper, max_damage), (maximum, max_falling)):
    assert abs(math.sqrt(2 * GRAVITY * d) - v) < 1e-5, ('runtime velocity is sqrt(2 g d)', d, v)
if (HALO_SOURCE / 'a30.map').exists():
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_actors import falling_record, number  # noqa: E402
    for cache in ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40'):
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            matg = next(tag for tag in halo.directory if tag.group == 'matg')
            record = falling_record(halo, matg)
        assert [number(v) for v in record['falling']] == line, (cache, record['falling'], line)
    print('player.txt\'s falling line is the globals\' falling_damage block of all ten caches.')
else:
    print(f'(HALO_SOURCE {HALO_SOURCE} missing: the cache comparison is skipped)')

t, sections, drops = halo_fall_scan.scan()
assert len(sections) == 55, ('every section has a route and a projection', len(sections))
assert all(s['jump_apex_halo'] < lower for s in sections), [s['name'] for s in sections if s['jump_apex_halo'] >= lower]
hurt = [d for d in drops if d['halo_damage'] > 0 or d['halo_lethal']]
assert not hurt, ('planned drops the distance model hurts', hurt)
engine_hurt = [d for d in drops if d['engine_damage'] > 0]
assert engine_hurt, 'the engine-velocity comparison should show why the engine goes by distance'
worst = max(drops, key=lambda d: d['fall_halo'])
print(f'Route scan: {len(drops)} planned drops, the deepest {worst["fall_halo"]} Halo units ({worst["section"]} row {worst["row"]}); '
      f'none hurts by distance, {len(engine_hurt)} would by the engine\'s landing speed.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, schedule, timeout=150):
    profile = scratch(f'falling-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_damagetrace 1; qa_campaign_resume 2 2; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def fields(text_line):
    return dict(re.findall(r'(\w+)=(\S+)', text_line))


def expected_landing(height, normal=1.0):
    v = math.sqrt(2 * GRAVITY * height) * normal
    if v <= min_damage:
        return 0.0
    s = min(max((v - min_damage) / (max_damage - min_damage), 0.0), 1.0)
    return (1 - s) * falling[0] + s * falling[1]


text, log = run('player', [
    (100, 'botsshoot 0; qa_fall_def; qa_damage_vitals 75 75'),
    (300, 'qa_fall 2 two'), (3300, 'qa_fall_state two'),
    (3400, 'qa_damage_vitals 75 75; qa_fall 4 four'), (6400, 'qa_fall_state four'),
    (6500, 'halo_difficulty 3; qa_damage_vitals 75 75; qa_fall 4 legendary'), (9500, 'qa_fall_state legendary; halo_difficulty 1'),
    (9600, 'halo_fallingdamage 0; qa_damage_vitals 75 75; qa_fall 7 off'), (12600, 'qa_fall_state off; halo_fallingdamage 1'),
    (12700, 'qa_damage_vitals 75 75; qa_fall 7 seven'), (15700, 'qa_fall_state seven'),
    (19000, 'qa_damage_vitals 75 75; qa_fall 20 twenty'), (23000, 'qa_fall_state twenty'),
    (23500, 'quit'),
])
definition = next((fields(l) for l in text.splitlines() if l.startswith('FALL_DEF ')), None)
assert definition and definition['loaded'] == '1' and definition['enabled'] == '1', ('qa_fall_def', log)
assert float(definition['min_damage']) == round(min_damage, 5) and definition['falling'] == '10.0/155.0/155.0', (definition, log)
lifts = {l.split()[1]: l for l in text.splitlines() if l.startswith('FALL_QA ')}
assert all('blocked' not in lifts.get(k, 'missing blocked') for k in ('two', 'four', 'legendary', 'off', 'seven', 'twenty')), \
    ('the player had headroom for every lift', lifts, log)
states = {l.split()[1]: fields(l) for l in text.splitlines() if l.startswith('FALL_STATE ')}
# Each FALL_DAMAGE line with the DAMAGE_TRACE of the hit it dealt (the next one on the same target).
lines, damage = text.splitlines(), []
for i, l in enumerate(lines):
    if l.startswith('FALL_DAMAGE '):
        hit = fields(l)
        hit['trace'] = next((fields(m) for m in lines[i + 1:] if m.startswith('DAMAGE_TRACE ') and f'target={hit["target"]} ' in m), {})
        damage.append(hit)
player_hits = [d for d in damage if d['target'] == 'player']
# 2 units: nothing, at full shield and health.
assert states['two']['hp'] == '75' and float(states['two']['shield']) == 75, (states['two'], log)
# 4 units: one landing hit, at the tag's scale for a 4-unit fall (the floor's normal 0.9 to 1), on the shield.
landings = [d for d in player_hits if d['kind'] == 'landing']
assert len(landings) == 3, ('landings at four, legendary and seven (twenty dies in the air)', player_hits, log)
for label, hit in (('four', landings[0]), ('legendary', landings[1])):
    amount = float(hit['amount'])
    assert expected_landing(4, .9) - .5 <= amount <= expected_landing(4) + .5, (label, hit, expected_landing(4))
    assert abs(float(hit['drop']) - 4) < .3, (label, hit)
    # Ignores difficulty: Legendary's friend_shield (1.4) does not divide it.
    assert abs(float(states[label]['shield']) - (75 - amount)) < .6 and states[label]['hp'] == '75', (label, states[label], hit, log)
assert states['off']['alive'] == '1' and float(states['off']['shield']) == 75, ('halo_fallingdamage 0', states['off'], log)
assert float(landings[2]['scale']) == 1 and float(landings[2]['amount']) == falling[1], ('7 units: scale 1', landings[2], log)
# 155 at 75 shield and 75 health: the shield takes 75 and the body the other 80, a kill.
assert int(landings[2]['trace'].get('health', 0)) >= 75, ('7 units kill', landings[2], log)
distance_hits = [d for d in player_hits if d['kind'] == 'distance']
assert distance_hits and float(distance_hits[0]['amount']) == distance[0], ('20 units: globals\\distance mid-air', player_hits, log)
assert float(distance_hits[0]['drop']) > maximum - .5, distance_hits[0]
# From 75 and 75: the first takes the shield and 30 health, the second kills; none lands after.
assert len(distance_hits) == 2 and int(distance_hits[1]['trace'].get('health', 0)) >= 45, ('20 units kill mid-air', distance_hits, log)
# Ownerless: no difficulty factor on any falling hit (the player's own damage, never scaled).
assert 'DAMAGE_DIFFICULTY owner=player target=player' not in text, log
print(f'Player: 2 units nothing, 4 units {float(landings[0]["amount"]):.1f} on the shield (Legendary the same), '
      f'7 units {landings[2]["amount"]} (dead), 20 units {len(distance_hits)} distance hits; off with halo_fallingdamage 0.')

text, log = run('actor', [
    (100, 'botsshoot 0; qa_actor_place lz_search'),
    (600, 'qa_fall_actor 2 actor-two'), (3600, 'qa_fall_actor 7 actor-seven'), (6600, 'quit'),
])
lines = text.splitlines()
lifts = {l.split()[1]: fields(l) for l in lines if l.startswith('FALL_QA ')}
assert 'name' in lifts.get('actor-two', {}) and 'name' in lifts.get('actor-seven', {}), ('actor lifts', lifts, log)
two, seven = lifts['actor-two']['name'], lifts['actor-seven']['name']
start = next(i for i, l in enumerate(lines) if l.startswith('FALL_QA actor-two '))
end = next(i for i, l in enumerate(lines) if l.startswith('FALL_QA actor-seven '))
assert not [l for l in lines[start:end] if l.startswith(f'FALL_DAMAGE target={two} ')], ('2 units hurt no actor', log)
damage = [fields(l) for l in lines[end:] if l.startswith('FALL_DAMAGE ')]
hits = [d for d in damage if d['target'] == seven and d['kind'] == 'landing']
assert hits and float(hits[0]['scale']) == 1 and float(hits[0]['amount']) == falling[1], ('the actor dropped 7 units', damage, log)
print(f'Actor: {two} dropped 2 units, {seven} 7 units: {hits[0]["amount"]} ownerless falling damage.')
