"""Scripted seats (a10's cryotube), effects at an object's marker, and the Monitor spared by ai_kill.

Not yet run: written while testing was on hold (batch 9; see docs/macos-build.md, "Script scenes and
player moves" and "Factions and special enemies").

a10 section 1 (retail collision):
1. Normal (tutorial_setup): the script's (unit_enter_vehicle (player0) cryotube_1 ct-driver) holds
   the player in the cryotube (CARGO_QA boarded=cryotube_1 scripted=1), E does not get them out, the
   view is at the tube's camera marker and does not drift; effect_new_on_object_marker at
   cryotube_1's "driver" marker is placed at the marker; the tutorial's own unit_exit_vehicle lets
   the player out (the tutorial polls time out on their own), within halo_scriptseatmax.
2. Heroic (fast_setup): seated, then let out by the script within a few seconds.
3. halo_scriptseatmax 4 on Normal: the guard lets the player out after about 4 s.
4. halo_scriptseat 0 on Heroic: the player is never seated (the old behaviour).
5. A QA teleport out of the seat lets the player go where they were put.

c20 section 1: ai_kill of the Monitor's squad (bsp0_monitor) leaves him alive; ai_erase removes him
with no death (no killed line, living count 0).
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(root / 'packages/halo/a10.hscb', 'run tools/import_halo_scripts.py')
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=300):
    profile = scratch(f'seats-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_scripttrace 1; halo_fxtrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def cargo(text):
    out = []
    for line in text.splitlines():
        if line.startswith('CARGO_QA ') and ' loaded=1 ' in line:
            row = fields(line)
            row['label'] = line.split()[1]
            out.append(row)
    return out


def point(row):
    return tuple(map(float, row['player'].split(',')))


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** .5


# ---- 1. Normal: the tutorial holds the player in the cryotube and lets them out itself
# A tag without spaces (qa_hs_cmd splits its arguments on spaces): not in effects.txt, so it
# falls back to a burst, still drawn where the marker is.
effect = 's:qa_marker_effect obj:cryotube_1 s:driver'
schedule = [(1500, 'qa_cargo_state seated'), (1700, 'qa_cargo_board; qa_cargo_state pressed'),
            (2500, 'qa_cargo_state still'), (2600, f'qa_hs_cmd effect_new_on_object_marker "{effect}"')]
schedule += [(5000 + 5000 * i, f'qa_cargo_state t{i}') for i in range(36)]
schedule += [(185000, 'quit')]
text, log = run('normal', 'halo_difficulty 1; qa_campaign_play 1 1', schedule, timeout=320)
rows = cargo(text)
by = {r['label']: r for r in rows}
assert 'seated' in by, ('CARGO_QA lines', log)
seated = by['seated']
assert seated['boarded'] == 'cryotube_1' and seated['scripted'] == '1', ('the tutorial seats the player in the cryotube', seated, log)
assert 'Halo: the script seats the player in cryotube_1' in text, log
assert by['pressed']['boarded'] == 'cryotube_1' and by['pressed']['scripted'] == '1', ('E does not get out of a scripted seat', by['pressed'])
assert dist(point(seated), point(by['still'])) < .5, ('held at the seat', seated, by['still'])
assert re.search(r"effect at cryotube_1's marker driver", text), ('effect_new_on_object_marker resolves the model marker', log)
released = [r for r in rows if r['label'].startswith('t') and r['boarded'] == '-']
assert released, ('the tutorial lets the player out of the cryotube', [(r['label'], r['boarded'], r['held']) for r in rows], log)
assert re.search(r'the scripted seat lets the player out \(unit_exit_vehicle', text), ('released by the script, not the guard', log)
assert 'held too long' not in text, log
print(f'a10 Normal: held in the cryotube, E ignored, the marker effect placed, let out by the tutorial by {released[0]["label"]}.')

# ---- 2. Heroic: fast_setup seats and releases at once
text, log = run('heroic', 'halo_difficulty 2; qa_campaign_play 1 1',
                [(300, 'qa_cargo_state early'), (6000, 'qa_cargo_state late'), (6200, 'quit')], timeout=120)
by = {r['label']: r for r in cargo(text)}
assert 'Halo: the script seats the player in cryotube_1' in text, ('fast_setup seats the player too', log)
assert by['late']['boarded'] == '-', ('fast_setup lets the player out within seconds', by.get('late'), log)
assert re.search(r'the scripted seat lets the player out \(unit_exit_vehicle', text), log
print('a10 Heroic: fast_setup seated the player and let them out within six seconds.')

# ---- 3. The guard: halo_scriptseatmax
text, log = run('guard', 'halo_difficulty 1; halo_scriptseatmax 4; qa_campaign_play 1 1',
                [(1500, 'qa_cargo_state seated'), (7000, 'qa_cargo_state guarded'), (7200, 'quit')], timeout=120)
by = {r['label']: r for r in cargo(text)}
assert by['seated']['scripted'] == '1' and by['guarded']['boarded'] == '-', (by, log)
assert 'held too long (halo_scriptseatmax)' in text, log
print('a10: halo_scriptseatmax 4 let the player out after about 4 s.')

# ---- 4. Off
text, log = run('off', 'halo_difficulty 2; halo_scriptseat 0; qa_campaign_play 1 1',
                [(300, 'qa_cargo_state early'), (1500, 'qa_cargo_state mid'), (1700, 'quit')], timeout=120)
assert 'the script seats the player' not in text and all(r['boarded'] == '-' for r in cargo(text)), log
print('a10: halo_scriptseat 0 leaves the player free.')

# ---- 5. A QA teleport out of the seat
text, log = run('moved', 'halo_difficulty 1; qa_campaign_play 1 1',
                [(1500, 'qa_cargo_state seated'), (1700, 'qa_campaign_advance'), (2200, 'qa_cargo_state moved'), (2400, 'quit')],
                timeout=120)
by = {r['label']: r for r in cargo(text)}
assert by['seated']['scripted'] == '1', (by, log)
assert by['moved']['boarded'] == '-' and dist(point(by['seated']), point(by['moved'])) > 2, (by, log)
assert 'lets the player out (moved elsewhere' in text, log
print('a10: a QA teleport out of the scripted seat is kept.')

# ---- c20 section 1: the Monitor survives ai_kill and ai_erase takes him away silently
text, log = run('monitor', 'qa_campaign_resume 7 1', [
    (300, 'qa_hs_ai ai_living_count bsp0_monitor 0'),
    (400, 'qa_hs_ai ai_place bsp0_monitor 1'),
    (900, 'qa_hs_ai ai_living_count bsp0_monitor 0'),
    (1000, 'qa_hs_ai ai_kill bsp0_monitor 0'),
    (1300, 'qa_hs_ai ai_living_count bsp0_monitor 0'),
    (1400, 'qa_hs_ai ai_erase bsp0_monitor 0'),
    (1700, 'qa_hs_ai ai_living_count bsp0_monitor 0; quit'),
], timeout=120)
counts = [float(m) for m in re.findall(r'HS_AI ai_living_count bsp0_monitor (\S+)', text)]
assert len(counts) == 4, (counts, log)
assert counts[1] >= 1, ('the Monitor is placed', counts, log)
assert counts[2] == counts[1], ('ai_kill spares the Monitor', counts, log)
assert counts[3] == 0, ('ai_erase removes him', counts, log)
assert 'ai_kill spares' in text and 'ai_erase removes' in text, log
print('c20 section 1: ai_kill spared the Monitor and ai_erase removed him without a death.')
