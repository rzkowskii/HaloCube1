"""Nav points and the HUD parts the mission scripts hide or blink.

NOT YET RUN: written while testing was on hold.

1. packages/halo/hud.txt (tools/halo_hud.py) has the hud globals' waypoint bitmap and its ten
   arrow types, default and default_red among them. No game needed.
2. QA client on a30 section 2 (Halo): activate_team_nav_point_flag puts a default_red nav point
   on cave_nav_flag (raised by the offset), activate_team_nav_point_object one on an object;
   deactivating removes each; hud_show_health false hides the health meter, hud_blink_shield
   true flashes the shield; a revert brings back the nav points and HUD state of the checkpoint.
   A screenshot (build/qa/nav-points-*.png) shows the arrow for a visual check.
"""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

hud = root / 'packages/halo/hud.txt'
require(hud, 'run tools/halo_hud.py')
lines = hud.read_text().splitlines()
waypoint = [line for line in lines if line.startswith('waypoint ')]
arrows = {line.split()[1]: line.split() for line in lines if line.startswith('arrow ')}
assert len(waypoint) == 1 and int(waypoint[0].split()[1]) >= 0, waypoint
assert {'default', 'default_red'} <= set(arrows) and len(arrows) == 10, sorted(arrows)
assert arrows['default'][5:8] == ['1', '0', '6'], arrows['default']
print('hud.txt carries the waypoint bitmap and the ten arrow types.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
profile = scratch('nav-points-')
steps = [
    (100, 'qa_nav_state none'),
    (200, 'qa_hs_cmd activate_team_nav_point_flag "s:default_red s:player s:cave_nav_flag 0.5"; qa_nav_state flag'),
    (400, 'screenshot nav-points-flag'),
    (600, 'qa_hs_cmd activate_team_nav_point_flag "s:default s:player s:cave_nav_flag 0"; qa_nav_state replaced'),
    (800, 'qa_hs_cmd hud_show_health "false"; qa_hs_cmd hud_blink_shield "true"; qa_nav_state hud'),
    (1000, 'qa_checkpoint_save'),
    (1200, 'qa_hs_cmd deactivate_team_nav_point_flag "s:player s:cave_nav_flag"; qa_hs_cmd hud_show_health "true"; qa_nav_state off'),
    (1400, 'qa_halo_damage 100000'),
    (3200, 'qa_halo_revert'),
    (4200, 'qa_nav_state reverted'),
    (4400, 'quit'),
]
body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in steps)
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                    f'-ehalo_difficulty 2; qa_campaign_play 2 2; qa_after_gameplay [{body}]'],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=120, check=True)
text = log.read_text(errors='replace')


def state(name):
    found = [line for line in text.splitlines() if line.startswith(f'QA_NAV {name} ')]
    assert found, (name, text[-2000:])
    return found[0]


def points(name):
    return [line for line in text.splitlines() if line.startswith(f'QA_NAV_POINT {name} ')]


assert 'count=0' in state('none'), state('none')
assert 'count=1' in state('flag') and 'type=default_red' in points('flag')[0], points('flag')
assert 'count=1' in state('replaced') and 'type=default ' in points('replaced')[0] + ' ', points('replaced')
# Offset 0.5 Halo units raises the point by half the section's vertical scale.
raised = float(points('flag')[0].split('at=')[1].split()[0].split(',')[2])
flat = float(points('replaced')[0].split('at=')[1].split()[0].split(',')[2])
assert raised > flat, (raised, flat)
assert 'hidden=1' in state('hud') and 'blinking=2' in state('hud'), state('hud')
assert 'count=0' in state('off') and 'hidden=0' in state('off'), state('off')
assert 'count=1' in state('reverted') and 'hidden=1' in state('reverted'), state('reverted')
print('Nav points activate, replace, deactivate and revert with the checkpoint; HUD parts hide and blink.')
