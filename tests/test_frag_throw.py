"""The importer's planned frag throws at d40's s3 manifold, thrown in the game (no walk).

NOT YET RUN: written while testing was on hold.

tools/import_halo_campaign.py plans the s3 manifold of d40 section 5 (The Maw, the engine room) as a
ThrowTarget: no walkable spot sees either of its detectors, so the route stands on one spot (since the
route batch on floor 2's walkway below the channel, before it on the top floor) and throws frags into
the channel, one route row per aim (flag 2048, `target@health ax ay az`),
from a simulation of the engine's grenade physics (halo_collision.GrenadeArcs). This checks the
simulation against the game without walking the section's 5,000 rows:

1. halo_d40_05.route has throw rows: consecutive, at one spot, after the use row of
   enc5_1_pistons3, naming enc5_1_detector_s3 or s3b, with an aim point.
2. QA client on d40 section 5: enc5_1_manager runs, the player stands on the s3 control's use row
   and uses it (the piston opens), then stands on the throw row's spot and, for each aim in turn,
   aims from the eye at the aim point and throws two frags (qa_grenade_throw), each followed by the
   frag's position just before it goes off (`GRENADE throw...`) and both detectors' health. It
   passes when one of the two detectors is down to the row's health (0.1) after the throws, as
   enc5_1_destroyed_poll needs (the rows may name either detector: a blast reaches both); otherwise it prints where every frag lay against the detectors'
   centres, which is what to compare with the planner's simulation.
"""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

route = root / 'packages/maps/halo_d40_05.route'
require(route, 'run tools/import_halo_campaign.py "$maps" --mission d40 --part 5')
rows = [line.split() for line in route.read_text().splitlines() if line.strip()]
flags = [int(r[3]) if len(r) > 3 else 0 for r in rows]
uses = [i for i, f in enumerate(flags) if f & 256 and rows[i][4].startswith('enc5_1_pistons3@')]
throws = [i for i, f in enumerate(flags) if f & 2048]
assert uses, 'no use row for enc5_1_pistons3'
assert throws, 'no throw rows (2048): the importer found no landing throw (WARNING no simulated frag throw lands ...)'
assert throws == list(range(throws[0], throws[0] + len(throws))), throws
assert len({tuple(rows[i][:3]) for i in throws}) == 1, [rows[i][:3] for i in throws]
assert uses[0] < throws[0], (uses, throws)
assert all(len(rows[i]) == 8 and rows[i][4].split('@')[0] in ('enc5_1_detector_s3', 'enc5_1_detector_s3b') for i in throws), \
    [rows[i] for i in throws]
target_health = float(rows[throws[0]][4].split('@')[1])
print(f'halo_d40_05.route throws at s3 from one spot with {len(throws)} aims, after its control.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
use = rows[uses[0]]
spot = rows[throws[0]]
steps = [
    (100, 'qa_combat_protect 1; qa_hs_script enc5_1_manager 0'),
    (600, f'qa_halo_feet {use[0]} {use[1]} {use[2]}'),
    (1100, 'halo_use'),
    (4000, 'qa_hs_cmd device_get_position "obj:enc5_1_pistons3"; '
           'qa_target_state enc5_1_detector_s3 open; qa_target_state enc5_1_detector_s3b open'),
]
clock = 4500
for number, i in enumerate(throws):
    ax, ay, az = rows[i][5:8]
    for attempt in range(2):
        label = f'throw{number}_{attempt}'
        steps += [
            (clock, f'qa_halo_feet {spot[0]} {spot[1]} {spot[2]}; qa_grenade_give 0 1; qa_grenade_select 0'),
            (clock + 300, f'qa_target_aim {ax} {ay} {az}; qa_grenade_throw'),
            (clock + 1700, f'qa_grenade_state {label}'),
            (clock + 3100, f'qa_target_state enc5_1_detector_s3 {label}; qa_target_state enc5_1_detector_s3b {label}'),
        ]
        clock += 3600
steps.append((clock + 200, 'quit'))
body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in steps)
profile = scratch('frag-throw-')
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                    f'-ehalo_scripttrace 1; halo_difficulty 2; qa_campaign_play 10 5; qa_after_gameplay [{body}]'],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=120 + clock // 1000, check=True)
text = log.read_text(errors='replace')
lines = text.splitlines()
for entry in lines:
    if entry.startswith(('TARGET_QA', 'GRENADE throw', 'COMBAT_QA throw', 'HS_CMD device_get_position', 'Halo target:')):
        print('   ', entry)


def health(label):
    values = []
    for name in ('enc5_1_detector_s3', 'enc5_1_detector_s3b'):
        found = [e for e in lines if e.startswith(f'TARGET_QA {label} name={name} ')]
        assert found, (label, name, text[-3000:])
        values.append(float(found[0].split('health=')[1].split()[0]))
    return values


opened = health('open')
assert opened == [1.0, 1.0], ('the detectors were hurt before the first throw', opened)
final = health(f'throw{len(throws) - 1}_1')
print(f's3 detectors after {2 * len(throws)} throws: {final[0]:.2f} and {final[1]:.2f} (destroyed at {target_health:.2f})')
if min(final) > target_health:
    raise SystemExit(f'the planned throws did not destroy s3 (health {final}); compare the GRENADE throw positions '
                     f'above with the detectors\' centres (TARGET_QA ... centre=) and the importer\'s printed aims; log {log}')
print('The planned frag throws destroy the s3 manifold.')
