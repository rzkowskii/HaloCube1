"""Explosion deaths keep Halo's bodies (weapon.cpp halohit, clientgame.cpp dokill, halo.cpp halokilled).

Written while testing was on hold, NOT YET RUN.

Halo destroys no unit's body by damage: a grenade, a blast or a vehicle's destruction leaves the
body, thrown by the jpt!'s instantaneous acceleration times the object's acceleration scale along
the damage direction tilted 0.45 up (../Halo3DS objects/damage.c:1005-1016, units/bipeds.c:1639-1655),
and only a unit destroyed after dying (unit flags bit 1: the infection forms, the carrier, the
Sentinels, the Engineer) goes at the end of its dying (units/units.c:6675-6679, 10021-10043). So:

0. Static (needs HALO_SOURCE): actors.txt's `accelscale` lines equal obje +32 of each variant's
   unit in the caches; the Grunt's is 0.9, the Elite's 0.6, the marine's 1.
1. On the preview at Normal (`botsshoot 0`, `halo_damagetrace 1`) a Grunt killed by the player's frag
   0.8 units off leaves no AssaultCube gib (`gibs=0`), a `DEATH_PUSH` of acceleration 4 and scale 0.9
   whose horizontal velocity points away from the blast, and a body that is drawn (`nocorpse=0`,
   `lies=90000`, no AssaultCube blood: `blood=0`) and still unsunk 6.5 s later (`sink=0.000`), where
   AssaultCube's sank from 2 s on.
2. A marine killed the same way is drawn with `blood=1` and listed for the infection forms
   (`CORPSE_ENTRY kind=9` naming it); an infection form spawned beside takes it ("infection form takes
   a Marine body"), a Flood combat form rises and the marine's body is no longer drawn.
3. An infection form and a carrier killed by frags leave no body (`nocorpse=1`); no gib either.
4. A frag 2 units before the player leaves no ear damage (`ear=0`: AssaultCube's tinnitus).
"""
import mmap
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/halo/actors.txt', 'run tools/halo_actors.py')

GRUNT, INFECTION, CARRIER, MARINE = 0, 6, 7, 9

scales = {}
current = None
for line in (root / 'packages/halo/actors.txt').read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'path':
        current = rest
    elif key == 'accelscale' and current:
        scales[current] = float(rest)
assert scales, ('actors.txt has accelscale lines: re-run tools/halo_actors.py', root / 'packages/halo/actors.txt')

# 0. The acceleration scale against the tag bytes.
if (HALO_SOURCE / 'a10.map').exists():
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_weapons import CAMPAIGN  # noqa: E402
    checked = 0
    for cache in CAMPAIGN:
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group != 'actv' or tag.name not in scales:
                    continue
                unit = halo.reference(halo.tags, tag.address + 20)
                if not unit or unit.group not in ('bipd', 'vehi'):
                    continue
                value, = halo.tags.read('f', unit.address + 32)
                assert abs(scales[tag.name] - value) < 1e-5, (cache, tag.name, scales[tag.name], value)
                checked += 1
    assert checked, 'no actv of actors.txt in the caches'
    print(f'Acceleration scale: {checked} actv units match obje +32.')
for path, want in ((r'characters\grunt\grunt', .9), (r'characters\elite\elite', .6), (r'characters\marine\marine', 1)):
    assert any(abs(v - want) < 1e-5 for p, v in scales.items() if p.startswith(path)), (path, want)


def run(name, schedule, timeout=120):
    profile = scratch(f'corpses-{name}-')
    # The actors do not shoot (botsshoot 0, set once the section runs).
    body = '; '.join(f'sleep {delay} [{"botsshoot 0; " if i == 0 else ""}{action}] 1' for i, (delay, action) in enumerate(schedule))
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_damagetrace 1; halo_preview; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def rows(text, tag, label=None):
    found = []
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] == tag and (label is None or (len(parts) > 1 and parts[1] == label)):
            found.append(dict(re.findall(r'(\w+)=(\S+)', line)))
    return found


def vec(text):
    return [float(v) for v in text.split(',')]


def body(text, label, kind):
    found = [r for r in rows(text, 'CORPSE_QA', label) if int(r['kind']) == kind and r['dead'] == '1']
    assert found, (f'a dead kind {kind} at {label}', text[-2000:])
    return found[0]


# 1. A Grunt killed by the player's frag keeps its body, thrown away from the blast.
text, log = run('grunt', [
    (0, f'qa_actor_spawn {GRUNT} -1 4 0'),
    (1500, f'qa_corpse_kill {GRUNT} 0'),
    (1700, 'qa_corpse_state early'),
    (8200, 'qa_corpse_state late'),
    (8400, 'quit'),
])
kill = rows(text, 'CORPSE_KILL')
assert kill and kill[0]['ok'] == '1', ('the frag went off', log)
blast = vec(kill[0]['pos'])
for label in ('early', 'late'):
    state = rows(text, 'CORPSE_STATE', label)
    assert state and state[0]['gibs'] == '0', (f'{label}: no AssaultCube gibs', state, log)
early, late = body(text, 'early', GRUNT), body(text, 'late', GRUNT)
assert early['nocorpse'] == '0' and early['drawn'] == '1' and early['lies'] == '90000' and early['blood'] == '0', (early, log)
assert late['drawn'] == '1' and float(late['sink']) == 0, ('the body still lies, unsunk, 6.5 s on', late, log)
pushes = [r for r in rows(text, 'DEATH_PUSH')]
assert pushes and abs(float(pushes[0]['acceleration']) - 4) < 1e-3 and abs(float(pushes[0]['scale']) - .9) < 1e-3, (pushes, log)
v, at = vec(pushes[0]['vel']), vec(early['pos'])
assert v[0] * (at[0] - blast[0]) + v[1] * (at[1] - blast[1]) > 0 and v[2] > 0, ('thrown away from the blast and up', v, at, blast, log)
print(f'Grunt: frag kill kept the body (pushed {v}), drawn and unsunk after 6.5 s, no gibs, no red blood.')

# 2. A marine's body from a frag kill is still the infection forms' to take.
text, log = run('marine', [
    (0, f'qa_actor_spawn {MARINE} 1 8 0'),
    (1500, f'qa_corpse_kill {MARINE} 0'),
    (1700, f'qa_corpse_state marine; qa_actor_spawn {INFECTION} 2 6 3'),
    (11000, 'qa_actor_count late; qa_corpse_state risen'),
    (11300, 'quit'),
])
marine = body(text, 'marine', MARINE)
assert marine['drawn'] == '1' and marine['blood'] == '1', (marine, log)
entries = [r for r in rows(text, 'CORPSE_ENTRY', 'marine') if int(r['kind']) == MARINE]
assert entries and entries[0]['body'] == marine['cn'], ('the frag-killed marine is listed for the infection forms', entries, marine, log)
assert 'infection form takes a Marine body' in text, log
late = rows(text, 'FLOOD_QA', 'late')
assert late and int(late[-1]['flood_human']) == 1, ('a combat form rose', late, log)
risen = [r for r in rows(text, 'CORPSE_QA', 'risen') if r['cn'] == marine['cn']]
assert risen and risen[0]['nocorpse'] == '1' and risen[0]['drawn'] == '0', ('the risen body no longer lies there', risen, log)
print('Marine: frag-killed body listed, taken by an infection form; the combat form rose and the body went.')

# 3. Units destroyed after dying leave no body.
text, log = run('destroyed', [
    (0, f'qa_actor_spawn {INFECTION} 2 5 -4; qa_actor_spawn {CARRIER} 2 8 4'),
    (1500, f'qa_corpse_kill {INFECTION} 0'),
    (2500, f'qa_corpse_kill {CARRIER} 0'),
    (2800, 'qa_corpse_state destroyed'),
    (3000, 'quit'),
])
assert rows(text, 'CORPSE_STATE', 'destroyed')[0]['gibs'] == '0', log
for kind in (INFECTION, CARRIER):
    dead = body(text, 'destroyed', kind)
    assert dead['nocorpse'] == '1' and dead['drawn'] == '0', (kind, dead, log)
print('Destroyed after dying: the infection form and the carrier left no body.')

# 4. No AssaultCube tinnitus from a grenade.
text, log = run('ear', [
    (500, 'qa_damage_vitals 75 75; qa_corpse_blast_player 2'),
    (700, 'qa_corpse_state ear'),
    (900, 'quit'),
])
ear = rows(text, 'CORPSE_STATE', 'ear')
assert ear and ear[0]['ear'] == '0', ('no ear damage', ear, log)
print('Ear: a frag two units off leaves no AssaultCube tinnitus.')
