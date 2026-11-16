"""Squad cap and the Flood life cycle in the QA client.

c10 encounters placed one after another (each squad killed before the next)
all report actors, so dead entries no longer eat the 128-actor cap. A carrier
form that dies bursts into infection forms. An infection form that
reaches a dead marine turns the body into a Flood combat form after a delay.
Infection forms leap at a target a few strides away.

The carrier burst, the infection pop and the speed stagger from the tags (written while testing was
on hold, NOT YET RUN): actors.txt's optional `unit`, `spawn` and `pop` lines equal the tag bytes in
every cache that has the unit (static, needs HALO_SOURCE); a killed carrier releases its `spawn`
line's lower..upper forms of the named variant (3 to 6 flood_infection, ../Halo3DS units/units.c:
1666-1693), thrown (FLOOD_THROW), or 4 to 8 without the line; an infection form's death pops the
forms inside its jpt!'s cutoff (objects/damage.c:2343-2421): on Normal every one (FLOOD_POP hit equals
candidates), on Impossible one at most; c10's nopop forms have the random speed increase flag and,
fighting a target on Impossible, move at infection_forms * (cn % 137) * 0.00729927 + 1 (bipeds.c:
3580-3590), the ordinary forms at 1.

The forms join the carrier's squad (written while testing was on hold, NOT YET RUN; items 7 to 11):
actors.txt's `destroyed` lines and effects.txt's carrier blast equal the tags (static); on c20
section 1 a killed carrier's forms join enc2_1/carriers (FLOOD_ADOPT), ai_living_count counts them,
ai_nonswarm_count does not, ai_living_fraction counts one swarm actor for them (../Halo3DS
ai/ai_script.c:4681-4725, ai/actors.c:2539-2555), ai_migrate takes them along and ai_erase deletes
them with no pop and no burst (actors.c:3845-3886) where ai_kill pops them; a checkpoint, a revert
and a Continue keep them (`s spawned`); the carrier's body destroyed effect deals its damage before
the forms appear.
"""
import shutil
import mmap
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/halo/actors.txt', 'run tools/halo_actors.py')


def actor_lines():
    """actors.txt records by actv path: {key: [fields...]} for the optional lines."""
    records, current = {}, None
    for line in (root / 'packages/halo/actors.txt').read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'actor':
            current = {'index': int(rest)}
        elif key == 'path' and current is not None:
            records[rest] = current
        elif key in ('unit', 'spawn', 'pop') and current is not None:
            current[key] = rest.split()
        elif key == 'destroyed' and current is not None:
            current[key] = rest
    return records


actors = actor_lines()
by_index = {r['index']: path for path, r in actors.items()}
carrier = actors.get(r'characters\floodcarrier\floodcarrier', {})
spawn_line = carrier.get('spawn')

# 0. Static: the optional lines against the tag bytes (unit +380 flags, bipd +756 biped flags,
#    +588 spawned actor variant, +604/+606 bounds, +608 throw velocity; coll +188 body destroyed
#    effect's jpt! with the pop flag).
if (HALO_SOURCE / 'c20.map').exists():
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_actors import number, unit_record  # noqa: E402
    for cache in ('c10', 'c20', 'c40', 'd20', 'd40'):
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group != 'actv' or tag.name not in actors:
                    continue
                unit = halo.reference(halo.tags, tag.address + 20)
                if not unit or unit.group != 'bipd':
                    continue
                flags, = halo.tags.read('I', unit.address + 380)
                biped, = halo.tags.read('I', unit.address + 756)
                assert actors[tag.name].get('unit') == [str(flags), str(biped)], (cache, tag.name, actors[tag.name].get('unit'))
                record = unit_record(halo, unit)
                if 'spawn' in record:
                    name, lower, upper, velocity = record['spawn']
                    want = [str(next(r['index'] for p, r in actors.items() if p == name)), str(lower), str(upper), number(velocity)]
                    assert actors[tag.name].get('spawn') == want, (cache, tag.name, actors[tag.name].get('spawn'), want)
                else:
                    assert 'spawn' not in actors[tag.name], (cache, tag.name)
                if 'pop' in record:
                    assert actors[tag.name].get('pop') == [number(v) for v in record['pop']], (cache, tag.name)
    # The retail values the engine is expected to use.
    assert spawn_line and by_index[int(spawn_line[0])] == r'characters\flood_infection\flood_infection' \
        and spawn_line[1:] == ['3', '6', '3'], ('the carrier releases 3 to 6 flood_infection at 3.0', spawn_line)
    pops = {p for p, r in actors.items() if 'pop' in r}
    assert pops == {r'characters\flood_infection\flood_infection', r'characters\flood_infection\flood_infection nopop'}, pops
    nopop = actors[r'characters\flood_infection\flood_infection nopop']
    assert int(nopop['unit'][1]) >> 11 & 1 and not int(actors[r'characters\flood_infection\flood_infection']['unit'][1]) >> 11 & 1
    print('Flood tags: actors.txt\'s unit, spawn and pop lines equal the tag bytes; the carrier releases 3 to 6 forms at 3.0.')


def run(name, start, schedule, timeout=120):
    profile = scratch(f'flood-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def rows(text, tag, label=None):
    found = []
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] == tag and (label is None or parts[1] == label):
            found.append(dict(f.split('=', 1) for f in re.findall(r'(\w+=\S+)', line)))
    return found


def counts(text, label):
    return {k: int(v) for k, v in rows(text, 'FLOOD_QA', label)[-1].items()}


# 1. c10 section 3 places its swarm encounters one by one, each squad killed before the next.
#    Behind 128 dead entries (the cap used to count them) every encounter must still place
#    exactly what it places on a fresh list.
def place_all(name, pad):
    text, log = run(name, 'qa_campaign_resume 6 3', [(500, f'qa_squad_pad {pad}; qa_place_all'), (4000, 'quit')], timeout=150)
    result = {}
    for line in text.splitlines():
        m = re.match(r'SQUAD_QA (\S+) starts=(-?\d+) placed=(\d+)', line)
        if m and m.group(1) != 'total' and int(m.group(2)) > 0:
            result[m.group(1)] = int(m.group(3))
    return result, log


fresh, log = place_all('cap-fresh', 0)
padded, log = place_all('cap-padded', 128)
assert sum(fresh.values()) >= 40 and any(v > 0 for v in fresh.values()), ('swarm encounters place actors', fresh, log)
assert padded == fresh, ('dead entries do not use up the cap', fresh, padded, log)
print(f'Squad cap: {sum(padded.values())} actors across {sum(v > 0 for v in padded.values())} encounters placed behind 128 dead entries, as on a fresh list.')

# 2. A carrier form bursts into infection forms when killed: its spawn line's lower..upper (3 to 6),
#    each thrown, or the engine's earlier 4 to 8 without the line.
text, log = run('carrier', 'halo_damagetrace 1; halo_preview', [
    (0, 'qa_actor_spawn 7 2 14 0'),
    (1500, 'qa_actor_count before; qa_actor_kill 7'),
    (3000, 'qa_actor_count after'),
    (3300, 'quit'),
])
before, after = counts(text, 'before'), counts(text, 'after')
assert before['carrier'] == 1 and before['infection'] == 0, before
lower, upper = (int(spawn_line[1]), int(spawn_line[2])) if spawn_line else (4, 8)
assert after['carrier'] == 0 and lower <= after['infection'] <= upper, ('carrier burst', after, (lower, upper), log)
if spawn_line:
    # Actor names carry spaces ("Carrier Form 1").
    spawned = re.search(r'FLOOD_SPAWN .*? variant=(\d+) lower=(\d+) upper=(\d+) count=(\d+) velocity=([\d.]+)', text)
    assert spawned and spawned.group(1) == spawn_line[0] and lower <= int(spawned.group(4)) <= upper, ('FLOOD_SPAWN', log)
    throws = re.findall(r'FLOOD_THROW .*? speed=([\d.]+)', text)
    # Halo's throw: velocity * [0.5, 1] / 2 horizontally (actors.c:2944-2958, bipeds.c:1641).
    v = float(spawn_line[3])
    assert len(throws) == after['infection'] and all(v * .25 - .01 <= float(s) <= v * .5 + .01 for s in throws), ('throws', throws, log)
print(f'Carrier: killed form burst into {after["infection"]} infection forms.')

# 3. An infection form reaching a dead marine turns the body into a Flood combat form.
text, log = run('infect', 'halo_preview', [
    (0, 'qa_actor_spawn 9 1 8 0'),
    (1500, 'qa_actor_kill 9; qa_actor_spawn 6 2 6 3'),
    (2300, 'qa_actor_count early'),
    (10000, 'qa_actor_count late'),
    (10300, 'quit'),
])
early, late = counts(text, 'early'), counts(text, 'late')
assert early['flood_human'] == 0, ('the body does not rise at once', early, log)
assert late['flood_human'] == 1 and late['infection'] == 0, ('the infected body becomes a combat form', late, log)
assert 'infection form takes a Marine body' in text, log
print('Corpse: infection form took a marine body, a Flood combat form rose.')

# 4. Infection forms leap at a target a few strides away.
text, log = run('leap', 'halo_preview', [
    (0, 'qa_actor_spawn 6 2 10 0'),
    (5000, 'qa_actor_state leap'),
    (5300, 'quit'),
])
leaps = [int(a['leaps']) for a in rows(text, 'ACTOR_QA', 'leap')]
assert leaps and max(leaps) >= 1, ('infection form leaps', leaps, log)
print(f'Leap: infection form leapt {max(leaps)} time(s).')

# 5. The infection pop: three forms 0.4 world units apart, the first killed. On Normal its pop hits
#    every form inside the cutoff; on Impossible one at most (infection_forms 1 > 0: damage.c:2357).
if actors[r'characters\flood_infection\flood_infection'].get('pop'):
    for level, at_most_one in ((1, False), (3, True)):
        text, log = run(f'pop-{level}', f'halo_damagetrace 1; halo_difficulty {level}; halo_preview', [
            (0, 'qa_actor_spawn 6 2 6 0; qa_actor_spawn 6 2 6 0.4; qa_actor_spawn 6 2 6 -0.4'),
            (200, 'qa_actor_count before; qa_actor_kill_one 6'),
            (600, 'qa_actor_count after'),
            (900, 'quit'),
        ])
        first = re.search(r'FLOOD_POP variant=.*? candidates=(\d+) hit=(\d+) resisted=(\d+) one=(\d) toughness=([-\d.]+)', text)
        assert first, ('no pop traced', log)
        candidates, hit, resisted, one = (int(first.group(i)) for i in range(1, 5))
        assert counts(text, 'before')['infection'] == 3, log
        assert candidates >= 1, ('the neighbours stand inside the 0.75 unit cutoff', first.group(0), log)
        if at_most_one:
            assert one == 1 and hit <= 1 and hit + resisted <= candidates, ('Impossible: one form a pop', first.group(0), log)
        else:
            assert one == 0 and resisted == 0 and hit == candidates, ('Normal: every form inside the cutoff', first.group(0), log)
        print(f'Pop on difficulty {level}: {hit} of {candidates} neighbours popped ({resisted} resisted).')

# 6. The speed stagger: c10's nopop form has the flag; fighting the player on Impossible it moves at
#    infection_forms * (cn % 137) * 0.00729927 + 1. An ordinary form keeps 1.
text, log = run('stagger', 'halo_difficulty 3; halo_preview', [
    (0, 'qa_variant_spawn "flood_infection nopop" 2 8 0; qa_variant_spawn "flood_infection\\flood_infection" 2 8 2'),
    (2500, 'qa_flood_stagger fight'),
    (2800, 'quit'),
])
staggers = rows(text, 'FLOOD_STAGGER', 'fight')
assert len(staggers) == 2, ('two forms', staggers, log)
for row in staggers:
    flag, combat, scale, value, cn = int(row['flag']), int(row['combat']), float(row['scale']), float(row['value']), int(row['cn'])
    want = value * (cn % 137) * 0.00729927 + 1 if flag and combat else 1.0
    assert abs(scale - max(0.05, want)) < 1e-3, ('stagger', row, want, log)
    assert combat or not flag, ('the nopop form fights the player by now', row, log)
assert sorted(int(r['flag']) for r in staggers) == [0, 1], staggers
print(f'Stagger on Impossible: {[(r["flag"], r["combat"], r["scale"]) for r in staggers]}.')


# 7. Static: actors.txt's `destroyed` lines (the body destroyed effect of a unit destroyed after
#    dying, coll +188, unless it is the pop) equal the tags, and effects.txt holds the carrier's blast
#    (characters\floodcarrier\bdoy destroyed: 1 to 2 world units, 40 to 70-85).
destroyed = {p: r['destroyed'] for p, r in actors.items() if 'destroyed' in r}
assert destroyed.get(r'characters\floodcarrier\floodcarrier') == r'characters\floodcarrier\effects\body destroyed', destroyed
assert not any('flood_infection' in p for p in destroyed), ('the infection forms pop instead', destroyed)
if (HALO_SOURCE / 'c20.map').exists():
    from halo_actors import destroyed_effect  # noqa: E402
    for cache in ('b30', 'c10', 'c20', 'd20', 'd40'):
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group != 'actv' or tag.name not in actors:
                    continue
                unit = halo.reference(halo.tags, tag.address + 20)
                assert actors[tag.name].get('destroyed') == destroyed_effect(halo, unit), (cache, tag.name)
effects_text = (root / 'packages/halo/effects.txt').read_text()
blast = re.search(r'path characters\\floodcarrier\\effects\\body destroyed\n(?:(?!end\n).*\n)*?damage (.*)\n', effects_text)
assert blast and blast.group(1).split()[:5] == ['1', '2', '40', '70', '85'], ('effects.txt: the carrier blast', blast and blast.group(1))
print(f'Body destroyed effects: {len(destroyed)} variants, the carrier\'s blast 1 to 2 units, 40 to 70-85.')


def squad_counts(text, label):
    """SQUAD_COUNT rows of a label by squad name."""
    found = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) > 2 and parts[0] == 'SQUAD_COUNT' and parts[1] == label:
            found[parts[2]] = {k: float(v) for k, v in re.findall(r'(\w+)=(\S+)', line)}
    return found


def hs(text, command, name):
    values = re.findall(rf'HS_AI {command} {re.escape(name)} (-?[\d.]+)', text)
    return [float(v) for v in values]


C20 = 'halo_difficulty 1; halo_damagetrace 1; halo_scripttrace 1; halo_unitblast 0; qa_campaign_resume 7 1'
if not (root / 'packages/maps/halo_c20_01.cgz').exists():
    print('SKIP squad membership: c20 section 1 is not imported.')
else:
    # 8. The forms join enc2_1/carriers: counted by ai_living_count, not by ai_nonswarm_count, once
    #    as a swarm actor by ai_living_fraction; ai_migrate takes them along; ai_erase deletes them
    #    (no FLOOD_POP), and the carriers it erases release nothing.
    text, log = run('squad', C20, [
        (0, 'botsshoot 0; qa_combat_protect 1; qa_actor_place enc2_1'),
        (1500, 'qa_squad_counts before enc2_1; qa_hs_ai ai_living_count enc2_1/carriers 0'),
        (1600, 'qa_squad_kill_one enc2_1/carriers'),
        (2600, 'qa_squad_counts after enc2_1; qa_hs_ai ai_living_count enc2_1/carriers 0; '
               'qa_hs_ai ai_nonswarm_count enc2_1/carriers 0; qa_hs_ai ai_living_fraction enc2_1/carriers 0'),
        (2700, 'qa_hs_cmd ai_migrate "enc2_1/carriers enc2_1/combats"'),
        (2800, 'qa_squad_counts migrated enc2_1'),
        (2900, 'echo ERASE_MARK; qa_hs_ai ai_erase enc2_1/combats 0'),
        (3400, 'qa_squad_counts erased enc2_1; qa_hs_ai ai_living_count enc2_1 0'),
        (3500, 'quit'),
    ], timeout=180)
    before, after = squad_counts(text, 'before'), squad_counts(text, 'after')
    assert 'enc2_1/carriers' in before and before['enc2_1/carriers']['living'] >= 1, ('enc2_1 placed its carriers', before, log)
    spawn = re.search(r'FLOOD_SPAWN .*? count=(\d+) velocity=[\d.]+ squad=(-?\d+)/(-?\d+)', text)
    assert spawn and spawn.group(2) != '-1', ('the burst names the carrier\'s squad', log)
    adopted = re.findall(r'FLOOD_ADOPT .*? squad=enc2_1/carriers variant=(\d+) swarm=(\d+)', text)
    assert 1 <= len(adopted) <= int(spawn.group(1)), ('the forms join the squad', adopted, log)
    assert len({s for _, s in adopted}) == 1, ('fewer than 16 bodies: one swarm actor', adopted)
    b, a = before['enc2_1/carriers'], after['enc2_1/carriers']
    k = len(adopted)
    assert a['spawned'] == k and a['spawned_living'] == k, ('every adopted form lives in the squad', a, k)
    assert a['living'] == b['living'] - 1 + k, ('ai_living_count: the dead carrier out, its forms in', b, a, k)
    assert hs(text, 'ai_living_count', 'enc2_1/carriers')[-1] == a['living'], log
    assert a['nonswarm'] == b['nonswarm'] - 1 and hs(text, 'ai_nonswarm_count', 'enc2_1/carriers')[-1] == a['nonswarm'], (b, a)
    # The original count: every carrier entry, dead or alive, and one swarm actor for the forms.
    assert a['swarms'] == 1 and a['actors'] == a['members'] - k + 1, a
    assert abs(hs(text, 'ai_living_fraction', 'enc2_1/carriers')[-1] - a['living'] / a['actors']) < 1e-3, (a, log)
    migrated = squad_counts(text, 'migrated')
    assert migrated['enc2_1/combats']['spawned_living'] == k and migrated.get('enc2_1/carriers', {'living': 0})['living'] == 0, migrated
    tail = text[text.index('ERASE_MARK'):]
    assert 'FLOOD_POP' not in tail and 'FLOOD_SPAWN' not in tail, ('ai_erase deletes: no pop, no burst', log)
    assert 'ai_erase deletes' in tail, log
    assert squad_counts(text, 'erased')['enc2_1/combats']['living'] == 0, log
    print(f'Squad: the carrier\'s {k} forms joined enc2_1/carriers (living {b["living"]:.0f} -> {a["living"]:.0f}, '
          f'fraction {a["fraction"]:.3f} over {a["actors"]:.0f} actors), migrated with it, and ai_erase deleted them without a pop.')

    # 9. ai_kill kills them: the forms pop (FLOOD_POP) and the living carriers burst again into the squad.
    text, log = run('squad-kill', C20, [
        (0, 'botsshoot 0; qa_combat_protect 1; qa_actor_place enc2_1'),
        (1500, 'qa_squad_kill_one enc2_1/carriers'),
        (2500, 'echo KILL_MARK; qa_hs_ai ai_kill enc2_1/carriers 0'),
        (3500, 'qa_squad_counts killed enc2_1'),
        (3600, 'quit'),
    ], timeout=180)
    tail = text[text.index('KILL_MARK'):]
    assert 'FLOOD_POP' in tail, ('ai_kill kills the forms: they pop', log)
    assert re.search(r'FLOOD_ADOPT .*? squad=enc2_1/carriers', tail) or 'FLOOD_SPAWN' not in tail, ('a killed carrier\'s forms join', log)
    print('Kill: ai_kill popped the forms and the killed carriers\' forms joined the squad again.')

    # 10. The checkpoint keeps them (Halo's checkpoint is the whole game state): a revert and a
    #     Continue bring the forms back as squad members, and the progress file holds `s spawned`.
    def run_profile(name, script, profile=None, timeout=200):
        profile = profile or scratch(f'flood-{name}-')
        log = profile / f'{name}.log'
        with log.open('w') as output:
            subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600', f'-e{script}'],
                           cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
        return log.read_text(errors='replace'), profile

    def spawned_rows(text, label):
        return [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines()
                if line.startswith(f'SQUAD_STOOD {label} ') and ' spawned=' in line and ' spawned=-1 ' not in line + ' ']

    text, profile = run_profile('save', 'halo_difficulty 1; halo_damagetrace 1; halo_unitblast 0; qa_campaign_resume 7 1; '
                                'qa_after_gameplay [botsshoot 0; qa_combat_protect 1; qa_actor_place enc2_1; '
                                'sleep 1500 [qa_squad_kill_one enc2_1/carriers] 1; '
                                'sleep 2500 [qa_squad_stood saved; qa_checkpoint_save] 1; '
                                'sleep 2800 [qa_combat_protect 0; qa_halo_damage 100000] 1; sleep 4500 [qa_halo_revert] 1; '
                                'sleep 4700 [qa_squad_stood reverted; qa_squad_counts reverted enc2_1] 1; sleep 5000 [quit] 1]')
    saved, reverted = spawned_rows(text, 'saved'), spawned_rows(text, 'reverted')
    assert saved and len(reverted) == len(saved), ('the revert brings the forms back as members', len(saved), len(reverted), text[-2000:])
    assert all(r['spawned'] == saved[0]['spawned'] for r in reverted), reverted
    records = [r.split() for r in read_progress(profile)['records']['s'] if r.startswith('spawned ')]
    assert len(records) == len(saved) and all(r[2] == '=enc2_1' and r[4] == '=carriers' for r in records), records[:3]
    copy = scratch('flood-continue-')
    shutil.copytree(profile / 'config', copy / 'config')
    text, _ = run_profile('continue', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [botsshoot 0; '
                          'qa_squad_stood continued; qa_squad_counts continued enc2_1; sleep 300 [quit] 1]', copy)
    assert 'could not be restored' not in text, text[-2000:]
    continued = spawned_rows(text, 'continued')
    assert len(continued) == len(saved), ('a Continue brings them back', len(saved), len(continued), text[-2000:])
    print(f'Checkpoint: {len(saved)} forms kept through a revert and a Continue ({len(records)} `s spawned` records).')

    # 11. The carrier's blast: its body destroyed effect deals its damage (UNIT_BLAST dealt=1) before
    #     its forms appear; with halo_unitblast 0 it deals none.
    text, log = run('blast', C20.replace('halo_unitblast 0', 'halo_unitblast 1'), [
        (0, 'botsshoot 0; qa_combat_protect 1; qa_actor_place enc2_1'),
        (1500, 'qa_squad_kill_one enc2_1/carriers'),
        (2500, 'quit'),
    ], timeout=180)
    blast_at = text.find('UNIT_BLAST variant=characters\\floodcarrier\\floodcarrier')
    assert blast_at >= 0 and 'dealt=1' in text[blast_at:text.index('\n', blast_at)], ('the carrier blast deals damage', log)
    first_adopt = text.find('FLOOD_ADOPT')
    assert first_adopt < 0 or blast_at < first_adopt, ('the blast comes before the forms', log)
    print('Blast: the carrier\'s body destroyed effect went off before its forms appeared.')
