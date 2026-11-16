"""Scene meshes current: every halo_scene_*.mesh and its textures are what the exporter writes now, and
every cinematic cast export is on disk and in its .hscb.

    python3 tests/test_scene_meshes.py

No game. Needs the expanded Xbox caches (HALO_SOURCE, as tests/test_damage_targets.py; skipped
without them). tools/import_halo_scripts.py exports a cinematic world (packages/maps/
halo_scene_<map>_NN.mesh, packages/textures/halo_scene_<map>_NN/) only when it is missing unless
--rebuild is given, so a mesh written by an older exporter stays on disk unseen; the use-aware rule's
batch found 61 of the 68 campaign scene meshes stale that way and regenerated them. This runs
`import_halo_scripts.py "$HALO_SOURCE" --check-scenes` (stale_scene_meshes: a fresh export of every
scene BSP of the ten missions into a temporary directory, meshes compared byte for byte and PNG
textures by content, their decoded header and inflated IDAT data, so a Python or zlib upgrade does not
make them stale; it writes nothing under packages/; about a minute) and fails on any stale, missing or leftover file. Since
the MD3 range fix it also fails on the cast (check_scenes): a cinematic export the scripts use with no
tris.md3 on disk (its clip would be missing from the .hscb), an orphan directory under
packages/models/halo/scene/<map>/, or a .hscb whose model paths are out of step with the disk. Fix with
`tools/import_halo_scripts.py "$maps" --mission <cache>` (orphans are pruned by any import), or with
`--rebuild` for stale meshes. Written while testing was on hold: not yet run.
"""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

if not (HALO_SOURCE / 'a10.map').is_file():
    print(f'No expanded caches in {HALO_SOURCE} (set HALO_SOURCE): skipped.')
    raise SystemExit(0)
result = subprocess.run([sys.executable, str(root / 'tools/import_halo_scripts.py'), str(HALO_SOURCE), '--check-scenes'],
                        timeout=900)
if result.returncode:
    raise SystemExit('stale scene meshes or cast exports (listed above)')
print('Every campaign scene mesh and its textures match a fresh export; every cast export is on disk and in its .hscb.')
