import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import os, re, subprocess, tempfile
exe=APP_EXE
intros=['x10','cutscene_intro','cutscene_insertion','cutscene_insertion','cutscene_insertion_a','insertion',
        'cinematic_intro','cutscene_insertion','cinematic_intro','cinematic_intro']
with (SCRATCH/'signed-mission-smoke.log').open('w') as report:
    for n, intro in enumerate(intros, 1):
        profile=Path(tempfile.mkdtemp(prefix=f'cinematic-qa-{n:02}-',dir=SCRATCH))
        command=(f'halo_mission {n}; halo_scripttrace 1; maxfps 100; '
                 'sleep 1500 [screenshot intro] 1; sleep 2700 [halo_next] 1; '
                 'sleep 3500 [screenshot play; quit] 1')
        result=subprocess.run([exe,'-t0','-w640','-h480','-e'+command],
            env={**os.environ,'HALO_AC_PROFILE':str(profile)},capture_output=True,timeout=12)
        text=result.stdout.decode(errors='replace')
        report.write(text)
        report.flush()
        assert result.returncode==0, (n,result.returncode,text[-1000:])
        assert f'Halo scene: {intro} start' in text and f'Halo scene: {intro} skipped' in text, (n,text[-2000:])
        assert 'Loaded Halo BSP geometry' in text, (n,text[-1800:])
        assert (profile/'screenshots/play.jpg').is_file(), n
        print(n,intro,'start and skip',flush=True)
print('Signed app: ten mission openings.',flush=True)
