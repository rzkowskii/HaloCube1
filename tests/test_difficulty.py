"""Difficulty: halo_difficulty selects squad sizes, game_difficulty_get and enemy scaling.

The importer stores each squad's Easy, Normal, Hard and Impossible start counts
(HSCB version 7 or later), by Halo's rule (../Halo3DS source/ai/encounters.c:2191-2201): Easy and
Normal min_count, Hard (min_count + max_count) / 2, Impossible max_count, even below min_count
(each at least 0). On a30 section 2 the lz_search encounter is placed at each difficulty: the
starts the engine selects equal the sum of the tags' counts for that level (Normal: min_count),
Easy selects what Normal does, and the counts are not monotonic (lz_search's grunt squad is
4/3, so Hard selects 16 starts against Normal's 17 and Impossible 18); the actors actually
placed never exceed that (some starts have no walkable floor), game_difficulty_get returns the
cvar, enemy health and the damage multiplier grow with the level, and the choice persists
across runs. Needs the retail caches (HALO_SOURCE) for the expected counts.

The squad-size rule changed while testing was on hold (the squad-count batch): the static part
and the selected counts have not been run with it.

Team scaling (the difficulty-table work, written while testing was on hold, NOT YET RUN): the
damage, vitality and recharge scales are the globals' (matg) difficulty_information table that
tools/halo_actors.py writes to actors.txt (`difficulty` lines; globals+284, ../Halo3DS
game/game_globals.h:63-99), by Halo's team rules (objects/damage.c, game/game_globals.c:204-225).
Static: the ten caches carry one table (identical), and actors.txt's lines equal it. Game, per
difficulty: `qa_difficulty`'s DIFFICULTY line gives enemy_damage, the Covenant's enemy_vitality,
enemy_shield and enemy_recharge, the marines' and the player's friend_vitality, friend_shield
and friend_recharge, no team's enemy_vitality, and table=1; the DIFFICULTY_DAMAGE lines give
enemy_damage on hostile-owned damage (Covenant on the player, the marines, the Flood; Flood on the
Covenant; Covenant on no team), divided back out on its own side (Covenant on Covenant: factor 1,
divisor enemy_damage), and 1 for the player's, the marines' and ownerless damage; the spawned
Elite's health is its Normal health times enemy_vitality.
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
# those of version 7 (by Halo's rule since the squad-count batch; version 10 adds major upgrades).
assert int.from_bytes((root / 'packages/halo/a30.hscb').read_bytes()[4:8], 'little') in (7, 8, 9, 10), \
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
            low, high = (max(0, v) for v in raw[(name, squad_name)])
            assert ai[11] == (low, low, (low + high) // 2, high), ('Halo\'s squad sizes, encounters.c:2191', name, squad_name, ai[11])
    _, _, _, squads, (_, _, squad_ai) = encounters['lz_search']
    expected = []
    for d in range(4):
        total = 0
        for (squad_name, _, starts), ai in zip(squads, squad_ai):
            placeable = sum(1 for start in starts if start[0] >= 0)
            total += min(ai[11][d], placeable)
        expected.append(total)
assert expected[1] == sum(min(raw[('lz_search', sq[0])][0], sum(1 for st in sq[2] if st[0] >= 0)) for sq in squads), expected
# Easy places Normal's count; Hard and Impossible follow the tags and need not be larger.
assert expected[0] == expected[1], expected
assert expected == [17, 17, 16, 18], ('lz_search: grunt 4/3, far_grunt 2/3, elite 1/2 (the retail tags)', expected)

# The globals' difficulty table: one in every cache, all ten the same, and actors.txt carries it.
from halo_actors import DIFFICULTY_VALUES, difficulty_rows, number  # noqa: E402
tables = {}
for cache in ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40'):
    with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        tables[cache] = difficulty_rows(HaloMap(data))
    assert tables[cache] and len(tables[cache]) == len(DIFFICULTY_VALUES), (cache, 'no matg difficulty_information')
table = tables['a30']
assert all(t == table for t in tables.values()), ('the caches disagree on the difficulty table', [c for c, t in tables.items() if t != table])
exported = {}
for entry in (root / 'packages/halo/actors.txt').read_text().splitlines():
    if entry.startswith('difficulty '):
        _, index, name, *values = entry.split()
        exported[int(index)] = (name, values)
assert sorted(exported) == list(range(len(DIFFICULTY_VALUES))), ('actors.txt has no full difficulty table; rerun tools/halo_actors.py', sorted(exported))
for index, row in enumerate(table):
    assert exported[index] == (DIFFICULTY_VALUES[index], [number(v) for v in row]), (index, exported[index], row)
# Halo's team values on each level (float32 from the tag, as the engine reads them).
enemy_damage, enemy_vitality, enemy_shield, enemy_recharge = (table[i] for i in range(4))
friend_vitality, friend_shield, friend_recharge = (table[i] for i in range(5, 8))
print(f'The ten caches share one difficulty table; actors.txt carries it (enemy_damage {[round(v, 2) for v in enemy_damage]}, '
      f'friend_vitality {[round(v, 2) for v in friend_vitality]}).')


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
    teams = re.search(r'DIFFICULTY \d+ .* shield=([\d.]+) recharge=([\d.]+) human=([\d.]+),([\d.]+),([\d.]+) '
                      r'player=([\d.]+),([\d.]+),([\d.]+) none=([\d.]+) table=(\d)', text)
    assert teams, ('qa_difficulty printed no team values', log)
    pairs = {m.group(1): (float(m.group(2)), float(m.group(3)))
             for m in re.finditer(r'DIFFICULTY_DAMAGE (\S+) factor=([\d.]+) divisor=([\d.]+)', text)}
    return int(count), selected, tuple(float(v) for v in level.groups()), hp, profile, log, \
        tuple(float(v) for v in teams.groups()), pairs


def close(a, b):
    return abs(a - b) <= .006   # printed with two decimals


placed, chosen, levels, hps, vitalities = [], [], [], [], []
for d in range(4):
    count, selected, (level, script, damage, health), hp, profile, log, teams, pairs = run(d)
    assert level == d and script == d, ('game_difficulty_get returns the cvar', d, level, script, log)
    # Halo's team values (game_globals.c:204-225): the Covenant and no team take enemy_*, the
    # marines (allied) and the player (the player team, friendly to itself) friend_*.
    shield, recharge, h_vit, h_shield, h_recharge, p_vit, p_shield, p_recharge, none, table_seen = teams
    assert table_seen == 1, ('actors.txt carried the globals table', log)
    assert close(damage, enemy_damage[d]) and close(health, enemy_vitality[d]) and close(shield, enemy_shield[d]) \
        and close(recharge, enemy_recharge[d]) and close(none, enemy_vitality[d]), ('enemy values', d, damage, health, shield, recharge, none)
    for got in ((h_vit, h_shield, h_recharge), (p_vit, p_shield, p_recharge)):
        assert all(close(a, b) for a, b in zip(got, (friend_vitality[d], friend_shield[d], friend_recharge[d]))), ('friend values', d, got)
    # damage.c:1415-1419 (hostile owner: x enemy_damage) and 1603-1612 (on its own side: divided back).
    ed = enemy_damage[d]
    want = {'covenant>player': (ed, 1), 'covenant>human': (ed, 1), 'covenant>covenant': (1, ed), 'covenant>flood': (ed, 1),
            'flood>covenant': (ed, 1), 'human>player': (1, 1), 'human>covenant': (1, 1), 'player>covenant': (1, 1),
            'player>player': (1, 1), 'none>player': (1, 1), 'covenant>none': (ed, 1)}
    if ed == 1:
        want['covenant>covenant'] = (1, 1)
    assert sorted(pairs) == sorted(want), (sorted(pairs), log)
    for key, (factor, divisor) in want.items():
        assert close(pairs[key][0], factor) and close(pairs[key][1], divisor), ('damage factor', d, key, pairs[key], (factor, divisor))
    vitalities.append(enemy_vitality[d])
    placed.append(count)
    chosen.append(selected)
    levels.append(damage)
    hps.append(hp)
    if d == 3:
        persisted = profile
assert chosen == expected, ('selected starts per difficulty match the importer table', chosen, expected)
# The same starts on Easy and Normal, so the same actors; no order between the other levels.
assert all(p <= c for p, c in zip(placed, chosen)) and placed[0] == placed[1], placed
assert levels == sorted(levels) and levels[0] < 1.0 == levels[1] < levels[3], levels
assert hps == sorted(hps) and hps[0] < hps[1] < hps[3], ('enemy health grows with difficulty', hps)
# The Covenant Elite spawned on each level: its Normal health times enemy_vitality (spawnEnemy).
assert all(abs(hp - round(hps[1] * v)) <= 1 for hp, v in zip(hps, vitalities)), ('enemy_vitality on the spawn', hps, vitalities)

# The choice is saved with the profile: a later run without setting it is still Impossible.
count, selected, (level, script, *_), _, _, log, _, _ = run(3, persisted, set_first=False)
assert level == 3 and selected == expected[3], ('halo_difficulty persists', level, count, log)
print(f'Difficulty: lz_search selects {chosen} starts and places {placed} actors on Easy..Impossible, damage x{levels}, elite health {hps}, choice persists.')
