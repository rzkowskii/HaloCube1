"""Major upgrades at squad placement and Halo's squad sizes per difficulty.

Written while testing was on hold (the squad-count and major-upgrade batch): this test has not
been run yet.

Halo places an actor as its variant's major (elite minor plasma rifle as elite major plasma rifle,
a grunt, jackal or hunter minor as its major, a marine as marine_armored's major) when its actv
has a major reference and the squad's roll succeeds (../Halo3DS source/ai/encounters.c:3040-3051,
actors.c:2687). The squad's major_upgrade enum (squad+128, ai_scenario_definitions.h:298; enum at
:37) picks the chance (ai.c:615): normal 0 the globals' major_upgrade, few 1 major_upgrade_1, many
2 major_upgrade_2 (game_globals.h:63-99, values 28 to 30), none 3 never, all 4 always; the choice is
an error diffusion over the squad's error and the map-wide error (ai.c:1875).

Static (no game; the caches through HALO_SOURCE where noted):
1. actors.txt is format 3 and carries the globals' 35-value difficulty table (`difficulty` lines,
   named after the enum); with the caches, equal to every cache's matg difficulty_information;
   rows 28 to 30 equal the fallback chances in source/src/halo_ai.cpp.
2. Every `major` line names a variant in actors.txt other than itself; with the caches, each
   variant's major equals its actv's major reference (+36); the known majors are there.
3. Every packages/halo/<cache>.hscb is version 10: per squad its major_upgrade enum (with the
   caches, squad+128) and its start counts by Halo's rule (encounters.c:2191-2201, with the
   caches against squad+124/+126); across the ten caches 2,851 normal, 9 few, 25 many, 89 none
   and 14 all squads, and the rule differs from the importer's old one on 601 squads on Easy, 444
   on Hard and 132 on Impossible (none on Normal).
4. A Python port of ai_consider_major_upgrade (float32, Halo's LCG) keeps every squad within one
   actor of its chance and the whole list within three.

Game (QA client):
5. qa_major_upgrade_sim (the engine's halomajorupgradechance and haloconsidermajorupgrade off any
   scenario) on each difficulty: the chance per enum is the table's, few/many/normal random, none
   never and all always (forced), the unknown enum 7 as normal, and the majors per squad equal the
   Python port's (within one per squad and one overall, for float contraction).
6. a30 section 2, lz_search placed on Impossible and on Easy (qa_actor_place, qa_major_upgrades):
   every member placed as a major is its start variant's major, every other one its start's
   variant; none-enum squads never upgrade, all-enum squads always do; the placement log line
   ends ", N major"; Impossible places more majors than Easy among the members that can upgrade.
7. A checkpoint (qa_checkpoint_save), the player's death and a revert bring every member back as
   the variant it was (same squad, start and major), and the diffusion state is the checkpoint's.
8. The checkpoint's state block has a major flag of 1 on every `s squad` record of a major (0 on the
   others, followed by the health, shield and stood point since the stood-members change) and an
   `s upgrade` record with the seed; a Continue brings the same members back as the same variants.
"""
import math
import mmap
import re
import struct
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))

ACTORS = root / 'packages/halo/actors.txt'
HALO = root / 'packages/halo'
require(ACTORS, 'run python3 tools/halo_actors.py with the expanded caches')
require(HALO / 'a30.hscb', 'run tools/import_halo_scripts.py')
NAMES = ('enemy_damage', 'enemy_vitality', 'enemy_shield', 'enemy_recharge', 'friend_damage', 'friend_vitality',
         'friend_shield', 'friend_recharge', 'infection_forms', 'unused9', 'rate_of_fire', 'projectile_error',
         'burst_error', 'new_target_delay', 'burst_separation', 'target_tracking', 'target_leading',
         'overcharge_chance', 'special_fire_delay', 'guidance_vs_player', 'melee_delay_base', 'melee_delay_scale',
         'unused22', 'grenade_chance_scale', 'grenade_timer_scale', 'unused25', 'unused26', 'unused27',
         'major_upgrade', 'major_upgrade_1', 'major_upgrade_2', 'unused31', 'unused32', 'unused33', 'unused34')
close = lambda a, b: abs(a - b) <= 1e-5 * max(1.0, abs(b))


# ---- 1. The difficulty table in actors.txt.
text = ACTORS.read_text()
assert text.startswith('format 3\n'), 'actors.txt is format 3 (re-run tools/halo_actors.py)'
table, variants, current = {}, {}, None
for line in text.splitlines():
    key, _, rest = line.partition(' ')
    if key == 'difficulty':
        index, name, *values = rest.split()
        assert current is None, ('difficulty lines come before the variants', line)
        table[int(index)] = (name, [float(v) for v in values])
    elif key == 'actor':
        current = {'index': int(rest), 'major': ''}
    elif current is not None and key == 'path':
        current['path'] = rest
        variants[rest] = current
    elif current is not None and key == 'major':
        current['major'] = rest
assert sorted(table) == list(range(35)) and all(table[i][0] == NAMES[i] for i in range(35)), table
assert all(len(v) == 4 for _, v in table.values())
source = (root / 'source/src/halo_ai.cpp').read_text()
fallback = {name: [float(v.rstrip('f')) for v in re.search(name + r'\[4\] = \{([^}]*)\}', source).group(1).split(',')]
            for name in ('normal', 'few', 'many')}
for index, name in ((28, 'normal'), (29, 'few'), (30, 'many')):
    assert all(close(a, b) for a, b in zip(table[index][1], fallback[name])), (index, table[index], fallback[name])
CHANCES = {0: table[28][1], 1: table[29][1], 2: table[30][1]}

caches = (HALO_SOURCE / 'a30.map').exists()
if caches:
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_actors import actor_record, difficulty_rows  # noqa: E402
    from halo_weapons import CAMPAIGN, weapon_index  # noqa: E402
    from import_halo_scripts import difficulty_counts, squad_upgrade_rows  # noqa: E402
    weapons = weapon_index(root)
print('1. actors.txt carries the 35-value difficulty table; the major upgrade rows match the engine fallback.')

# ---- 2. Major references.
majors = {path: v['major'] for path, v in variants.items() if v['major']}
assert len(majors) >= 19, majors
for path, major in majors.items():
    assert major in variants and major != path, (path, major)
assert majors[r'characters\elite\elite minor\elite minor plasma rifle'] == r'characters\elite\elite major\elite major plasma rifle'
assert majors[r'characters\hunter\hunter'] == r'characters\hunter\hunter major'
assert majors[r'characters\grunt\grunt minor plasma pistol'] == r'characters\grunt\grunt major plasma pistol'
assert majors[r'characters\jackal\jackal minor plasma pistol'] == r'characters\jackal\jackal major plasma pistol'

# ---- 3. The .hscb squad tables.
programs = {p.stem: read_hscb(p) for p in sorted(HALO.glob('*.hscb'))}
assert len(programs) == 10 and all(h['version'] == 10 for h in programs.values()), {k: h['version'] for k, h in programs.items()}
enums, changed = {}, [0, 0, 0, 0]
seen_tables = []


def old_rule(normal, insane):
    """The importer's squad sizes before the squad-count batch (Easy 3/4 of Normal, Hard rounded up)."""
    normal = max(0, normal)
    top = max(normal, insane)
    return (normal * 3 + 2) // 4, normal, normal + (top - normal + 1) // 2, top


for cache, h in programs.items():
    for squads in h['squad_upgrades']:
        for v in squads:
            enums[v] = enums.get(v, 0) + 1
    if not caches:
        continue
    with (HALO_SOURCE / f'{cache}.map').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        t, s = halo.tags, halo.scenario
        assert list(squad_upgrade_rows(halo)) == h['squad_upgrades'], cache
        raw = [[t.read('2h', sq + 124) for sq in t.block(enc + 128, 232)] for enc in t.block(s + 1068, 176)]
        for counts, tags in zip(h['squad_counts'], raw):
            for got, (low, high) in zip(counts, tags):
                lo, hi = max(0, low), max(0, high)
                assert tuple(got) == (lo, lo, (lo + hi) // 2, hi) == difficulty_counts(low, high), (cache, got, low, high)
                changed = [c + (a != b) for c, a, b in zip(changed, got, old_rule(low, high))]
        seen_tables.append(difficulty_rows(halo))
        for tag in halo.directory:
            if tag.group == 'actv':
                record = actor_record(halo, tag, weapons)
                assert variants[tag.name]['major'] == record['major'], (tag.name, record['major'])
assert enums == {0: 2851, 1: 9, 2: 25, 3: 89, 4: 14}, enums
if caches:
    assert changed == [601, 0, 444, 132], ('squads whose size the rule changed, Easy..Impossible', changed)
    for rows in seen_tables:
        assert rows is not None and all(all(close(a, b) for a, b in zip(rows[i], table[i][1])) for i in range(35)), rows
print(f'2-3. {len(majors)} variants have majors; the ten .hscb files are version 10 with each squad\'s major_upgrade '
      f'({sorted(enums.items())}) and Halo\'s squad sizes' + (f' (changed {changed} against the old rule).' if caches else '.'))


# ---- 4. The Python port of ai_consider_major_upgrade.
def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def consider(state, squad, chance):
    """ai.c:1875-1893 in float32, with real_seed_random's LCG (math/random_math.c:69-70, 211-216)."""
    g = f32(state['global'] * f32(-1.0 / 3.0))
    q = f32(-state['squads'][squad])
    error = g if abs(g) > abs(q) else q
    state['seed'] = (state['seed'] * 1664525 + 1013904223) & 0xffffffff
    draw = f32((state['seed'] >> 16) / f32(65535.0))
    upgrade = draw < f32(error + chance)
    delta = f32((1.0 if upgrade else 0.0) - chance)
    state['squads'][squad] = f32(state['squads'][squad] + delta)
    state['global'] = f32(state['global'] + delta)
    return upgrade


def simulate(chance, squads, per, seed):
    state = {'seed': seed, 'global': 0.0, 'squads': [0.0] * squads}
    return [sum(consider(state, k, f32(chance)) for _ in range(per)) for k in range(squads)], state['global']


for chance in (0.05, 0.1, 0.15, 0.25, 0.3, 0.5, 0.75, 1.0):
    for per in (1, 2, 3, 5, 8):
        counts, _ = simulate(chance, 40, per, 0xdeadbeef)
        assert all(abs(c - per * chance) < 1 for c in counts), (chance, per, counts)
        assert abs(sum(counts) - 40 * per * chance) <= 3, (chance, per, sum(counts))
print('4. The diffusion keeps each squad within one actor of its chance and the list within three.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/maps/halo_a30_02.cgz', 'import the campaign (docs/macos-build.md)')


def qa(script, profile=None, timeout=150):
    profile = profile or scratch('major-')
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600', f'-e{script}'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), profile, log


# ---- 5. The engine's chance table and diffusion.
SEED = 0x1234abcd
for level in range(4):
    sims = '; '.join(f'qa_major_upgrade_sim {kind} 12 5 {SEED:08x}' for kind in (0, 1, 2, 3, 4, 7))
    out, _, log = qa(f'halo_difficulty {level}; {sims}; quit', timeout=60)
    rows = [dict(re.findall(r'(\w+)=(\S+)', line)) | {'counts': [int(v) for v in re.search(r'majors=([\d ]*) global', line).group(1).split()]}
            for line in out.splitlines() if line.startswith('MAJOR_SIM ')]
    assert len(rows) == 6, (level, log)
    for row in rows:
        kind = int(row['kind'])
        assert int(row['level']) == level and row['table'] == '1', (row, log)
        if kind in (3, 4):
            assert row['random'] == '0' and row['force'] == ('1' if kind == 4 else '0'), (row, log)
            assert row['counts'] == [5 if kind == 4 else 0] * 12, (row, log)
            continue
        chance = CHANCES[kind if kind in CHANCES else 0][level]
        assert row['random'] == '1' and close(float(row['chance']), chance), (row, chance, log)
        expected, _ = simulate(chance, 12, 5, SEED)
        assert all(abs(a - b) <= 1 for a, b in zip(row['counts'], expected)) and abs(sum(row['counts']) - sum(expected)) <= 1, \
            ('the engine diffusion matches the Python port', level, kind, row['counts'], expected, log)
print('5. qa_major_upgrade_sim: the chances per enum and difficulty are the table\'s, and the diffusion matches the port.')


# ---- 6. Placement on a30 section 2.
def members(out, label):
    found = {}
    block = out.split(f'MAJOR_LABEL {label}', 1)
    assert len(block) == 2, (label, out[-3000:])
    for line in block[1].splitlines():
        if line.startswith('MAJOR_STATE'):
            found['state'] = dict(re.findall(r'(\w+)=(\S+)', line))
            break
        m = re.match(r'MAJOR_UPGRADE (\S+) start=(\d+) enum=(-?\d+) major=([01]) variant=(.+) \| base=(.+) \| base_major=(.+)$', line)
        if m:
            squad, start, enum, major, variant, base, base_major = m.groups()
            found[(squad, int(start))] = (int(enum), major == '1', variant, base, base_major)
    assert 'state' in found, (label, out[-3000:])
    return found


def check_members(found, log):
    eligible = majors_placed = 0
    for key, value in found.items():
        if key == 'state':
            continue
        enum, major, variant, base, base_major = value
        if major:
            assert base_major != '-' and variant == base_major, ('a major is its start variant\'s major', key, value, log)
            majors_placed += 1
        else:
            assert variant == base, ('a member not upgraded is its start\'s variant', key, value, log)
        if base_major != '-':
            eligible += 1
            if enum == 3:
                assert not major, ('a none squad never upgrades', key, value, log)
            if enum == 4:
                assert major, ('an all squad always upgrades', key, value, log)
    return eligible, majors_placed


def place(level, extra='', profile=None, label='placed'):
    body = (f'sleep 0 [qa_actor_place lz_search] 1; sleep 300 [echo MAJOR_LABEL {label}; qa_major_upgrades] 1; {extra}'
            'sleep 9000 [quit] 1')
    return qa(f'halo_difficulty {level}; halo_scripttrace 1; qa_campaign_resume 2 2; qa_after_gameplay [botsshoot 0; {body}]',
              profile, timeout=200)


results = {}
for level in (0, 3):
    out, _, log = place(level, extra='sleep 600 [quit] 1; ')
    assert re.search(r'placed \d+ actors from lz_search \(\d+ starts on difficulty \d\), \d+ major', out), log
    found = members(out, 'placed')
    results[level] = check_members(found, log)
assert results[3][0] > 0 and results[3][1] * 2 >= results[3][0], ('most upgradeable members are majors on Impossible', results)
assert results[0][1] < results[3][1], ('Easy places fewer majors than Impossible', results)
print(f'6. lz_search: eligible/majors on Easy {results[0]}, on Impossible {results[3]}; every member is its start\'s variant or its major.')

# ---- 7 and 8. A revert and a Continue keep the variants.
extra = ('sleep 600 [qa_checkpoint_save; qa_major_upgrades] 1; sleep 900 [qa_halo_damage 100000] 1; sleep 2500 [qa_halo_revert] 1; '
         'sleep 4200 [echo MAJOR_LABEL reverted; qa_major_upgrades] 1; sleep 4500 [quit] 1; ')
out, profile, log = place(3, extra)
before, after = members(out, 'placed'), members(out, 'reverted')
alive_before = {k: v for k, v in before.items() if k != 'state'}
alive_after = {k: v for k, v in after.items() if k != 'state'}
assert alive_after and all(alive_before.get(k) == v for k, v in alive_after.items()), \
    ('a revert brings every member back as the variant it was', alive_before, alive_after, log)
assert after['state']['seed'] == after['state']['saved_seed'] and after['state']['global'] == after['state']['saved_global'], \
    ('the revert restores the checkpoint\'s diffusion state', after['state'], log)
print(f'7. After a revert {len(alive_after)} lz_search members came back as the same variants; the diffusion state is the checkpoint\'s.')

saved = read_progress(profile)
squads = [r.split() for r in saved['records']['s'] if r.startswith('squad ') and ' =lz_search ' in r]
upgrade = [r.split() for r in saved['records']['s'] if r.startswith('upgrade ')]
assert upgrade and len(upgrade) == 1, ('the state block has one upgrade record', saved['records']['s'][-10:])
# squad <e> =<encounter> <s> =<squad> <start> <suspended> <flags> [<major 0|1> [<health> <shield> [<x> <y> <z> <yaw>]]]
# (the health, shield and stood point since the stood-members change: tests/test_frontend_progress.py item 16)
flagged = sum(1 for r in squads if len(r) >= 9 and r[8] == '1')
assert all(len(r) in (8, 9, 11, 15) for r in squads), squads
assert flagged == sum(1 for k, v in alive_before.items() if k[0].startswith('lz_search/') and v[1]), \
    ('one flagged record per lz_search major', flagged, alive_before, squads)
out, _, log = qa('halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [botsshoot 0; sleep 1500 [echo MAJOR_LABEL continued; '
                 'qa_major_upgrades] 1; sleep 1800 [quit] 1]', profile, timeout=200)
continued = {k: v for k, v in members(out, 'continued').items() if k != 'state'}
assert continued and all(alive_before.get(k) == v for k, v in continued.items()), \
    ('a Continue brings every member back as the variant it was', alive_before, continued, log)
print(f'8. The state block flags {flagged} majors with the diffusion state; a Continue brought {len(continued)} members back as the same variants.')
