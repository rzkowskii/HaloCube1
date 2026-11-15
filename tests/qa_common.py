"""Shared paths for the macOS verification scripts.

Run any test from anywhere; paths resolve from the repository root. Scratch
profiles and logs go to build/qa/, which Git ignores.
"""
import os
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
BUILD = root / 'build'
SCRATCH = BUILD / 'qa'
RUNTIME = BUILD / 'runtime'
CLIENT = BUILD / 'macos/halo-ac'
QA_BIN = BUILD / 'macos/halo-ac-qa'
APP = BUILD / 'Halo in AssaultCube.app'
APP_EXE = APP / 'Contents/MacOS/Halo in AssaultCube'
DATA = root / 'tests/data'
# Expanded Xbox caches, only needed by the tools that read retail data.
# The asset library keeps copies of the caches in build/retail-assets/sources.
HALO_SOURCE = Path(os.environ.get('HALO_SOURCE', BUILD / 'retail-assets/sources'))

SCRATCH.mkdir(parents=True, exist_ok=True)


def scratch(prefix):
    """A fresh profile directory under build/qa/."""
    return Path(tempfile.mkdtemp(prefix=prefix, dir=SCRATCH))


def require(path, hint):
    if not Path(path).exists():
        raise SystemExit(f'missing {path}: {hint}')


def qa_key():
    """Keystroke helper for the packaged app, built from tests/tools/qa-key.swift."""
    helper = SCRATCH / 'qa-key'
    source = root / 'tests/tools/qa-key.swift'
    if not helper.exists() or helper.stat().st_mtime < source.stat().st_mtime:
        subprocess.run(['swiftc', '-O', '-o', str(helper), str(source)], check=True)
    return helper


def progress_hash(text, value=2166136261):
    """FNV-1a, as source/src/halo.cpp checks config/halo-progress.txt (and its version 3 state block)."""
    for byte in text.encode():
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


STATE_KINDS = 'svwp'   # script, vehicles, world (ground items), player limits and powerups


def state_block(header, records):
    """A version 3 state block: `header` the fields after 'state' up to the map hash
    (format mission section checkpoint script_hash map_hash, the hashes as hex text), `records`
    the record lines with their 's '/'v '/'w '/'p ' prefixes. Returns the block's lines."""
    body = ''.join(line + '\n' for line in records)
    return [f"state {' '.join(map(str, header))} {len(records)} {progress_hash(body):08x}"] + list(records)


def write_progress(path, lines, version=2):
    """Writes a valid progress file (version 2 by default) from its lines (without the header and check)."""
    text = f'halo-progress {version}\n' + ''.join(line + '\n' for line in lines)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + f'check {progress_hash(text):08x}\n')


def read_progress(profile):
    """config/halo-progress.txt of a profile as {key: [fields]} (None if missing); fails on a bad check.

    Version 2 and 3 files are line based ('furthest 0 1', 'beaten 0', 'game 0 1 2 1 0', ...); version 1
    files ('1 0 1') come back as {'furthest': ['0', '1'], 'beaten': [...]}. A version 3 state block
    comes back as 'state' (its header fields) and 'records' ({'s': [...], 'v': [...], 'w': [...],
    'p': [...]}, each record without its prefix); its record count and FNV-1a are checked too.
    """
    path = Path(profile) / 'config/halo-progress.txt'
    if not path.exists():
        return None
    text = path.read_text()
    if not text.startswith('halo-progress '):
        fields = text.split()
        return {'version': ['1'], 'furthest': fields[1:3], 'beaten': [fields[3] if len(fields) > 3 else '0']}
    body, _, check = text.rpartition('check ')
    assert check.strip() == f'{progress_hash(body):08x}', f'{path}: bad check line'
    rows = {'version': [body.split('\n', 1)[0].split()[1]]}
    records = {kind: [] for kind in STATE_KINDS}
    block = []
    for line in body.splitlines():
        key, _, rest = line.partition(' ')
        if len(key) == 1 and key in STATE_KINDS:
            records[key].append(rest)
            block.append(line)
        elif key == 'weapon':
            rows.setdefault('weapon', []).append(rest.split(' ', 4))   # slot loaded reserve battery path
        else:
            rows[key] = rest.split()
    if 'state' in rows:
        header = rows['state']
        assert int(header[6]) == len(block), f'{path}: the state block holds {len(block)} records, its header {header[6]}'
        digest = progress_hash(''.join(line + '\n' for line in block))
        assert header[7] == f'{digest:08x}', f'{path}: bad state block hash'
        rows['records'] = records
    return rows
