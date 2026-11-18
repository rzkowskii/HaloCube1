"""Halo's communication events the brain and the damage path raise (halo_dialogue.cpp, halo_ai.cpp).

Written while testing was on hold, NOT YET RUN.

Static (no game):
1. Every type the engine raises (halo_dialogue.h HALO_COMM_*) has its rows in halo_dialogue.cpp's
   usages[], and those rows equal, in order and field for field, the port's global_dialogue_table
   rows of that type (../Halo3DS source/ai/ai_communication.c:1215-1484; skipped without the port).
2. tools/halo_dialogue.py speaks every vocalization those rows name; dialogue.txt gives the Grunt's and
   Jackal's udlg dive (180) and grenade sighted (117), the Elite's berserk, melee and dive (178-180),
   the marines' damaged friend player (22), hurt friend player (31) and the five shooting lines
   (149-153), and the Flood's leap (182).

Game (halo_dialoguetrace 1):
3. Friendly fire (a30 section 2, a marine 6 units ahead, actors not shooting): the player's small hit
   on it logs COMM_DAMAGE unit=<marine> cause=player hostility=2 delayed=0 and COMM_EVENT type=3
   hostility=2; with a possibility the marine speaks hurt friend player or hurt friend (31, 29) at
   priority talk. 45 ticks on, the delayed call (delayed=1) raises no second damage event.
4. Hurt: an Elite's hit of 40 on the player's full shield (0.53 of it) logs COMM_DAMAGE unit=player
   hostility=3 fraction>=0.3 (Halo's total_damage, in points: 40 here) and COMM_EVENT type=2 hostility=3 (the Elite's damaged enemy taunt, 23,
   when it has a possibility); the delayed call follows with delayed=1 on the peak (>= 0.5).
5. The new rows' lines: qa_dialogue_event 42, 43 and 44 on the Elite play 178, 179 and 180 at
   priority exclaim.
6. A hit and a silent INT_MAX kill in the same millisecond (qa_dialogue_hitkill): the kill takes no
   report from the hit, so no death line and no death event for that actor.
7. The hooks on the preview (halo_dangeravoid 2, as tests/test_actor_ai.py's danger runs): a frag by a
   Grunt raises grenade sighted at its notice (COMM_EVENT type=11 hostility=3) and dive with the dive
   (type=44; the Grunt's 180 when it has a possibility); a plasma grenade stuck to an Elite makes it
   berserk and it calls once (type=42, one event); a marine and a Grunt that fight raise a shooting
   event (types 26-30) at a burst's start.
"""
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


def rows(text):
    """Every {...} row of numbers in a C table body, as tuples (floats with their f dropped)."""
    out = []
    for body in re.findall(r'\{([^{}]*)\}', text):
        values = [v.strip().rstrip('f') for v in body.split(',') if v.strip()]
        if values and all(re.fullmatch(r'-?\d+(\.\d+)?', v) for v in values):
            out.append(tuple(float(v) for v in values))
    return out


# ---- 1. The rows against the port
source = (root / 'source/src/halo_dialogue.cpp').read_text()
engine = rows(source[source.index('const Usage usages[] = {'):source.index('constexpr int usageCount')])
header = (root / 'source/src/halo_dialogue.h').read_text()
raised = {int(v) for v in re.findall(r'HALO_COMM_\w+ = (\d+)', header)}
types = {int(r[0]) for r in engine}
assert types == raised, ('every raised type has its rows, no row for a type never raised', sorted(types ^ raised))
port = root.parent / 'Halo3DS/source/ai/ai_communication.c'
if port.exists():
    text = port.read_text(errors='replace')
    start = text.index('global_dialogue_table[NUMBER_OF_DIALOGUE_USAGES] =')
    table = rows(text[start:text.index('};', start)])
    for t in sorted(types):
        mine = [r for r in engine if int(r[0]) == t]
        theirs = [r for r in table if int(r[0]) == t]
        assert mine == theirs, ('the rows of type', t, mine, theirs)
    print(f'usages[]: {len(engine)} rows of {len(types)} types equal the port\'s.')
else:
    print(f'({port} missing: the row comparison is skipped)')

# ---- 2. The vocalizations exported
wanted = set(halo_dialogue.vocalizations())
assert {int(r[2]) for r in engine} <= wanted, ('every row\'s vocalization is exported', {int(r[2]) for r in engine} - wanted)
udlgs, current = {}, None
for line in TABLE.read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'udlg':
        current = udlgs.setdefault(rest.split(' ', 1)[1], set())
    elif key == 'voc':
        current.add(int(rest.split()[0]))
    elif key == 'end':
        current = None
expect = {'sound\\dialog\\grunt\\conditional\\grunt': {117, 180}, 'sound\\dialog\\jackal\\jackal': {117, 180},
          'sound\\dialog\\elite\\conditional\\elite': {178, 179, 180}, 'sound\\dialog\\flood\\flood': {182},
          'sound\\dialog\\marines\\aussie\\conditional\\aussie': {22, 31, 149, 150, 151, 152, 153}}
for tag, need in expect.items():
    assert tag in udlgs and need <= udlgs[tag], (tag, need - udlgs.get(tag, set()))
print('dialogue.txt carries the dive, berserk, melee, leap, grenade sighted, damage and shooting lines.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=150):
    profile = scratch(f'dialogue-events-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_dialoguetrace 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def lines(text, prefix):
    # Every field of every `prefix` line; a missing one reads '' (a DISABLED event has no subject).
    out = []
    for l in text.splitlines():
        if l.startswith(prefix + ' '):
            row = dict(re.findall(r'(\w+)=(\S+)', l))
            out.append({k: row.get(k, '') for k in ('type', 'subject', 'cause', 'hostility', 'possibilities', 'unit', 'priority',
                                                    'fraction', 'delayed', 'dialogue')} | row)
    return out


def spawned(text):
    return [l.split()[1] for l in text.splitlines() if l.startswith('FACTION_SPAWN ')]


# ---- 3. Friendly fire
text, log = run('friendly', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_variant_spawn "marine assault rifle" 1 6 0'),
    (3000, 'qa_dialogue_hit 5 0 ff'), (6000, 'quit'),
])
marine = spawned(text)[0]
damage = [d for d in lines(text, 'COMM_DAMAGE') if d['unit'] == marine]
assert damage and damage[0]['cause'] == 'player' and damage[0]['hostility'] == '2' and damage[0]['delayed'] == '0', (damage, log)
events = [e for e in lines(text, 'COMM_EVENT') if e['type'] == '3' and e['subject'] == marine]
assert len(events) == 1 and events[0]['hostility'] == '2', ('one damage event, none from the delayed call', events, log)
assert any(d['delayed'] == '1' for d in damage), ('the delayed call 45 ticks on', damage, log)
if int(events[0]['possibilities']) > 0:
    said = [v for v in lines(text, 'VOCALIZATION') if v['unit'] == marine and v['priority'] == 'talk']
    assert said and said[0]['type'] in ('29', '31'), ('the marine\'s hurt friend line (its pain line aside)', said, log)
print(f'Friendly fire: {marine} complained ({events[0]["possibilities"]} possibilities).')

# ---- 4 to 6. Hurt, the new lines, the same-millisecond kill
text, log = run('hurt', 'qa_campaign_resume 2 2', [
    (100, 'botsshoot 0; qa_variant_spawn "elite minor plasma rifle" 0 6 0'),
    (1500, 'qa_damage_vitals 75 75; qa_dialogue_hit 40 1 hurt'),
    (5000, 'qa_dialogue_event 42 3 berserk'), (9000, 'qa_dialogue_event 43 3 melee'), (13000, 'qa_dialogue_event 44 0 dive'),
    (17000, 'qa_dialogue_hitkill 2 hitkill'), (19000, 'quit'),
])
elite = spawned(text)[0]
hurt = [d for d in lines(text, 'COMM_DAMAGE') if d['unit'] == 'player']
assert hurt and hurt[0]['cause'] == elite and hurt[0]['hostility'] == '3' and float(hurt[0]['fraction']) >= 0.3, (hurt, log)
assert any(d['delayed'] == '1' and float(d['fraction']) >= 0.5 for d in hurt), ('the delayed call on the peak', hurt, log)
events = [e for e in lines(text, 'COMM_EVENT') if e['type'] == '2' and e['subject'] == 'player']
assert events and events[0]['hostility'] == '3', ('the hurt event', events, log)
said = [v for v in lines(text, 'VOCALIZATION') if v['unit'] == elite]
if int(events[0]['possibilities']) > 0:
    assert any(v['type'] == '23' for v in said), ('the Elite\'s taunt', said, log)
for kind, voc in (('42', '178'), ('43', '179'), ('44', '180')):
    assert any(v['type'] == voc and v['priority'] == 'exclaim' for v in said), ('the line of event', kind, said, log)
killed = re.search(r'DIALOGUE_HITKILL label=hitkill victim=(\S+) alive=0', text)
assert killed, ('the hit and the kill', log)
victim = killed.group(1)
assert not [v for v in lines(text, 'VOCALIZATION') if v['unit'] == victim and v['priority'] == 'death'], ('a silent kill', log)
assert not [e for e in lines(text, 'COMM_EVENT') if e['type'] == '0' and e['subject'] == victim], ('no death event', log)
print(f'Hurt: the Elite\'s hit raised the hurt event; berserk, melee and dive spoke; the same-millisecond kill was silent.')

# ---- 7. The hooks
text, log = run('dive', 'halo_damagetrace 1; halo_difficulty 1; halo_dangeravoid 2; halo_preview', [
    (0, 'qa_combat_protect 1; botsshoot 0; qa_variant_spawn "grunt minor plasma pistol" -1 6 0'),
    (1500, 'qa_grenade_place 0 -1 0.8 0 -1'), (4000, 'quit'),
])
grunt = spawned(text)[0]
events = [e for e in lines(text, 'COMM_EVENT') if e['subject'] == grunt]
sighted = [e for e in events if e['type'] == '11']
dived = [e for e in events if e['type'] == '44']
assert sighted and sighted[0]['hostility'] == '3', ('grenade sighted at the notice', events, log)
assert dived, ('the dive\'s call', events, log)
if int(dived[0]['possibilities']) > 0:
    assert any(v['unit'] == grunt and v['type'] == '180' for v in lines(text, 'VOCALIZATION')), ('the Grunt\'s dive scream', log)
text, log = run('berserk', 'halo_damagetrace 1; halo_difficulty 1; halo_dangeravoid 2; halo_preview', [
    (0, 'qa_combat_protect 1; botsshoot 0; qa_variant_spawn "elite minor plasma rifle" -1 3 0'),
    (1500, 'qa_grenade_place 1 -1 0 1 -1'), (3800, 'quit'),
])
elite = spawned(text)[0]
berserk = [e for e in lines(text, 'COMM_EVENT') if e['subject'] == elite and e['type'] == '42']
assert len(berserk) == 1, ('one berserk call per berserk', berserk, log)
text, log = run('shooting', 'halo_difficulty 1; halo_preview', [
    (0, 'qa_combat_protect 1; qa_variant_spawn "marine assault rifle" 1 4 0; qa_variant_spawn "grunt minor plasma pistol" 0 12 0'),
    (12000, 'quit'),
])
fighters = set(spawned(text))
shooting = [e for e in lines(text, 'COMM_EVENT') if e['subject'] in fighters and e['type'] in ('26', '27', '28', '29', '30')]
assert shooting, ('a burst raises a shooting event', log)
print(f'Hooks: grenade sighted, dive, berserk and {len(shooting)} shooting events.')
