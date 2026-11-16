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
is checked: the pit room's two and a short hop further on (about x 688). The jump governors (the
jump governor batch): the importer writes `halo_vehiclegovern X Y Z RADIUS SPEED` into
halo_d40_08.cfg for each take-off of a drive planned below full speed (metrics
`vehicle_route.drive.governors`), and the engine holds the player's car at that speed there
(halo_vehiclegovernor, on by default), so a player at full throttle makes the drop. This checks the
engine against the plan without driving the whole section:

1. Static: halo_d40_08.route ends in its exit (metrics `ends_in_exit`), has at least two jump rows
   (flag 2 on a step longer than 1.5 cells), and its metrics record the drive's gravity, time and
   speed; vehicles.txt carries the Warthog's `physics` line. With a drive planned below 0.9 of top
   speed, the metrics list one governor per planned jump, at its take-off row and the drive's
   speed, and halo_d40_08.cfg has the same `halo_vehiclegovern` lines.
2. QA client on d40 section 8 (the escape seats the player at the wheel), the governors off
   (halo_vehiclegovernor 0) and the QA throttle capped at the planned speed
   (`qa_vehicle_cruise halo_d40_08 <speed>`): for each planned jump, the Warthog is put on the
   route about 30 units before the take-off row, heading at it, at the planned speed, and held at
   full throttle straight ahead (`qa_vehicle_jump`), its motion logged every 50 ms. This checks the
   engine's arc against the planner's.
3. The same with the governors on (halo_vehiclegovernor 1), no cruise cap and the Warthog put down
   at its top speed: a player at full throttle. `qa_vehicle_governors` must list the cfg's
   governors; each governed jump must make the same landing.

Each jump passes when the engine's gravity matches the metrics' (within 0.5), and the jump leaves
the ground at its take-off (the first airborne stretch that starts within NEAR units of the take-off
row, or past it: a crest hop during the 30-unit run-up is not the take-off, and is reported
separately), comes down on floor within 3 units of the landing row's height, at least 60% of the
planned jump's length on along the take-off's heading, upright, and never falls more than 8 units
below the landing row (into the pit). Otherwise it prints the motion lines to compare with the
planner's arc.
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
cfg = maps / 'halo_d40_08.cfg'
require(route, 'run tools/import_halo_campaign.py "$maps" --mission d40 --part 8')
require(metrics_path, 'run tools/import_halo_campaign.py "$maps" --mission d40 --part 8')
table = root / 'packages/halo/vehicles.txt'
require(table, 'run tools/halo_vehicles.py "$maps"')
NEAR = 4.0   # units (cells) from the take-off row within which the take-off may start

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

# The jump governors: one per planned take-off of a drive planned below 0.9 of top speed.
governors = drive.get('governors')
assert governors is not None, ('metrics without the drive\'s governors: re-run the import', drive)
slow = bool(drive.get('top')) and drive['speed'] < .9 * drive['top'] - .01
if slow:
    takeoffs = [rows[landing - 1][0] for landing in jumps]
    assert len(governors) == len(jumps), ('one governor per planned jump', governors, takeoffs)
    for (x, y, z, radius, speed), (tx, ty, tz) in zip(governors, takeoffs):
        assert (x, y) == (round(tx), round(ty)) and abs(z - tz) < .01 and abs(speed - drive['speed']) < .01 and radius >= 12, \
            ('a governor off its take-off row', (x, y, z, radius, speed), (tx, ty, tz))
    lines = [line.split()[1:] for line in cfg.read_text().splitlines() if line.startswith('halo_vehiclegovern ')] \
        if cfg.is_file() else []
    assert [[float(v) for v in words] for words in lines] == [[float(v) for v in g] for g in governors], \
        ('halo_d40_08.cfg does not hold the metrics\' governors: re-import d40 section 8', lines, governors)
    print(f'halo_d40_08.cfg: {len(governors)} jump governors at {drive["speed"]} units a second, radius {governors[0][3]}.')
else:
    assert not governors, ('governors on a drive planned at full speed', governors)

# 2 and 3. The jumps in the game.
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
SPEED, TOP, RUNUP, HOLD = float(drive['speed']), float(drive.get('top') or drive['speed']), 30, 4500
steps = [(100, f'qa_combat_protect 1; qa_vehicle_protect 1; qa_vehicle_state start; qa_vehicle_governors')]
clock = 2500
plans = []
passes = [('capped', f'halo_vehiclegovernor 0; qa_vehicle_cruise halo_d40_08 {SPEED}', SPEED)]
if slow:
    passes.append(('governed', 'halo_vehiclegovernor 1; qa_vehicle_cruise halo_d40_08 0', TOP))
for label, setup, start_speed in passes:
    for number, landing in enumerate(jumps):
        takeoff = landing - 1
        first = takeoff
        # Back along the route, but not over an earlier jump: the run-up starts on its landing at most.
        while first > 0 and first not in jumps and math.dist(rows[first][0][:2], rows[takeoff][0][:2]) < RUNUP:
            first -= 1
        (x0, y0, z0), _ = rows[first]
        (x1, y1, _), _ = rows[takeoff]
        yaw = math.degrees(math.atan2(x1 - x0, -(y1 - y0))) % 360
        key = f'{label}-{number}'
        plans.append((key, number, first, takeoff, landing, yaw))
        steps += [(clock, f'{setup}; echo JUMP_QA begin {key}; qa_vehicle_jump {x0} {y0} {z0} {yaw:.2f} {start_speed} {HOLD}'),
                  (clock + HOLD + 200, f'echo JUMP_QA end {key}')]
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

failures = []
if slow:
    loaded = re.search(r'VEHICLE_QA governors count=(\d+) enabled=(\d)', text)
    found = [tuple(map(float, m.groups())) for m in
             re.finditer(r'VEHICLE_QA governor at=(\S+?),(\S+?),(\S+) radius=(\S+) speed=(\S+)', text)]
    if not loaded or int(loaded.group(1)) != len(governors) or loaded.group(2) != '1':
        failures.append(f'the engine did not load halo_d40_08.cfg\'s {len(governors)} governors (on): '
                        f'{loaded.group(0) if loaded else "no VEHICLE_QA governors line"}')
    elif any(max(abs(a - b) for a, b in zip(f, g)) > .01 for f, g in zip(found, governors)):
        failures.append(f'the engine\'s governors {found} are not the cfg\'s {governors}')

# The motion lines between each jump's begin and end markers.
sections, current = {}, None
for line in text.splitlines():
    if line.startswith('JUMP_QA begin'):
        current = line.split()[2]
        sections[current] = []
    elif line.startswith('JUMP_QA end'):
        current = None
    elif current is not None and line.startswith('VEHICLE_QA jump t='):
        values = dict(re.findall(r'(\w+)=(\S+)', line))
        sections[current].append({'t': int(values['t']), 'pos': tuple(map(float, values['pos'].split(','))),
                                  'grounded': values['grounded'] == '1', 'flipped': values['flipped'] == '1',
                                  'gravity': float(values['gravity']), 'tangent': float(values['tangent'])})


def airborne_runs(samples):
    """(first airborne index, first grounded index after it or None) for each stretch in the air."""
    runs, i = [], 0
    while i < len(samples):
        if samples[i]['grounded']:
            i += 1
            continue
        j = i
        while j < len(samples) and not samples[j]['grounded']:
            j += 1
        runs.append((i, j if j < len(samples) else None))
        i = j
    return runs


for key, number, first, takeoff, landing, yaw in plans:
    samples = sections.get(key) or []
    if not samples:
        failures.append(f'jump {key}: no motion lines (qa_vehicle_jump did not run: seated?)')
        continue
    if abs(samples[0]['gravity'] - drive['gravity']) > .5:
        failures.append(f'jump {key}: the engine\'s gravity {samples[0]["gravity"]} is not the planner\'s {drive["gravity"]}')
    (tx, ty, tz), _ = rows[takeoff]
    (lx, ly, lz), _ = rows[landing]
    planned = math.dist((tx, ty), (lx, ly))
    heading = (math.sin(math.radians(yaw)), -math.cos(math.radians(yaw)))

    def along(s):
        return (s['pos'][0] - tx) * heading[0] + (s['pos'][1] - ty) * heading[1]
    detail = '; '.join(f'{s["t"]}:{s["pos"][0]:.1f},{s["pos"][1]:.1f},{s["pos"][2]:.1f}{"g" if s["grounded"] else "a"}'
                       for s in samples[::2])
    # The take-off: the first stretch in the air that starts within NEAR of the take-off row, or past
    # it (a late take-off); stretches before it are crest hops on the run-up, reported, not judged.
    runs = airborne_runs(samples)
    chosen = next((r for r in runs if math.dist(samples[r[0]]['pos'][:2], (tx, ty)) <= NEAR or along(samples[r[0]]) >= -NEAR), None)
    for start, down in runs:
        if chosen is not None and start >= chosen[0]:
            break
        print(f'jump {key}: a hop on the run-up at {samples[start]["t"]} ms, {along(samples[start]):.1f} units before the take-off '
              f'row ({samples[start]["pos"][0]:.1f},{samples[start]["pos"][1]:.1f},{samples[start]["pos"][2]:.1f}), '
              f'{"down after " + str(samples[down]["t"] - samples[start]["t"]) + " ms" if down is not None else "never down"}')
    if chosen is None or chosen[1] is None:
        failures.append(f'jump {key}: never {"left the ground at the take-off" if chosen is None else "came back to the ground"}: {detail}')
        continue
    air, down = chosen
    # Lowest between the take-off and the landing (after it the drive goes on down the corridor).
    lowest = min(s['pos'][2] for s in samples[air:down + 1])
    at = samples[down]['pos']
    travelled = (at[0] - tx) * heading[0] + (at[1] - ty) * heading[1]
    print(f'jump {key}: planned {planned:.1f} units from {tx:.0f},{ty:.0f},{tz:.1f} to {lx:.0f},{ly:.0f},{lz:.1f}; '
          f'left the ground at {samples[air]["t"]} ms, {along(samples[air]):.1f} units from the take-off row '
          f'(tangent {samples[air - 1]["tangent"] if air else 0:.1f}), '
          f'landed at {at[0]:.1f},{at[1]:.1f},{at[2]:.1f} after {samples[down]["t"] - samples[air]["t"]} ms, '
          f'{travelled:.1f} units on')
    if abs(at[2] - lz) > 3:
        failures.append(f'jump {key}: landed at height {at[2]:.1f}, the plan {lz:.1f}: {detail}')
    if travelled < .6 * planned:
        failures.append(f'jump {key}: landed {travelled:.1f} units on, the plan {planned:.1f}: {detail}')
    if samples[down]['flipped']:
        failures.append(f'jump {key}: landed flipped: {detail}')
    if lowest < lz - 8:
        failures.append(f'jump {key}: fell to {lowest:.1f}, below the landing ({lz:.1f}): {detail}')

if failures:
    raise SystemExit('\n'.join(failures) + f'\nlog {log}')
print(f'The Warthog makes the {len(jumps)} planned jumps of d40 section 8 as the planner flies them'
      f'{", capped and held by the jump governors from full throttle" if slow else ""}.')
