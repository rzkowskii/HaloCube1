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
# On retail collision a10_01's completion can walk straight into its authored bsp0,1
# transition before qa_campaign_state runs: a completion logged on a10_01 counts too.
def completed_first():
    if any('map=halo_a10_01 stage=5' in row for row in rows): return True
    loaded=[i for i,row in enumerate(rows) if row.startswith('read map packages/maps/halo_a10_0')]
    first=next((i for i in loaded if 'halo_a10_01' in rows[i]),None)
    second=next((i for i in loaded if 'halo_a10_02' in rows[i]),None)
    return first is not None and second is not None and any(row=='Halo: mission complete' for row in rows[first:second])
assert completed_first(),rows[-30:]
assert any('map=halo_a10_02 stage=' in row for row in rows),rows[-30:]
# Progress file version 2: the furthest section reached and the game Continue resumes.
progress=read_progress(profile)
assert progress and progress['furthest']==['0','1'],progress
assert progress['game'][:2]==['0','1'],progress
print('Scripted sector completion advanced to the next map and saved progression.')
