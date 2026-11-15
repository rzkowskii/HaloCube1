"""Stub-route gate: no section's route may become a stub, and the known-bad table may not grow.

    python3 tests/test_route_metrics.py            check packages/maps/*.metrics.json
    python3 tests/test_route_metrics.py --table    also print every section's figures

No game and no Mac needed; it reads the importer's metrics (git-ignored, beside the maps). Every
campaign import writes `packages/maps/<section>.metrics.json`; refresh them without touching the
maps with

    python3 tools/import_halo_campaign.py "$maps" --metrics-only

A route is a stub (the definition of done in the roadmap) when it has fewer than 100 steps, its
reachable floor is under 5% of the section's floor columns, or it ends more than 25 source units
from the section's exit point (`STUB_*` and `stub_reasons` in tools/import_halo_campaign.py). A
walked route (retail collision) that does not end inside its exit volume counts too, except the
sections in EXIT_FALLS, which end at the edge above it; a walked route that does end inside it is
not held to the reach share (judgement call: the planner proved it connects entry and exit, and a
low share only means a big map window). The test fails when

  * a section has no metrics, metrics of another format, or a mode (walked, raster, driven,
    flown, stock) that the importer's tables (VALIDATED_COLLISION, STOCK_SECTIONS,
    DRIVEN_SECTIONS, FLOWN_SECTIONS) do not give it;
  * the maps on disk are not what the importer writes (`cgz_current`, `route_current` false after
    --metrics-only: re-import the section);
  * a stub is not in KNOWN_STUBS below (the table must not grow), or a KNOWN_STUBS section is no
    longer a stub (remove it, so the table stays current).

halo_a30, the Crash Site's standalone map (tools/import_halo_a30.py), has an authored layout and
no metrics; it is only checked for existence. Static and fast: part of the quick tier. Written
while testing was on hold; run once, statically, to fill KNOWN_STUBS.
"""
import argparse
import json
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import (DRIVEN_SECTIONS, EXIT_FALLS, FLOWN_SECTIONS, METRICS_FORMAT, MISSIONS,  # noqa: E402
                                  STOCK_SECTIONS, VALIDATED_COLLISION, stub_reasons)

# Sections whose route is a stub today, with why. Names and reasons only (no retail figures).
# Measured with --metrics-only on the tree of this batch; see docs/macos-build.md (Status).
KNOWN_STUBS = {
    'halo_a30_01': 'a30 section 2: the walked route stops a few dozen steps in, far short of bsp 0,1 '
                   '(importer: WARNING route ends outside its exit)',
    'halo_b30_03': "the Silent Cartographer's return keeps the b30 work's authored raster layout "
                   '(tests/test_b30_retail_collision.py), which ends short of beach_lz',
    'halo_b40_03': 'the walked route ends just outside bsp 1,10 at the b3 lift (importer: WARNING route ends outside its exit)',
    'halo_c10_03': "stock geometry (STOCK_SECTIONS): BSP 2's floors do not join within the engine's jump; the raster route ends short",
    'halo_c10_05': "stock geometry (STOCK_SECTIONS): bridge_c's flicker; the raster route ends short of the exit",
    'halo_c10_06': "stock geometry (STOCK_SECTIONS): the upper storey is out of reach; the raster route ends short",
    'halo_c40_01': 'flown (FLOWN_SECTIONS): the raster route ends far from door_c1; the player flies up from fly_away_1',
    'halo_d40_08': "driven (DRIVEN_SECTIONS): the Warthog run's raster route ends far from the finale point",
}

parser = argparse.ArgumentParser()
parser.add_argument('--table', action='store_true')
args = parser.parse_args()
maps = root / 'packages/maps'
require(maps / 'halo_a30.cgz', 'import the assets first (docs/macos-build.md)')


def expected_mode(cache, part):
    retail = cache in VALIDATED_COLLISION and (cache, part) not in STOCK_SECTIONS
    if not retail:
        return 'stock'
    if (cache, part) in FLOWN_SECTIONS:
        return 'flown'
    if (cache, part) in DRIVEN_SECTIONS:
        return 'driven'
    return 'raster' if cache == 'b30' else 'walked'


problems, stubs, rows = [], {}, []
for mission in MISSIONS:
    for part in range(len(mission.sections)):
        name = f'halo_{mission.cache}_{part + 1:02}'
        path = maps / f'{name}.metrics.json'
        if not path.is_file():
            problems.append(f'{name}: no metrics (run tools/import_halo_campaign.py "$maps" --metrics-only)')
            continue
        m = json.loads(path.read_text())
        if m.get('format') != METRICS_FORMAT:
            problems.append(f'{name}: metrics format {m.get("format")}, expected {METRICS_FORMAT} (re-run --metrics-only)')
            continue
        mode = expected_mode(mission.cache, part)
        if m['mode'] != mode:
            problems.append(f'{name}: mode {m["mode"]}, the importer tables say {mode}')
        if not m['cgz_current']:
            problems.append(f'{name}: {name}.cgz on disk is not what the importer writes now (re-import the section)')
        if not m['route_current']:
            problems.append(f'{name}: {name}.route on disk is not what the importer writes now (re-import the section)')
        reasons = stub_reasons(m, (mission.cache, part) in EXIT_FALLS)
        if reasons:
            stubs[name] = reasons
        rows.append((name, m['mode'], m['steps'], m['reach_pct'], m['entry_offset'], m['exit_offset'],
                     m['ends_in_exit'], ', '.join(reasons) or '-'))

if not (maps / 'halo_a30.cgz').is_file():
    problems.append('halo_a30: the standalone Crash Site map is missing (tools/import_halo_a30.py)')

if args.table or problems or set(stubs) != set(KNOWN_STUBS):
    print(f'{"section":13} {"mode":7} {"steps":>6} {"reach%":>7} {"entry":>6} {"exit":>6} {"in exit":>7}  stub')
    for name, mode, steps, reach, entry, exit_offset, inside, why in rows:
        print(f'{name:13} {mode:7} {steps:6d} {reach:7.2f} {entry:6.1f} {exit_offset:6.1f} {str(inside):>7}  {why}')

for name in sorted(set(stubs) - set(KNOWN_STUBS)):
    problems.append(f'{name}: new stub route ({", ".join(stubs[name])}); the known-bad table must not grow')
for name in sorted(set(KNOWN_STUBS) - set(stubs)):
    if name in {r[0] for r in rows}:
        problems.append(f'{name}: no longer a stub; remove it from KNOWN_STUBS ({KNOWN_STUBS[name]})')
if problems:
    print('\n'.join(['FAIL route metrics:'] + [f'  {p}' for p in problems]))
    raise SystemExit(1)
print(f'Route metrics: {len(rows)} sections, {len(stubs)} known stubs, none new.')
