import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import os, subprocess, tempfile
app=APP
profile=Path(tempfile.mkdtemp(prefix='opening-updated-', dir=SCRATCH))
log=SCRATCH/'signed-opening-updated.log'
script=('halo_campaign; halo_scripttrace 1; maxfps 75; '
        'sleep 1200 [bind RETURN []; bind F5 []; bind ESCAPE []] 1; '
        'sleep 8500 [screenshot ring] 1; sleep 26000 [screenshot autumn] 1; '
        'sleep 48200 [screenshot keyes-deck] 1; sleep 71200 [screenshot bridge-crew] 1; '
        'sleep 84700 [screenshot keyes-display] 1; sleep 137800 [screenshot hangar] 1; '
        'sleep 201000 [screenshot cryo] 1; sleep 235000 [halo_script_status; screenshot handoff; quit] 1')
print(profile,flush=True)
with log.open('w') as out:
    result=subprocess.run([app/'Contents/MacOS/Halo in AssaultCube','-t0','-w960','-h600','-e'+script],
                          env={**os.environ,'HALO_AC_PROFILE':str(profile)},stdout=out,stderr=subprocess.STDOUT,timeout=255)
text=log.read_text(errors='replace')
assert result.returncode==0, result.returncode
for expected in ('a10 BSP 7','a10 BSP 1','a10 BSP 8','x10 finished','audio playing','fly_straight started','keyes_walk_1 finished','x10_pelican_1_out finished'):
    assert expected in text, (expected,text[-3000:])
assert (profile/'screenshots/handoff.jpg').is_file()
assert 'x10 skipped' not in text
assert 'Halo: Halo, section 1' not in text
print('Signed app completed the retail opening at normal speed.',flush=True)
