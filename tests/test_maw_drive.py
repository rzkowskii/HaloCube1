import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
from pathlib import Path
import subprocess
import tempfile
import time
profile=Path(tempfile.mkdtemp(prefix='maw-native-',dir=SCRATCH))
app=APP_EXE
helper=qa_key()
startup='halo_section 10 6; sleep 1800 [screenshot maw-start] 1; sleep 5300 [screenshot maw-driving] 1; sleep 6100 [quit] 1'
with (SCRATCH/'maw-native.log').open('w') as log:
    process=subprocess.Popen([app,f'--home={profile}','-t0','-w1280','-h800',f'-e{startup}'],stdout=log,stderr=subprocess.STDOUT)
    try:
        time.sleep(2.2)
        subprocess.run([helper,str(process.pid),'13','1.75'],check=True,timeout=4)
        subprocess.run([helper,str(process.pid),'2','0.25'],check=True,timeout=3)
        assert process.wait(timeout=14)==0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
print(profile)
