import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from qa_common import *  # noqa: E402,F403
import os
from pathlib import Path
import subprocess
import time

profile = SCRATCH/'qa-package-profile'
helper = qa_key()
app = APP
launcher = app/'Contents/MacOS/Halo in AssaultCube'
profile.mkdir(exist_ok=True)
command = ('halo_campaign; bind J attack; sleep 1700 [screenshot insertion] 1; sleep 3600 [screenshot shots] 1; '
           'sleep 5000 [screenshot switched] 1; sleep 6200 [screenshot frag] 1; sleep 9400 [screenshot restart] 1; '
           'sleep 10300 [quit] 1')
with (SCRATCH/'qa-package.log').open('w') as log:
    env = {**os.environ, 'HALO_AC_PROFILE': str(profile)}
    process = subprocess.Popen([launcher, '-t0', '-w1280', '-h800', f'-e{command}'], stdout=log, stderr=subprocess.STDOUT, env=env)
    def key(code, duration):
        subprocess.run([helper, str(process.pid), str(code), str(duration)], check=True, timeout=duration + 2)
    try:
        time.sleep(2.6)
        key(38, .65) # J: bound to the normal attack command
        time.sleep(.55)
        key(19, .12) # 2: M6D
        time.sleep(1.0)
        key(5, .12) # G: frag
        time.sleep(1.15)
        key(53, .12) # Escape: campaign pause menu
        time.sleep(.8)
        key(53, .12) # Escape: resume
        time.sleep(.7)
        key(96, .12) # F5: restart
        print('exit', process.wait(timeout=13))
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=4)
