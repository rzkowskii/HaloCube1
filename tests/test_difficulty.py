"""Difficulty: halo_difficulty selects squad sizes, game_difficulty_get and enemy scaling.

The importer stores each squad's Easy, Normal, Hard and Impossible start counts
(HSCB version 7 or 8). On a30 section 2 the lz_search encounter is placed at each
difficulty: the starts the engine selects equal the sum of the tags' counts for that
level (Normal: the normal counts), Easy and Impossible differ monotonically, the actors
actually placed never exceed that (some starts have no walkable floor), game_difficulty_get returns the cvar, enemy health and the
damage multiplier grow with the level, and the choice persists across runs.
Needs the retail caches (HALO_SOURCE) for the expected counts.
"""
import mmap
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(HALO_SOURCE / 'a30.map', 'set HALO_SOURCE to the expanded Xbox caches')
require(root / 'packages/halo/a30.hscb', 'run tools/import_halo_scripts.py')
from import_halo_a30 import HaloMap  # noqa: E402
from import_halo_scripts import difficulty_counts, squad_rows  # noqa: E402

# Version 8 adds the damage targets' vitality and box to each object row; the squad counts are
# those of version 7.
assert int.from_bytes((root / 'packages/halo/a30.hscb').read_bytes()[4:8], 'little') in (7, 8, 9), \
    'a30.hscb is older than version 7; rerun tools/import_halo_scripts.py'

# Expected sizes from the retail tags, through the importer.
with (HALO_SOURCE / 'a30.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
    halo = HaloMap(data)
    t, s = halo.tags, halo.scenario
    raw = {}
    for enc in t.block(s + 1068, 176):
        for sq in t.block(enc + 128, 232):
            raw[(t.name(enc, 32), t.name(sq, 32))] = t.read('2h', sq + 124)
    encounters = {e[0]: e for e in squad_rows(halo)}
    # The importer's rows already carry the counts; check they follow the tag.
    for name, _, _, squads, (_, _, squad_ai) in encounters.values():
        for (squad_name, _, starts), ai in zip(squads, squad_ai):
            assert ai[11] == difficulty_counts(*raw[(name, squad_name)]), (name, squad_name, ai[11])
            assert list(ai[11]) == sorted(ai[11]), ('counts never shrink with difficulty', name, squad_name, ai[11])
    _, _, _, squads, (_, _, squad_ai) = encounters['lz_search']
    expected = []
    for d in range(4):
        total = 0
        for (squad_name, _, starts), ai in zip(squads, squad_ai):
            placeable = sum(1 for start in starts if start[0] >= 0)
            total += min(ai[11][d], placeable)
        expected.append(total)
assert expected[1] == sum(min(raw[('lz_search', sq[0])][0], sum(1 for st in sq[2] if st[0] >= 0)) for sq in squads), expected
assert expected[0] <= expected[1] < expected[2] <= expected[3] and expected[0] < expected[3], expected


def run(difficulty, profile=None, set_first=True):
    profile = profile or scratch(f'difficulty-{difficulty}-')
    log = profile / 'run.log'
    pick = f'halo_difficulty {difficulty}; ' if set_first else ''
    body = ('sleep 0 [qa_actor_place lz_search] 1; sleep 300 [qa_hs_ai ai_living_count lz_search 0; qa_difficulty] 1; '
            'sleep 400 [qa_actor_spawn 1 0 9 0] 1; sleep 1500 [quit] 1')
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{pick}qa_campaign_resume 2 2; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=120, check=True)
    text = log.read_text(errors='replace')
    count = float(re.search(r'HS_AI ai_living_count lz_search ([\d.]+)', text).group(1))
    level = re.search(r'DIFFICULTY (\d+) script=(\d+) damage=([\d.]+) health=([\d.]+)', text)
    selected = int(re.findall(r'placed \d+ actors from lz_search \((\d+) starts on difficulty (\d)\)', text)[-1][0])
    hp = int(re.search(r'ACTOR_SPAWN \S+ \S+ \d+ hp=(\d+)', text).group(1))
    return int(count), selected, tuple(float(v) for v in level.groups()), hp, profile, log


placed, chosen, levels, hps = [], [], [], []
for d in range(4):
    count, selected, (level, script, damage, health), hp, profile, log = run(d)
    assert level == d and script == d, ('game_difficulty_get returns the cvar', d, level, script, log)
    placed.append(count)
    chosen.append(selected)
    levels.append(damage)
    hps.append(hp)
    if d == 3:
        persisted = profile
assert chosen == expected, ('selected starts per difficulty match the importer table', chosen, expected)
assert all(p <= c for p, c in zip(placed, chosen)) and placed == sorted(placed) and placed[0] < placed[3], placed
assert levels == sorted(levels) and levels[0] < 1.0 == levels[1] < levels[3], levels
assert hps == sorted(hps) and hps[0] < hps[1] < hps[3], ('enemy health grows with difficulty', hps)

# The choice is saved with the profile: a later run without setting it is still Impossible.
count, selected, (level, script, *_), _, _, log = run(3, persisted, set_first=False)
assert level == 3 and selected == expected[3], ('halo_difficulty persists', level, count, log)
print(f'Difficulty: lz_search selects {chosen} starts and places {placed} actors on Easy..Impossible, damage x{levels}, elite health {hps}, choice persists.')
