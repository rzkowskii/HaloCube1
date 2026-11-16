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

Halo's actor melee (written while testing was on hold, NOT YET RUN; items 12 to 16): actors.txt's `melee`
lines and player.txt's `unit` line equal the tag bytes (static); an unarmed Flood combat form carries no
gun and strikes the player with its unit melee jpt! (20 to 30) at its animation's key frame; an infection
form touching the shielded player deals its jpt! (3) and is spent with no pop, and with the shield down
deals Halo's continuous melee (about 2.9 every ten ticks); a carrier within its melee range dies of its
own fatal melee as its melee animation ends, setting off its blast and releasing its forms, and never swings.
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

# 4. Infection forms leap at a target a few strides away. Since Halo's melee (item 13) a form that
#    reaches the shielded player is spent on it, so this item leaves the melee out (halo_actormelee 0).
text, log = run('leap', 'halo_actormelee 0; halo_preview', [
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
#    infection_forms * (cn % 137) * 0.00729927 + 1. An ordinary form keeps 1. Without Halo's melee
#    (halo_actormelee 0), so neither form is spent on the player's shield before the probe.
text, log = run('stagger', 'halo_actormelee 0; halo_difficulty 3; halo_preview', [
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


# Halo's actor melee (written while testing was on hold, NOT YET RUN).
def melee_lines():
    """actors.txt `melee` lines by actv path, as strings, and player.txt's `unit` line."""
    found, current = {}, None
    for line in (root / 'packages/halo/actors.txt').read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'path':
            current = rest
        elif key == 'melee' and current:
            found[current] = rest.split()
    player = root / 'packages/halo/player.txt'
    unit = next((l.split()[1:] for l in player.read_text().splitlines() if l.startswith('unit ')), None) if player.exists() else None
    return found, unit


melee, player_unit = melee_lines()

# 12. Static: the `melee` lines against the tag bytes (actv +352, +356, +368, +372; actr +892, +896,
#     +932; the carried weapon's flags bit 15; the unit's 'stand' melee animation) in every cache, the
#     retail values the engine is expected to use, and player.txt's cyborg flags (bit 22: shields fry
#     infection forms).
if (HALO_SOURCE / 'c10.map').exists():
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_actors import melee_animation, number  # noqa: E402
    from halo_weapons import CAMPAIGN  # noqa: E402
    for cache in CAMPAIGN:
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            t = halo.tags
            for tag in halo.directory:
                if tag.group != 'actv' or tag.name not in melee:
                    continue
                a = tag.address
                actr, weapon, unit = (halo.reference(t, a + o) for o in (4, 100, 20))
                weapon = weapon if weapon and weapon.group == 'weap' else None
                want = [*t.read('2f', a + 352), *t.read('2f', a + 368)]
                want += [*t.read('2f', actr.address + 892), t.read('f', actr.address + 932)[0]] if actr and actr.group == 'actr' else [0.0] * 3
                want.append(1 if weapon and t.read('I', weapon.address + 776)[0] >> 15 & 1 else 0)
                want += list(melee_animation(halo, unit, weapon))
                assert melee[tag.name] == [number(v) for v in want], (cache, tag.name, melee[tag.name], want)
    retail = {
        r'characters\floodcombat_human\floodcombat_human unarmed': ['4', '6', '6', '8', '0.75', '3', '0', '0', '50', '15'],
        r'characters\floodcombat elite\floodcombat elite unarmed': ['4', '6', '14', '18', '0.75', '3', '0', '0', '51', '16'],
        r'characters\floodcarrier\floodcarrier': ['2.5', '6', '4.5', '6', '0.1', '3', '3.7', '0', '40', '0'],
        r'characters\flood_infection\flood_infection': ['2.8', '4', '1.5', '4', '1', '3', '0', '0', '40', '0'],
        r'characters\elite\elite minor\elite minor plasma rifle': ['1.75', '2', '2.5', '2.75', '0', '3', '0', '0', '40', '20'],
        r'characters\elite\elite commander\elite commander energy sword': ['12', '15', '16', '20', '0.4', '8', '0', '1', '28', '15'],
    }
    for path, fields in retail.items():
        assert melee[path][:10] == fields, (path, melee[path], fields)
    assert not any(float(v) for p, f in melee.items() if p.startswith(r'characters\marine') for v in f[:4]), 'marines never melee'
    assert player_unit and int(player_unit[0]) >> 22 & 1, ('player.txt: the cyborg\'s shields fry infection forms', player_unit)
    print(f'Melee tags: {len(melee)} `melee` lines equal the tag bytes; the cyborg\'s unit flags {player_unit[0]} carry bit 22.')


def melee_rows(text, phase):
    """ACTOR_MELEE lines of a phase: (actor name, fields)."""
    found = []
    for line in text.splitlines():
        m = re.match(rf'ACTOR_MELEE {phase} (.+?) variant=', line)
        if m:
            found.append((m.group(1), dict(re.findall(r'(\w+)=(\S+)', line))))
    return found


def player_hits(text, name):
    """The exact amounts the DAMAGE_TRACE lines give for `name`'s hits on the player."""
    return [float(m.group(1)) for m in re.finditer(rf'DAMAGE_TRACE attacker={re.escape(name)} target=player exact=1 amount=([\d.]+) side=0 gun=0 ', text)]


if melee:
    # 13. An unarmed Flood combat form (weapon -1) carries no gun, charges inside its berserk melee range
    #     (6 for the human form: unarmed and not stalking) and strikes the unprotected player with its
    #     unit melee jpt! (20 to 30) at the key frame (15 ticks), unmodified and unrolled by the slot.
    text, log = run('melee-unarmed', 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
        (0, 'qa_combat_protect 0; qa_variant_spawn "floodcombat_human unarmed" 2 3 0'),
        (300, 'qa_actor_state armed'),
        (9000, 'qa_actor_state struck'),
        (9300, 'quit'),
    ])
    states = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('ACTOR_MELEE_QA armed ')]
    assert states and states[0]['model'] == '1' and states[0]['knife'] == '1' and states[0]['ranged'] == '0', ('melee slot, no gun', states, log)
    assert abs(float(states[0]['reach']) - (0.75 + 0.895889 - 0.465491)) < 2e-3 and states[0]['key_ms'] == str(15 * 33), (states, log)
    charges, strikes = melee_rows(text, 'charge'), melee_rows(text, 'strike_unit')
    assert charges and float(charges[0][1]['distance']) <= 6.01 and charges[0][1]['target'] == 'player', ('the charge', charges, log)
    assert strikes and all(f['target'] == 'player' and 20 <= float(f['value']) <= 30 for _, f in strikes), ('unit melee 20 to 30', strikes, log)
    hits = player_hits(text, strikes[0][0])
    assert hits and all(20 <= h <= 30 for h in hits), ('halohit carries the roll to the player', hits, log)
    print(f'Unarmed form: {len(strikes)} blow(s) of {[round(h, 1) for h in hits]} on the player.')

    # 14. An infection form touching the player's full shield deals its jpt! (0.2, 3 to 3) and is spent:
    #     no pop, no body (units.c:4430-4451).
    text, log = run('melee-fried', 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
        (0, 'qa_combat_protect 0; qa_actor_spawn 6 2 2 0'),
        (6000, 'qa_actor_count after'),
        (6300, 'quit'),
    ])
    fried = melee_rows(text, 'contact_fried')
    assert fried and float(fried[0][1]['value']) == 3.0 and fried[0][1]['target'] == 'player', ('the form strikes the shield', fried, log)
    assert player_hits(text, fried[0][0]) == [3.0], ('3 points on the shield', log)
    assert counts(text, 'after')['infection'] == 0 and 'FLOOD_POP' not in text, ('spent without a pop', log)
    print('Infection on the shield: 3 points, the form spent without a pop.')

    # 15. With the shield down the form stays on the player and deals Halo's continuous melee: ten ticks
    #     of (1 - 1/30) x 0.2 + 3/30 = 2.93 each time (units.c:11336-11428, damage.c:1374-1383).
    text, log = run('melee-attached', 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
        (0, 'qa_combat_protect 0; qa_halo_damage 75; qa_actor_spawn 6 2 2 0'),
        (3500, 'qa_actor_count after'),
        (3800, 'quit'),
    ])
    attached = melee_rows(text, 'contact_attached')
    want = 10 * ((1 - 1 / 30) * 0.2 + 3 / 30)
    assert attached and all(abs(float(f['value']) - want) < 2e-3 for _, f in attached), ('continuous melee', attached, want, log)
    assert counts(text, 'after')['infection'] == 1 and not melee_rows(text, 'contact_fried'), ('the form stays on', log)
    print(f'Infection on the bare player: {len(attached)} contact blows of {want:.3f}.')

    # 16. A carrier within its melee range (2.5: unarmed and stalking) dies of its fatal melee (unit bit 8,
    #     units.c:4209-4217): destroyed when its 40-tick melee animation ends (10020-10045), its blast
    #     (UNIT_BLAST) and its forms (FLOOD_SPAWN) follow; it never swings.
    text, log = run('melee-carrier', 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
        (0, 'qa_combat_protect 1; qa_actor_spawn 7 2 2 0'),
        (5000, 'qa_actor_count after'),
        (5300, 'quit'),
    ])
    suicides = melee_rows(text, 'suicide')
    assert suicides and float(suicides[0][1]['distance']) < 3.7, ('the carrier bursts at the player', suicides, log)
    assert melee_rows(text, 'burst') and text.find('ACTOR_MELEE suicide') < text.find('ACTOR_MELEE burst') < text.find('UNIT_BLAST'), \
        ('the burst follows the melee animation', log)
    assert counts(text, 'after')['carrier'] == 0 and 'UNIT_BLAST variant=characters\\floodcarrier\\floodcarrier' in text, log
    spawned = re.search(r'FLOOD_SPAWN .*? count=(\d+)', text)
    assert spawned and 3 <= int(spawned.group(1)) <= 6, ('3 to 6 forms released', log)
    assert not melee_rows(text, 'strike_unit') and not player_hits(text, suicides[0][0]), ('no swing', log)
    print(f'Carrier: burst at {suicides[0][1]["distance"]} world units into {spawned.group(1)} forms.')
