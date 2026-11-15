"""Retail weapon definitions: importer output and the engine's parsed table agree.

Checks packages/halo/weapons.txt (tools/halo_weapons.py) against values read
from the Xbox tags, that every player weapon has its viewmodel and pickup model,
and that halo-ac-qa parses the same table.
"""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

TABLE = root / 'packages/halo/weapons.txt'
require(TABLE, 'run python3 tools/halo_weapons.py with the expanded caches')


def records():
    weapons, current = [], None
    for line in TABLE.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'weapon':
            current = {'index': int(rest), 'magazines': [], 'triggers': [], 'projectiles': []}
            weapons.append(current)
        elif current is None or key.startswith('#'):
            continue
        elif key == 'path':
            current['path'] = rest
        elif key == 'label':
            current['label'], current['class'] = rest.split()
        elif key in ('viewmodel', 'worldmodel'):
            current[key] = rest
        elif key in ('heat', 'zoom', 'aim', 'melee', 'ready'):
            current[key] = [float(x) for x in rest.split()]
        elif key == 'magazine':
            current['magazines'].append([float(x) for x in rest.split()])
        elif key == 'trigger':
            current['triggers'].append([float(x) for x in rest.split()])
        elif key == 'projectile':
            # speeds, range, gravity, guided, damage lower, upper min, upper max, category, radius, path
            parts = rest.split(' ', 10)
            current['projectiles'].append(None if rest == '-' else [float(x) for x in parts[:10]] + [parts[10]])
    return weapons


weapons = records()
by_path = {w['path']: w for w in weapons}
assert [w['index'] for w in weapons] == list(range(len(weapons)))


def weapon(name):
    return by_path[rf'weapons\{name}\{name}']


def close(a, b, tolerance=1e-3):
    return abs(a - b) <= tolerance * max(1.0, abs(b))


# Magazine: loaded, initial total, max total, reload s. Trigger: rate initial, final.
ar = weapon('assault rifle')
assert ar['class'] == 'assault' and ar['magazines'][0][:4] == [60, 240, 600, 3.4], ar['magazines']
assert ar['triggers'][0][:2] == [15, 15] and close(ar['projectiles'][0][0], 324) and ar['projectiles'][0][6] == 10
pistol = weapon('pistol')
assert pistol['magazines'][0][:3] == [12, 60, 120] and pistol['zoom'] == [1, 2, 2] and pistol['projectiles'][0][6] == 25
sniper = weapon('sniper rifle')
assert sniper['class'] == 'sniper' and sniper['zoom'] == [2, 2, 8] and sniper['projectiles'][0][6] == 101
needler = weapon('needler')
assert needler['triggers'][0][:2] == [3, 10] and needler['projectiles'][0][4] > 0, 'needles home on targets'
plasma = weapon('plasma pistol')
assert len(plasma['triggers']) == 2 and plasma['heat'] == [0.25, 1, 0.65] and close(plasma['triggers'][0][13], 0.6)
assert plasma['magazines'][0][0] == 0, 'plasma weapons run on a battery'
rocket = weapon('rocket launcher')
assert rocket['projectiles'][0][9] > 0 and rocket['projectiles'][0][6] >= 100, 'rockets deal area damage'
shotgun = weapon('shotgun')
assert shotgun['triggers'][0][5] >= 10, 'shotgun fires a spread of pellets'

# jpt! damage layout (tools/halo_weapons.py JPT_*): lower bound, upper bound min and max, category.
assert rocket['projectiles'][0][5:9] == [80, 300, 330, 4], rocket['projectiles'][0]
assert shotgun['projectiles'][0][5:9] == [8, 18, 25, 12], shotgun['projectiles'][0]
assert weapon('plasma rifle')['projectiles'][0][5:9] == [10, 12, 14, 10], weapon('plasma rifle')['projectiles'][0]
assert ar['projectiles'][0][5:8] == [10, 10, 10] and sniper['projectiles'][0][5:9] == [101, 101, 101, 5]
assert ar['melee'] == [40, 50, 60] and pistol['melee'] == [40, 50, 60], (ar['melee'], pistol['melee'])
for w in weapons:
    for p in w['projectiles']:
        assert p is None or p[5] <= p[6] <= p[7], (w['path'], p)

players = [w for w in weapons if w['class'] in ('assault', 'pistol', 'shotgun', 'sniper', 'projectile')]
assert len(players) == 9, [w['path'] for w in players]
for w in players:
    for key in ('viewmodel', 'worldmodel'):
        assert w[key] != '-' and (root / 'packages/models' / w[key] / 'tris.md3').exists(), (w['path'], key, w[key])

profile = scratch('weapon-table-')
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480',
                    '-eqa_weapon_table; quit'], cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT,
                   timeout=60, check=True)
rows = [line.split()[1:] for line in log.read_text(errors='replace').splitlines() if line.startswith('WEAPON_TABLE')]
assert len(rows) == len(weapons), (len(rows), len(weapons), log)
for row, w in zip(rows, weapons):
    index, label, _, loaded, maximum, triggers, rate = row[:7]
    magazine = w['magazines'][0] if w['magazines'] else [0, 0, 0]
    assert (int(index), label, int(loaded), int(maximum), int(triggers)) == \
        (w['index'], w['label'], int(magazine[0]), int(magazine[2]), len(w['triggers'])), (row, w['path'])
    assert close(float(rate), w['triggers'][0][0] if w['triggers'] else 0), (row, w['path'])
print(f'{len(weapons)} retail weapons imported, {len(players)} playable, engine table matches.')
