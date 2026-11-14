#!/usr/bin/env python3
"""Copy a Mach-O binary's non-system dylibs into an app bundle and relink them.

    bundle-dylibs.py <executable> <Frameworks dir>

Every dependency outside /usr/lib and /System (Homebrew, Cellar, @rpath) is
copied, transitively, to the Frameworks directory and referenced from there as
@executable_path/../Frameworks/<name>. The caller re-signs the bundle.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

SYSTEM = ('/usr/lib/', '/System/')
FALLBACK = ['/opt/homebrew/lib', '/usr/local/lib']


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def deps(path):
    lines = run('otool', '-L', str(path)).splitlines()[1:]
    return [line.strip().split(' (')[0] for line in lines if line.strip()]


def rpaths(path):
    out, found = run('otool', '-l', str(path)).splitlines(), []
    for i, line in enumerate(out):
        if line.strip() == 'cmd LC_RPATH':
            found.append(out[i + 2].strip().split(' (')[0][len('path '):])
    return found


def resolve(ref, owner):
    if ref.startswith(SYSTEM) or ref.startswith('@executable_path'):
        return None
    if ref.startswith('@rpath/') or ref.startswith('@loader_path/'):
        name = ref.split('/', 1)[1]
        loader = owner.parent
        candidates = [Path(p.replace('@loader_path', str(loader))) / name for p in rpaths(owner)]
        candidates += [loader / name] + [Path(d) / name for d in FALLBACK]
        for c in candidates:
            if c.exists():
                return c.resolve()
        raise SystemExit(f'cannot resolve {ref} for {owner}')
    if not os.path.isabs(ref):
        raise SystemExit(f'unsupported dependency {ref} in {owner}')
    if not Path(ref).exists():
        raise SystemExit(f'missing dependency {ref} of {owner}')
    return Path(ref).resolve()


def main():
    exe, frameworks = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
    frameworks.mkdir(parents=True, exist_ok=True)
    copied = {}      # real source path -> bundled path
    queue = [(exe, exe)]
    while queue:
        original, target = queue.pop()
        if target.name.startswith('libSDL2-') and not (frameworks / 'libSDL3.dylib').exists():
            # sdl2-compat dlopens libSDL3.dylib at run time, so it is not in otool -L.
            for rp in rpaths(original):
                real = (original.parent / rp.replace('@loader_path/', '')).resolve() / 'libSDL3.dylib'
                if real.exists():
                    real = real.resolve()
                    dest = frameworks / real.name
                    shutil.copy2(real, dest)
                    os.chmod(dest, 0o755)
                    copied[real] = dest
                    queue.append((real, dest))
                    if real.name != 'libSDL3.dylib':
                        os.symlink(real.name, frameworks / 'libSDL3.dylib')
                    break
            else:
                raise SystemExit('sdl2-compat found but libSDL3 was not (checked its rpaths)')
        for ref in deps(target):
            source = resolve(ref, original)
            if source is None:
                continue
            if source not in copied:
                dest = frameworks / source.name
                shutil.copy2(source, dest)
                os.chmod(dest, 0o755)
                copied[source] = dest
                queue.append((source, dest))
            subprocess.run(['install_name_tool', '-change', ref,
                            f'@executable_path/../Frameworks/{source.name}', str(target)],
                           check=True, capture_output=True)
    for dest in copied.values():
        subprocess.run(['install_name_tool', '-id', f'@executable_path/../Frameworks/{dest.name}',
                        str(dest)], check=True, capture_output=True)
        # Drop stale rpaths that point back into Homebrew.
        for rp in rpaths(dest):
            subprocess.run(['install_name_tool', '-delete_rpath', rp, str(dest)],
                           capture_output=True)
    for rp in rpaths(exe):
        if rp.startswith(('/opt/', '/usr/local')):
            subprocess.run(['install_name_tool', '-delete_rpath', rp, str(exe)], capture_output=True)
    print(f'bundled {len(copied)} libraries')


main()
