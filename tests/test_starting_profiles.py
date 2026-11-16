"""Player starting profiles and player_add_equipment (HSCB version 9).

Written while testing was on hold: not yet run (see docs/macos-build.md, "Not yet verified in the
game", and the starting profiles under Retail weapons).

Static (assets, no game): every packages/halo/<cache>.hscb is version 9 and reads to its last byte
(qa_common.read_hscb); each mission's profile 0 is the player's start: a10 carries no weapon and no
grenades (it starts unarmed), a10 has bridge0_profile (a pistol, 6 loaded, 48 more) for Keyes's
bridge, a30 the assault rifle 60/120 and the pistol 12/64 with no grenades, a50 the sniper rifle 4/64
and the assault rifle 60/360 with four frags, c20 an easy_start profile (a shotgun 12/48 and the pistol
12/60, four frags). With HALO_SOURCE the importer's profile_rows read the same from the retail tags.

QA client (fresh profiles):
1. a10 section 1 as a fresh start: no weapon in either slot (QA_LOADOUT unarmed=1, the knife slot
   selected, no frags); `player_add_equipment (player0) bridge0_profile false` through the interpreter
   puts the pistol (6/48) in the selected slot and selects it; a death and revert before the first
   checkpoint returns to the unarmed start (the section's loadout is the profile's).
2. a10 section 3 started on its own gets the pistol at once (FreshProfile: past the bridge scene).
3. a30 section 2 started on its own: the a30 profile (assault rifle 60/120, pistol 12/64, no frags).
4. c20 section 1 on Easy: the startup's diff_control replaces the loadout with easy_start (shotgun
   12/48, pistol 12/60, four frags); on Normal the profile 0 rifle and pistol.
5. a10 section 1 finished into section 2: the unarmed loadout is carried, not the profile again.
6. A checkpoint saved unarmed writes `empty 0` and `empty 1` and no weapon line; Continue in a second
   launch is still unarmed.
7. The fresh-start script (freshScripts in source/src/halo_script.cpp: gap 6 of batch 5's critic for
   sections 5 and 6, the fresh-start batch for sections 2 to 4 and the Normal tutorial resume; not yet run): each a10
   section from 2 to 7 started on its own on Normal logs "a10 section N started afresh: the script brought
   forward (G globals set, 2 threads passed over, W woken)" (section 2: 1 and 0; sections 3 and 4: 5 and 0;
   sections 5 and 6: 5 and 2; section 7, since the final integration review of the fresh-start batch: 5
   and 0) and "tutorial_setup resumed after its statement S (wake T)" (section 2: 63,
   tutorial_first_contact; the others 65, tutorial_weapon); the player is never seated in cryotube_1 (no
   "seats the player"); mark_fast_setup is set; the section's own mission threads have started (section 2
   mission_crossfire and mission_bridge, with mission_shoot still waiting for the bridge scene and
   mark_bridge_cutscene unset; section 3 mission_airlock_1, mission_flank, mission_loop and
   mission_airlock_2; section 4 mission_knot and mission_stairs; sections 5 and 6 mission_tunnel,
   cryo_search and mission_boom; section 7 mission_final); tutorial_setup is still waiting in section 2 (for the bridge flag) and has
   finished in 3 to 7, where tutorial_motiontracker has started and tutorial_weapon has not. On Heroic
   (section 3) the fresh start is the same but nothing resumes tutorial_setup (passed over, finished,
   tutorial_motiontracker never started). Section 1 is untouched: no "started afresh" line, and on Normal
   the tutorial seats the player in cryotube_1 as before.
8. The melee door (the fresh-start batch, Scenario::meleeDevice; not yet run): a10 section 6 started on its own on
   Normal: tutorial_melee has put in melee_door_2 (visible, at 0.8325, unpowered) and
   tutorial_motiontracker powered off motiontracker_door_2; the player's melee square on to melee_door_2
   (qa_device_melee) logs "Halo: a melee opened melee_door_2" and leaves it at 1.0; a second swing logs
   nothing more. On Heroic melee_door_2 is never created and motiontracker_door_2 keeps its power.
"""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(root / 'packages/halo/a10.hscb', 'run tools/import_halo_scripts.py')

# ---- Static: the profile tables.
programs = {p.stem: read_hscb(p) for p in sorted((root / 'packages/halo').glob('*.hscb'))}
old = [name for name, h in programs.items() if h['version'] != 9]
assert not old, (old, 'rerun tools/import_halo_scripts.py (HSCB version 9)')


def weapons(profile):
    return [(path.rsplit('\\', 1)[-1] if path else '', loaded, total) for path, loaded, total in profile['weapons']]


a10 = programs['a10']['profiles']
assert weapons(a10[0]) == [('', 0, 0), ('', 0, 0)] and a10[0]['frags'] == 0, ('a10 starts unarmed', a10[0])
bridge = next(p for p in a10 if p['name'] == 'bridge0_profile')
assert weapons(bridge)[0] == ('pistol', 6, 48) and weapons(bridge)[1][0] == '', bridge
a30 = programs['a30']['profiles'][0]
assert weapons(a30) == [('assault rifle', 60, 120), ('pistol', 12, 64)] and a30['frags'] == 0, a30
a50 = programs['a50']['profiles'][0]
assert weapons(a50) == [('sniper rifle', 4, 64), ('assault rifle', 60, 360)] and a50['frags'] == 4, a50
easy = next(p for p in programs['c20']['profiles'] if p['name'] == 'easy_start')
assert weapons(easy) == [('shotgun', 12, 48), ('pistol', 12, 60)] and easy['frags'] == 4, easy
if (HALO_SOURCE / 'a10.map').exists():
    import mmap
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from import_halo_scripts import profile_rows  # noqa: E402
    for cache, program in programs.items():
        with (HALO_SOURCE / f'{cache}.map').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            rows_ = list(profile_rows(HaloMap(data)))
        assert [r[0] for r in rows_] == [p['name'] for p in program['profiles']], cache
        assert [(r[3], r[4], r[5]) for r in rows_] == [p['weapons'][0] for p in program['profiles']], cache
print(f'{len(programs)} .hscb files are version 9; the starting profiles hold a10 unarmed, its bridge pistol, a30, a50, c20 easy_start.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/halo/weapons.txt', 'run tools/halo_weapons.py')


def run(name, script, profile=None, timeout=120):
    profile = profile or scratch(f'profiles-{name}-')
    log = profile / f'{name}.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600', f'-e{script}'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), profile


def loadout(text, label):
    found = [dict(f.split('=', 1) for f in line.split()[2:] if '=' in f)
             for line in text.splitlines() if line.startswith(f'QA_LOADOUT {label} ')]
    assert found, (label, text[-3000:])
    return found[-1]


# 1. a10 unarmed, the bridge pistol, the revert.
text, _ = run('a10', 'halo_difficulty 2; qa_campaign_play 1 1; qa_after_gameplay [qa_profile_state; qa_loadout_state start; '
              'sleep 300 [qa_hs_cmd player_add_equipment "player s:bridge0_profile false"; qa_loadout_state pistol] 1; '
              'sleep 600 [qa_halo_damage 100000] 1; sleep 2200 [qa_halo_revert] 1; sleep 3500 [qa_loadout_state reverted; quit] 1]')
assert 'QA_PROFILE count=5' in text, text[-3000:]
start = loadout(text, 'start')
assert start['unarmed'] == '1' and start['w0'].startswith('-:') and start['w1'].startswith('-:') and start['frags'] == '0', start
assert start['gun'] == '0', ('the empty slot is the knife', start)
pistol = loadout(text, 'pistol')
assert pistol['unarmed'] == '0' and 'hp:6/48' in (pistol['w0'], pistol['w1']), pistol
assert 'Halo: starting profile bridge0_profile added' in text, text[-3000:]
reverted = loadout(text, 'reverted')
assert reverted['unarmed'] == '1' and reverted['w0'].startswith('-:'), ('a revert before the first checkpoint: the profile', reverted)
print('a10 started unarmed, bridge0_profile gave the pistol, a revert went back to the unarmed start.')

# 2. a10 section 3 on its own: the pistol.
text, _ = run('a10-3', 'halo_difficulty 2; qa_campaign_resume 1 3; qa_after_gameplay [qa_loadout_state start; quit]')
start = loadout(text, 'start')
assert 'hp:6/48' in (start['w0'], start['w1']) and start['unarmed'] == '0', start
print('a10 section 3 started on its own with the bridge pistol.')

# 3. a30 section 2.
text, _ = run('a30', 'qa_campaign_resume 2 2; qa_after_gameplay [qa_loadout_state start; quit]')
start = loadout(text, 'start')
assert start['w0'] == 'ar:60/120' and start['w1'] == 'hp:12/64' and start['frags'] == '0', start
print('a30 started with its profile: assault rifle 60/120, pistol 12/64, no grenades.')

# 4. c20 Easy and Normal.
text, _ = run('c20-easy', 'halo_difficulty 0; qa_campaign_resume 7 1; qa_after_gameplay [sleep 1500 [qa_loadout_state start; quit] 1]')
start = loadout(text, 'start')
assert start['w0'] == 'sg:12/48' and start['w1'] == 'hp:12/60' and start['frags'] == '4', start
text, _ = run('c20-normal', 'halo_difficulty 1; qa_campaign_resume 7 1; qa_after_gameplay [sleep 1500 [qa_loadout_state start; quit] 1]')
start = loadout(text, 'start')
assert start['w0'] == 'ar:60/180' and start['w1'] == 'hp:12/60' and start['frags'] == '4', start
print('c20: easy_start on Easy, the profile 0 rifle and pistol on Normal.')

# 5. Carried into a10 section 2.
text, _ = run('carry', 'halo_difficulty 2; qa_campaign_play 1 1; qa_after_gameplay [qa_halo_finale; '
              'sleep 500 [halo_next] 1; sleep 1500 [halo_next] 1; sleep 7000 [qa_loadout_state after; quit] 1]', timeout=150)
after = loadout(text, 'after')
assert after['map'] == 'halo_a10_02' and after['unarmed'] == '1', after
print('The unarmed loadout carried from a10 section 1 into section 2.')

# 6. Saved unarmed, continued unarmed.
text, profile = run('save', 'halo_title; sleep 300 [halo_new_game 2] 1; qa_after_gameplay [sleep 600 [qa_checkpoint_save] 1; '
                    'sleep 900 [quit] 1]')
saved = read_progress(profile)
lines = (profile / 'config/halo-progress.txt').read_text().splitlines()
assert 'empty 0' in lines and 'empty 1' in lines and 'weapon' not in saved, lines[:12]
text, profile = run('continue', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [qa_loadout_state continued; quit]', profile)
assert loadout(text, 'continued')['unarmed'] == '1', loadout(text, 'continued')
print('A checkpoint saved unarmed kept the empty slots through Continue.')

# 7. The fresh-start script fast-forward: a10 sections 2 to 7 started on their own (gap 6 of batch 5's
# critic, the fresh-start batch). Per section: globals set, threads woken, the tutorial_setup statement resumed after
# (and the script it woke), the mission threads that must have started, and whether tutorial_setup ends.
FRESH = {
    2: (1, 0, 63, 'tutorial_first_contact', ('mission_crossfire', 'mission_bridge'), False),
    3: (5, 0, 65, 'tutorial_weapon', ('mission_airlock_1', 'mission_flank', 'mission_loop', 'mission_airlock_2'), True),
    4: (5, 0, 65, 'tutorial_weapon', ('mission_knot', 'mission_stairs'), True),
    5: (5, 2, 65, 'tutorial_weapon', ('mission_tunnel', 'cryo_search', 'mission_boom'), True),
    6: (5, 2, 65, 'tutorial_weapon', ('mission_tunnel', 'cryo_search', 'mission_boom'), True),
    7: (5, 0, 65, 'tutorial_weapon', ('mission_final',), True),
}
PROBED = ('tutorial_setup', 'tutorial_motiontracker', 'tutorial_weapon', 'mission_shoot')


def scripts_state(text):
    out = {}
    for line in text.splitlines():
        if line.startswith('HS_SCRIPT '):
            _, name, action, value = line.split()[:4]
            out[(name, action)] = value
    return out


def global_value(text, name):
    found = [line.split()[-1] for line in text.splitlines() if line.startswith(f'QA_HS_GLOBAL {name} ')]
    assert found, (name, text[-2000:])
    return float(found[-1])


def fresh_run(part, difficulty, names):
    probes = '; '.join(f'qa_hs_script {n} 1; qa_hs_script {n} 2' for n in names)
    return run(f'a10-{part}-fresh-{difficulty}', f'halo_scripttrace 1; halo_difficulty {difficulty}; qa_campaign_resume 1 {part}; '
               f'qa_after_gameplay [sleep 12000 [{probes}; qa_hs_global mark_fast_setup; qa_hs_global mark_bridge_cutscene; quit] 1]',
               timeout=150)[0]


for part, (globals_set, woken, statement, after, threads, finished) in FRESH.items():
    text = fresh_run(part, 1, PROBED + threads)
    assert f'a10 section {part} started afresh: the script brought forward ({globals_set} globals set, 2 threads passed over, {woken} woken)' in text, \
        (part, text[-2000:])
    assert f'a10 section {part} started afresh on difficulty 1: tutorial_setup resumed after its statement {statement} (wake {after})' in text, \
        (part, text[-2000:])
    assert 'the script seats the player in cryotube_1' not in text, ('the cryo exit is passed over', part, text[-2000:])
    state = scripts_state(text)
    missing = [name for name in threads if state.get((name, '1')) != '1']
    assert not missing, ('the section\'s own mission threads run', part, missing, state)
    assert global_value(text, 'mark_fast_setup') != 0, (part, text[-2000:])
    assert state.get(('tutorial_setup', '1')) == '1' and state.get(('tutorial_setup', '2')) == ('1' if finished else '0'), (part, state)
    if part == 2:
        assert global_value(text, 'mark_bridge_cutscene') == 0 and state.get(('mission_shoot', '1')) == '0', \
            ('section 2 waits for its own bridge scene', state)
        assert state.get(('tutorial_motiontracker', '1')) == '0', state
    else:
        assert global_value(text, 'mark_bridge_cutscene') != 0, (part, text[-2000:])
        assert state.get(('tutorial_motiontracker', '1')) == '1' and state.get(('tutorial_weapon', '1')) == '0', \
            ('the later lessons, not section 2\'s', part, state)
    print(f'a10 section {part} on its own (Normal): brought forward, tutorial_setup resumed after (wake {after}), '
          f'{", ".join(threads)} running, the cryotube passed over.')
text = fresh_run(3, 2, PROBED + FRESH[3][4])
assert 'a10 section 3 started afresh: the script brought forward (5 globals set, 2 threads passed over, 0 woken)' in text, text[-2000:]
assert 'tutorial_setup resumed' not in text and 'the script seats the player in cryotube_1' not in text, text[-2000:]
state = scripts_state(text)
assert state.get(('tutorial_setup', '2')) == '1' and state.get(('tutorial_motiontracker', '1')) == '0', ('Heroic runs no tutorial', state)
print('a10 section 3 on its own (Heroic): brought forward, no tutorial.')
text, _ = run('a10-1-fresh', 'halo_scripttrace 1; halo_difficulty 1; qa_campaign_resume 1 1; qa_after_gameplay [sleep 3000 [quit] 1]', timeout=150)
assert 'started afresh: the script brought forward' not in text and 'tutorial_setup resumed' not in text, text[-2000:]
assert 'the script seats the player in cryotube_1' in text, ('section 1 keeps its cryo exit', text[-2000:])
print('a10 section 1 on its own is untouched: the tutorial seats the player in the cryotube.')


# 8. The melee door (the fresh-start batch): a10 section 6 on its own.
def hs_value(text, command, index=-1):
    found = [float(line.split()[-1]) for line in text.splitlines() if line.startswith(f'HS_CMD {command} ')]
    assert found, (command, text[-2000:])
    return found[index]


text, _ = run('a10-6-melee', 'halo_scripttrace 1; halo_difficulty 1; qa_campaign_resume 1 6; qa_after_gameplay ['
              'sleep 3000 [qa_hs_object visible obj:melee_door_2; qa_hs_cmd device_get_position "obj:melee_door_2"; '
              'qa_hs_cmd device_get_power "obj:melee_door_2"; qa_hs_cmd device_get_power "obj:motiontracker_door_2"] 1; '
              'sleep 3500 [qa_device_melee melee_door_2 3 1 first] 1; sleep 5000 [qa_device_melee melee_door_2 3 1 second] 1; '
              'sleep 6000 [quit] 1]', timeout=150)
objects = {line.split()[2]: line.split()[-1] for line in text.splitlines() if line.startswith('HS_OBJECT visible ')}
assert objects.get('obj:melee_door_2') == '1', ('tutorial_melee put in the melee door', objects, text[-2000:])
assert abs(hs_value(text, 'device_get_position', 0) - 0.8325) < 1e-3 and hs_value(text, 'device_get_power', 0) == 0, text[-2000:]
assert hs_value(text, 'device_get_power', 1) == 0, ('tutorial_motiontracker powered off motiontracker_door_2', text[-2000:])
struck = [line for line in text.splitlines() if line.startswith('DEVICE_QA melee ')]
assert len(struck) == 2 and 'placed=1' in struck[0] and struck[0].endswith('position=1.0000'), struck
assert text.count('Halo: a melee opened melee_door_2') == 1, ('one swing opens it, the second finds it open', text[-2000:])
text, _ = run('a10-6-melee-heroic', 'halo_scripttrace 1; halo_difficulty 2; qa_campaign_resume 1 6; qa_after_gameplay ['
              'sleep 3000 [qa_hs_object visible obj:melee_door_2; qa_hs_cmd device_get_power "obj:motiontracker_door_2"; quit] 1]',
              timeout=150)
objects = {line.split()[2]: line.split()[-1] for line in text.splitlines() if line.startswith('HS_OBJECT visible ')}
assert objects.get('obj:melee_door_2') == '0' and hs_value(text, 'device_get_power') == 1, (objects, text[-2000:])
print('a10 section 6 on Normal: the melee opened melee_door_2; on Heroic it is never put in.')
