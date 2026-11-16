"""Kill credit: kills nobody owns credit nobody (clientgame.cpp dodamage and dokill, halo.cpp haloownerless).

Written while testing was on hold, NOT YET RUN.

Falls, the scripts' kills and damage (ai_kill, ai_erase_all, damage_object, damage_new, object
depletion), ownerless blasts (a vehicle's death explosion, effect_new) and the forced reverts
(game_lost, game_revert, the escape deadline, a recovery with no floor) are dealt in the player's
name inside an ownerless scope, because the stock hit and kill paths need an attacker. Halo credits
such a kill to nobody (owner_player_index NONE, ../Halo3DS objects/damage.c:1081-1085) and its
campaign shows no kill message at all (game/game_engine.c:4019). So, on a30 section 2 (qa_campaign_resume
2 2, actors not shooting, halo_damagetrace 1): an lz_search actor dropped 7 units and the squad's
ai_kill leave the player's frags, team kills and last credited hit (`lasthit`, which the hit sound
goes by) unchanged, log `<name> died` (console only) and no `you <verb> <name>` HUD message, and
each DEATH_TRACE reads killer=- ownerless=1; the player killed by a 7-unit fall and then by
game_revert reads `you died` each time, never `you suicided`, and loses no frag.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')

schedule = [
    (100, 'botsshoot 0; qa_actor_place lz_search; qa_kill_credit start'),
    (600, 'qa_fall_actor 7 fell'), (3600, 'qa_kill_credit fall'),
    (3700, 'echo AI_KILL_MARK; qa_hs_ai ai_kill lz_search 0'), (4200, 'qa_kill_credit scripted'),
    (4300, 'echo PLAYER_FALL_MARK; qa_damage_vitals 75 75; qa_fall 7 player'), (7300, 'qa_kill_credit player-fall'),
    (11400, 'qa_kill_credit pre-revert'), (11500, 'echo REVERT_MARK; qa_hs_cmd game_revert ""'), (12500, 'qa_kill_credit revert'),
    (13000, 'quit'),
]
profile = scratch('kill-credit-')
log = profile / 'run.log'
body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                    f'-ehalo_damagetrace 1; qa_campaign_resume 2 2; qa_after_gameplay [{body}]'],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=150, check=True)
# Colour codes (\f2 and the like) stripped.
text = re.sub(r'\f.', '', log.read_text(errors='replace'))
lines = text.splitlines()


def fields(text_line):
    result = dict(re.findall(r'(\w+)=(\S+)', text_line))
    # Actor names have a space ("Grunt 3"): DEATH_TRACE's victim= and FALL_QA's name= in full.
    for key, until in (('victim', 'killer'), ('name', 'kind')):
        m = re.search(rf'\b{key}=(.+?) {until}=', text_line)
        if m:
            result[key] = m.group(1)
    return result


credit = {l.split()[1]: fields(l) for l in lines if l.startswith('KILL_CREDIT ')}
assert all(k in credit for k in ('start', 'fall', 'scripted', 'player-fall', 'pre-revert', 'revert')), ('every probe ran', credit, log)
start = credit['start']


def mark(name):
    return next(i for i, l in enumerate(lines) if l.strip().endswith(name))


def unchanged(label):
    c = credit[label]
    assert (c['frags'], c['tks'], c['lasthit']) == (start['frags'], start['tks'], start['lasthit']), \
        (f'{label}: no frag, team kill or credited hit', start, c, log)


deaths = [fields(l) for l in lines if l.startswith('DEATH_TRACE ')]
assert not [d for d in deaths if d.get('killer') == 'player'], ('no ownerless kill is the player\'s', deaths, log)


def neutral(victim):
    # colorname may add " (cn)" to a duplicate name.
    assert any(re.search(rf'\b{re.escape(victim)}( \(\d+\))? died$', l.strip()) for l in lines), (f'{victim}\'s death reads neutral', log)
    assert not [l for l in lines if re.search(rf'\byou \w+ {re.escape(victim)}\b', l)], (f'no "you ..." message for {victim}', log)


# 1. An actor's fatal fall.
fell = next((fields(l) for l in lines if l.startswith('FALL_QA fell actor ')), None)
assert fell, ('the fall lift ran', log)
fall_death = [d for d in deaths if d.get('victim') == fell['name']]
if fall_death:
    assert fall_death[0]['ownerless'] == '1' and fall_death[0]['killer'] == '-', fall_death
    neutral(fell['name'])
unchanged('fall')
# 2. ai_kill on the squad.
killed = [fields(l) for l in lines[mark('AI_KILL_MARK'):mark('PLAYER_FALL_MARK')] if l.startswith('DEATH_TRACE ')]
assert killed, ('ai_kill killed lz_search', log)
assert all(d['ownerless'] == '1' and d['killer'] == '-' for d in killed), killed
for d in killed:
    neutral(d['victim'])
unchanged('scripted')
# 3. The player's fatal fall: "you died", no suicide, no frag lost.
fall_lines = lines[mark('PLAYER_FALL_MARK'):mark('REVERT_MARK')]
assert any(l.strip().endswith('you died') for l in fall_lines), ('the player\'s fall death reads "you died"', log)
assert 'you suicided' not in text, ('no ownerless death of the player reads as a suicide', log)
unchanged('player-fall')
assert int(credit['player-fall']['deaths']) == int(start['deaths']) + 1, (credit['player-fall'], log)
# 4. game_revert: the same when the player was alive again by then.
if credit['pre-revert']['alive'] == '1':
    assert any(l.strip().endswith('you died') for l in lines[mark('REVERT_MARK'):]), ('game_revert reads "you died"', log)
    assert int(credit['revert']['deaths']) == int(credit['pre-revert']['deaths']) + 1, (credit, log)
unchanged('revert')
print(f'Kill credit: {len(killed)} ai_kill deaths and {len(fall_death)} fatal actor fall left frags {start["frags"]}; '
      f'the player\'s fall{" and game_revert" if credit["pre-revert"]["alive"] == "1" else ""} read "you died".')
