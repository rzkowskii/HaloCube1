"""Squad cap and the Flood life cycle in the QA client.

c10 encounters placed one after another (each squad killed before the next)
all report actors, so dead entries no longer eat the 128-actor cap. A carrier
form that dies bursts into 4 to 8 infection forms. An infection form that
reaches a dead marine turns the body into a Flood combat form after a delay.
Infection forms leap at a target a few strides away.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


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

# 2. A carrier form bursts into 4 to 8 infection forms when killed.
text, log = run('carrier', 'halo_preview', [
    (0, 'qa_actor_spawn 7 2 14 0'),
    (1500, 'qa_actor_count before; qa_actor_kill 7'),
    (3000, 'qa_actor_count after'),
    (3300, 'quit'),
])
before, after = counts(text, 'before'), counts(text, 'after')
assert before['carrier'] == 1 and before['infection'] == 0, before
assert after['carrier'] == 0 and 4 <= after['infection'] <= 8, ('carrier burst', after, log)
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
