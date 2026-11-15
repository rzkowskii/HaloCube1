"""Soak: load all 55 campaign sections in sequence in one process and track its memory.

    python3 tests/test_soak.py                 the QA client (build/macos/halo-ac-qa), two passes
    python3 tests/test_soak.py --asan          the ASan/UBSan client (scripts/build-macos.sh --asan)
    python3 tests/test_soak.py --passes 3 --settle 3000

Not yet run: written while testing was on hold (docs/macos-build.md, "Not yet verified in the
game"). Part of the release tier (`python3 tests/run.py --release`), not the quick tier.

`qa_soak PASSES SETTLE` (tests/native/soak_qa.inc) loads a10 section 1 to d40 section 8 (the
Crash Site's standalone map as a30's first section) as section selects, PASSES times over, lets
each run for SETTLE ms once it is live (opening scenes skipped) and prints a SOAK_QA row with the
process's resident size, peak and physical footprint (Mach task_info). The test fails on

  * a crash: the client exits non-zero or on a signal, or never prints `SOAK_QA done`;
  * a sanitizer report: AddressSanitizer, UndefinedBehaviorSanitizer (`runtime error:`) or
    LeakSanitizer output anywhere in the log (UBSan reports and goes on, so every report counts;
    tests/data/ubsan.supp holds the triaged stock-engine findings, empty to begin with);
  * a section that does not load or leaves the player dead (`SOAK_QA FAIL timeout`, `alive=0`, a
    row for another map than the one queued);
  * unbounded growth. Pass 1 loads every texture, model and sound for the first time and the
    engine's caches keep them, so pass 1 grows by design. From pass 2 on nothing new should be
    loaded: the median, over the sections, of a section's footprint in this pass minus its
    footprint in the pass before must be at most --growth-mb (64 MB by default), and the footprint
    at the end of the last pass at most --growth-mb times the extra passes above the end of pass 1.
    With --passes 1 only the crash and sanitizer checks apply.

Results go to build/qa/soak.log and build/qa/soak-memory.csv (one row per section and pass).
"""
import argparse
import csv
import os
import re
import signal
import statistics
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import MISSIONS  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('--asan', action='store_true', help='use build/macos-asan/halo-ac-qa (scripts/build-macos.sh --asan)')
parser.add_argument('--client', type=Path, help='another QA client binary')
parser.add_argument('--passes', type=int, default=2)
parser.add_argument('--settle', type=int, default=None, help='ms each live section runs (1500, 4000 with --asan)')
parser.add_argument('--growth-mb', type=float, default=64.0, help='allowed footprint growth per pass after the first')
parser.add_argument('--timeout', type=int, default=None, help='seconds for the whole run')
args = parser.parse_args()

client = args.client or (BUILD / 'macos-asan/halo-ac-qa' if args.asan else QA_BIN)
require(client, 'run scripts/build-macos.sh --asan' if args.asan else 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(RUNTIME, 'run scripts/build-macos.sh (it stages build/runtime)')
settle = args.settle if args.settle is not None else (4000 if args.asan else 1500)

# The order qa_soak walks: halo_a30 is a30's first section (the standalone Crash Site map).
order = []
for mission in MISSIONS:
    if mission.cache == 'a30':
        order.append('halo_a30')
    order += [f'halo_{mission.cache}_{part:02}' for part in range(1, len(mission.sections) + 1)]
assert len(order) == 55, len(order)

per_section = (25 if args.asan else 8) + settle / 1000
timeout = args.timeout or int(60 + per_section * len(order) * args.passes)
profile = scratch('soak-')
logfile = SCRATCH / 'soak.log'
env = dict(os.environ)
# halt_on_error: the first memory error ends the run (a report, then a non-zero exit). LeakSanitizer
# is not supported by Apple's ASan on arm64; detect_leaks=0 keeps it from complaining. Uninstrumented
# system frameworks trip container-overflow checks on their own vectors, hence detect_container_overflow=0.
env.setdefault('ASAN_OPTIONS', 'halt_on_error=1:abort_on_error=0:detect_leaks=0:detect_container_overflow=0:'
                               'print_summary=1:strict_string_checks=1')
env.setdefault('UBSAN_OPTIONS', f'print_stacktrace=1:halt_on_error=0:suppressions={root / "tests/data/ubsan.supp"}')
commands = '; '.join([
    'maxfps 120',
    f'qa_soak_timeout {120000 if args.asan else 90000}',
    f'sleep 800 [qa_soak {args.passes} {settle}] 1',
])
print(f'soak: {client.relative_to(root) if client.is_relative_to(root) else client}, {args.passes} passes of '
      f'{len(order)} sections, {settle} ms each, timeout {timeout} s', flush=True)
with logfile.open('w') as log:
    try:
        result = subprocess.run([str(client), '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480',
                                 '-e' + commands], cwd=RUNTIME, stdout=log, stderr=subprocess.STDOUT, timeout=timeout, env=env)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = None
text = logfile.read_text(errors='replace')
failures = []
if code is None:
    failures.append(f'timed out after {timeout} s')
elif code < 0:
    failures.append(f'crashed: signal {signal.Signals(-code).name}')
elif code:
    failures.append(f'exit code {code}')
if 'SOAK_QA done' not in text:
    failures.append('never printed SOAK_QA done')

markers = ('ERROR: AddressSanitizer', 'runtime error:', 'SUMMARY: UndefinedBehaviorSanitizer',
           'ERROR: LeakSanitizer', 'SUMMARY: AddressSanitizer')
reports = [line for line in text.splitlines() if any(m in line for m in markers)]
if reports:
    failures.append(f'{len(reports)} sanitizer report lines, the first: {reports[0][:300]}')

row = re.compile(r'SOAK_QA pass=(\d+) index=(\d+) map=(\S+) alive=(\d) stage=(-?\d+) load=(-?\d+) '
                 r'rss=(\d+) peak=(\d+) footprint=(\d+) millis=(\d+)')
rows = [m.groups() for m in row.finditer(text)]
for line in text.splitlines():
    if line.startswith('SOAK_QA FAIL'):
        failures.append(line)
memory = {}     # (pass, index) -> footprint (or rss when footprint is 0) in bytes
with (SCRATCH / 'soak-memory.csv').open('w', newline='') as out:
    writer = csv.writer(out)
    writer.writerow(['pass', 'index', 'map', 'alive', 'stage', 'load_ms', 'rss', 'peak', 'footprint', 'millis'])
    for p, i, name, alive, stage, load, rss, peak, footprint, millis in rows:
        writer.writerow([p, i, name, alive, stage, load, rss, peak, footprint, millis])
        p, i = int(p), int(i)
        if name != order[i]:
            failures.append(f'pass {p} row {i}: map {name}, expected {order[i]}')
        if alive != '1':
            failures.append(f'pass {p} {name}: player not alive')
        memory[p, i] = int(footprint) or int(rss)
expected = len(order) * args.passes
if len(rows) != expected:
    failures.append(f'{len(rows)} section rows, expected {expected}')

mb = 1024 * 1024
def pass_end(p):
    found = re.search(rf'SOAK_QA pass-end pass={p} rss=(\d+) peak=(\d+) footprint=(\d+)', text)
    return (int(found.group(3)) or int(found.group(1))) if found else None

baseline = re.search(r'SOAK_QA baseline rss=(\d+) peak=(\d+) footprint=(\d+)', text)
if baseline:
    print(f'baseline footprint {(int(baseline.group(3)) or int(baseline.group(1))) / mb:.0f} MB', flush=True)
for p in range(1, args.passes + 1):
    end = pass_end(p)
    print(f'pass {p}: end footprint {end / mb:.0f} MB' if end else f'pass {p}: no pass-end row', flush=True)
for p in range(2, args.passes + 1):
    deltas = [memory[p, i] - memory[p - 1, i] for i in range(len(order)) if (p, i) in memory and (p - 1, i) in memory]
    if not deltas:
        continue
    median = statistics.median(deltas) / mb
    worst = max(range(len(deltas)), key=lambda k: deltas[k])
    print(f'pass {p} against pass {p - 1}: median growth {median:+.1f} MB, largest {deltas[worst] / mb:+.1f} MB', flush=True)
    if median > args.growth_mb:
        failures.append(f'pass {p}: median per-section growth {median:.1f} MB over pass {p - 1} (limit {args.growth_mb} MB)')
first, last = pass_end(1), pass_end(args.passes)
if args.passes > 1 and first and last and (last - first) / mb > args.growth_mb * (args.passes - 1):
    failures.append(f'footprint grew {(last - first) / mb:.0f} MB from the end of pass 1 to the end of pass {args.passes} '
                    f'(limit {args.growth_mb * (args.passes - 1):.0f} MB)')
peaks = [int(r[7]) for r in rows]
if peaks:
    print(f'peak resident {max(peaks) / mb:.0f} MB; slowest load '
          f'{max(int(r[5]) for r in rows) / 1000:.1f} s ({max(rows, key=lambda r: int(r[5]))[2]})', flush=True)

if failures:
    print('\n'.join(['FAIL soak:'] + [f'  {f}' for f in failures[:30]]), flush=True)
    print(f'log: {logfile}', flush=True)
    raise SystemExit(1)
print(f'Soak passed: {len(rows)} section loads, no crash, no sanitizer report, bounded memory.')
