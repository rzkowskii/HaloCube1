"""Walking assists on retail collision (ASSISTS in tools/import_halo_campaign.py; the .hcol's AST1 trailer).

    python3 tests/test_retail_assists.py

For each section ASSISTS lists (c10 sections 3, 5 and 6 today), starts it as campaign play and checks that
  * the engine loaded its .hcol with as many walking assists as ASSISTS lists ("Halo: loaded N retail
    collision surfaces for <map> (M static machines, K walking assists)"),
  * every assist (qa_retail_assists in tests/native/retail_qa.inc) lets a ray through (shots, sight
    lines and probes never stop on one), stops a body's capsule swept down onto it (the player and
    actors stand on it) and lets a grenade's sweep through,
  * the player put one unit above each assist's centre (qa_retail_assist_stand) comes to rest on it,
    alive and on the floor, within half a unit of its centre's height.
The route across the assists is walked by tests/test_retail_collision.py (PENDING_WALK) and checked
statically by tests/test_traversal_routes.py. Written while testing was on hold: not yet run.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import ASSISTS, MISSIONS, STOCK_SECTIONS  # noqa: E402

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
maps = root / 'packages/maps'
checked, problems = 0, []
for (cache, part), assists in sorted(ASSISTS.items()):
    if (cache, part) in STOCK_SECTIONS or not assists:
        continue
    name = f'halo_{cache}_{part + 1:02}'
    require(maps / f'{name}.hcol', f'run tools/import_halo_campaign.py --mission {cache} --part {part + 1}')
    mission = next(i for i, m in enumerate(MISSIONS) if m.cache == cache) + 1
    # The engine's a30 has the standalone Crash Site map as its first section.
    section = part + 1 + (cache == 'a30')
    steps = ['sleep 500 [qa_retail_assists; qa_retail_assist_stand 0] 1']
    for i in range(1, len(assists)):
        steps.append(f'sleep {500 + 1500 * i} [qa_retail_where; qa_retail_assist_stand {i}] 1')
    steps.append(f'sleep {500 + 1500 * len(assists)} [qa_retail_where; quit] 1')
    script = f'halo_difficulty 2; qa_campaign_play {mission} {section}; qa_after_gameplay [{"; ".join(steps)}]'
    profile = scratch(f'retail-assists-{name}-')
    logfile = SCRATCH / f'retail-assists-{name}.log'
    with logfile.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w640', '-h480', '-e' + script],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=180, check=True)
    log = logfile.read_text(errors='replace')
    loaded = re.search(rf'loaded \d+ retail collision surfaces for {name} \(\d+ static machines, (\d+) walking assists\)', log)
    if not loaded or int(loaded.group(1)) != len(assists):
        problems.append(f'{name}: loaded {loaded.group(1) if loaded else "no"} walking assists, ASSISTS lists {len(assists)} ({logfile})')
        continue
    found = re.findall(r'RETAIL_QA assist (\d+) centre=(\S+),(\S+),(\S+) normal=\S+ ray_through=(\d) body_stands=(\d) '
                       r'stand_z=(\S+) bounce_through=(\d)', log)
    if len(found) != len(assists):
        problems.append(f'{name}: {len(found)} assists reported, expected {len(assists)} ({logfile})')
        continue
    where = re.findall(r'RETAIL_QA where map=(\S+) collision=(\d) feet=(\S+),(\S+),(\S+) onfloor=(\d) alive=(\d)', log)
    for (index, x, y, z, ray, body, stand, bounce), why in zip(found, (a.why for a in assists)):
        if (ray, body, bounce) != ('1', '1', '1'):
            problems.append(f'{name}: assist {index} ({why}): ray_through={ray} body_stands={body} bounce_through={bounce} ({logfile})')
        rest = where[int(index)] if int(index) < len(where) else None
        if rest is None:
            problems.append(f'{name}: no position after standing on assist {index} ({logfile})')
            continue
        _, collision, fx, fy, fz, onfloor, alive = rest
        if collision != '1' or onfloor != '1' or alive != '1' or abs(float(fz) - float(z)) > .5:
            problems.append(f'{name}: after standing on assist {index} ({why}): feet {fx},{fy},{fz} onfloor={onfloor} '
                            f'alive={alive}, assist centre at z {z} ({logfile})')
    checked += 1
    print(f'{name}: {len(assists)} walking assists load, let rays and grenades through and carry a body.')

if problems:
    raise SystemExit('walking assists: ' + '; '.join(problems))
print(f'{checked} sections with walking assists checked.')
