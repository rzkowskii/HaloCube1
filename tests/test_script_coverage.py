"""Script commands that gate sleep_until are bound.

Static: every call inside a sleep_until, if, cond or while condition in the ten
retail missions has a handler in source/src/halo_script.cpp or halo_script_ai.inc (or is a script of
that mission, an opcode or one of the few names other work owns). Needs the
expanded Xbox maps in HALO_SOURCE.

Runtime (QA client): game_safe_to_save and the tutorial polls answer; a10's
tutorial_looking chain, which waits on look input, finishes on its own; and the
d40 escape countdown follows the engine's deadline, so timer_begin does not wake
endgame_cinematics until the deadline passes.
"""
import json
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

# Arithmetic, comparison and boolean opcodes are not named handlers.
OPCODES = {'!=', '*', '+', '-', '/', '<', '<=', '=', '>', '>=', 'and', 'or', 'if', 'cond', 'while', 'sleep_until'}
# Object values (bound as player0 and player1 by the AI and unit command work).
PENDING = {'player0', 'player1'}

source = (root / 'source/src/halo_script.cpp').read_text() + (root / 'source/src/halo_script_ai.inc').read_text()
literals = set(re.findall(r'"([A-Za-z_0-9]+)"', source))
prefixes = re.findall(r'strncmp\(name, "(\w+)"', source)

require(HALO_SOURCE / 'a10.map', 'set HALO_SOURCE to the expanded Xbox maps')
calls = json.loads(subprocess.run([sys.executable, str(root / 'tests/tools/hs_conditions.py')],
                                  capture_output=True, text=True, check=True).stdout)
missing = {}
for cache, (used, scripts) in calls.items():
    for name in used:
        if name in OPCODES or name in PENDING or name in scripts or name in literals or any(name.startswith(p) for p in prefixes):
            continue
        missing.setdefault(name, []).append(cache)
assert not missing, ('conditions call unbound commands', missing)
print(f'{sum(len(v[0]) for v in calls.values())} condition calls across {len(calls)} missions are all bound.')

# Every ai_, unit_, units_, object_, objects_, vehicle_ and damage_ command any script calls, not only
# those in conditions, has a handler in the engine (halo_script.cpp or halo_script_ai.inc).
everything = json.loads(subprocess.run([sys.executable, str(root / 'tests/tools/hs_conditions.py'), '--all'],
                                       capture_output=True, text=True, check=True).stdout)
family = re.compile(r'^(ai_|unit_|units_|object_|objects_|vehicle_|damage_)')
unbound = {}
for cache, (used, scripts) in everything.items():
    for name in used:
        if family.match(name) and name not in scripts and name not in literals and not any(name.startswith(p) for p in prefixes):
            unbound.setdefault(name, []).append(cache)
assert not unbound, ('AI, unit and object commands without a handler', unbound)
print(f'All ai_, unit_, object_ and vehicle_ commands used by the ten missions are bound.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=120):
    profile = scratch(f'script-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def lines(text, kind):
    return [line.split()[1:] for line in text.splitlines() if line.startswith(kind + ' ')]


# 1. a10: save and tutorial polls, then the looking tutorial finishes unaided.
text, log = run('a10', 'qa_campaign_resume 1 1', [
    (0, 'qa_hs_call game_safe_to_save 0 0; qa_hs_call player_action_test_back 0 0; qa_hs_call player0_joystick_set_is_normal 0 0; '
        'qa_hs_call player0_look_pitch_is_inverted 0 0'),
    (7000, 'qa_hs_call player_action_test_back 0 0'),
    # The tutorial resets every poll, so it starts after the poll check.
    (7100, 'qa_hs_script tutorial_looking 0; qa_hs_script tutorial_looking 2'),
    (40000, 'qa_hs_script tutorial_looking 2'),
    (40500, 'halo_script_status; quit'),
], timeout=300)
calls = {c[0]: float(c[1]) for c in lines(text, 'HS_CALL')}
assert calls['game_safe_to_save'] == 1, ('no enemies, so saving is safe', log)
assert calls['player0_joystick_set_is_normal'] == 1 and calls['player0_look_pitch_is_inverted'] == 0, ('look defaults', log)
polls = [float(c[1]) for c in lines(text, 'HS_CALL') if c[0] == 'player_action_test_back']
assert polls == [0.0, 1.0], ('a prompt waits at first and then times out', polls, log)
done = [c[2] for c in lines(text, 'HS_SCRIPT') if c[1] == '2']
assert done[0] == '0' and done[-1] == '1', ('the looking tutorial finishes without input', done, log)
for name in ('game_safe_to_save', 'hud_get_timer_ticks', 'player_action_test_back', 'unit_get_shield'):
    assert f'unbound {name}' not in text, (name, log)
print('a10: save query, tutorial polls and the looking tutorial chain work.')

# 2. d40 escape run: the countdown mirrors the engine deadline.
text, log = run('d40', 'qa_maw_route', [
    (0, 'qa_hs_script timer_begin 0'),
    (5000, 'qa_hs_call hud_get_timer_ticks 0 0'), (15000, 'qa_hs_call hud_get_timer_ticks 0 0'),
    (25000, 'qa_hs_call hud_get_timer_ticks 0 0'), (35000, 'qa_hs_call hud_get_timer_ticks 0 0'),
    (36000, 'qa_hs_script endgame_cinematics 1; qa_timeout'),
    (40000, 'qa_hs_call hud_get_timer_ticks 0 0; qa_hs_script endgame_cinematics 1; quit'),
], timeout=300)
ticks = [float(c[1]) for c in lines(text, 'HS_CALL') if c[0] == 'hud_get_timer_ticks']
started = [c[2] for c in lines(text, 'HS_SCRIPT') if c[0] == 'endgame_cinematics' and c[1] == '1']
assert len(ticks) == 5, (ticks, log)
# timer_begin shows the timer once Cortana's briefing ends, then it counts down from about three minutes.
running = [t for t in ticks[:4] if t > 0]
assert len(running) >= 2 and all(0 < t <= 5400 for t in running), ('the countdown runs on the escape deadline', ticks, log)
assert running == sorted(running, reverse=True) and running[0] - running[-1] >= 150, ('the countdown falls in step with play', ticks, log)
assert started[0] == '0', ('endgame_cinematics stays asleep while time remains', started, log)
assert ticks[4] == 0, ('the countdown reaches zero with the deadline', ticks, log)
print(f'd40: countdown {running[0]:.0f} down to {running[-1]:.0f} ticks, endgame_cinematics asleep until the deadline.')
