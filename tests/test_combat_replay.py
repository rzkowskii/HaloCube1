import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
import os
import subprocess
from pathlib import Path

profile = scratch('qa-replay-')
commands = {
    100: 'qa_halo_status spawned; screenshot insertion',
    300: 'qa_halo_move 1',
    3500: 'qa_halo_move 0; qa_halo_status movement',
    3700: 'qa_halo_position 214 379 20',
    3900: 'qa_halo_status first-wave; screenshot first-wave',
    3950: 'botsshoot 0',
    4000: 'qa_halo_fire 1',
    4700: 'qa_halo_fire 0; qa_halo_status shot; screenshot fired',
    4800: 'reload',
    5200: 'screenshot reloading',
    8400: 'qa_halo_status reloaded; qa_halo_clear',
    8800: 'qa_halo_status first-wave-clear; qa_campaign_reach',
    10600: 'qa_halo_status checkpoint; screenshot checkpoint',
    10700: 'qa_halo_damage 42; qa_halo_status shield-hit',
    14300: 'qa_halo_status before-recharge',
    17400: 'qa_halo_status recharging',
    21100: 'qa_halo_status recharged; qa_halo_damage 1000',
    21400: 'qa_halo_status dead; screenshot down',
    22400: 'qa_halo_revert; qa_halo_status reverted',
    22900: 'qa_halo_clear',
    24800: 'qa_halo_status second-wave-clear; qa_campaign_reach',
    26300: 'qa_halo_status extraction; screenshot complete',
    27100: 'quit',
}
# The Crash Site is the standalone preview; halo_campaign resumes at a10 once it is installed.
# Delays count from live gameplay, after any opening cinematic is skipped.
command = 'halo_preview; qa_after_gameplay [' + '; '.join(f'sleep {delay} [{action}] 1' for delay,action in commands.items()) + ']'
with (SCRATCH/'qa-replay.log').open('w') as log:
    result = subprocess.run([
        QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '--init', '-t0', '-w1280', '-h800',
        f'-e{command}',
    ], cwd=RUNTIME, stdout=log, stderr=subprocess.STDOUT, timeout=90)
log = (SCRATCH/'qa-replay.log').read_text(errors='replace')
assert result.returncode == 0, result.returncode
for marker in ('QA shot ', 'QA reverted ', 'QA second-wave-clear stage=4 live=0', 'QA extraction stage=5'):
    assert marker in log, (marker, log[-2000:])
assert 'QA reverted' in log and ' hp=75 shield=75.00' in log.split('QA reverted', 1)[1].splitlines()[0]
print('Combat, shield recharge, death, revert and extraction replayed.')
