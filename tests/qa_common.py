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


def read_hscb(path):
    """A compiled mission script (packages/halo/<cache>.hscb, tools/import_halo_scripts.py), read as
    source/src/halo_script.cpp Program::load reads it, versions 5 to 9. Returns a dict: version,
    scripts and globals (names), objects (names), conversations (names), loops (looping sound tag
    ids, as sound_looping_start takes them), encounters ([(name, [squad names])]) and, from
    version 9, profiles (player starting profiles as dicts), squad_defaults (per encounter, per squad:
    (ACTORS kind, actors.txt index)) and object_flags (a byte per object; 1: a story figure drawn
    outside scenes). Fails unless the file is read to its last byte."""
    import struct as _struct
    data = Path(path).read_bytes()
    offset = 0

    def take(fmt):
        nonlocal offset
        values = _struct.unpack_from('<' + fmt, data, offset)
        offset += _struct.calcsize('<' + fmt)
        return values

    def one(fmt):
        return take(fmt)[0]

    def text():
        nonlocal offset
        size = one('H')
        offset += size
        return data[offset - size:offset].decode('latin-1')

    def rows(row):
        return [row() for _ in range(one('I'))]

    assert data[:4] == b'HSCB', path
    offset = 4
    version = one('I')
    assert 5 <= version <= 9, (path, version)
    out = {'version': version}
    rows(lambda: take('4H3I'))                                       # nodes
    size = one('I')                                                  # strings
    offset += size
    out['scripts'] = rows(lambda: (text(), take('HHI'))[0])
    out['globals'] = rows(lambda: (text(), take('HI'))[0])
    rows(lambda: (text(), take('7f')))                               # cameras
    rows(lambda: (text(), take('5f')))                               # flags
    rows(lambda: (text(), take('H12f')))                             # volumes
    rows(lambda: (text(), text(), take('3f')))                       # titles
    rows(lambda: (text(), take('H'), rows(lambda: take('HBH8f'))))   # recordings

    def obj():
        name = text()
        text()
        take('6fh9f')
        rows(lambda: (text(), take('5f')))                           # markers
        rows(lambda: (text(), text()))                               # seats
        for _ in range(3):
            text()                                                   # locomotion
        if version >= 8:
            take('7fB')                                              # damage target
        return name
    out['objects'] = rows(obj)
    rows(lambda: (take('HH'), text(), text(), take('H')))            # animations
    rows(lambda: (take('HII'), text()))                              # sounds
    out['conversations'] = [c[0] for c in rows(lambda: (text(), rows(lambda: (take('H'), rows(lambda: take('H'))))))]
    out['loops'] = [loop[0][0] for loop in rows(lambda: (take('HB'), rows(lambda: take('f5i'))))]   # looping sound tag ids
    rows(lambda: take('H6f'))                                        # projections
    rows(lambda: take('H'))                                          # worlds
    rows(lambda: (text(), text()))                                   # HUD messages
    rows(lambda: take('f'))                                          # device groups
    out['encounters'] = rows(lambda: (text(), take('2h'), rows(lambda: (text(), take('h'), rows(lambda: take('h4f')))[0]))[::2])
    rows(lambda: take('4h'))                                         # BSP switches
    rows(lambda: take('I'))                                          # group flags

    def device():
        take('4h5IB2Hi8f')
        text()
        take('H6f')
        rows(lambda: (take('H'), rows(lambda: take('6f'))))
    rows(device)
    if version >= 6:
        for _ in out['encounters']:
            rows(lambda: take('3f2h'))                               # firing positions
            rows(lambda: take('I4h'))                                # platoons
            rows(lambda: (take('2hIh7I'), take('4h') if version >= 7 else (), rows(lambda: take('4h'))))
        rows(lambda: (text(), take('I'), rows(lambda: take('2h2f7h')), rows(lambda: take('3f'))))   # command lists
    if version >= 9:
        def profile():
            name = text()
            health, shield = take('2f')
            primary = text(); loaded, total = take('2h')
            secondary = text(); loaded2, total2 = take('2h')
            frags, plasmas = take('2B')
            return {'name': name, 'health': health, 'shield': shield, 'weapons': [(primary, loaded, total), (secondary, loaded2, total2)],
                    'frags': frags, 'plasmas': plasmas}
        out['profiles'] = rows(profile)
        out['squad_defaults'] = rows(lambda: rows(lambda: take('2h')))
        out['object_flags'] = rows(lambda: one('B'))
        assert len(out['squad_defaults']) == len(out['encounters']), path
        assert all(len(d) == len(e[1]) for d, e in zip(out['squad_defaults'], out['encounters'])), path
        assert len(out['object_flags']) == len(out['objects']), path
    assert offset == len(data), (path, offset, len(data))
    return out
