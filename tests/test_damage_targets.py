"""Damage targets: scripted units with no AI that the player shoots (d40's manifold detectors).

NOT YET RUN: written while testing was on hold.

1. Every packages/halo/<cache>.hscb is version 8 (object rows carry a damage target's vitality,
   collision box and whether it is placed at the start). With HALO_SOURCE set, the importer finds
   d40's eight `invisible damage detector` bipeds (enc5_1_detector_n1, _n1b ... _s3b) as damage
   targets with the retail body vitality 125 and a flat 1.1 x 1.6 x 0.24 box, and no other
   mission has one. No game needed.
2. halo_d40_05.route (tools/import_halo_campaign.py ShootTarget) uses the four manifold controls and
   shoots three detectors; each shoot row follows its use row's manifold.
3. QA client on d40 section 5 (The Maw, the engine room): a detector exists from the start at full
   health (`unit_get_health` 1, not -1 as before, which destroyed every manifold at once);
   `object_cannot_take_damage` stops damage and `object_can_take_damage` lets it in again; 50
   points leave 0.6; a trace from the player through a detector meets it; once enc5_1_manager runs,
   a b detector shot down to 0 destroys its manifold (`manifold_s1_destroyed` 1) and the s1
   detector stops taking damage with its piston closed (enc5_1_damage_detector_control). Then a
   real shot: the player stands on the route's first use row, uses the control (the piston opens),
   stands on the shoot row, aims at its aim point and fires; the detector loses health
   (`Halo target: ... took` with halo_scripttrace).
"""
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

halo = root / 'packages/halo'
require(halo / 'd40.hscb', 'run tools/import_halo_scripts.py')
for path in sorted(halo.glob('*.hscb')):
    assert path.read_bytes()[4:8] == (8).to_bytes(4, 'little'), f'{path.name} is not HSCB version 8; rerun tools/import_halo_scripts.py'
print('Every .hscb is version 8.')

if os.environ.get('HALO_SOURCE') or (HALO_SOURCE / 'd40.map').is_file():
    import mmap
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from import_halo_campaign import damage_targets  # noqa: E402
    for cache in ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40'):
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            found = damage_targets(HaloMap(data))
        if cache != 'd40':
            assert not found, (cache, sorted(found))
            continue
        assert sorted(found) == sorted(f'enc5_1_detector_{m}{b}' for m in ('n1', 'n3', 's1', 's3') for b in ('', 'b')), sorted(found)
        for name, (centre, position, yaw, low, high) in found.items():
            size = [round(b - a, 2) for a, b in zip(low, high)]
            assert size == [1.1, 1.6, .24], (name, size)
    print('d40 has the eight manifold detectors as damage targets; no other mission has any.')

route = root / 'packages/maps/halo_d40_05.route'
require(route, 'run tools/import_halo_campaign.py "$maps" --mission d40')
rows = [line.split() for line in route.read_text().splitlines() if line.strip()]
uses = [r for r in rows if len(r) > 3 and int(r[3]) & 256]
shots = [r for r in rows if len(r) > 3 and int(r[3]) & 512]
assert [r[4].split('@')[0] for r in uses] == ['enc5_1_pistons1', 'enc5_1_pistonn1', 'enc5_1_pistonn3', 'enc5_1_pistons3'], uses
assert [r[4].split('@')[0].rstrip('b') for r in shots] == ['enc5_1_detector_s1', 'enc5_1_detector_n1', 'enc5_1_detector_n3'], shots
print('halo_d40_05.route uses the four manifold controls and shoots three detectors.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
# The real shot: the second manifold's use row (n1, not touched before), then its shoot row.
use, shot = uses[1], shots[1]
target = shot[4].split('@')[0]
profile = scratch('damage-targets-')
steps = [
    (100, 'qa_target_state enc5_1_detector_s1 start; qa_hs_cmd unit_get_health "obj:enc5_1_detector_s1"'),
    (200, 'qa_hs_cmd object_cannot_take_damage "obj:enc5_1_detector_s1"; qa_target_damage enc5_1_detector_s1 50'),
    (300, 'qa_hs_cmd object_can_take_damage "obj:enc5_1_detector_s1"; qa_target_damage enc5_1_detector_s1 50; '
          'qa_target_state enc5_1_detector_s1 half'),
    (400, 'qa_target_trace enc5_1_detector_s3'),
    (500, 'qa_hs_script enc5_1_manager 0'),
    (900, 'qa_target_damage enc5_1_detector_s1b 125'),
    (2000, 'qa_hs_global manifold_s1_destroyed ""; qa_target_state enc5_1_detector_s1 managed'),
    (2200, f'qa_halo_feet {use[0]} {use[1]} {use[2]}'),
    (2700, 'halo_use'),
    (4700, f'qa_halo_feet {shot[0]} {shot[1]} {shot[2]}; qa_target_state {target} before'),
    (5200, f'qa_target_aim {shot[5]} {shot[6]} {shot[7]}; qa_halo_fire 1'),
    (5400, 'qa_halo_fire 0'), (5800, 'qa_halo_fire 1'), (6000, 'qa_halo_fire 0'),
    (6400, 'qa_halo_fire 1'), (6600, 'qa_halo_fire 0'), (7000, 'qa_halo_fire 1'), (7200, 'qa_halo_fire 0'),
    (7600, f'qa_target_state {target} after'),
    (7800, 'quit'),
]
body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in steps)
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                    f'-ehalo_scripttrace 1; halo_difficulty 2; qa_campaign_play 10 5; qa_after_gameplay [{body}]'],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=180, check=True)
text = log.read_text(errors='replace')


def line(prefix):
    found = [entry for entry in text.splitlines() if entry.startswith(prefix)]
    assert found, (prefix, text[-3000:])
    return found


def state(label, name='enc5_1_detector_s1'):
    return line(f'TARGET_QA {label} name={name} ')[0]


assert 'found=1 health=1.000 damageable=1' in state('start'), state('start')
assert line('HS_CMD unit_get_health ')[0].endswith('1.0000'), line('HS_CMD unit_get_health ')
damage = line('TARGET_QA damage name=enc5_1_detector_s1 ')
assert 'dealt=0' in damage[0] and 'dealt=1' in damage[1], damage
assert 'health=0.600' in state('half'), state('half')
assert line('TARGET_QA trace name=enc5_1_detector_s3 ')[0].split('hit=')[1].split()[0] in \
    ('enc5_1_detector_s3', 'enc5_1_detector_s3b'), line('TARGET_QA trace')
assert line('QA_HS_GLOBAL manifold_s1_destroyed ')[0].endswith('1.0000'), line('QA_HS_GLOBAL manifold_s1_destroyed')
assert 'damageable=0' in state('managed'), state('managed')
before = float(state('before', target).split('health=')[1].split()[0])
after = float(state('after', target).split('health=')[1].split()[0])
assert before == 1 and after < before, (before, after, [entry for entry in text.splitlines() if 'Halo target:' in entry][:5])
print(f'Detectors start whole, take damage only when allowed, destroy their manifold, and a real shot from the '
      f'route\'s stand hurts {target} ({after:.2f} left).')
