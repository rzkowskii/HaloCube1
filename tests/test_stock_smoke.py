"""The unmodified AssaultCube path still boots, renders a map and exits cleanly."""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(CLIENT, 'run scripts/build-macos.sh')
profile = scratch('stock-smoke-')
log = SCRATCH / 'stock-smoke.log'
with log.open('w') as out:
    result = subprocess.run([CLIENT, f'--home={profile}', '--init', '-t0', '-w960', '-h600', '--loadmap=ac_complex',
                             '-esleep 1000 [screenshot] 1; sleep 1800 [quit] 1'],
                            cwd=RUNTIME, stdout=out, stderr=subprocess.STDOUT, timeout=30)
assert result.returncode == 0, (result.returncode, log.read_text(errors='replace')[-1500:])
assert list((profile / 'screenshots').glob('*.jpg')), 'no screenshot written'
print('Stock AssaultCube booted ac_complex and exited cleanly.')
