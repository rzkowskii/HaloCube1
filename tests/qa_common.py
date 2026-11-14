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
