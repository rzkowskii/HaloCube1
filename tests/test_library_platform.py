import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import subprocess,tempfile
profile=Path(tempfile.mkdtemp(prefix='library-platform-',dir=SCRATCH))
s=('halo_scripttrace 1; qa_campaign_resume 7 1; sleep 800 [halo_next] 1; '
   'sleep 1600 [qa_halo_feet 93 397 0] 1; sleep 2300 [qa_halo_feet 120 462 3.06] 1; '
   'sleep 2600 [qa_campaign_state] 1; sleep 6000 [qa_campaign_state] 1; '
   'sleep 10500 [qa_campaign_state] 1; sleep 17500 [qa_scenario_state; quit] 1')
with (SCRATCH/'library-platform.log').open('w') as log:
 subprocess.run([QA_BIN,'--halo',f'--home={profile}',f'--mod={root}','-t0','-w640','-h480','-e'+s],
  cwd=RUNTIME,stdout=log,stderr=subprocess.STDOUT,timeout=23,check=True)
import re
log = (SCRATCH/'library-platform.log').read_text(errors='replace')
heights = [int(z) for z in re.findall(r'CAMPAIGN_QA map=halo_c20_01 stage=\d alive=1 spawn=\d+,\d+,(-?\d+)', log)]
assert len(heights) == 3 and heights[0] < heights[1] < heights[2], (heights, log[-1500:])
print(f'Library platform carried the player from z={heights[0]} to z={heights[-1]}.')
