"""Missing and damaged generated assets: the game logs and falls back, it never crashes.

    python3 tests/test_asset_robustness.py                 every case (39 short runs)
    python3 tests/test_asset_robustness.py hud hcol        cases whose names contain these words
    python3 tests/test_asset_robustness.py --asan          with the ASan/UBSan client (build/macos-asan)
    python3 tests/test_asset_robustness.py --list

Not yet run: written while testing was on hold (docs/macos-build.md, "Not yet verified in the
game"). Needs the imported assets (packages/) and the QA client.

Each case builds a mod directory under build/qa/ that mirrors packages/ through symlinks, with one
file removed or replaced by a damaged copy (truncated, random bytes, a bad header or version, an
out-of-range or non-finite number), so the real packages/ is never touched. The QA client loads
a30 section 2 (halo_a30_01, retail collision, a mission script, Covenant and marines) from it and,
once the section is live, `qa_robust_exercise` (tests/native/soak_qa.inc) fires, zooms and swaps the
weapons, throws both grenade types, spawns, drives and leaves a Warthog, dies and reverts, while
the HUD draws every frame. A case passes when

  * the client exits cleanly (no signal, exit code 0) after printing `ROBUST_QA end`,
  * the log has the case's fallback line (for example `hud.txt is missing`, `retail collision
    unavailable for halo_a30_01`, `no retail scenario program for a30`, `campaign progress was
    corrupt`) and its ROBUST_QA row shows the fallback (hud=0, weapons=0, vehicledefs=1, ...),
  * the player is alive at the end and no sanitizer report is in the log (with --asan).

Route files are read only by the QA walker: a damaged route must give `RETAIL_QA FAIL
invalid-route-file` and a clean exit. Progress-file cases put the damaged file in the profile and
press Continue on the title menu. Bit-flip cases (`*-flip`) use fixed seeds and only require a
clean run: the loader may accept the file or refuse it.
"""
import argparse
import os
import random
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

parser = argparse.ArgumentParser()
parser.add_argument('filters', nargs='*')
parser.add_argument('--asan', action='store_true', help='use build/macos-asan/halo-ac-qa (scripts/build-macos.sh --asan)')
parser.add_argument('--list', action='store_true')
parser.add_argument('--keep', action='store_true', help='keep the case directories under build/qa/')
args = parser.parse_args()

PACKAGES = root / 'packages'
SECTION = 'halo_a30_01'          # a30 section 2: qa_campaign_load 2 2
client = BUILD / 'macos-asan/halo-ac-qa' if args.asan else QA_BIN


for needed in ('halo/hud.txt', 'halo/weapons.txt', 'halo/equipment.txt', 'halo/vehicles.txt', 'halo/actors.txt',
               'halo/a30.hscb', f'maps/{SECTION}.hcol', f'maps/{SECTION}.mesh'):
    require(PACKAGES / needed, 'import the assets first (docs/macos-build.md; a30 with retail collision)')


def real(rel):
    return (PACKAGES / rel).read_bytes()


def truncated(rel, fraction):
    data = real(rel)
    return data[:int(len(data) * fraction)]


def garbage(size, seed):
    return random.Random(seed).randbytes(size)


def flipped(rel, seed, count=64, skip=16):
    """The file with `count` random bytes flipped past its first `skip` bytes (the header)."""
    data = bytearray(real(rel))
    rng = random.Random(seed)
    for _ in range(count):
        i = rng.randrange(skip, len(data))
        data[i] ^= 1 << rng.randrange(8)
    return bytes(data)


def edited(rel, old, new, count=1):
    text = real(rel).decode()
    assert old in text, (rel, old)
    return text.replace(old, new, count).encode()


def first_line(rel, prefix):
    return next(line for line in real(rel).decode().splitlines() if line.startswith(prefix))


def hscb_version(version):
    data = bytearray(real('halo/a30.hscb'))
    data[4:8] = version.to_bytes(4, 'little')
    return bytes(data)


def progress_lines(lines, version=2, bad_check=False):
    """A progress file's bytes; with bad_check its check line does not match."""
    text = f'halo-progress {version}\n' + ''.join(line + '\n' for line in lines)
    check = progress_hash(text) ^ (1 if bad_check else 0)
    return (text + f'check {check:08x}\n').encode()


GAME_LINES = ['furthest 1 2', 'beaten 0', 'game 1 2 1 0 0', 'vitals 100 75.000', 'grenades 2 0 0', 'current 0',
              'stock 6 60 180 12 48']

# name: (removed files, {file: bytes}, expected log text (regex), {ROBUST_QA field: value at the end}, kind)
# kind: 'section' (load a30 section 2 and exercise it), 'flip' (the same, but the damaged file may be
# accepted, so only a clean exit counts), 'route' (the QA walker with a damaged route), 'progress'
# (the damaged progress file in the profile, then Continue).
CASES = {
    'baseline': ((), {}, r'Halo HUD: \d+ sprite sheets', {'hud': '1', 'collision': '1', 'script': '1'}, 'section'),
    'hud-missing': (('halo/hud.txt',), {}, r'hud\.txt is missing', {'hud': '0'}, 'section'),
    'hud-garbage': ((), {'halo/hud.txt': garbage(8192, 1)}, r'hud\.txt is unusable', {'hud': '0'}, 'section'),
    'hud-bitmap-negative': ((), {'halo/hud.txt': edited('halo/hud.txt', 'format 1\n', 'format 1\nbitmap -7\n')},
                            r'invalid line in packages/halo/hud\.txt: bitmap -7', {'hud': '0'}, 'section'),
    'hud-bitmap-huge': ((), {'halo/hud.txt': edited('halo/hud.txt', 'format 1\n', 'format 1\nbitmap 2147483647\n')},
                        r'invalid line in packages/halo/hud\.txt: bitmap 2147483647', {'hud': '0'}, 'section'),
    'hud-truncated': ((), {'halo/hud.txt': truncated('halo/hud.txt', .4)}, r'Halo HUD', {}, 'section'),
    'weapons-missing': (('halo/weapons.txt',), {}, r'weapons\.txt is missing', {'weapons': '0'}, 'section'),
    'weapons-old-format': ((), {'halo/weapons.txt': edited('halo/weapons.txt', 'format 3\n', 'format 2\n')},
                           r'weapons\.txt is format 2, the game reads format 3', {'weapons': '0'}, 'section'),
    'weapons-truncated': ((), {'halo/weapons.txt': truncated('halo/weapons.txt', .55)},
                          r'weapons\.txt (ends inside|is unusable)|invalid line \d+ in packages/halo/weapons\.txt',
                          {'weapons': '0'}, 'section'),
    'weapons-nan-speed': ((), {'halo/weapons.txt': edited('halo/weapons.txt', first_line('halo/weapons.txt', 'projectile '),
                                                          'projectile nan ' + first_line('halo/weapons.txt', 'projectile ').split(' ', 2)[2])},
                          r'invalid line \d+ in packages/halo/weapons\.txt', {'weapons': '0'}, 'section'),
    'weapons-garbage': ((), {'halo/weapons.txt': garbage(6000, 2)}, r'weapons\.txt is unusable', {'weapons': '0'}, 'section'),
    'equipment-missing': (('halo/equipment.txt',), {}, r'equipment\.txt is missing', {'grenades': '00'}, 'section'),
    'equipment-truncated': ((), {'halo/equipment.txt': truncated('halo/equipment.txt', .5)},
                            r'equipment\.txt (ends inside|is unusable)|invalid line \d+ in packages/halo/equipment\.txt',
                            {'grenades': '00'}, 'section'),
    'equipment-garbage': ((), {'halo/equipment.txt': garbage(3000, 3)}, r'equipment\.txt is unusable', {'grenades': '00'}, 'section'),
    'vehicles-missing': (('halo/vehicles.txt',), {}, r'vehicles\.txt is missing.*built-in Warthog', {'vehicledefs': '1'}, 'section'),
    'vehicles-inside-out': ((), {'halo/vehicles.txt': edited('halo/vehicles.txt', first_line('halo/vehicles.txt', 'bounds '),
                                                             'bounds 1 1 1 -1 -1 -1')},
                            r'vehicles\.txt is unusable', {'vehicledefs': '1'}, 'section'),
    'vehicles-truncated': ((), {'halo/vehicles.txt': truncated('halo/vehicles.txt', .5)},
                           r'vehicles\.txt (is unusable|ends inside)', {'vehicledefs': '1'}, 'section'),
    'actors-missing': (('halo/actors.txt',), {}, r'actors\.txt is missing', {'actors': '0'}, 'section'),
    'actors-truncated': ((), {'halo/actors.txt': truncated('halo/actors.txt', .5)},
                         r'actors\.txt (ends inside|is unusable)|invalid line in packages/halo/actors\.txt', {'actors': '0'}, 'section'),
    'hcol-missing': ((f'maps/{SECTION}.hcol',), {}, r'Halo: a30|QA_GAMEPLAY', {'collision': '0'}, 'section'),
    'hcol-truncated': ((), {f'maps/{SECTION}.hcol': truncated(f'maps/{SECTION}.hcol', .5)},
                       rf'retail collision unavailable for {SECTION}', {'collision': '0'}, 'section'),
    'hcol-garbage': ((), {f'maps/{SECTION}.hcol': b'HCL1' + garbage(200000, 4)},
                     rf'retail collision unavailable for {SECTION}', {'collision': '0'}, 'section'),
    'hcol-flip': ((), {f'maps/{SECTION}.hcol': flipped(f'maps/{SECTION}.hcol', 5)}, r'QA_GAMEPLAY', {}, 'flip'),
    'mesh-missing': ((f'maps/{SECTION}.mesh',), {}, rf'{SECTION}\.mesh is missing or unreadable', {}, 'section'),
    'hscb-missing': (('halo/a30.hscb',), {}, r'no retail scenario program for a30', {'script': '0'}, 'section'),
    'hscb-truncated': ((), {'halo/a30.hscb': truncated('halo/a30.hscb', .5)}, r'no retail scenario program for a30',
                       {'script': '0'}, 'section'),
    'hscb-version': ((), {'halo/a30.hscb': hscb_version(99)}, r'no retail scenario program for a30', {'script': '0'}, 'section'),
    'hscb-garbage': ((), {'halo/a30.hscb': b'HSCB' + (8).to_bytes(4, 'little') + garbage(100000, 6)},
                     r'no retail scenario program for a30', {'script': '0'}, 'section'),
    'hscb-flip': ((), {'halo/a30.hscb': flipped('halo/a30.hscb', 7)}, r'QA_GAMEPLAY', {}, 'flip'),
    'route-garbage': ((), {f'maps/{SECTION}.route': garbage(2000, 8)}, r'RETAIL_QA FAIL invalid-route-file', None, 'route'),
    'route-nan': ((), {f'maps/{SECTION}.route': b'1 2 3\nnan 4 5\n6 7 8\n'}, r'RETAIL_QA FAIL invalid-route-file', None, 'route'),
    'route-missing': ((f'maps/{SECTION}.route',), {}, r'RETAIL_QA FAIL invalid-route-file', None, 'route'),
    'progress-garbage': ((), {'progress': garbage(5000, 9)}, r'campaign progress was corrupt', {}, 'progress'),
    'progress-bad-check': ((), {'progress': progress_lines(GAME_LINES, bad_check=True)}, r'campaign progress was corrupt', {}, 'progress'),
    'progress-nan-checkpoint': ((), {'progress': progress_lines(GAME_LINES[:2] + ['game 1 2 1 1 0', 'checkpoint nan 1 2 0 1 0']
                                                                + GAME_LINES[3:])},
                                r'campaign progress was corrupt', {}, 'progress'),
    'progress-out-of-range': ((), {'progress': progress_lines(['furthest 42 7', 'beaten 0', 'game 42 7 9 0 0'])},
                              r'campaign progress was corrupt', {}, 'progress'),
    'progress-nul': ((), {'progress': progress_lines(GAME_LINES).replace(b'beaten', b'bea\0ten')},
                     r'campaign progress was corrupt', {}, 'progress'),
    'progress-huge': ((), {'progress': b'halo-progress 3\n' + b'#' * (33 << 20)}, r'campaign progress was corrupt', {}, 'progress'),
    'progress-state-garbage': ((), {'progress': progress_lines(GAME_LINES[:2] + ['game 1 2 1 0 0'] + GAME_LINES[3:] +
                                                               state_block([1, 1, 2, 0, '00000000', '00000000'],
                                                                           ['s thread =nothing 99999 x', 'v vehicle =x', 'w ground 1 2',
                                                                            'p vitality nan 1 2 0']), version=3)},
                               r'could not be restored', {}, 'progress'),
}

if args.list:
    print('\n'.join(CASES))
    raise SystemExit(0)
cases = [name for name in CASES if not args.filters or any(f in name for f in args.filters)]
if not cases:
    raise SystemExit('no cases match')
require(client, 'run scripts/build-macos.sh --asan' if args.asan else 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

env = dict(os.environ)
env.setdefault('ASAN_OPTIONS', 'halt_on_error=1:abort_on_error=0:detect_leaks=0:detect_container_overflow=0:print_summary=1')
env.setdefault('UBSAN_OPTIONS', f'print_stacktrace=1:halt_on_error=0:suppressions={root / "tests/data/ubsan.supp"}')
SANITIZER = ('ERROR: AddressSanitizer', 'runtime error:', 'SUMMARY: UndefinedBehaviorSanitizer', 'SUMMARY: AddressSanitizer')
ROBUST = re.compile(r'ROBUST_QA end (.*)')


def mirror(name, removed, replaced):
    """A mod directory: config and packages/ as symlinks, packages/halo and packages/maps file by file."""
    mod = scratch(f'robust-{name}-')
    (mod / 'config').symlink_to(root / 'config', target_is_directory=True)
    packages = mod / 'packages'
    packages.mkdir()
    for entry in PACKAGES.iterdir():
        if entry.name in ('halo', 'maps'):
            (packages / entry.name).mkdir()
            for item in entry.iterdir():
                (packages / entry.name / item.name).symlink_to(item)
        else:
            (packages / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())
    for rel in removed:
        (packages / rel).unlink()
    for rel, data in replaced.items():
        if rel == 'progress':
            continue
        target = packages / rel
        target.unlink(missing_ok=True)
        target.write_bytes(data)
    return mod


failures = []
for name in cases:
    removed, replaced, expect, fields, kind = CASES[name]
    mod = mirror(name, removed, replaced)
    profile = mod / 'profile'
    profile.mkdir()
    run_env = dict(env)
    if kind == 'progress':
        (profile / 'config').mkdir()
        (profile / 'config/halo-progress.txt').write_bytes(replaced['progress'])
        commands = ('maxfps 100; halo_title; sleep 800 [qa_title_state; qa_progress_state] 1; sleep 1200 [halo_continue] 1; '
                    'sleep 1300 [qa_after_gameplay [qa_robust_state start; sleep 3000 [qa_robust_state end] 1; '
                    'sleep 3400 [quit] 1]] 1; sleep 40000 [qa_robust_state end; quit] 1')
    elif kind == 'route':
        run_env['HALOQA_RETAIL_ROUTE'] = str(mod / f'packages/maps/{SECTION}.route')
        commands = 'maxfps 100; sleep 500 [qa_retail_start 2 2] 1; sleep 20000 [quit] 1'
    else:   # section, flip
        commands = ('maxfps 100; qa_campaign_load 2 2; qa_after_gameplay [qa_robust_state start; qa_robust_exercise; '
                    'sleep 9500 [qa_robust_state end; qa_campaign_state] 1; sleep 10000 [quit] 1]; '
                    'sleep 60000 [qa_robust_state end; quit] 1')
    logfile = SCRATCH / f'robust-{name}.log'
    with logfile.open('w') as log:
        try:
            code = subprocess.run([str(client), '--halo', f'--home={profile}', f'--mod={mod}', '-t0', '-w640', '-h480',
                                   '-e' + commands], cwd=RUNTIME, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=240 if args.asan else 90, env=run_env).returncode
        except subprocess.TimeoutExpired:
            code = None
    text = logfile.read_text(errors='replace')
    problems = []
    if code is None:
        problems.append('timed out (hang)')
    elif code < 0:
        problems.append(f'crashed: signal {signal.Signals(-code).name}')
    elif code:
        problems.append(f'exit code {code}')
    if not re.search(expect, text):
        problems.append(f'no log line matching /{expect}/')
    reports = [line for line in text.splitlines() if any(m in line for m in SANITIZER)]
    if reports:
        problems.append(f'sanitizer: {reports[0][:200]}')
    if fields is not None:
        row = ROBUST.search(text)
        if not row:
            problems.append('no ROBUST_QA end row')
        else:
            values = dict(pair.split('=', 1) for pair in row.group(1).split() if '=' in pair)
            if values.get('alive') != '1' and kind == 'section':
                problems.append(f'player not alive at the end: {row.group(0)}')
            for key, value in fields.items():
                if values.get(key) != value:
                    problems.append(f'{key}={values.get(key)} (expected {value})')
        if kind == 'section' and 'ROBUST_QA exercised' not in text:
            problems.append('the exercise did not finish (ROBUST_QA exercised missing)')
    print(f'{"PASS" if not problems else "FAIL"}  {name}' + ('' if not problems else ': ' + '; '.join(problems)), flush=True)
    if problems:
        failures.append((name, logfile))
    if not args.keep and not problems:
        shutil.rmtree(mod, ignore_errors=True)

if failures:
    print('\nFailed cases (logs):\n' + '\n'.join(f'  {name}: {log}' for name, log in failures))
    raise SystemExit(1)
print(f'All {len(cases)} damaged-asset cases fell back without crashing.')
