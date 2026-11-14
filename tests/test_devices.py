import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import re, subprocess, tempfile

exe = QA_BIN

def run(name, command, timeout=18):
    profile = Path(tempfile.mkdtemp(prefix=f'device-qa-{name}-', dir=SCRATCH))
    cmd = [exe, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
           '-ehalo_scripttrace 1; maxfps 90; ' + command]
    result = subprocess.run(cmd, cwd=RUNTIME, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    log = result.stdout.decode(errors='replace')
    (SCRATCH/f'device-qa-{name}.log').write_text(log)
    assert result.returncode == 0, (name, log[-1500:])
    print(name, profile, flush=True)
    return log

door = run('door', 'halo_mission 1; sleep 950 [halo_next] 1; '
    'sleep 1500 [qa_halo_block 140.60 118.08 .08; qa_halo_feet 140.6 112.7 .08] 1; '
    'sleep 2600 [qa_halo_block 140.60 118.08 .08; screenshot open; qa_halo_feet 143 108 .08] 1; '
    'sleep 5300 [qa_halo_block 140.60 118.08 .08; quit] 1')
print(re.findall(r'DEVICE_QA.*|Halo device: unnamed 13 .*', door))
assert re.findall(r'DEVICE_QA blocked=(\d)',door) == ['1','0','1'], door[-2100:]

switch = run('switch', 'halo_mission 1; sleep 950 [halo_next] 1; '
    'sleep 1500 [qa_halo_warp 225.95 188.04 8.0] 1; sleep 2700 [qa_scenario_state; quit] 1')
assert 'Halo trigger: bsp0,1 BSP 0 to 1' in switch and 'RETAIL_QA mission=0 section=1' in switch, switch[-2000:]

lift = run('lift', 'qa_campaign_resume 5 2; sleep 1000 [qa_halo_feet 607.78 106.17 -54.736; halo_use; qa_scenario_state] 1; '
    'sleep 1400 [qa_campaign_state; screenshot lift] 1; sleep 3500 [qa_campaign_state] 1; '
    'sleep 7400 [qa_campaign_state; quit] 1')
print(re.findall(r'Halo (?:control|lift|device):.*|CAMPAIGN_QA map=.*',lift))
assert 'Halo control: device 75 group 1 -> 1.00' in lift and 'Halo lift: carrying player on device 55' in lift, lift[-2200:]
assert 'Halo device: lift_b 55 reached 1' in lift, lift[-2200:]
