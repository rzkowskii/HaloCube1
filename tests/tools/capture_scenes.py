import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import os, subprocess, tempfile, sys
label=sys.argv[1] if len(sys.argv)>1 else 'a10'
script=sys.argv[2] if len(sys.argv)>2 else 'maxfps 80; sleep 7600 [screenshot autumn-space] 1; sleep 27000 [screenshot autumn-bridge] 1; sleep 37000 [quit] 1'
profile=Path(tempfile.mkdtemp(prefix='native-scenes-'+label+'-',dir=SCRATCH))
log=SCRATCH/f'native-scenes-{label}.log'
print(profile,flush=True)
with log.open('w') as out:
 result=subprocess.run([CLIENT,'--halo',f'--home={profile}',f'--mod={root}','-t0','-w960','-h600','-ehalo_campaign; '+script],
  cwd=RUNTIME,stdout=out,stderr=subprocess.STDOUT,timeout=int(os.getenv('HALO_QA_TIMEOUT','55')))
print('exit',result.returncode,flush=True)
sys.exit(result.returncode)
