import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
import subprocess
from pathlib import Path
# A fresh profile: halo_campaign resumes from any saved progress.
profile=scratch('campaign-progression-')
with (SCRATCH/'campaign-progression.log').open('w') as log:
    # Delays count from live gameplay, after the x10 opening cinematic is skipped.
    actions=[f'sleep {200+i*125} [qa_campaign_advance] 1' for i in range(65)]
    actions.extend(['sleep 8600 [qa_campaign_state; halo_next] 1','sleep 9500 [qa_campaign_state; screenshot next-sector] 1','sleep 10200 [quit] 1'])
    p=subprocess.run([QA_BIN,'--halo',f'--home={profile}',f'--mod={root}','-t0','-w960','-h600',
                    '-ehalo_campaign; qa_after_gameplay ['+'; '.join(actions)+']'],cwd=RUNTIME,stdout=log,stderr=subprocess.STDOUT,timeout=90)
    assert p.returncode==0
rows=(SCRATCH/'campaign-progression.log').read_text().splitlines()
assert any('map=halo_a10_01 stage=5' in row for row in rows),rows[-30:]
assert any('map=halo_a10_02 stage=' in row for row in rows),rows[-30:]
assert (profile/'config/halo-progress.txt').read_text().strip()=='1 0 1'
print('Scripted sector completion advanced to the next map and saved progression.')
