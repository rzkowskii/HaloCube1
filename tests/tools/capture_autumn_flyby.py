import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import os, subprocess, sys, tempfile
profile=Path(tempfile.mkdtemp(prefix='autumn-ac-',dir=SCRATCH))
binary=Path(sys.argv[1]) if len(sys.argv)>1 else CLIENT
script='halo_campaign; halo_scripttrace 1; maxfps 90; sleep 650 [gamespeed 500] 1; '
for n,tick in enumerate((130,193,266,340,440,540,640,740,840,940,1040),1):
 script += f'sleep {round(tick*1000/30+400)} [screenshot autumn-{n:02}] 1; '
script+='sleep 36500 [quit] 1'
options=[binary,'-t0','-w800','-h480','-e'+script]
if binary.name=='halo-ac':
 options[1:1]=['--halo',f'--home={profile}',f'--mod={root}']
print(profile,flush=True)
with (profile/'trace.log').open('w') as out:
 subprocess.run(options,cwd=RUNTIME,env={**os.environ,'HALO_AC_PROFILE':str(profile)},
                stdout=out,stderr=subprocess.STDOUT,timeout=45,check=True)
