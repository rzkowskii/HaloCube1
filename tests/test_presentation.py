"""Presentation: script effects, camera shake, screen effects, flashlight, damage indicator, sounds.

Not yet run: written while testing was on hold (see docs/macos-build.md, "Presentation").

Tables (no game): packages/halo/effects.txt (tools/halo_effects.py) is format 1 with the cyborg's
flashlight (radius 6, cone 20-45 degrees), the script effects (the explosions with their sounds,
sizes and lights) and a `classes` line per campaign cache; weapons.txt names the zoom sounds of the
pistol, sniper rifle and rocket launcher and the flamethrower's firing loop; vehicles.txt names the
engine loop of every vehicle and the Warthog's crash and suspension sounds; every one of those
sounds was decoded, and packages/audio/halo/tags/classes.txt gives each decoded sample its class.

In the QA client:
1. a30 section 2 (no enemies): the flashlight turns on and off with halo_flashlight, drains while
   on and recovers off; a script effect ahead of the player is known, drawn and shakes the camera;
   an unknown one falls back; player_effect_* ramps the shake up and down; cinematic_screen_effect_*
   ramps the blur and the filter and stop clears them; sound_class_set_gain fades music and the
   vehicle classes; a hit from the side raises the damage-direction indicator, which fades in a
   second; a zoom step plays (no crash).
2. a30 section 2 with a Grunt spawned just out of the blow's reach: facing it, the melee lunges
   onto it (MELEE_QA lunge), and its hit on the player raises the indicator through halodamage.
3. c10 section 1: effect_new through the script interpreter at the cutscene flag
   enc_swamp2_turret draws the table's effect.
4. d40 section 3 (stock geometry: qa_stock_geometry 1, as no campaign section is stock since the d40
   section 3 batch): a spawned Warthog driven by the player plays its engine loop,
   which stops when the player gets out.
5. d40 section 3 (stock geometry likewise): a spawned Covenant turret manned by the player: turning it at 50 degrees a second
   plays its engine loop, still held 150 ms after the last turn, gone 600 ms after it; turning at 5
   degrees a second (under the 15 degree threshold) plays nothing.
6. a30 section 2: after a checkpoint, a running camera shake, cinematic screen effect and damage
   arc are cleared by a revert (the player killed and respawned: halofxrevert), and a sound class
   fade set before it is kept.
7. a30 section 2: the blur's frame copy and the flashlight's spot are made again after resetgl (a
   GL context rebuild, halofxcleanupgl) and the copy follows a window of a new size (screenres);
   a power-of-two copy is only used without non-power-of-two textures.

Batch 5's critic, gap 11 (written while testing was on hold, not yet run):
8. Tables: effects.txt gives the damaging explosions a `damage` line (the large explosion's medium
   explosion jpt!: 1.5 2.5 5 15 25, category 4) with a 33-value `materials` line, and the cinematic
   variant none. c10 section 1: effect_new of the large explosion at enc_swamp2_turret deals nothing
   with halo_effectdamage 0 (the default: no "jpt! damage" line); with it 1 it deals its jpt! damage
   ("HaloScript: ... deals its jpt! damage"), and the medium explosion's cinematic variant has none.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

EFFECTS = root / 'packages/halo/effects.txt'
WEAPONS = root / 'packages/halo/weapons.txt'
VEHICLES = root / 'packages/halo/vehicles.txt'
CLASSES = root / 'packages/audio/halo/tags/classes.txt'
require(EFFECTS, 'run python3 tools/halo_effects.py with the expanded caches')
require(CLASSES, 'run python3 tools/import_halo_audio.py after tools/halo_effects.py')
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def sample(tag):
    """tools/import_halo_audio.py sample_name."""
    name = tag.lower()
    if name.startswith('sound\\'):
        name = name[len('sound\\'):]
    return 'tags/' + ''.join('-' if c == '\\' else '_' if c == ' ' else c for c in name)


# ---- Tables
lines = EFFECTS.read_text().splitlines()
assert lines[0] == 'format 1', lines[0]
flash = [l for l in lines if l.startswith('flashlight ')]
assert flash, 'the flashlight light'
radius, falloff, cutoff = map(float, flash[0].split()[1:4])
assert abs(radius - 6) < .01 and abs(falloff - .349) < .01 and abs(cutoff - .785) < .01, flash
effects, block = {}, None
for line in lines:
    key, _, rest = line.partition(' ')
    if key == 'effect':
        block = {}
    elif key == 'path' and block is not None:
        effects[rest] = block
    elif key == 'end':
        block = None
    elif block is not None:
        block[key] = rest
large = effects.get(r'effects\explosions\large explosion')
assert large and large['kind'].split()[0] == 'explosion' and float(large['kind'].split()[1]) > 1.4, large
assert large['sound'].startswith('play ') and 'light' in large, large
assert len(effects) >= 15, sorted(effects)
caches = {l.split()[1] for l in lines if l.startswith('classes ')}
assert caches == {'a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40'}, caches

sounds, current = {}, None
for line in WEAPONS.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'path':
        current = rest
    elif key == 'sound':
        role, _, tag = rest.partition(' ')
        sounds.setdefault(current, {})[role] = tag
for weapon in ('pistol', 'sniper rifle', 'rocket launcher'):
    got = sounds.get(rf'weapons\{weapon}\{weapon}', {})
    assert {'zoom-in', 'zoom-out'} <= set(got), (weapon, got)
assert 'loop' in sounds.get(r'weapons\flamethrower\flamethrower', {}), 'the flamethrower fire loop'
assert 'fire' not in sounds.get(r'weapons\flamethrower\flamethrower', {}), 'the flamethrower has no per-shot sound'

vehicle_sounds, current = {}, None
for line in VEHICLES.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'vehicle':
        current = rest.split()[1]
    elif key == 'sound':
        role, _, tag = rest.partition(' ')
        vehicle_sounds.setdefault(current, {})[role] = tag
for name in ('warthog', 'turret', 'ghost', 'scorpion', 'banshee', 'wraith'):
    assert 'engine' in vehicle_sounds.get(name, {}), (name, vehicle_sounds)
assert {'crash', 'suspension'} <= set(vehicle_sounds['warthog']), vehicle_sounds['warthog']

classes = dict(l.split() for l in CLASSES.read_text().splitlines() if l and not l.startswith('#'))
wanted = [t['sound'].split(' ', 1)[1] for t in effects.values() if 'sound' in t]
wanted += [tag for roles in vehicle_sounds.values() for tag in roles.values()]
wanted += [sounds[r'weapons\flamethrower\flamethrower']['loop'], sounds[r'weapons\sniper rifle\sniper rifle']['zoom-in']]
missing = [t for t in wanted if not (root / 'packages/audio/halo' / (sample(t) + '.wav')).exists()]
assert not missing, ('sounds not decoded', missing)
unclassed = [t for t in wanted if sample(t) not in classes]
assert not unclassed, ('decoded samples without a class', unclassed)
assert classes[sample(vehicle_sounds['warthog']['engine'])] == '23', 'the engine loop is class vehicle_engine'
print(f'Tables: {len(effects)} effects, flashlight, classes for 10 caches, zoom and loop sounds, '
      f'{len(vehicle_sounds)} vehicles with engine loops; every sound decoded and classed.')


def run(name, start, schedule, timeout=150):
    profile = scratch(f'presentation-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_fxtrace 1; halo_meleetrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace')


def states(text):
    out = {}
    for line in text.splitlines():
        if line.startswith(('FX_QA ', 'FX_VEHICLE ')):
            fields = line.split()
            out.setdefault(fields[1], {}).update(dict(f.split('=', 1) for f in fields[2:] if '=' in f))
    return out


# ---- 1. a30 section 2: the flashlight, effects, shake, screen effects, class gains, the indicator
text = run('player', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1; qa_fx_state start'),
    (200, 'halo_flashlight 1'),
    (2300, 'qa_fx_state lit'),
    (2400, 'halo_flashlight 1'),
    (4500, 'qa_fx_state unlit'),
    (4600, r'qa_fx_effect "effects\explosions\large explosion" 3'),
    (4700, 'qa_fx_state boom'),
    (4800, r'qa_fx_effect "effects\explosions\not an effect" 3'),
    (4900, 'qa_fx_state unknown'),
    (5000, 'qa_hs_cmd player_effect_set_max_rotation "0 2 2"; qa_hs_cmd player_effect_start "1 0.5"'),
    (6000, 'qa_fx_state shaking'),
    (6100, 'qa_hs_cmd player_effect_stop "0.5"'),
    (7200, 'qa_fx_state still'),
    (7300, 'qa_hs_cmd cinematic_screen_effect_start "true"; qa_hs_cmd cinematic_screen_effect_set_convolution "1 2 0.001 10 1"; '
           'qa_hs_cmd cinematic_screen_effect_set_filter "0 1 0 1 false 1"'),
    (8600, 'qa_fx_state screen'),
    (8700, 'qa_hs_cmd cinematic_screen_effect_stop ""'),
    (8800, 'qa_fx_state cleared'),
    (8900, 'qa_hs_cmd sound_class_set_gain "s:music 0.25 30"; qa_hs_cmd sound_class_set_gain "s:vehicle 0 0"'),
    (10200, 'qa_fx_state faded'),
    (10300, 'qa_fx_hit 90 4'),
    (10400, 'qa_fx_state hit'),
    (11600, 'qa_fx_state hitgone; qa_weapon_zoom'),
    (11800, 'qa_weapon_zoom; quit'),
])
s = states(text)
for label in ('start', 'lit', 'unlit', 'boom', 'unknown', 'shaking', 'still', 'screen', 'cleared', 'faded', 'hit', 'hitgone'):
    assert label in s, (label, text[-3000:])
assert s['start']['loaded'] == '1' and int(s['start']['table']) >= 15 and s['start']['flashlight'] == '0', s['start']
assert s['lit']['flashlight'] == '1' and float(s['lit']['charge']) < .99, ('the flashlight drains', s['lit'])
assert s['unlit']['flashlight'] == '0' and float(s['unlit']['charge']) > float(s['lit']['charge']), ('it recovers off', s['unlit'])
boom = s['boom']
assert boom['effects'] == '1' and boom['visual'] == '1' and boom['unknown'] == '0', boom
assert boom['last'] == r'effects\explosions\large_explosion' and boom['impulse'] == '1', ('a blast near the player knocks the camera', boom)
assert s['unknown']['unknown'] == '1' and s['unknown']['effects'] == '2', s['unknown']
assert float(s['shaking']['shake']) > .95 and s['shaking']['camera'] != '0.000,0.000,0.000', s['shaking']
assert float(s['still']['shake']) < .01, s['still']
screen = s['screen']
assert screen['screen'] == '1' and float(screen['blur']) > 9 and float(screen['desat']) > .95 and float(screen['tint']) > .95, screen
assert s['cleared']['screen'] == '0' and float(s['cleared']['blur']) == 0, s['cleared']
faded = s['faded']
assert abs(float(faded['music']) - .25) < .02 and float(faded['vehicle_engine']) == 0 and float(faded['weapon_fire']) == 1, faded
assert s['hit']['hits'] == '1' and s['hitgone']['hits'] == '0', (s['hit'], s['hitgone'])
assert 'Halo effect: effects\\explosions\\large explosion' in text, 'halo_fxtrace logs the effect'
print('a30 section 2: flashlight, effects (known and fallback), shake ramps, screen effects, class fades, indicator.')

# ---- 2. The melee lunge and an actor's hit through halodamage (a Grunt 1.6 world units ahead)
# Not protected: a protected player takes no hit, so halodamage raises no indicator. The actors
# hold their fire (botsshoot 0); the second Grunt is the one that hits after the first is meleed.
text = run('lunge', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0'),
    (300, 'qa_actor_spawn 0 0 1.6 0; qa_actor_spawn 0 0 6 2'),
    (600, 'qa_fx_face_actor'),
    (700, 'halo_melee 1'),
    (900, 'qa_fx_state lunged'),
    (1100, 'qa_fx_actor_hit 5'),
    (1200, 'qa_fx_state actorhit; quit'),
])
face = re.search(r'FX_QA face \S+ distance=([\d.]+)', text)
assert face, text[-3000:]
distance = float(face.group(1))
if 4.0 < distance < 7.2:
    assert 'MELEE_QA lunge target=' in text, ('the melee lunges at a Grunt just out of reach', distance, text[-3000:])
    print(f'lunge: the melee reached a Grunt {distance:.1f} units away.')
else:
    print(f'lunge: the Grunt stood {distance:.1f} units away, outside the lunge band 4-7.2; not asserted.')
assert re.search(r'FX_QA actor-hit from=(?!-)\S+', text), text[-3000:]
assert states(text)['actorhit']['hits'] == '1', ('an actor hit on the shield raises the indicator', states(text)['actorhit'])

# ---- 3. effect_new through the script interpreter (c10 section 1, flag enc_swamp2_turret)
text = run('script', 'halo_section 6 1', [
    (400, r'qa_fx_script_effect "effects\explosions\large explosion no objects" enc_swamp2_turret'),
    (500, 'qa_fx_state scripted; quit'),
])
flag = re.search(r'FX_QA script-effect flag=(-?\d+)', text)
assert flag and int(flag.group(1)) >= 0, ('c10 has the flag', text[-3000:])
scripted = states(text)['scripted']
assert int(scripted['effects']) >= 1 and scripted['unknown'] == '0', scripted
assert 'large_explosion_no_objects' in scripted['last'] or int(scripted['effects']) > 1, scripted
print('c10: effect_new at a cutscene flag draws the table effect.')

# ---- 4. A Warthog's engine loop (d40 section 3, stock geometry: qa_stock_geometry keeps its .hcol unloaded)
text = run('engine', 'qa_stock_geometry 1; halo_section 10 3', [
    (300, 'qa_vehicle_spawn 0 8'), (600, 'qa_vehicle_enter 0 0'),
    (700, 'qa_vehicle_input 1 0 1500'), (1500, 'qa_fx_state driving'),
    (2400, 'qa_vehicle_use'), (2800, 'qa_fx_state parked; quit'),
])
s = states(text)
assert s['driving']['engines'] == '1', ('the driven Warthog plays its engine loop', s.get('driving'), text[-3000:])
assert s['parked']['engines'] == '0', ('the loop stops when the driver gets out', s.get('parked'))
print('d40 section 3: the driven Warthog plays its engine loop and stops when left.')

# ---- 5. A turret's engine loop: above 15 degrees a second it plays, held 250 ms after the turn
text = run('turret', 'qa_stock_geometry 1; halo_section 10 3', [
    (300, 'qa_combat_protect 1; qa_vehicle_spawn 1 8'), (600, 'qa_vehicle_enter 0 1'), (800, 'qa_fx_state manned')] +
    [(1000 + 100 * i, 'qa_fx_yaw 5') for i in range(15)] +                 # 50 degrees a second for 1.5 s
    [(2250, 'qa_fx_state turning'), (2550, 'qa_fx_state held'), (3100, 'qa_fx_state settled')] +
    [(3300 + 100 * i, 'qa_fx_yaw 0.5') for i in range(15)] +               # 5 degrees a second
    [(4850, 'qa_fx_state creeping'), (5000, 'quit')])
assert re.search(r'VEHICLE_QA enter vehicle=0 role=1 ok=1', text), ('the player mans the spawned turret', text[-3000:])
s = states(text)
for label in ('manned', 'turning', 'held', 'settled', 'creeping'):
    assert label in s, (label, text[-3000:])
assert s['manned']['engines'] == '0', ('a still turret plays no loop', s['manned'])
assert s['turning']['engines'] == '1', ('turning at 50 degrees a second plays the loop', s['turning'])
assert s['held']['engines'] == '1', ('the loop is held for 250 ms after the last turn', s['held'])
assert s['settled']['engines'] == '0', ('600 ms after the turn the loop has stopped', s['settled'])
assert s['creeping']['engines'] == '0', ('under 15 degrees a second the turret is silent', s['creeping'])
print('d40 section 3: the turret loop plays above 15 degrees a second, holds 250 ms, stays off below.')

# ---- 6. A revert clears the shake, the screen effects and the arcs, and keeps the class gains
text = run('revert', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0'),
    (300, 'qa_checkpoint_save'),
    (600, 'qa_hs_cmd sound_class_set_gain "s:music 0.25 0"; qa_hs_cmd player_effect_set_max_rotation "0 2 2"; '
          'qa_hs_cmd player_effect_start "1 0.1"; qa_hs_cmd cinematic_screen_effect_start "true"; '
          'qa_hs_cmd cinematic_screen_effect_set_convolution "1 2 10 10 0"'),
    (1000, 'qa_fx_state before'),
    (1100, 'qa_halo_damage 1000'),
    (2600, 'qa_fx_hit 90 4; qa_fx_state dead; qa_halo_revert'),
    (2700, 'qa_fx_state reverted'),
    (3500, 'qa_fx_state later; quit'),
])
s = states(text)
for label in ('before', 'dead', 'reverted'):
    assert label in s, (label, text[-3000:])
before, dead, reverted = s['before'], s['dead'], s['reverted']
assert float(before['shake']) > .9 and before['screen'] == '1' and float(before['blur']) > 9, before
assert dead['hits'] == '1', ('an arc just before the revert', dead)
assert float(reverted['shake']) < .01 and reverted['screen'] == '0' and float(reverted['blur']) == 0, ('the revert clears them', reverted)
assert reverted['hits'] == '0', ('the revert clears the damage arcs', reverted)
assert abs(float(reverted['music']) - .25) < .02, ('the class fade outlives the revert', reverted)
print('a30 section 2: a revert cleared the shake, the blur and the arc and kept the music fade.')

# ---- 7. GL resources after a context rebuild and a new window size
text = run('gl', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_hs_cmd cinematic_screen_effect_start "true"; qa_hs_cmd cinematic_screen_effect_set_convolution "1 2 10 10 0"; '
          'halo_flashlight 1'),
    (800, 'qa_fx_state drawn'),
    (900, 'resetgl'),
    (1800, 'qa_fx_state rebuilt'),
    (1900, 'screenres 640 480'),
    (2800, 'qa_fx_state resized; quit'),
], timeout=180)
s = states(text)
for label in ('drawn', 'rebuilt', 'resized'):
    assert label in s, (label, text[-3000:])


def size(value):
    w, h = value.split('x')
    return int(w), int(h)


drawn, rebuilt, resized = s['drawn'], s['rebuilt'], s['resized']
assert size(drawn['copy']) == (800, 600) and drawn['screen_tex'] == '1', ('the blur copies the 800x600 frame', drawn)
tw, th = size(drawn['copytex'])
if drawn['npot'] == '1':
    assert (tw, th) == (800, 600), ('with non-power-of-two textures the copy is the frame size', drawn)
else:
    assert (tw, th) == (1024, 1024), ('without them, the next powers of two', drawn)
assert rebuilt['screen_tex'] == '1' and size(rebuilt['copy']) == (800, 600), ('the copy is made again after resetgl', rebuilt)
assert rebuilt['flashlight'] == '1', rebuilt
if 'spot_tex' in rebuilt and drawn['spot_tex'] == '1':
    assert rebuilt['spot_tex'] == '1', ('the flashlight spot is made again after resetgl', rebuilt)
assert 'resized' in s and resized['screen_tex'] == '1', resized
if size(resized['copy']) != (800, 600):
    assert size(resized['copy']) == (640, 480), ('the copy follows the new window size', resized)
    print('a30 section 2: blur copy and flashlight spot made again after resetgl; the copy followed 640x480.')
else:
    print('a30 section 2: blur copy and flashlight spot made again after resetgl; screenres kept 800x600 (a fixed window?).')


# ---- 8. effect_new's jpt! damage behind halo_effectdamage (gap 11 of batch 5's critic; off by default)
table, current = {}, None
for line in EFFECTS.read_text().splitlines():
    if line.startswith('path '):
        current = table.setdefault(line[5:], {})
    elif current is not None and line.startswith(('damage ', 'materials ')):
        current[line.split()[0]] = [float(x) for x in line.split()[1:]]
large = table.get(r'effects\explosions\large explosion no objects', {})
assert large.get('damage', [])[:5] == [1.5, 2.5, 5, 15, 25] and large['damage'][6] == 4, ('re-run tools/halo_effects.py', large)
assert len(large.get('materials', [])) == 33, large
assert 'damage' not in table.get(r'effects\explosions\medium explosion cinematic', {}), 'the cinematic variant deals nothing'
text = run('effect-damage', 'halo_scripttrace 1; halo_section 6 1', [
    (400, r'qa_fx_script_effect "effects\explosions\large explosion no objects" enc_swamp2_turret'),
    (600, r'halo_effectdamage 1; qa_fx_script_effect "effects\explosions\large explosion no objects" enc_swamp2_turret'),
    (800, r'qa_fx_script_effect "effects\explosions\medium explosion cinematic" enc_swamp2_turret'),
    (1000, 'quit'),
])
dealt = re.findall(r'HaloScript: (.+?) (deals its jpt! damage|has no jpt! damage)', text)
assert dealt == [(r'effects\explosions\large explosion no objects', 'deals its jpt! damage'),
                 (r'effects\explosions\medium explosion cinematic', 'has no jpt! damage')], ('only with halo_effectdamage 1', dealt, text[-3000:])
print('c10: effect_new deals its jpt! damage only with halo_effectdamage 1; the cinematic variant has none.')
