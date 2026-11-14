"""Retail weapon behaviour in the QA client.

On a30 section 2 (no enemies, three placed weapons): the assault rifle's rate
of fire, pistol and sniper zoom, plasma rifle overheat and recovery, the
needler's rate ramp, a charged plasma pistol shot, swapping for a placed weapon
and the loadout after a revert. On a30 section 3: enemies carry their actor
variant's weapon and drop it when killed.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, section, schedule, timeout=90):
    profile = scratch(f'weapon-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-eqa_campaign_resume 2 {section}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    text = log.read_text(errors='replace')
    states = {}
    for line in text.splitlines():
        if line.startswith('WEAPON_QA '):
            fields = line.split()
            states[fields[1]] = dict(field.split('=', 1) for field in fields[2:] if '=' in field)
    return text, states, log


text, s, log = run('player', 2, [
    (100, 'botsshoot 0; qa_weapon_state start'),
    (300, 'qa_weapon_fire 1000'),
    (1600, 'qa_weapon_state ar'),
    (1800, 'qa_weapon_switch'),
    (2600, 'qa_weapon_zoom; qa_weapon_state pistol-zoom'),
    (2800, 'qa_weapon_zoom; qa_weapon_state pistol-unzoom'),
    (3000, 'qa_weapon_give sr'),
    (3800, 'qa_weapon_zoom; qa_weapon_state sniper-1'),
    (4000, 'qa_weapon_zoom; qa_weapon_state sniper-2'),
    (4200, 'qa_weapon_zoom; qa_weapon_give pr; qa_weapon_fire 4500'),
    (6200, 'qa_weapon_state pr-a'), (7200, 'qa_weapon_state pr-b'), (8200, 'qa_weapon_state pr-c'),
    (12000, 'qa_weapon_state pr-cool'),
    (12200, 'qa_weapon_give ne; qa_weapon_state ne-start'),
    (12400, 'qa_weapon_fire 1200'),
    (13500, 'qa_weapon_state ne-ramped'),
    (13800, 'qa_weapon_give pp'),
    (14600, 'qa_weapon_charge 1000'),
    (16000, 'qa_weapon_state pp-charged'),
    # The section's placements are assault rifles: carry a pistol instead so one can be swapped in.
    (16100, 'qa_weapon_switch'),
    (16700, 'qa_weapon_give hp'),
    (16900, 'qa_weapon_goto_ground'),
    (17200, 'qa_weapon_state ground'),
    (17300, 'halo_use'),
    (18300, 'qa_weapon_state swapped'),
    (18500, 'qa_halo_damage 1000'),
    (19700, 'qa_halo_revert'),
    (20700, 'qa_weapon_state reverted'),
    (21200, 'quit'),
])
need = ['start', 'ar', 'pistol-zoom', 'pistol-unzoom', 'sniper-1', 'sniper-2', 'pr-cool', 'ne-start', 'ne-ramped',
        'pp-charged', 'ground', 'swapped', 'reverted']
missing = [n for n in need if n not in s]
assert not missing, (missing, log)
i = lambda name, key: int(s[name][key])
f = lambda name, key: float(s[name][key])

assert s['start']['weapon'] == 'ar' and s['start']['other'] == 'hp' and i('start', 'mag') == 60 and i('start', 'ammo') == 180
shots = i('ar', 'shots') - i('start', 'shots')
assert 12 <= shots <= 18, f'assault rifle fired {shots} shots in one second, expected about 15'
assert i('ar', 'mag') == 60 - shots

# Zoom narrows the field of view by the tag magnification: tan(fov/2) scales by 1/m.
import math
def magnification(name):
    return math.tan(math.radians(f(name, 'basefov') / 2)) / math.tan(math.radians(f(name, 'fov') / 2))
assert s['pistol-zoom']['weapon'] == 'hp' and abs(magnification('pistol-zoom') - 2) < 0.05, s['pistol-zoom']
assert i('pistol-unzoom', 'zoom') == 0 and f('pistol-unzoom', 'fov') == f('pistol-unzoom', 'basefov')
assert abs(magnification('sniper-1') - 2) < 0.05 and abs(magnification('sniper-2') - 8) < 0.2, (s['sniper-1'], s['sniper-2'])

hot = [s[n] for n in ('pr-a', 'pr-b', 'pr-c') if n in s]
assert any(state['overheated'] == '1' for state in hot), ('plasma rifle never overheated', hot)
assert s['pr-cool']['overheated'] == '0' and f('pr-cool', 'heat') <= 0.26, s['pr-cool']
assert f('pr-cool', 'battery') < 1.0, 'plasma rifle shots drain the battery'

assert i('ne-start', 'delay') >= 300 and i('ne-ramped', 'delay') <= 120, (s['ne-start'], s['ne-ramped'])
assert s['pp-charged']['overheated'] == '1' and f('pp-charged', 'battery') <= 0.9, s['pp-charged']

assert i('ground', 'nearby') >= 0, s['ground']
assert s['ground']['weapon'] == 'hp' and s['swapped']['weapon'] == 'ar' and s['swapped']['other'] == 'pp', s['swapped']
# Standing among the other placed rifles collects their rounds into the reserve.
assert i('swapped', 'ammo') > 180, ('walking over carried weapon types takes their ammo', s['swapped'])
assert s['reverted']['weapon'] == 'ar' and s['reverted']['other'] == 'hp' and i('reverted', 'mag') == 60 \
    and i('reverted', 'ammo') == 180, s['reverted']
print(f'Player weapons: {shots} AR shots/s, 2x/8x zoom, overheat and recovery, needler ramp, charged shot, swap, revert.')

text, s, log = run('enemies', 3, [
    (100, 'botsshoot 0; qa_weapon_state before'),
    # This section's actors are in the second wave: walk to the checkpoint to spawn them.
    (200, 'qa_campaign_reach'),
    (2200, 'qa_campaign_reach'),
    (4500, 'qa_enemy_weapons; botsshoot 0; qa_weapon_state before-kill'),
    (4700, 'qa_halo_clear'),
    (5700, 'qa_weapon_state after'),
    (6000, 'quit'),
])
armed = re.findall(r'ENEMY_WEAPON \S+(?: \S+)? (\S+) damage=(\d+)', text)
assert armed and any(label != '-' for label, _ in armed), (armed, log)
assert int(s['after']['ground']) > int(s['before-kill']['ground']), ('killed enemies drop their weapons', s, log)
print(f'Enemies: {len(armed)} armed with retail weapons, drops on death.')
