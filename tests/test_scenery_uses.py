"""The use-aware scenery rule: which named scenery stays in the .hcol (halo_collision.scenery_scripted).

    python3 tests/test_scenery_uses.py

No game. Needs the expanded Xbox caches (HALO_SOURCE, as tests/test_damage_targets.py; skipped
without them) and the imported maps. halo_collision.scenery_surfaces leaves a placed scenery
placement out of a section's .hcol only when a script may create, destroy, move or attach it as a
child while the section plays (import_halo_scripts.object_uses; SCENERY_READ_ONLY,
SCENERY_PARENT_ARGUMENT, SCENERY_SCRIPTS_AT_END, SCENERY_KEPT). It checks that
  * object_uses finds the uses the decisions in docs/macos-build.md (Scenery collision, Scripted
    scenery by use) rest on: a30's pod_of_death only as objects_attach's parent, a50's
    field_generator_a and b40's pelican_crashed only in objects_predict, b30's shaft_box_1 to _3 only
    in cutscene_extraction_exit, c10's crashed_dropship and x50_rock only in ini_x50_preclean, a10's
    field_x30 only in mission_final;
  * scenery_scripted keeps those baked (with the mission's SCENERY_SCRIPTS_AT_END) and still leaves out
    what a script really changes during play: a10's bridge_barricade_1 (object_create and
    object_destroy in mission_shoot), d40's fighter_hangar (object_create_anew, object_teleport,
    scenery_animation_start), b30's rock_kick; an unknown function or a set counts as a change;
  * each section that holds one of the kept placements lists it in its .hcol's SCN1 trailer
    (the Crash Site, a10 section 7, a50 section 1, b30 sections 1 and 3, c10 section 1; b40's
    pelican_crashed is outside every b40 section's map window, so none lists it);
  * the Crash Site draws no mapmodel for intro_tree (not-placed flag 1, created by no a30 script:
    import_halo_campaign.crash_site_scenery leaves it out).
Written while testing was on hold: not yet run.
"""
import mmap
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from halo_collision import scenery_scripted  # noqa: E402
from import_halo_campaign import CRASH_SITE, SCENERY_KEPT, SCENERY_SCRIPTS_AT_END, crash_site_scenery  # noqa: E402

maps = root / 'packages/maps'
if not (HALO_SOURCE / 'a30.map').is_file():
    print(f'No expanded caches in {HALO_SOURCE} (set HALO_SOURCE): skipped.')
    raise SystemExit(0)

from import_halo_a30 import HaloMap  # noqa: E402
from import_halo_scripts import Program, named_objects, object_uses  # noqa: E402


def scn1(path):
    """The scenario scenery indices an .hcol's SCN1 trailer lists (tests/test_retail_scenery.py's walk)."""
    data = path.read_bytes()
    signature, count = struct.unpack_from('<4sI', data, 0)
    assert signature == b'HCL1', path.name
    offset = 8
    for _ in range(count):
        points, = struct.unpack_from('<I', data, offset + 16)
        offset += 20 + 12 * points
    while offset < len(data):
        tag, entries = struct.unpack_from('<4sI', data, offset)
        width = 3 if tag == b'SCN1' else 1
        values = struct.unpack_from(f'<{entries * width}I', data, offset + 8)
        if tag == b'SCN1':
            return set(values[0::3])
        offset += 8 + 4 * entries * width
    return set()


class Identity:
    """A projection that leaves retail points as they are (crash_site_scenery only moves them)."""
    @staticmethod
    def point(p):
        return p


# (mission, name, the functions its uses may be, the scripts they may be in, the maps that must bake it)
KEPT = [
    ('a30', 'pod_of_death', {'objects_attach'}, {'setup_dead'}, ['halo_a30'] if CRASH_SITE else []),
    ('a50', 'field_generator_a', {'objects_predict'}, {'mission_area2'}, ['halo_a50_01']),
    ('b40', 'pelican_crashed', {'objects_predict'}, {'ext_a_setup'}, []),
    *[('b30', f'shaft_box_{n}', {'object_destroy'}, {'cutscene_extraction_exit'}, ['halo_b30_01', 'halo_b30_03'])
      for n in (1, 2, 3)],
    ('c10', 'crashed_dropship', {'object_destroy'}, {'ini_x50_preclean'}, ['halo_c10_01']),
    ('c10', 'x50_rock', {'object_destroy'}, {'ini_x50_preclean'}, ['halo_c10_01']),
    ('a10', 'field_x30', {'object_destroy'}, {'mission_final'}, ['halo_a10_07']),
]
LEFT_OUT = [('a10', 'bridge_barricade_1'), ('d40', 'fighter_hangar'), ('b30', 'rock_kick')]

problems = []
for cache in sorted({row[0] for row in KEPT} | {row[0] for row in LEFT_OUT}):
    with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        program = Program(halo)
        uses = object_uses(program)
        objects = [name for name, _, _ in named_objects(program)]
        at_end = SCENERY_SCRIPTS_AT_END.get(cache, ())
        kept = {name for (mission, _), names in SCENERY_KEPT.items() if mission == cache for name in names}
        tags = halo.tags
        placements = tags.block(halo.scenario + 528, 72)
        index_of = {}
        for index, placement in enumerate(placements):
            _, name_index = tags.read('2h', placement)
            if 0 <= name_index < len(objects):
                index_of[objects[name_index]] = index
        for mission, name, functions, scripts, baked in KEPT:
            if mission != cache:
                continue
            found = uses.get(name, [])
            if not found or {f for f, _, _ in found} - functions or {s for _, _, s in found} - scripts:
                problems.append(f'{cache} {name}: uses {found}, expected only {sorted(functions)} in {sorted(scripts)}')
            if scenery_scripted(found, at_end) and name not in kept:
                problems.append(f'{cache} {name}: counted as scripted (expected baked)')
            for map_name in baked:
                hcol = maps / f'{map_name}.hcol'
                if not hcol.is_file():
                    problems.append(f'{map_name}: no .hcol (re-import the section)')
                elif index_of.get(name) not in scn1(hcol):
                    problems.append(f'{map_name}: SCN1 does not list {name} (re-import the section)')
        for mission, name in LEFT_OUT:
            if mission == cache and not scenery_scripted(uses.get(name, []), at_end):
                problems.append(f'{cache} {name}: not counted as scripted, uses {uses.get(name)}')
        if cache == 'a30':
            if CRASH_SITE and any(row[-1] == index_of.get('intro_tree') for row in crash_site_scenery(halo, Identity())):
                problems.append('Crash Site: intro_tree still has a mapmodel')
# Conservative for anything not listed.
for uses_, expected in (([('object_set_scale', 0, 's')], True), ([('set', 1, 's')], True),
                        ([('(global)', 0, 'global g')], True), ([('objects_attach', 2, 's')], True),
                        ([('objects_attach', 0, 's'), ('objects_predict', 0, 's')], False),
                        ([('object_destroy', 0, 'end')], False)):
    if scenery_scripted(uses_, ('end',)) != expected:
        problems.append(f'scenery_scripted({uses_}) is not {expected}')

for problem in problems:
    print(problem)
if problems:
    raise SystemExit(f'{len(problems)} problems')
print(f'The use-aware scenery rule keeps {len(KEPT)} named placements baked and {len(LEFT_OUT)} scripted ones out.')
