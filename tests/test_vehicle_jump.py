"""The Warthog's jumps on d40 section 8's planned drive, driven in the game (no walk).

NOT YET RUN: written while testing was on hold.

tools/import_halo_campaign.py plans d40 section 8 (the Maw's last stretch) with jumps
(halo_collision.VehicleGround.jump): the Warthog leaves the ground with its velocity along the
surface it was on and falls at its retail-derived gravity (vehicle_gravity: the world's gravity
from the globals' falling damage block at the section's scale, times the square of how much faster
the engine's Warthog runs than Halo's). source/src/halo_vehicle.cpp flies the same launch and
gravity (simulate, vehicleGravity). In the pit room the planned drive leaves the entry road, drops
about 23 Halo units under the hanging bridge segment into the lower bridge's bowl, and the bowl's
far side kicks the Warthog over the gap into the corridor that leads down to grand_finale. The drop
clears the hanging segment only below about 0.78 of the Warthog's top speed, so the drive is planned
at 0.75 of it (`vehicle_route.drive.speed` in the metrics, 27 units a second). Every planned jump
is checked: the pit room's two and a short hop further on (about x 688). This checks the
engine against the plan without driving the whole section:

1. Static: halo_d40_08.route ends in its exit (metrics `ends_in_exit`), has at least two jump rows
   (flag 2 on a step longer than 1.5 cells), and its metrics record the drive's gravity, time and
   speed; vehicles.txt carries the Warthog's `physics` line.
2. QA client on d40 section 8 (the escape seats the player at the wheel), the QA throttle capped at
   the planned speed (`qa_vehicle_cruise halo_d40_08 <speed>`): for each planned jump, the Warthog
   is put on the route about 30 units before the take-off row, heading at it, at the planned speed,
   and held at full throttle straight ahead (`qa_vehicle_jump`), its motion logged every 50 ms.
   It passes when the engine's gravity matches
   the metrics' (within 0.5), and each jump leaves the ground, comes down on floor within 3 units
   of the landing row's height, at least 60% of the planned jump's length on along the take-off's
   heading, upright, and never falls more than 8 units below the landing row (into the pit).
   Otherwise it prints the motion lines to compare with the planner's arc.
"""
import json
import math
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

maps = root / 'packages/maps'
route = maps / 'halo_d40_08.route'
metrics_path = maps / 'halo_d40_08.metrics.json'
require(route, 'run tools/import_halo_campaign.py "$maps" --mission d40 --part 8')
require(metrics_path, 'run tools/import_halo_campaign.py "$maps" --mission d40 --part 8')
table = root / 'packages/halo/vehicles.txt'
require(table, 'run tools/halo_vehicles.py "$maps"')

# 1. The plan.
rows = []
for line in route.read_text().splitlines():
    words = line.split()
    if len(words) >= 3:
        rows.append(((float(words[0]), float(words[1]), float(words[2])), int(words[3]) if len(words) > 3 else 0))
metrics = json.loads(metrics_path.read_text())
assert metrics.get('ends_in_exit'), ('the planned drive does not reach grand_finale', metrics.get('exit_offset'))
drive = (metrics.get('vehicle_route') or {}).get('drive')
assert drive and drive.get('gravity') and drive.get('seconds') and drive.get('speed'), \
    ('metrics without the drive totals: re-run the import', drive)
jumps = [i for i in range(1, len(rows)) if rows[i][1] & 2 and math.dist(rows[i - 1][0][:2], rows[i][0][:2]) > 1.5]
assert len(jumps) >= 2, ('expected the pit room jumps', jumps)
warthog = table.read_text().split('vehicle 2 warthog', 1)[-1].split('\nend', 1)[0]
assert '\nphysics ' in warthog, 'vehicles.txt has no physics line for the Warthog: re-run tools/halo_vehicles.py'
print(f'halo_d40_08.route: {len(rows)} rows to grand_finale, {len(jumps)} jumps, gravity {drive["gravity"]}, '
      f'about {drive["seconds"]} s at {drive["speed"]} units a second (top {drive.get("top")}).')

# 2. The jumps in the game.
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
SPEED, RUNUP, HOLD = float(drive['speed']), 30, 4500
steps = [(100, f'qa_combat_protect 1; qa_vehicle_protect 1; qa_vehicle_cruise halo_d40_08 {SPEED}; qa_vehicle_state start')]
clock = 2500
plans = []
for number, landing in enumerate(jumps):
    takeoff = landing - 1
    first = takeoff
    # Back along the route, but not over an earlier jump: the run-up starts on its landing at most.
    while first > 0 and first not in jumps and math.dist(rows[first][0][:2], rows[takeoff][0][:2]) < RUNUP:
        first -= 1
    (x0, y0, z0), _ = rows[first]
    (x1, y1, _), _ = rows[takeoff]
    yaw = math.degrees(math.atan2(x1 - x0, -(y1 - y0))) % 360
    plans.append((number, first, takeoff, landing, yaw))
    steps += [(clock, f'echo JUMP_QA begin {number}; qa_vehicle_jump {x0} {y0} {z0} {yaw:.2f} {SPEED} {HOLD}'),
              (clock + HOLD + 200, f'echo JUMP_QA end {number}')]
    clock += HOLD + 600
steps.append((clock + 200, 'quit'))
body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in steps)
profile = scratch('vehicle-jump-')
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                    f'-ehalo_vehicletrace 1; maxfps 120; halo_difficulty 1; qa_campaign_play 10 8; qa_after_gameplay [{body}]'],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=120 + clock // 1000, check=True)
text = log.read_text(errors='replace')
assert 'escape Warthog at' in text, ('section 8 starts at the wheel', log)

# The motion lines between each jump's begin and end markers.
sections, current = {}, None
for line in text.splitlines():
    if line.startswith('JUMP_QA begin'):
        current = int(line.split()[2])
        sections[current] = []
    elif line.startswith('JUMP_QA end'):
        current = None
    elif current is not None and line.startswith('VEHICLE_QA jump t='):
        values = dict(re.findall(r'(\w+)=(\S+)', line))
        sections[current].append({'t': int(values['t']), 'pos': tuple(map(float, values['pos'].split(','))),
                                  'grounded': values['grounded'] == '1', 'flipped': values['flipped'] == '1',
                                  'gravity': float(values['gravity']), 'tangent': float(values['tangent'])})

failures = []
for number, first, takeoff, landing, yaw in plans:
    samples = sections.get(number) or []
    if not samples:
        failures.append(f'jump {number}: no motion lines (qa_vehicle_jump did not run: seated?)')
        continue
    if abs(samples[0]['gravity'] - drive['gravity']) > .5:
        failures.append(f'jump {number}: the engine\'s gravity {samples[0]["gravity"]} is not the planner\'s {drive["gravity"]}')
    (tx, ty, tz), _ = rows[takeoff]
    (lx, ly, lz), _ = rows[landing]
    planned = math.dist((tx, ty), (lx, ly))
    heading = (math.sin(math.radians(yaw)), -math.cos(math.radians(yaw)))
    air = next((i for i, s in enumerate(samples) if not s['grounded']), None)
    down = next((i for i in range(air + 1, len(samples)) if samples[i]['grounded']), None) if air is not None else None
    # Lowest between the take-off and the landing (after it the drive goes on down the corridor).
    lowest = min(s['pos'][2] for s in samples[air:down + 1]) if air is not None and down is not None else None
    detail = '; '.join(f'{s["t"]}:{s["pos"][0]:.1f},{s["pos"][1]:.1f},{s["pos"][2]:.1f}{"g" if s["grounded"] else "a"}'
                       for s in samples[::2])
    if air is None or down is None:
        failures.append(f'jump {number}: never {"left" if air is None else "came back to"} the ground: {detail}')
        continue
    at = samples[down]['pos']
    along = (at[0] - tx) * heading[0] + (at[1] - ty) * heading[1]
    print(f'jump {number}: planned {planned:.1f} units from {tx:.0f},{ty:.0f},{tz:.1f} to {lx:.0f},{ly:.0f},{lz:.1f}; '
          f'left the ground at {samples[air]["t"]} ms (tangent {samples[air - 1]["tangent"] if air else 0:.1f}), '
          f'landed at {at[0]:.1f},{at[1]:.1f},{at[2]:.1f} after {samples[down]["t"] - samples[air]["t"]} ms, '
          f'{along:.1f} units on')
    if abs(at[2] - lz) > 3:
        failures.append(f'jump {number}: landed at height {at[2]:.1f}, the plan {lz:.1f}: {detail}')
    if along < .6 * planned:
        failures.append(f'jump {number}: landed {along:.1f} units on, the plan {planned:.1f}: {detail}')
    if samples[down]['flipped']:
        failures.append(f'jump {number}: landed flipped: {detail}')
    if lowest is not None and lowest < lz - 8:
        failures.append(f'jump {number}: fell to {lowest:.1f}, below the landing ({lz:.1f}): {detail}')

if failures:
    raise SystemExit('\n'.join(failures) + f'\nlog {log}')
print(f'The Warthog makes the {len(plans)} planned jumps of d40 section 8 as the planner flies them.')
