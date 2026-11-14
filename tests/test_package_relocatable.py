"""Release hygiene: the packaged app is relocatable, asset checks are strict, retail files stay out of git.

Needs the packaged app (scripts/package-macos.sh). No retail data is read.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

check = root / 'scripts/check-no-retail.sh'
package = root / 'scripts/package-macos.sh'


def run(*cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


# 1. check-no-retail passes on the real tree.
result = run(check)
assert result.returncode == 0, result.stderr
print('check-no-retail: clean tree passes')

# 2. ...and fails when a retail file is staged, or sits untracked and un-ignored.
repo = scratch('retail-repo-')
git = lambda *a: subprocess.run(['git', '-C', str(repo), *a], check=True, capture_output=True)
git('init', '-q')
(repo / 'README.md').write_text('x')
git('add', '.')
assert run(check, repo).returncode == 0
(repo / 'packages/halo').mkdir(parents=True)
(repo / 'packages/halo/a10.hscb').write_bytes(b'HSCB')
bad = run(check, repo)
assert bad.returncode != 0 and 'a10.hscb' in bad.stderr, 'untracked retail file not caught'
git('add', '-f', 'packages/halo/a10.hscb')
bad = run(check, repo)
assert bad.returncode != 0 and 'a10.hscb' in bad.stderr, 'staged .hscb not caught'
git('rm', '-q', '--cached', 'packages/halo/a10.hscb')
(repo / 'packages/halo/a10.hscb').unlink()
(repo / 'tools_out.hcol').write_bytes(b'x')
assert run(check, repo).returncode != 0, 'stray .hcol outside packages not caught'
shutil.rmtree(repo)
print('check-no-retail: staged and stray retail files fail')

# 3. .gitignore denies by default under packages/.
for name in ('packages/halo/a10.hscb', 'packages/new-kind/file.bin', 'packages/maps/halo_zz_01.cgz'):
    assert run('git', '-C', root, 'check-ignore', '-q', name).returncode == 0, name
assert run('git', '-C', root, 'check-ignore', '-q', 'packages/maps/halo_a30.cfg').returncode != 0
print('.gitignore: packages/ is deny-by-default')

# 4. The package script refuses an incomplete asset import, with and without --partial.
stub = scratch('stub-packages-')
env = {**os.environ, 'HALO_PACKAGES_DIR': str(stub)}
for args in ([], ['--partial']):
    result = run(package, *args, env=env)
    assert result.returncode != 0 and 'Incomplete asset import' in result.stderr, (args, result.stderr[-500:])
(stub / 'maps').mkdir()
(stub / 'maps/halo_a30.cgz').write_bytes(b'x')   # a lone Crash Site file is still incomplete
assert run(package, env=env).returncode != 0
shutil.rmtree(stub)
print('package-macos.sh: stub packages dir rejected')

# 5. The packaged app has no Homebrew paths and runs from another location.
require(APP_EXE, 'run scripts/package-macos.sh')
contents = APP / 'Contents'
libs = [contents / 'MacOS/halo-ac', *sorted((contents / 'Frameworks').glob('*.dylib'))]
assert len(libs) > 3, 'no bundled libraries'
for lib in libs:
    out = run('otool', '-L', lib).stdout
    assert not any(p in out for p in ('/opt/homebrew', '/usr/local', '/Cellar/')), (lib.name, out)
assert run('codesign', '--verify', '--deep', '--strict', APP).returncode == 0
for notice in ('NOTICE.md', 'README_CUBEENGINE.txt', 'LICENSE'):
    assert (contents / 'Resources/licenses' / notice).is_file(), notice
assert (root / 'NOTICE.md').is_file()

moved = scratch('relocated-') / 'Halo in AssaultCube.app'
shutil.copytree(APP, moved, symlinks=True)
profile = scratch('relocated-profile-')
# Launch the binary directly: the bash wrapper is SIP-protected and would drop DYLD_PRINT_LIBRARIES.
res = moved / 'Contents/Resources'
result = subprocess.run(
    [str(moved / 'Contents/MacOS/halo-ac'), f'--home={profile}', '--init', '--halo', f'--mod={res}/mod',
     '-ehalo_campaign', '-t0', '-w320', '-h240', '-ehalo_mission 1; maxfps 100; sleep 3000 [quit] 1'],
    cwd=res / 'runtime', capture_output=True, text=True, timeout=90,
    env={**os.environ, 'DYLD_PRINT_LIBRARIES': '1'})
assert result.returncode == 0, (result.returncode, result.stdout[-800:], result.stderr[-800:])
loaded = [l for l in result.stderr.splitlines() if '.dylib' in l]
assert any('/Frameworks/libSDL2' in l for l in loaded), 'SDL2 not loaded from the bundle'
assert any('/Frameworks/libSDL3' in l for l in loaded), 'SDL3 (dlopened by sdl2-compat) not loaded from the bundle'
assert not [l for l in loaded if '/opt/homebrew' in l or '/usr/local/' in l], 'Homebrew library loaded'
shutil.rmtree(moved.parent)
print('Packaged app: relocatable, signed, licensed.')
