"""Walk The Silent Cartographer on retail BSP collision.

Drives the player along tests/data/b30-collision-qa-route.txt, proves weapon
and bot traces respect stacked floors, saves and reverts an authored
checkpoint, operates the security control and finishes the map cinematic.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

MARKERS = ('B30_QA upper', 'B30_QA lower', 'B30_QA authored-checkpoint', 'B30_QA reverted',
           'Halo control: device 129 group 0 -> 1.00', 'B30_QA PASS cinematic-finished',
           'weaponDown=1', 'weaponUp=1', 'weaponClear=1', 'botDown=1 botUp=1 botClear=1')

parser = argparse.ArgumentParser()
parser.add_argument('--route', type=Path, default=DATA / 'b30-collision-qa-route.txt')
args = parser.parse_args()
require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

profile = scratch('b30-native-qa-')
logfile = profile / 'run.log'
with logfile.open('w') as output:
    result = subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                             '-ehalo_scripttrace 1; maxfps 120; sleep 600 [qa_b30_start] 1'],
                            cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT,
                            env={**os.environ, 'HALOQA_B30_ROUTE': str(args.route.resolve())}, timeout=285)
log = logfile.read_text(errors='replace')
for line in log.splitlines():
    if line.startswith(('B30_QA', 'B30_TRACE_QA')) or 'Halo control:' in line or 'Halo scene: cutscene_map' in line:
        print(line)
missing = [marker for marker in MARKERS if marker not in log]
if result.returncode or missing:
    raise SystemExit(f'b30 route failed (exit {result.returncode}), missing {missing}; log {logfile}')
print('Silent Cartographer route passed on retail collision.')
