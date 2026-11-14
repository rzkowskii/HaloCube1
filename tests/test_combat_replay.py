import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
import os
import subprocess
from pathlib import Path

profile = SCRATCH/'qa-replay-profile'
profile.mkdir(exist_ok=True)
commands = {
    2100: 'qa_halo_status spawned; screenshot insertion',
    2300: 'qa_halo_move 1',
    5500: 'qa_halo_move 0; qa_halo_status movement',
    5700: 'qa_halo_position 214 379 20',
    5900: 'qa_halo_status first-wave; screenshot first-wave',
    5950: 'botsshoot 0',
    6000: 'qa_halo_fire 1',
    6700: 'qa_halo_fire 0; qa_halo_status shot; screenshot fired',
    6800: 'reload',
    7200: 'screenshot reloading',
    10400: 'qa_halo_status reloaded; qa_halo_clear',
    10800: 'qa_halo_status first-wave-clear; qa_halo_position 189 245 0',
    11100: 'qa_halo_status checkpoint; screenshot checkpoint',
    11200: 'qa_halo_damage 42; qa_halo_status shield-hit',
    14800: 'qa_halo_status before-recharge',
    17900: 'qa_halo_status recharging',
    21600: 'qa_halo_status recharged; qa_halo_damage 1000',
    21900: 'qa_halo_status dead; screenshot down',
    22900: 'qa_halo_revert; qa_halo_status reverted',
    23400: 'qa_halo_clear',
    23800: 'qa_halo_status second-wave-clear; qa_halo_position 184 165 0',
    24300: 'qa_halo_status extraction; screenshot complete',
    25100: 'quit',
}
command = 'halo_campaign; ' + '; '.join(f'sleep {delay} [{action}] 1' for delay,action in commands.items())
with (SCRATCH/'qa-replay.log').open('w') as log:
    result = subprocess.run([
        QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '--init', '-t0', '-w1280', '-h800',
        f'-e{command}',
    ], cwd=RUNTIME, stdout=log, stderr=subprocess.STDOUT, timeout=38)
log = (SCRATCH/'qa-replay.log').read_text(errors='replace')
assert result.returncode == 0, result.returncode
for marker in ('QA shot ', 'QA reverted ', 'QA second-wave-clear stage=4 live=0', 'QA extraction stage=5'):
    assert marker in log, (marker, log[-2000:])
assert 'QA reverted' in log and ' hp=75 shield=75.00' in log.split('QA reverted', 1)[1].splitlines()[0]
print('Combat, shield recharge, death, revert and extraction replayed.')
