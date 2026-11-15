"""Walk campaign sections on retail BSP collision.

    python3 tests/test_retail_collision.py                  mission a10 (the default)
    python3 tests/test_retail_collision.py a10 b30          several missions
    python3 tests/test_retail_collision.py --section a10:3  one section on its own

A mission is walked as one playthrough: the QA client starts its first section and, at each
transition, carries on into the next section with the mission script's state, the way a
player arrives there. If a section fails, the walk starts again from the next section in a
new client. A section started on its own begins the mission script afresh, so doors that
earlier sections' scripts open can stay shut; such results are marked.

For every section that has an imported packages/maps/<section>.hcol this checks that
  * 'loaded N retail collision surfaces' is in the log (retail collision is active),
  * the player spawns standing on retail floor (onfloor, floor within 5 cm below the feet),
  * every spawn, route marker and item entity has retail floor under it,
  * walking the importer's route (packages/maps/<section>.route) with ordinary movement
    input at full speed reaches the exit: the authored transition, or the section's
    completion,
  * movement feels the same as on stock AssaultCube geometry: on the first long straight of
    the route the QA client runs from a standstill on retail collision and again on the
    map's stock cube geometry. Sustained ground speed (about 16 u/s), the time to cover the
    first 4 units from a standstill and a standing jump's height must be within 10% of the
    stock run, and the route
    follower's speed on every straight it walks must be too.
A mission's walk stops at the first section without an .hcol (stock geometry, reported and
skipped, not failed). A missing .route or a route that stalls fails the section; the walk
logs go to build/qa/. Lift rides, light-bridge waits, the actions of control, shoot and door rows
(route flags 256, 512 and 1024: `RETAIL_QA act ...`) and the engine's fall and wedge recoveries
are printed under each section; PENDING_WALK lists the sections they are expected to make pass
that have not been walked yet.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import DRIVEN_SECTIONS, MISSIONS  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('missions', nargs='*', help='mission cache names, e.g. a10 b30 c10')
parser.add_argument('--section', action='append', default=[], metavar='MISSION:N', help='one-based section, e.g. a10:3')
parser.add_argument('--difficulty', type=int, default=2, choices=range(4),
                    help='halo_difficulty for the walk; Heroic (2) skips a10\'s scripted tutorial, which Normal gates doors behind')
parser.add_argument('--timeout', type=int, default=520, help='seconds per section')
args = parser.parse_args()
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

maps = root / 'packages/maps'
NOMINAL_SPEED = 16.0
# Sections whose walk is known to stop at a script-gated door (docs/macos-build.md). They are
# walked and reported every time, but only fail the test once they pass (so the list is pruned).
KNOWN_BLOCKED = {
    'halo_a10_05': 'tunnel_door_2 stayed shut on the last walk (mission_tunnel powers it after the tunnel conversations)',
    'halo_a10_06': 'reached only through section 5; on its own cryo_search never opens cryo_door_2',
    # The route rides lift_d down the c1 tower (flag 4). These reasons are what stopped the last walk,
    # made before the walker rode lifts (route flags 16 to 128); PENDING_WALK says what should pass now.
    'halo_b40_06': 'lift_d ride (route flag 4): last walked before the walker rode lifts',
    # c10 leaves these sections half way down a lift shaft (bsp 0,1 and bsp 1,3): the route ends
    # beside the lift's control, and the section ends only once the player rides it down.
    'halo_c10_01': 'lift_a ride to bsp 0,1: last walked before the walker rode lifts',
    'halo_c10_04': 'lift_b ride to bsp 1,3: last walked before the walker rode lifts',
    # c20 leaves its first three sections half way up a platform shaft (bsp 0,1, 1,2, 2,3): the
    # route ends beside the platform, which rises once the player stands on it.
    'halo_c20_01': 'platform1 ride to bsp 0,1: last walked before the walker boarded lifts',
    'halo_c20_02': 'platform2 ride to bsp 1,2: last walked before the walker boarded lifts',
    'halo_c20_03': 'platform3 ride to bsp 2,3: last walked before the walker boarded lifts',
    # d40's engine room: enc5_2_door opens only once the four manifolds are opened from their
    # controls and shot, and the section ends on the elevator, which enc5_3 raises to bsp 4,5.
    # The route uses all four controls (256 rows) and shoots the detectors of s1, n1 and n3 (512
    # rows); no walkable spot sees s3's detectors (docs/macos-build.md), so enc5_2_door stays shut.
    'halo_d40_05': "s3's detectors are out of sight of every walkable spot: enc5_2_door stays shut",
}
# Sections the lift and light-bridge machinery (route flags 16, 32, 64 and 128: the walker
# boards, calls and sends lifts, and waits for bridges; the engine carries every body on a lift
# deck and recovers falls) is expected to make pass, but which have not been walked in the game
# yet. They are reported as PENDING with the reason; a KNOWN_BLOCKED one that passes still fails
# the test until it is removed from KNOWN_BLOCKED (and this list), so the lists stay current.
PENDING_WALK = {
    'halo_b40_06': 'rides lift_d down from lift_d_control_a (deck row: send)',
    'halo_c10_01': 'rides lift_a down from lift_a_cont_a on its deck (deck row: send)',
    'halo_c10_02': 'new on retail collision: from the bottom of lift_a, drops into the lower hall beside bridge_a',
    'halo_c10_04': 'rides lift_b down from lift_b_cont_a on its deck (deck row: send)',
    'halo_c20_01': 'boards platform1 (0.38 units up: the engine lets a body step onto a lift deck) and rides up',
    'halo_c20_02': 'boards platform2 and rides up',
    'halo_c20_03': 'boards platform3 and rides up',
    # Door rows (1024, DOOR_WAITS): the walker stands at the door until it opens and logs its power
    # and position while it waits; a chained walk from section 1 carries cryo_search's open
    # cryo_door_2 into section 6.
    'halo_a10_05': 'waits at tunnel_door_2 on its door row until mission_tunnel powers it',
    'halo_a10_06': 'waits at cryo_door_2 on its door row (open when carried in from section 5)',
}


def section_name(mission, number):
    return f'halo_{mission.cache}_{number:02}'


# Runs: (mission, [section numbers walked in one client, in order]).
runs = []
for cache in args.missions or ([] if args.section else ['a10']):
    mission = next((m for m in MISSIONS if m.cache == cache), None)
    if mission is None:
        raise SystemExit(f'unknown mission {cache}; choose from {[m.cache for m in MISSIONS]}')
    runs.append((mission, list(range(1, len(mission.sections) + 1))))
for item in args.section:
    cache, number = item.split(':')
    mission = next(m for m in MISSIONS if m.cache == cache)
    runs.append((mission, [int(number)]))

failures, skipped, passed, known, driven = [], [], [], [], []


def check(name, log):
    """Problems with one section's part of a walk log, and a summary for a pass."""
    problems = []
    loaded = re.search(rf'loaded (\d+) retail collision surfaces for {name}\b', log)
    if not loaded:
        problems.append('retail collision not active (no "loaded N retail collision surfaces")')
    spawn = re.search(rf'RETAIL_QA spawn map={name} collision=(\d) feet=(\S+) floorgap=(\S+) onfloor=(\d) alive=(\d)', log)
    if not spawn:
        problems.append('player never reached live gameplay')
        return problems, ''
    _, _, gap, onfloor, alive = spawn.groups()
    if not (spawn.group(1) == '1' and onfloor == '1' and alive == '1' and 0 <= float(gap) <= .05):
        problems.append(f'spawn not standing on floor: {spawn.group(0)}')
    body = log[spawn.start():]
    end = re.search(rf'RETAIL_QA (PASS|FAIL) (\S+) map={name}\b.*', body)
    if end:
        body = body[:end.end()]
    audit = re.search(r'RETAIL_QA audit entities=(\d+) floorless=(\d+)', body)
    if audit and int(audit.group(2)):
        problems.append(f'{audit.group(2)} of {audit.group(1)} spawn/marker/item entities have no retail floor')
    feel = re.search(r'RETAIL_QA feel retail_speed=(\S+) stock_speed=(\S+) retail_accel=(\S+) stock_accel=(\S+) '
                     r'retail_apex=(\S+) stock_apex=(\S+) retail_air=\S+ stock_air=\S+ retail_ok=(\d) stock_ok=(\d)', body)
    note = ''
    if not feel:
        if end and end.group(1) == 'PASS':
            problems.append('movement feel not measured (no clear straight of 22+ steps on the route)')
    else:
        speed, stock_speed, accel, stock_accel, apex, stock_apex = map(float, feel.groups()[:6])
        retail_ok, stock_ok = feel.group(7) == '1', feel.group(8) == '1'
        # AssaultCube's ground speed is maxspeed 16 u/s. Where the stock cube layout has no
        # room for the straight (it is coarser than the retail floors), compare with that.
        reference = stock_speed if stock_ok else NOMINAL_SPEED
        note = f'speed {speed:.2f} vs {"stock" if stock_ok else "nominal"} {reference:.2f} u/s'
        if not retail_ok:
            problems.append('movement-feel straight stalled on retail collision')
        elif speed < .9 * reference:
            problems.append(f'sluggish: ground speed {speed:.2f} u/s, stock {reference:.2f}')
        if retail_ok and stock_ok:
            if accel < 0 or accel > 1.1 * stock_accel:
                problems.append(f'sluggish: first 4 units from a standstill in {accel:.0f} ms, stock {stock_accel:.0f} ms')
            if stock_apex > 0 and apex < .9 * stock_apex:
                problems.append(f'jump apex {apex:.2f}, stock {stock_apex:.2f}')
        # Straights where a door or another body was just ahead are reported but not judged.
        walked = [float(v) for v in re.findall(r'RETAIL_QA walk-speed waypoints=\S+ speed=(\S+) held=0', body)]
        slow = [v for v in walked if v < .9 * reference]
        if slow:
            problems.append(f'sluggish: {len(slow)} of {len(walked)} walked straights below 90% ({min(slow):.2f} u/s)')
        note += f'; walked straights {min(walked):.2f}..{max(walked):.2f}' if walked else ''
    if not end or end.group(1) != 'PASS':
        problems.append('exit not reached by walking: ' + (end.group(0) if end else 'no result'))
    crouched = re.search(r'RETAIL_QA crouched (\d+) times', body)
    summary = f'{end.group(2) if end else "-"}; {loaded.group(1) if loaded else "?"} surfaces; ' \
              f'{audit.group(0)[10:] if audit else ""}; {note}' + (f'; crouched {crouched.group(1)}x' if crouched else '')
    return problems, summary


def report_traversal(name, log):
    """Prints the lift rides, light-bridge waits and fall or wedge recoveries of a section's walk."""
    start = log.find(f'RETAIL_QA spawn map={name} ')
    if start < 0:
        return
    end = re.search(rf'RETAIL_QA (PASS|FAIL) \S+ map={name}\b.*', log[start:])
    body = log[start:start + end.end()] if end else log[start:]
    lines = [line for line in body.splitlines()
             if line.startswith(('RETAIL_QA ride', 'RETAIL_QA bridge', 'RETAIL_QA recover', 'RETAIL_QA act ', 'Halo recovery:',
                                 'Halo lift:', 'Halo target:'))]
    for line in lines[:12]:
        print('   ', line)
    if len(lines) > 12:
        print(f'    ... {len(lines) - 12} more ride, bridge and recovery lines')


def walk_run(mission, walk):
    """Walks sections `walk` of a mission in one client. Returns the sections not reached."""
    mission_index = MISSIONS.index(mission) + 1
    # qa_retail_start takes the engine's one-based section. a30's engine section 1 is the
    # standalone halo_a30 map, so its imported parts (halo_a30_01, _02) are sections 2 and 3.
    engine_section = walk[0] + (1 if mission.cache == 'a30' else 0)
    first = section_name(mission, walk[0])
    profile = scratch(f'retail-{first}-')
    logfile = SCRATCH / f'retail-collision-{first}{"-" + str(walk[-1]) if len(walk) > 1 else ""}.log'
    routes = [str(maps / f'{section_name(mission, n)}.route') for n in walk]
    # Long routes (d40's engine room walks every manifold's bay: 5,000 rows) get more time, as the
    # walker's own deadline does (90 ms a row past 3,000).
    extra = sum(max(0, len(Path(route).read_text().splitlines()) - 3000) * .09 for route in routes if Path(route).is_file())
    with logfile.open('w') as output:
        try:
            result = subprocess.run(
                [QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                 f'-ehalo_scripttrace 1; maxfps 120; halo_difficulty {args.difficulty}; sleep 600 [qa_retail_start {mission_index} {engine_section}] 1'],
                cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout * len(walk) + extra,
                env={**os.environ, 'HALOQA_RETAIL_ROUTE': routes[0], 'HALOQA_RETAIL_CHAIN': '\n'.join(routes[1:])})
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    log = logfile.read_text(errors='replace')
    for position, number in enumerate(walk):
        name = section_name(mission, number)
        problems, summary = check(name, log)
        if code != 0 and number == walk[-1]:
            problems.append(f'exit {code}')
        fresh = '' if position or walk[0] == 1 else ' (started on its own: the mission script began afresh)'
        exit_only = all(problem.startswith('exit not reached') for problem in problems)
        if problems and name in KNOWN_BLOCKED and exit_only:
            known.append(name)
            pending = f' (expected to pass, pending walk: {PENDING_WALK[name]})' if name in PENDING_WALK else ''
            print(f'{name}: KNOWN {KNOWN_BLOCKED[name]}{pending}: {problems[0]}{fresh}', flush=True)
            report_traversal(name, log)
            return walk[position + 1:]
        if problems:
            pending = ' (pending its first walk)' if name in PENDING_WALK else ''
            failures.append((name, '; '.join(problems) + fresh + pending))
            print(f'{name}: FAIL {"; ".join(problems)}{fresh}{pending}', flush=True)
            for line in log.splitlines():
                if line.startswith('RETAIL_QA') and (f'map={name}' in line or not re.search(r'map=\S+', line)) and \
                   any(w in line for w in ('PASS', 'FAIL', 'stuck', 'floorless', 'audit', 'feel', 'blocked', 'waiting',
                                           'crouching', 'squeezing', 'ride', 'bridge', 'recover', 'act ')):
                    print('   ', line)
            report_traversal(name, log)
            print(f'    log {logfile}', flush=True)
            return walk[position + 1:]
        passed.append(name)
        print(f'{name}: PASS {summary}{fresh}', flush=True)
        report_traversal(name, log)
        if name in KNOWN_BLOCKED:
            failures.append((name, 'now passes: remove it from KNOWN_BLOCKED' +
                             (' and PENDING_WALK' if name in PENDING_WALK else '') + ' and update docs/macos-build.md'))
    return []


for mission, numbers in runs:
    walk = []
    for number in numbers:
        name = section_name(mission, number)
        if not (maps / f'{name}.hcol').is_file():
            skipped.extend(section_name(mission, n) for n in numbers[numbers.index(number):])
            print(f'{name}: no .hcol, stock AssaultCube geometry (skipped with the rest of the run)', flush=True)
            break
        if (mission.cache, number - 1) in DRIVEN_SECTIONS:
            # The Warthog run has an .hcol for the vehicle but no walking route (tests/test_maw_drive.py).
            driven.extend(section_name(mission, n) for n in numbers[numbers.index(number):])
            print(f'{name}: driven in a Warthog, not walked (skipped with the rest of the run)', flush=True)
            break
        if not (maps / f'{name}.route').is_file():
            failures.append((name, 'no .route; re-run tools/import_halo_campaign.py'))
            print(f'{name}: FAIL no .route', flush=True)
            break
        walk.append(number)
    # After a failed section the walk carries on from the next section in a new client, so one
    # blocked section does not hide the rest.
    while walk:
        walk = walk_run(mission, walk)

print(f'\n{len(passed)} passed, {len(failures)} failed, {len(known)} known blocked, {len(skipped)} on stock geometry')
if known:
    print('known blocked:', ', '.join(f'{n} ({KNOWN_BLOCKED[n]})' for n in known))
pending = [n for n in known + [n for n, _ in failures] if n in PENDING_WALK]
if pending:
    print('expected to pass, pending walk:', ', '.join(f'{n} ({PENDING_WALK[n]})' for n in pending))
if skipped:
    print('stock geometry:', ', '.join(skipped))
if driven:
    print('driven, not walked:', ', '.join(driven))
if failures:
    raise SystemExit('retail collision failed: ' + '; '.join(f'{n} ({why})' for n, why in failures))
print('Retail collision route passed.')
