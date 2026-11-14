"""Retail actor behaviour tables: importer output and the engine's parsed copy agree.

Checks packages/halo/actors.txt (tools/halo_actors.py) against values read from
the Xbox actr and actv tags, and that halo-ac-qa parses the same table.
"""
import math
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

TABLE = root / 'packages/halo/actors.txt'
require(TABLE, 'run python3 tools/halo_actors.py with the expanded caches')


def records():
    actors, current = [], None
    for line in TABLE.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'actor':
            current = {'index': int(rest)}
            actors.append(current)
        elif current is None or key.startswith('#') or key in ('format', 'end'):
            continue
        elif key == 'path':
            current['path'] = rest
        elif key == 'family':
            family, kind, kind_type, *_ = rest.split()
            current.update(family=family, kind=int(kind), type=int(kind_type))
        else:
            current[key] = [float(x) for x in rest.split()]
    return actors


actors = records()
by_path = {a['path']: a for a in actors}
assert [a['index'] for a in actors] == list(range(len(actors)))
close = lambda a, b: abs(a - b) <= 1e-3 * max(1.0, abs(b))

# fire: max range, rate, error, first delay min/max, combat range min/max, lead, damage scale
marine = by_path[r'characters\marine\marine assault rifle']
assert marine['kind'] == 9 and marine['fire'][:2] == [13, 10] and marine['fire'][5:7] == [3, 6], marine['fire']
assert marine['burst'][:2] == [0.6, 0.8] and marine['burst'][2:] == [1, 1.5], marine['burst']
grunt = by_path[r'characters\grunt\grunt minor plasma pistol']
assert grunt['fire'][1] == 5 and close(math.degrees(grunt['fire'][2]), 3) and grunt['morale'][1] == 1.0, grunt
assert grunt['vision'][0] == 40 and grunt['vitality'][0] == 30
elite = by_path[r'characters\elite\elite minor\elite minor plasma rifle']
assert elite['defense'][0] == 0.5 and elite['vitality'] == [100, 100] and elite['weapon'][0] >= 0, elite
assert sum(a['kind'] >= 0 for a in actors) >= 70, 'most variants have engine bodies'

profile = scratch('actor-table-')
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480',
                    '-eqa_actor_table; quit'], cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT,
                   timeout=60, check=True)
rows = [line.split()[1:] for line in log.read_text(errors='replace').splitlines() if line.startswith('ACTOR_TABLE')]
assert len(rows) == len(actors), (len(rows), len(actors), log)
for row, a in zip(rows, actors):
    index, family, kind, weapon, rate, burst, vision = row
    assert (int(index), family, int(kind), int(weapon)) == (a['index'], a['family'], a['kind'], int(a['weapon'][0])), (row, a['path'])
    assert close(float(rate), a['fire'][1]) and close(float(burst), a['burst'][0]) and close(float(vision), a['vision'][0]), row
print(f'{len(actors)} retail actor variants imported, engine table matches.')
