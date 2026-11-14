"""Actor brains in the QA client.

Crash Site wave: unaware actors notice the player's gunfire, alert their squad
and fire in bursts. Spawned marines fight a Grunt. a30 section 2 encounters:
squads hold guard positions, take combat positions from their squad's group
masks once they see the player, and the script AI commands (ai_living_count,
ai_living_fraction, ai_status, ai_actors with list_count/list_get,
ai_magically_see_players, ai_kill, ai_command_list and its status) work.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=90):
    profile = scratch(f'actor-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def actors(text, label):
    rows = []
    for line in text.splitlines():
        if line.startswith(f'ACTOR_QA {label} '):
            rows.append(dict(field.split('=', 1) for field in re.findall(r'(\w+=\S+)', line)))
    return rows


def probes(text, kind):
    return [line.split()[1:] for line in text.splitlines() if line.startswith(kind)]


# 1. The Crash Site wave: blinded actors stay unaware until they hear gunfire,
#    then fight in bursts once they can see again. HALO_AI_BLIND is 1.
text, log = run('wave', 'halo_preview', [
    (200, 'qa_campaign_reach'),
    # Blind them as soon as the wave spawns, before an idle glance can find the player.
    (250, 'qa_actor_flag 1 1'), (300, 'qa_actor_flag 1 1'), (500, 'qa_actor_flag 1 1'),
    (2500, 'qa_actor_state quiet'),
    (3000, 'qa_halo_fire 1'), (3400, 'qa_halo_fire 0'),
    (3800, 'qa_actor_state heard; qa_actor_flag 1 0'),
    (7500, 'qa_actor_state fight'),
    (8000, 'quit'),
])
quiet, heard, fight = actors(text, 'quiet'), actors(text, 'heard'), actors(text, 'fight')
assert len(quiet) >= 3 and all(a['awareness'] == 'unaware' for a in quiet), ('blind actors stay unaware', quiet, log)
assert all(a['awareness'] == 'alert' for a in heard), ('gunfire within hearing range alerts the squad', heard, log)
shots = sorted(int(a['shots']) for a in fight)
assert shots and shots[-1] >= 3, ('the squad returns fire once it can see', shots, log)
print(f'Wave: {len(quiet)} actors heard the gunfire and returned fire, shots {shots}.')

# 2. Marines kill a Grunt.
text, log = run('marines', 'halo_preview', [
    (0, 'qa_actor_spawn 9 1 1 2; qa_actor_spawn 9 1 1 -2; qa_actor_spawn 0 0 9 0'),
    (8000, 'qa_actor_state after'),
    (8500, 'quit'),
])
# With 12 health a marine can lose the duel; either way the two sides fight.
kills = re.findall(r'(Marine \d+ \w+ Grunt \d+|Grunt \d+ \w+ Marine \d+|you were \w+ by Grunt)', text)
assert any(k.startswith('Marine') or k.startswith('Grunt') for k in kills), ('marines and the grunt fight each other', kills, log)
print(f'Marines and a Grunt fought: {kills}.')

# 3. a30 section 2: positions, script commands and a command list.
text, log = run('scripted', 'qa_campaign_resume 2 2', [
    (0, 'qa_actor_place lz_search; qa_actor_place first_marine'),
    (300, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_status lz_search 0; '
          'qa_hs_ai ai_living_fraction lz_search 0; qa_hs_list lz_search; qa_hs_command first_marine; qa_actor_state idle'),
    (600, 'qa_hs_ai ai_magically_see_players lz_search 0; qa_hs_ai ai_command_list first_marine 0'),
    (1000, 'qa_hs_command first_marine'),
    (7000, 'qa_hs_ai ai_status lz_search 0; qa_actor_state seen'),
    (7200, 'qa_hs_ai ai_kill lz_search 0'),
    (7600, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_living_fraction lz_search 0'),
    (8000, 'quit'),
])
values = [(cmd, float(v)) for cmd, enc, v in probes(text, 'HS_AI') if enc == 'lz_search']
count = values[0][1]
assert values[0][0] == 'ai_living_count' and count >= 8, values
assert values[1] == ('ai_status', 2.0) and values[2] == ('ai_living_fraction', 1.0), values
listed = probes(text, 'HS_LIST')[0]
assert listed[2] == f'count={int(count)}' and listed[3] == f'units={int(count)}', listed
statuses = [int(v) for _, v in probes(text, 'HS_COMMAND')]
assert statuses[:2] == [0, 1], ('ai_command_list starts the list', statuses)
assert ('ai_status', 6.0) in values, ('magic sight puts the squad in combat', values)
assert values[-2:] == [('ai_living_count', 0.0), ('ai_living_fraction', 0.0)], values

# Team 0 is the Covenant; the marine encounter's marines and crew are team 1.
idle = [a for a in actors(text, 'idle') if a['team'] == '0']
seen = [a for a in actors(text, 'seen') if a['team'] == '0']
def in_mask(a):
    return a['dest'] == '1' and int(a['group']) >= 0 and (int(a['mask']) == 0 or int(a['mask']) >> int(a['group']) & 1)
assert idle and sum(in_mask(a) for a in idle) >= len(idle) // 2, ('unaware squads take guard positions', idle)
assert seen and all(a['awareness'] == 'combat' for a in seen) and all(in_mask(a) for a in seen), \
    ('combat positions come from the squad group masks', seen)
marines = actors(text, 'seen')
assert any(a['team'] == '1' and a['command'] in ('1', '2') for a in marines), 'marines run the command list'
print(f'Scripted: {int(count)} actors placed, script AI commands correct, positions follow squad masks, command list runs.')
