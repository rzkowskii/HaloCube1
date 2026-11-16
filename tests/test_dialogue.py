"""Halo's unit dialogue in place of AssaultCube's stock pain and death sounds.

Written while testing was on hold, NOT YET RUN.

Static (no game):
1. packages/halo/dialogue.txt (tools/halo_dialogue.py) is format 1 and names, for every actor
   variant whose unit has dialogue_variants, that unit; the player is characters\\cyborg\\cyborg with
   sound\\dialog\\chief\\chief (deaths only: the Chief's udlg has no pain lines); the marines carry
   their four privates as variant 0 and the two sergeants as 100 and 101; every `voc` names a sound.
2. Against the caches (HALO_SOURCE): each unit's dialogue_variants block (unit +692, 24 bytes,
   variant number at +0, udlg at +8), each udlg's vocalization references (+16 + 16 x type) and
   each sound's permutation count (first pitch range +44), longest permutation (+132) and skip
   fractions (permutation +32) match the table.
3. Every permutation of every sound is decoded (packages/audio/halo/dialogue/<sample>_<n>.wav,
   tools/import_halo_audio.py) and has its sound class in tags/classes.txt.
4. The jpt! kinds the death lines read: weapons.txt `jptkind` (rocket 4 6, the Scorpion shell 8 6,
   the energy sword's melee 10 2.5, bullets 2 0), equipment.txt (both grenade blasts 3 4, the plasma
   grenade's attached 3 4), actors.txt `unitmeleekind` (the Hunter 6 9, Elites 6 2.5, combat forms
   6 2, infection forms 6 0) and effects.txt's.

Game, a30 section 2 (qa_campaign_resume 2 2, actors not shooting, halo_dialoguetrace 1):
5. qa_dialogue_state: the table is loaded and active.
6. An Elite's shield takes a small hit: VOCALIZATION ... type=8 or its fallback (pain shield to
   pain body minor) at priority pain; a lethal hit then plays a death type (14-19) at priority death
   from the same unit; with a second Elite standing by, the death raises COMM_EVENT type=0 and the
   bystander speaks one of the friend-died lines (96, 98-105) when the event found a possibility.
7. The player's hits play nothing (the Chief has no pain lines); the player's death plays type 14
   or 15 from sound\\dialog\\chief\\chief.
8. qa_dialogue_event 9 (grenade throwing) on a Grunt plays type 115 at priority exclaim;
   qa_dialogue_scream 0 (falling) type 10 at priority scream; a second scream while the first plays
   is refused (one line per unit).
9. halo_dialogue 0: no VOCALIZATION line at all for the same hits (the stock sounds play).
"""
import mmap
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))

TABLE = root / 'packages/halo/dialogue.txt'
require(TABLE, 'run tools/halo_dialogue.py and tools/import_halo_audio.py')
import halo_dialogue  # noqa: E402

# ---- 1. The table
sounds, udlgs, units, actors, player = {}, {}, {}, {}, None
current = None
for line in TABLE.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'format':
        assert rest == '1', line
    elif key == 'sound':
        i, count, longest, tag = rest.split(' ', 3)
        sounds[int(i)] = {'count': int(count), 'longest': int(longest), 'tag': tag, 'skip': []}
    elif key == 'skip':
        i, *values = rest.split()
        sounds[int(i)]['skip'] = [float(v) for v in values]
    elif key == 'udlg':
        i, tag = rest.split(' ', 1)
        current = udlgs.setdefault(int(i), {'tag': tag, 'voc': {}})
    elif key == 'voc':
        v, s = map(int, rest.split())
        assert s in sounds, line
        current['voc'][v] = s
    elif key == 'unit':
        i, tag = rest.split(' ', 1)
        current = units.setdefault(int(i), {'tag': tag, 'variants': []})
    elif key == 'variant':
        n, u = map(int, rest.split())
        assert u in udlgs, line
        current['variants'].append((n, u))
    elif key == 'end':
        current = None
    elif key == 'actor':
        i, tag = rest.split(' ', 1)
        actors[tag] = int(i)
    elif key == 'player':
        player = int(rest)
assert player is not None and units[player]['tag'] == 'characters\\cyborg\\cyborg', ('the player unit', player)
assert [udlgs[u]['tag'] for _, u in units[player]['variants']] == ['sound\\dialog\\chief\\chief'], units[player]
chief = udlgs[units[player]['variants'][0][1]]['voc']
assert not set(chief) & {6, 7, 8, 9}, ('the Chief speaks no pain', chief)
assert {14, 15} <= set(chief), ('the Chief\'s deaths', chief)
marine = next(u for u in units.values() if u['tag'] == 'characters\\marine\\marine')
numbers = sorted(n for n, _ in marine['variants'])
assert numbers == [0, 0, 0, 0, 100, 101], ('four privates and two sergeants', marine['variants'])
known = {l.split(' ', 1)[1] for l in (root / 'packages/halo/actors.txt').read_text().splitlines() if l.startswith('path ')}
assert set(actors) <= known, ('every actor line is an actors.txt variant', set(actors) - known)
assert len(actors) >= 60, ('the actor variants with dialogue', len(actors))
for tag in ('characters\\grunt\\grunt', 'characters\\elite\\elite', 'characters\\jackal\\jackal',
            'characters\\floodcombat_human\\floodcombat_human', 'characters\\hunter\\hunter'):
    assert any(u['tag'] == tag for u in units.values()), ('a unit with dialogue', tag)
wanted = set(halo_dialogue.vocalizations())
assert all(set(u['voc']) <= wanted for u in udlgs.values()), 'only the spoken vocalizations are exported'
print(f'dialogue.txt: {len(units)} units, {len(udlgs)} udlg, {len(sounds)} sounds, {len(actors)} actor variants.')

# ---- 2. Against the caches
if (HALO_SOURCE / 'a30.map').exists():
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_weapons import CAMPAIGN  # noqa: E402
    by_tag = {s['tag']: s for s in sounds.values()}
    udlg_by_tag = {u['tag']: u for u in udlgs.values()}
    checked_units, checked_sounds = set(), set()
    for cache in CAMPAIGN:
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            t = halo.tags
            for tag in halo.directory:
                if tag.group in ('bipd', 'vehi') and tag.name not in checked_units and any(u['tag'] == tag.name for u in units.values()):
                    unit = next(u for u in units.values() if u['tag'] == tag.name)
                    found = [(n, u.name) for n, u in halo_dialogue.dialogue_variants(halo, tag)]
                    assert found == [(n, udlgs[u]['tag']) for n, u in unit['variants']], (cache, tag.name, found)
                    checked_units.add(tag.name)
                elif tag.group == 'udlg' and tag.name in udlg_by_tag:
                    for v, s in udlg_by_tag[tag.name]['voc'].items():
                        ref = halo.reference(t, tag.address + 16 + 16 * v)
                        assert ref and ref.name == sounds[s]['tag'], (cache, tag.name, v, ref)
                elif tag.group == 'snd!' and tag.name in by_tag and tag.name not in checked_sounds:
                    count, longest, skips = halo_dialogue.sound_record(halo, tag)
                    s = by_tag[tag.name]
                    assert (count, longest) == (s['count'], s['longest']), (tag.name, count, longest, s)
                    assert [round(x, 4) for x in skips] == [round(x, 4) for x in s['skip']], (tag.name, skips, s['skip'])
                    checked_sounds.add(tag.name)
    assert len(checked_sounds) == len(sounds), ('every sound checked in a cache', len(checked_sounds), len(sounds))
    print(f'The caches agree: {len(checked_units)} units, {len(udlg_by_tag)} udlg, {len(checked_sounds)} sounds.')
else:
    print(f'(HALO_SOURCE {HALO_SOURCE} missing: the cache comparison is skipped)')

# ---- 3. Decoded permutations and their classes
from import_halo_audio import dialogue_sample  # noqa: E402
classes = {l.split()[0] for l in (root / 'packages/audio/halo/tags/classes.txt').read_text().splitlines() if l and l[0] != '#'}
missing = [(s['tag'], n) for s in sounds.values() for n in range(s['count'])
           if not (root / 'packages/audio/halo' / (dialogue_sample(s['tag'], n) + '.wav')).is_file()
           or dialogue_sample(s['tag'], n) not in classes]
assert not missing, ('every permutation decoded with its class', missing[:10])
print(f'{sum(s["count"] for s in sounds.values())} dialogue permutations decoded.')

# ---- 4. The jpt! kinds


def kinds(path, opener):
    """{record path: [(opener line key, category, acceleration)]} for the `jptkind` lines of a table."""
    out, name, last = {}, None, None
    for line in path.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'path':
            name = rest
        elif key in opener:
            last = key
        elif key in ('jptkind', 'unitmeleekind'):
            c, a = rest.split()
            out.setdefault(name, []).append((last if key == 'jptkind' else 'unitmelee', int(c), float(a)))
    return out


w = kinds(root / 'packages/halo/weapons.txt', ('melee', 'projectile'))
assert ('projectile', 4, 6.0) in w['weapons\\rocket launcher\\rocket launcher'], w.get('weapons\\rocket launcher\\rocket launcher')
assert ('melee', 10, 2.5) in w['weapons\\energy sword\\energy sword'], w.get('weapons\\energy sword\\energy sword')
assert ('projectile', 2, 0.0) in w['weapons\\pistol\\pistol'], w.get('weapons\\pistol\\pistol')
assert ('projectile', 8, 6.0) in w['vehicles\\scorpion\\scorpion cannon'], w.get('vehicles\\scorpion\\scorpion cannon')
equipment = [l for l in (root / 'packages/halo/equipment.txt').read_text().splitlines() if l.startswith('jptkind ')]
assert equipment.count('jptkind 3 4') == 3, ('both grenade blasts and the plasma grenade\'s attached damage', equipment)
a = kinds(root / 'packages/halo/actors.txt', ())
hunters = [v for k, v in a.items() if k.startswith('characters\\hunter\\')]
assert hunters and all(v == [('unitmelee', 6, 9.0)] for v in hunters), hunters
assert all(v == [('unitmelee', 6, 2.5)] for k, v in a.items() if k.startswith('characters\\elite\\')), \
    [(k, v) for k, v in a.items() if k.startswith('characters\\elite\\')]
print('The jpt! kinds are in weapons.txt, equipment.txt and actors.txt.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, schedule, timeout=150):
    profile = scratch(f'dialogue-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_dialoguetrace 1; qa_campaign_resume 2 2; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def fields(text_line):
    return dict(re.findall(r'(\w+)=(\S+)', text_line))


def lines(text, prefix):
    return [fields(l) for l in text.splitlines() if l.startswith(prefix + ' ')]


def spawned(text):
    return [l.split()[1] for l in text.splitlines() if l.startswith('FACTION_SPAWN ')]


# ---- 5 to 7. Pain, death, the death event and the player
text, log = run('pain', [
    (100, 'botsshoot 0; qa_dialogue_state start'),
    (300, 'qa_variant_spawn "elite minor plasma rifle" 0 6 0; qa_variant_spawn "elite minor plasma rifle" 0 7 2'),
    (1200, 'qa_damage_actor 2 0 shield'), (2500, 'qa_dialogue_state shield'),
    (5000, 'qa_damage_actor 900 0 kill'), (7500, 'qa_dialogue_state killed'),
    (7600, 'qa_damage_vitals 75 75; qa_damage_hit 10 0 0 0'), (8600, 'qa_damage_vitals 75 0; qa_damage_hit 10 0 0 0'),
    (9600, 'qa_dialogue_state player'), (9700, 'qa_damage_vitals 5 0; qa_damage_hit 50 0 0 0'), (11500, 'quit'),
])
state = next(s for s in lines(text, 'DIALOGUE_STATE') if s['label'] == 'start')
assert state['loaded'] == '1' and state['active'] == '1', ('the table loads', state, log)
elites = spawned(text)
assert len(elites) == 2, ('two Elites', elites, log)
said = lines(text, 'VOCALIZATION')
first = [v for v in said if v['unit'] in elites and v['priority'] in ('pain', 'involuntary')]
assert first and int(first[0]['type']) in (6, 7, 8), ('the shield hit\'s pain', said, log)
deaths = [v for v in said if v['priority'] == 'death' and v['unit'] in elites]
assert deaths and 14 <= int(deaths[0]['type']) <= 19, ('the Elite\'s death line', said, log)
victim = deaths[0]['unit']
events = [e for e in lines(text, 'COMM_EVENT') if e['type'] == '0' and e['subject'] == victim]
assert events, ('the death raises the death event', log)
if int(events[0]['possibilities']) > 0:
    friend = [v for v in said if v['unit'] in elites and v['unit'] != victim and int(v['type']) in (96, 98, 99, 100, 101, 102, 103, 104, 105)]
    assert friend, ('the bystander\'s friend-died line', said, log)
mine = [v for v in said if v['unit'] == 'player']
assert all(v['priority'] == 'death' for v in mine), ('the Chief speaks no pain', mine, log)
assert mine and mine[-1]['dialogue'] == 'sound\\dialog\\chief\\chief' and int(mine[-1]['type']) in (14, 15, 16, 18, 19), \
    ('the player\'s death line', mine, log)
print(f'a30: the Elite {victim} spoke {deaths[0]["type"]} at death, its friend answered, the player died with {mine[-1]["type"]}.')

# ---- 8. A combat call and a scream; one line per unit
text, log = run('calls', [
    (100, 'botsshoot 0; qa_variant_spawn "grunt minor plasma pistol" 0 6 0'),
    (800, 'qa_dialogue_event 9 0 throw'), (4000, 'qa_dialogue_scream 0 fear; qa_dialogue_scream 0 again'),
    (4100, 'qa_dialogue_state screaming'), (6000, 'quit'),
])
grunt = spawned(text)[0]
said = [v for v in lines(text, 'VOCALIZATION') if v['unit'] == grunt]
assert any(v['type'] == '115' and v['priority'] == 'exclaim' for v in said), ('the grenade call', said, log)
screams = [v for v in said if v['priority'] == 'scream']
assert len(screams) == 1 and screams[0]['type'] == '10', ('one scream of fear, the second refused', said, log)
print('a30: the Grunt called its grenade and screamed once.')

# ---- 9. halo_dialogue 0 keeps the stock sounds
text, log = run('stock', [
    (100, 'halo_dialogue 0; botsshoot 0; qa_variant_spawn "elite minor plasma rifle" 0 6 0'),
    (800, 'qa_damage_actor 2 0 shield'), (2000, 'qa_damage_actor 900 0 kill'), (3500, 'qa_dialogue_state off'), (4000, 'quit'),
])
assert not lines(text, 'VOCALIZATION'), ('halo_dialogue 0 speaks nothing', log)
assert next(s for s in lines(text, 'DIALOGUE_STATE') if s['label'] == 'off')['active'] == '0', log
print('halo_dialogue 0: no Halo line, the stock sounds stay.')
