import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(root/'tools'))
from import_halo_campaign import MISSIONS

profile=SCRATCH/'qa-campaign-native'
profile.mkdir(exist_ok=True)
failed=[]
for offset,mission in enumerate(MISSIONS,1):
    count=len(mission.sections)+(offset==2)
    commands=[]
    for section in range(1,count+1):
        base=(section-1)*650
        commands.extend((f'sleep {base+800} [qa_campaign_load {offset} {section}] 1',
                         f'sleep {base+1320} [qa_campaign_state] 1'))
        if section == 1:
            commands.append(f'sleep {base+1300} [screenshot {mission.cache}] 1')
    commands.append(f'sleep {count*650+950} [quit] 1')
    logfile=SCRATCH/f'campaign-native-{mission.cache}.log'
    with logfile.open('w') as log:
        p=subprocess.run([QA_BIN,'--halo',f'--home={profile}',f'--mod={root}','-t0','-w960','-h600',
                         '-e'+'; '.join(commands)],cwd=RUNTIME,stdout=log,stderr=subprocess.STDOUT,timeout=23)
    rows=[l for l in logfile.read_text().splitlines() if l.startswith('CAMPAIGN_QA')]
    print(mission.cache,p.returncode,len(rows),'/',count,flush=True)
    if p.returncode or len(rows)!=count or any('alive=1' not in row or f'map=halo_{mission.cache}' not in row for row in rows):
        print(rows,flush=True)
        failed.append(mission.cache)
assert not failed, failed
print('Every section loads with a live player.')
