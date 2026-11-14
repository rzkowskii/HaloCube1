import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import subprocess
import tempfile
profile=Path(tempfile.mkdtemp(prefix='campaign-full-', dir=SCRATCH))
profile.mkdir(exist_ok=True)
with (SCRATCH/'campaign-retail-replay.log').open('w') as log:
    result=subprocess.run([QA_BIN,'--halo',f'--home={profile}',f'--mod={root}',
          '-t0','-w640','-h480','-ehalo_campaign; halo_scripttrace 1; maxfps 160; sleep 800 [qa_campaign_full] 1'],
          cwd=RUNTIME,stdout=log,stderr=subprocess.STDOUT,timeout=180)
assert result.returncode==0
data=(SCRATCH/'campaign-retail-replay.log').read_text()
count=data.count('CAMPAIGN_QA completed')
assert count==55,(count,data[-2500:])
print(f'{count} sections completed using scripted positioning and combat outcomes.')
