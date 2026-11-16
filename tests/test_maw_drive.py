"""The Maw's Warthog escape, driven (source/src/halo_vehicle.cpp).

Written while testing was on hold: not yet run.

QA client, campaign play with the mission script (qa_maw_route starts section 6):
1. Section 6 starts the escape: on retail collision (DRIVEN_SECTIONS) the player starts at the
   wheel; on stock geometry on foot beside the parked Warthogs (or one the engine adds), and E
   by the driver's door gets in. E gets out and back in. The HUD countdown runs.
2. The QA autopilot drives at the route markers: section 6 hands over to section 7 with the
   player still at the wheel and the countdown carried over (not reset).
3. Section 8 to the finale: the QA vehicle walker (qa_retail_start, tests/native/retail_qa.inc)
   drives halo_d40_08.route's planned rows at full throttle and reaches grand_finale; the finale
   plays and the campaign ending is reached before the deadline. Batch 10: section 8's drive (the
   pit room's drop under the hanging bridge segment) is planned below full speed. The jump governor
   batch: no QA cruise cap any more; the section's map .cfg holds the car at the planned speed by
   each planned take-off (`halo_vehiclegovern`, halo_vehiclegovernor on by default), as it holds a
   player at full throttle, and the run must log the governor holding (halo_vehicletrace). The
   walker, not the autopilot: the autopilot steers straight at the objective marker and need not
   take the planned drop. Not yet run.
4. Killed at the wheel, the player reverts to the section start in a Warthog again.
5. Slow keystroke run in the packaged app: E, then W held, exits cleanly (the old smoke test).
6. The escape Warthog and the scenario's: on retail collision section 6 seats the player in one
   of the scenario's parked Warthogs (trench_jeep1 to 3, asspain_1 to 3; created then if the
   script has not yet), so once the script has created the others none stands on the player's;
   the script's seat tests (trench_jeep_test) see the player at the wheel in sections 6, 7 and 8.
   Added with the vehicle fixes; not yet run.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=420, env=None):
    profile = scratch(f'maw-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_vehicletrace 1; maxfps 120; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True,
                       env={**os.environ, **env} if env else None)
    return log.read_text(errors='replace'), log


def rows(text, kind):
    return [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith(f'VEHICLE_QA {kind}')]


# 1. Section 6: the start, getting in and out.
text, log = run('start', 'qa_maw_route', [
    (500, 'qa_vehicle_escape; qa_vehicle_state start'), (3500, 'qa_vehicle_state later'),
    (3600, 'qa_vehicle_escape'), (3700, 'quit')])
retail = 'retail collision surfaces for halo_d40_06' in text
start = rows(text, 'start')[0]
escape = rows(text, 'escape')
assert escape and escape[0]['escaping'] == '1' and escape[0]['section'] == '6', (escape, log)
if retail:
    assert start['seated'] == '1' and start['role'] == '0', ('on retail collision section 6 starts at the wheel', start, log)
    assert 'escape Warthog at' in text, log
else:
    later = rows(text, 'later')[0]
    assert start['seated'] == '0', ('on stock geometry section 6 starts on foot', start, log)
    assert int(later['count']) >= 1, ('a Warthog stands by the start', later, log)
remaining = [int(e['remaining']) for e in escape]
assert 0 < remaining[1] < remaining[0], ('the countdown runs', remaining, log)
print(f'd40 section 6 ({"retail collision" if retail else "stock geometry"}): escape started, countdown {remaining}.')

# Getting in by the driver's door (the player's Warthog, or the nearest), out, and in again.
text, log = run('door', 'qa_maw_route', [
    (3000, 'qa_vehicle_state parked'), (3200, 'qa_vehicle_approach -1 0'), (3400, 'qa_vehicle_use'),
    (3600, 'qa_vehicle_use'), (3800, 'qa_vehicle_state out'), (4000, 'qa_vehicle_use'),
    (4200, 'qa_vehicle_state back'), (4400, 'quit')])
approach = rows(text, 'approach')[0]
assert approach['found'] == '1' and 'Drive' in text.split('VEHICLE_QA approach')[1].splitlines()[0], (approach, log)
uses = rows(text, 'use')
assert len(uses) == 3, (uses, log)
out = rows(text, 'out')[0]
back = rows(text, 'back')[0]
assert uses[0]['seated'] == '1' and out['seated'] == '0' and back['seated'] == '1', (uses, out, back, log)
print('E at the driver door gets in, out and back in.')

# 2. Drive section 6 to section 7.
text, log = run('drive6', 'qa_maw_route', [(500, 'qa_vehicle_autopilot 170'), (172000, 'qa_vehicle_escape; quit')],
                timeout=300)
escapes = rows(text, 'escape')
sections = [e['section'] for e in escapes]
assert '7' in sections, ('the autopilot drives section 6 into section 7', sections[-5:], log)
first7 = next(i for i, e in enumerate(escapes) if e['section'] == '7')
last6 = escapes[first7 - 1]
assert escapes[first7]['seated'] == '1', ('the player arrives in section 7 at the wheel', escapes[first7], log)
assert int(escapes[first7]['remaining']) <= int(last6['remaining']) + 100, ('the countdown carries over', last6, escapes[first7], log)
print(f'section 6 driven to section 7 with {int(escapes[first7]["remaining"]) / 1000:.0f} s left.')

# 3. Section 8 to the finale: the vehicle walker drives the planned rows at full throttle, and the
#    jump governors of halo_d40_08.cfg (not a QA cruise cap) hold it at the planned speed by the
#    take-offs (the drop needs it: `vehicle_route.drive.speed` in the metrics).
metrics8 = root / 'packages/maps/halo_d40_08.metrics.json'
route8 = root / 'packages/maps/halo_d40_08.route'
require(route8, 'run tools/import_halo_campaign.py "$maps" --mission d40 --part 8')
drive8 = ((json.loads(metrics8.read_text()).get('vehicle_route') or {}).get('drive') or {}) if metrics8.is_file() else {}
governed8 = bool(drive8.get('governors'))
text, log = run('drive8', 'halo_vehiclegovernor 1; sleep 600 [qa_retail_start 10 8] 1', [(500, 'qa_vehicle_governors'), (175000, 'quit')],
                timeout=300, env={'HALOQA_RETAIL_ROUTE': str(route8), 'HALOQA_RETAIL_CHAIN': ''})
assert 'escape Warthog at' in text, ('section 8 starts at the wheel', log)
if governed8:
    count = re.search(r'VEHICLE_QA governors count=(\d+) enabled=1', text)
    assert count and int(count.group(1)) == len(drive8['governors']), \
        ('halo_d40_08.cfg\'s jump governors are loaded (re-import d40 section 8 if not)', count and count.group(0), log)
    assert 'Halo vehicle: governor at' in text, ('the governor held the car at a planned take-off', log)
# The walker stops the run once the finale's cue takes over at the route's end (RETAIL_QA PASS
# exit-cue) or the section completes; a FAIL names where it stopped.
assert re.search(r'RETAIL_QA PASS \S+ map=halo_d40_08\b', text) or 'Halo ending reached' in text or 'cinematic_finale' in text, \
    ('the walker reaches grand_finale and the finale plays', re.findall(r'RETAIL_QA FAIL .*', text)[:1], log)
assert 'cinematic_time_up' not in text, ('before the deadline', log)
print(f'section 8 driven along its planned route to the finale before the deadline'
      f'{", held at " + str(drive8["speed"]) + " by the jump governors" if governed8 else ""}.')

# 4. Killed at the wheel: revert into a Warthog at the start of the section.
text, log = run('death', 'qa_campaign_play 10 7', [
    (500, 'qa_vehicle_state alive'), (800, 'qa_halo_damage 1000'), (2200, 'qa_halo_revert'),
    (3500, 'qa_vehicle_state reverted'), (3700, 'quit')])
alive, reverted = rows(text, 'alive')[0], rows(text, 'reverted')[0]
assert alive['seated'] == '1' and reverted['seated'] == '1' and reverted['alive'] == '1', (alive, reverted, log)
print('a player killed at the wheel reverts into a Warthog.')

# 6. The escape takes over a scenario Warthog in section 6 (retail collision), and the script's
#    seat tests see the player at the wheel in sections 6 to 8.
JEEPS = ['trench_jeep1', 'trench_jeep2', 'trench_jeep3', 'trench_jeep4', 'asspain_1', 'asspain_2', 'asspain_3']
tests = [(3600 + 50 * i, f'qa_hs_cmd vehicle_test_seat_list "s:{name} s:w-driver -3"') for i, name in enumerate(JEEPS)]
for start_command, label in (('qa_maw_route', '6'), ('qa_campaign_play 10 7', '7'), ('qa_campaign_play 10 8', '8')):
    text, log = run(f'jeeps{label}', start_command, [(500, 'qa_vehicle_state start'), (3400, 'qa_vehicle_state later')] + tests +
                    [(4200, 'quit')])
    head = rows(text, 'start')[0]
    seat = [float(line.split()[2]) for line in text.splitlines() if line.startswith('HS_CMD vehicle_test_seat_list')]
    assert len(seat) == len(JEEPS) and any(seat), ('trench_jeep_test sees the player at the wheel', label, seat, log)
    if label == '6' and 'retail collision surfaces for halo_d40_06' in text:
        assert head['seated'] == '1', (head, log)
        later = [r for r in rows(text, 'later') if 'vehicle' in r and 'type' in r]
        mine = next(r for r in later if r['vehicle'] == rows(text, 'later')[0]['vehicle'])
        assert mine['object'] != '-1' and mine['escape'] == '1', ('the escape took a scenario Warthog', mine, log)
        here = tuple(map(float, mine['pos'].split(',')))
        crowded = [r for r in later if r is not mine and r['type'] == 'warthog' and r['exists'] == '1' and
                   sum((a - b) ** 2 for a, b in zip(map(float, r['pos'].split(',')), here)) ** .5 < float(mine['length'])]
        assert not crowded, ('no second Warthog on the player\'s', crowded, log)
    print(f'section {label}: seat tests {seat}.')

# 5. Keystrokes into the packaged app (slow tier): E, then W held, then a clean exit.
profile = Path(tempfile.mkdtemp(prefix='maw-native-', dir=SCRATCH))
helper = qa_key()
startup = 'halo_section 10 6; sleep 1800 [screenshot maw-start] 1; sleep 5300 [screenshot maw-driving] 1; sleep 6100 [quit] 1'
with (SCRATCH / 'maw-native.log').open('w') as output:
    process = subprocess.Popen([APP_EXE, f'--home={profile}', '-t0', '-w1280', '-h800', f'-e{startup}'],
                               stdout=output, stderr=subprocess.STDOUT)
    try:
        time.sleep(2.2)
        subprocess.run([helper, str(process.pid), '14', '0.1'], check=True, timeout=3)   # E
        subprocess.run([helper, str(process.pid), '13', '1.75'], check=True, timeout=4)  # W
        subprocess.run([helper, str(process.pid), '2', '0.25'], check=True, timeout=3)   # D
        assert process.wait(timeout=14) == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
print(profile)
