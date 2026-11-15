"""Fall and wedge recovery on retail BSP collision (halo.cpp updateRecovery).

    python3 tests/test_retail_recovery.py

Starts The Pillar of Autumn's first section (halo_a10_01, which has been walked on retail
collision), lets the player stand on the cryo bay floor, and then, with qa_retail_recover,
  * drops them 20 units below the section's mesh: the engine puts them back on the last retail
    floor they stood on ("Halo recovery: fell below the retail collision"),
  * puts them in the open air past the mesh's edge with nothing below: they are put back once
    they have fallen below the mesh or have been airborne for two seconds over nothing,
  * sinks their feet 0.6 units into the floor: after 0.75 s wedged they are nudged onto the
    nearest clear floor ("wedged in the collision").
After each, the player is alive, standing, on retail collision and within 4 units of where they
stood before, and nothing reverted to the checkpoint.
Then the teleport rule (halo.cpp haloplayermoved, which every move that is not walking calls):
qa_halo_moved drops the safe floors and starts the one-second grace; a wedge right after it is
still in the floor 1.1 s later (grace, then 0.75 s wedged) and nudged out by 3.3 s, after which
the player has safe floors again. Not yet run in the game (written while testing was on hold);
see docs/macos-build.md.
"""
import math
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/maps/halo_a10_01.hcol', 'run tools/import_halo_campaign.py (a10 is a retail-collision mission)')

profile = scratch('retail-recovery-')
logfile = SCRATCH / 'retail-recovery.log'
# Delays count from live gameplay, after the opening cinematic is skipped.
script = ('halo_scripttrace 1; halo_difficulty 2; qa_campaign_play 1 1; qa_after_gameplay ['
          'sleep 1500 [qa_retail_where; qa_retail_recover fall] 1; '
          'sleep 4500 [qa_retail_where] 1; '
          'sleep 5500 [qa_retail_recover void] 1; '
          'sleep 10500 [qa_retail_where] 1; '
          'sleep 11500 [qa_retail_recover wedge] 1; '
          'sleep 14000 [qa_retail_where; qa_hs_state settled] 1; '
          'sleep 14200 [qa_halo_moved; qa_hs_state moved; qa_retail_recover wedge] 1; '
          'sleep 15300 [qa_retail_where; qa_hs_state grace] 1; '
          'sleep 17500 [qa_retail_where; qa_hs_state after; quit] 1]')
with logfile.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480', '-e' + script],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=120, check=True)
log = logfile.read_text(errors='replace')

assert re.search(r'loaded \d+ retail collision surfaces for halo_a10_01\b', log), 'retail collision not active: ' + str(logfile)
starts = re.findall(r'RETAIL_QA recover-from (\w+) feet=(\S+),(\S+),(\S+)', log)
assert [kind for kind, *_ in starts] == ['fall', 'void', 'wedge', 'wedge'], (starts, str(logfile))
recoveries = re.findall(r'Halo recovery: ([^,]+), player put back at (\S+),(\S+),(\S+)', log)
reasons = [reason for reason, *_ in recoveries]
assert 'fell below the retail collision' in reasons, (reasons, str(logfile))
assert any(r in reasons for r in ('fell out of the world', 'fell below the retail collision')) and len(recoveries) >= 3, \
    (reasons, str(logfile))
assert 'wedged in the collision' in reasons, (reasons, str(logfile))
assert 'reverting to the checkpoint' not in log, 'a recovery found no safe floor: ' + str(logfile)
where = re.findall(r'RETAIL_QA where map=(\S+) collision=(\d) feet=(\S+),(\S+),(\S+) onfloor=(\d) alive=(\d)', log)
# One report before the first drop and one after each recovery (the last at 14 s, before the
# teleport), then the teleport rule's two: inside the grace (15.3 s) and after the nudge (17.5 s).
assert len(where) == 6, (where, str(logfile))
for (kind, *before), (_, collision, *after_onfloor) in zip(starts[:3], where[1:4]):
    x, y, z, onfloor, alive = after_onfloor
    assert collision == '1' and onfloor == '1' and alive == '1', (kind, where, str(logfile))
    offset = math.dist(tuple(map(float, before[:2])), (float(x), float(y)))
    assert offset <= 4.0, f'{kind}: put back {offset:.1f} units from where the player stood ({logfile})'
print('Retail collision recovery put the player back after a fall below the mesh, a fall past its edge and a wedge.')

# The teleport rule: the safe floors go, and the wedge check waits out the grace.
hs = {m.group(1): dict(re.findall(r'(\w+)=(\S+)', m.group(2))) for m in re.finditer(r'QA_HS_STATE (\w+) (.*)', log)}
assert int(hs['settled']['safe']) >= 1 and hs['settled']['grace'] == '0', (hs, str(logfile))
assert hs['moved']['safe'] == '0' and hs['moved']['grace'] == '1', ('a teleport drops the safe floors', hs, str(logfile))
assert 'Halo moved: QA teleport' in log, str(logfile)
stood = float(starts[3][3])
grace_z, after_z = float(where[4][4]), float(where[5][4])
assert grace_z < stood - .3, f'the wedge check nudged inside the grace (feet {grace_z:.2f}, stood at {stood:.2f}; {logfile})'
assert abs(after_z - stood) < .5 and where[5][5] == '1' and where[5][6] == '1', (where[5], str(logfile))
assert int(hs['after']['safe']) >= 1, ('safe floors are recorded again after the move', hs, str(logfile))
assert log.count('wedged in the collision') >= 2, str(logfile)
print('A teleport dropped the safe floors and held the wedge check for its grace, then the wedge was nudged out.')
