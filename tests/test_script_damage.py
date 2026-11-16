"""Script damage: damage_object and damage_new deal the jpt! they name.

Written while testing was on hold, NOT YET RUN.

Static (no game): effects.txt's optional `jpt` records (tools/halo_effects.py) are the six jpt!s the
ten scenarios' scripts name (HaloScript type 26 constants), with the tags' values: c40's
pulsegenerator 499 / 500-500, damage flags 0x40 (only hurts shields), 0.5 on the cyborg shield (22);
d40's oven heat 25 / 30-30, category 7 (flame), 0.5 on the shield; a30's shock explosion 1.5-2.5
units, 10 / 30-50; a10's zapper 0 damage; the guaranteed explosion of doom 500 and plummet 1000
(category 1). They follow every effect's `end`, so an older game (no effect open) skips them. With
the caches, each scenario's damage constants are exactly those records.

Game, a30 section 2 (qa_campaign_resume 2 2, actors not shooting, halo_damagetrace 1;
qa_hs_damage passes a jpt! path with spaces, `/` for `\\`):
1. pulsegenerator on the player at 10 health and a full shield: shield 0, health still 10, one
   DAMAGE_JPTFLAGS only_shields=1, a SCRIPT_DAMAGE line of 500.
2. oven heat at 75/75: the shield takes 15 (30 x 0.5; shield 0.8), the health none.
3. shock explosion at 75/75: the shield takes 30 to 50 (0.333 to 0.6), the health none.
4. halo_scriptjpt 0 restores the old rule: pulsegenerator deals the flat 40 (shield 0.467 at 10
   health).
5. pulsegenerator on a placed lz_search actor: its DAMAGE_TRACE takes no health.
6. object_cannot_take_damage: the guaranteed explosion of doom (500, the jpt!) leaves the protected
   player whole (Halo's protection holds; the old INT_MAX kill bypassed it); unprotected, the
   guaranteed plummet (1000) kills.
Game, a10 section 1 (qa_campaign_resume 1 1): the player teleported to tutorial_zapper_flag and
given back the shield tutorial_setup takes away (unit_set_maximum_vitality player 100 75), the
zapper (damage_new, 0 damage) leaves 75/75 and logs no DAMAGE_TRACE on the player (the old rule
dealt 40 within 4 units); the shock explosion by damage_new at the same flag is reported (its blast
needs the flag's line to the player's middle clear, so it is not asserted).
"""
import mmap
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))

EFFECTS = root / 'packages/halo/effects.txt'
require(EFFECTS, 'run python3 tools/halo_effects.py with the expanded caches')

PULSE = r'effects\damage effects\pulsegenerator'
OVEN = r'effects\damage effects\oven heat'
SHOCK = r'effects\damage effects\shock explosion'
ZAPPER = r'levels\a10\devices\shield charge\zapper'
DOOM = r'effects\damage effects\guaranteed explosion of doom'
PLUMMET = r'effects\damage effects\guaranteed plummet to untimely death'

# ---- Tables
lines = EFFECTS.read_text().splitlines()
records, block, last_effect, first_jpt = {}, None, -1, None
for number, line in enumerate(lines):
    key, _, rest = line.partition(' ')
    if key == 'effect':
        last_effect = number
        block = None
    elif key == 'jpt':
        first_jpt = number if first_jpt is None else first_jpt
        block = {}
    elif block is not None and key == 'path':
        records[rest] = block
    elif block is not None and key == 'end':
        block = None
    elif block is not None:
        assert key in ('jptdamage', 'materials'), ('unexpected line in a jpt record', line)
        block[key] = [float(v) for v in rest.split()]
assert first_jpt is not None and first_jpt > last_effect, ('the jpt records follow every effect', first_jpt, last_effect)
expected = {
    PULSE: [0, 0, 499, 500, 500, 0, 0, 0, 64, 0],
    OVEN: [0, 0, 25, 30, 30, 0, 7, 0, 0, 0],
    SHOCK: [1.5, 2.5, 10, 30, 50, 0, 4, 0, 0, 0],
    ZAPPER: [1, 1, 0, 0, 0, 0, 10, 0, 0, 0],
    DOOM: [1.5, 2.5, 500, 500, 500, 0, 3, 0, 32, 0],
    PLUMMET: [1.5, 2.5, 1000, 1000, 1000, 0, 1, 0, 0, 0],
}
assert set(records) == set(expected), ('the scripts\' jpt!s', sorted(records))
for path, values in expected.items():
    assert records[path]['jptdamage'] == values, (path, records[path]['jptdamage'], values)
    assert len(records[path].get('materials', [])) == 33, (path, 'materials')
assert records[PULSE]['materials'][22] == .5 and records[PULSE]['materials'][21] == 1, records[PULSE]['materials']
assert records[OVEN]['materials'][22] == .5 and records[OVEN]['materials'][21] == 1, records[OVEN]['materials']
print(f'effects.txt: {len(records)} script jpt! records after the effects (pulse generator only hurts shields, the zapper 0).')

if (HALO_SOURCE / 'a30.map').exists():
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_effects import jpt_record, script_damage  # noqa: E402
    from halo_weapons import CAMPAIGN, number as fmt  # noqa: E402
    named = {}
    for cache in CAMPAIGN:
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            wanted = script_damage(halo)
            for tag in halo.directory:
                if tag.group == 'jpt!' and tag.name in wanted:
                    d = jpt_record(halo, tag)['damage']
                    got = [float(fmt(v)) for v in (d.inner, d.outer, d.lower, d.upper_min, d.upper_max)] + \
                          [d.side_effect, d.category, float(fmt(d.core)), d.flags, jpt_record(halo, tag)['effect_flags']]
                    assert got == records[tag.name]['jptdamage'], (cache, tag.name, got)
                    named.setdefault(tag.name, set()).add(cache)
    assert set(named) == set(records), ('every scenario\'s damage constants', named)
    assert named[PULSE] == {'c40'} and named[OVEN] == {'d40'} and named[ZAPPER] == {'a10'}, named
    print('The records are every scenario\'s damage constants, equal to the caches\' tags.')
else:
    print(f'(HALO_SOURCE {HALO_SOURCE} missing: the cache comparison is skipped)')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/halo/player.txt', 'run tools/halo_actors.py (the cyborg shield modifier)')


def run(name, start, schedule, timeout=150):
    profile = scratch(f'script-damage-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_damagetrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def fields(text_line):
    return dict(re.findall(r'(\w+)=(\S+)', text_line))


def jpt(path):
    return '"' + path.replace('\\', '/') + '"'


def vitals(text):
    return [fields(l) for l in text.splitlines() if l.startswith('QA_VITALS ')]


text, log = run('a30', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0'),
    (300, f'qa_damage_vitals 10 75; qa_hs_damage damage_object {jpt(PULSE)} player; qa_player_vitals'),
    (600, f'qa_damage_vitals 75 75; qa_hs_damage damage_object {jpt(OVEN)} player; qa_player_vitals'),
    (900, f'qa_damage_vitals 75 75; qa_hs_damage damage_object {jpt(SHOCK)} player; qa_player_vitals'),
    (1200, f'halo_scriptjpt 0; qa_damage_vitals 10 75; qa_hs_damage damage_object {jpt(PULSE)} player; qa_player_vitals; '
           'halo_scriptjpt 1'),
    (1500, 'qa_actor_place lz_search'),
    (2000, f'echo ACTOR_PULSE; qa_hs_damage damage_object {jpt(PULSE)} unit:lz_search:0'),
    (2300, f'qa_damage_vitals 75 75; qa_hs_cmd object_cannot_take_damage "player"; '
           f'qa_hs_damage damage_object {jpt(DOOM)} player; qa_player_vitals'),
    (2600, f'qa_hs_cmd object_can_take_damage "player"; qa_damage_vitals 75 75; '
           f'qa_hs_damage damage_object {jpt(PLUMMET)} player; qa_player_vitals'),
    (3000, 'quit'),
])
v = vitals(text)
assert len(v) >= 6, ('six vitals probes', v, log)
pulse, oven, shock, old, protected, plummet = v[:6]
# 1. The pulse generator: shield gone, health untouched.
assert float(pulse['shield']) == 0 and pulse['health'] == '10', ('pulse generator: only the shield', pulse, log)
assert 'DAMAGE_JPTFLAGS target=player only_shields=1' in text, log
script = [fields(l) for l in text.splitlines() if l.startswith('SCRIPT_DAMAGE ')]
assert any(float(s.get('amount', 0)) == 500 and s.get('flags') == '0x40' for s in script), ('SCRIPT_DAMAGE 500 0x40', script, log)
# 2. Oven heat: 30 at 0.5 on the shield.
assert abs(float(oven['shield']) - .8) < .01 and oven['health'] == '75', ('oven heat: 15 on the shield', oven, log)
# 3. Shock explosion: 30 to 50 at 1 on the shield.
assert .32 <= float(shock['shield']) <= .61 and shock['health'] == '75', ('shock explosion: 30 to 50', shock, log)
# 4. halo_scriptjpt 0: the flat 40 of the old rule.
assert abs(float(old['shield']) - (35 / 75)) < .01 and old['health'] == '10', ('halo_scriptjpt 0: the old 40', old, log)
# 5. The actor: no health taken.
after = text.split('ACTOR_PULSE', 1)[-1].splitlines()
actor_traces = [fields(l) for l in after if l.startswith('DAMAGE_TRACE ') and 'target=player' not in l][:1]
assert actor_traces and actor_traces[0]['health'] == '0', ('pulse generator on an actor takes no health', actor_traces, log)
# 6. Protection holds against a guaranteed kill by its jpt!; unprotected, the plummet kills.
assert protected['protected'] == '1' and protected['health'] == '75', ('a protected player survives the doom jpt!', protected, log)
assert int(plummet['health']) <= 0, ('the guaranteed plummet kills', plummet, log)
print(f'a30: pulse generator shield {pulse["shield"]} health {pulse["health"]}, oven heat shield {oven["shield"]}, shock '
      f'explosion shield {shock["shield"]}, halo_scriptjpt 0 shield {old["shield"]}; protected doom survived, plummet killed.')

text, log = run('a10', 'qa_campaign_resume 1 1', [
    (100, 'botsshoot 0; qa_hs_cmd object_can_take_damage "player"; '
          'qa_hs_cmd object_teleport "player flag:tutorial_zapper_flag"'),
    # tutorial_setup gives the player no shield (unit_set_maximum_vitality 1 0) until the shield
    # tutorial: restore one in the same step, so qa_damage_vitals has a shield to set.
    (500, f'qa_hs_cmd unit_set_maximum_vitality "player 100 75"; qa_damage_vitals 75 75; echo ZAPPER; '
          f'qa_hs_damage damage_new {jpt(ZAPPER)} flag:tutorial_zapper_flag; qa_player_vitals; echo ZAPPER_END'),
    (800, f'qa_hs_cmd unit_set_maximum_vitality "player 100 75"; qa_damage_vitals 75 75; '
          f'qa_hs_damage damage_new {jpt(SHOCK)} flag:tutorial_zapper_flag; qa_player_vitals'),
    (1100, 'quit'),
])
v = vitals(text)
assert len(v) >= 2, ('two vitals probes', v, log)
zap = text.split('ZAPPER', 1)[-1].split('ZAPPER_END', 1)[0]
assert not [l for l in zap.splitlines() if l.startswith('DAMAGE_TRACE ') and 'target=player ' in l], ('the zapper deals nothing', zap, log)
assert v[0]['health'] == '75' and float(v[0]['shield']) > .99, ('the zapper leaves the player whole', v[0], log)
assert 'SCRIPT_DAMAGE ' + ZAPPER.replace('\\', '/') in text, ('damage_new logs the zapper', log)
print(f'a10: the zapper left {v[0]["health"]} health and shield {v[0]["shield"]}; the shock explosion by damage_new at the '
      f'flag left shield {v[1]["shield"]} (not asserted: the blast needs the line of sight).')
