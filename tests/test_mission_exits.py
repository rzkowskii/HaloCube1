"""Every mission's exit scene, the finale, and the scenes the mission scripts call.

Static (needs the expanded Xbox maps in HALO_SOURCE, no Mac): every retail script that takes the
camera (cinematic_start or camera_control) is a row of the cue table in
source/src/halo_script.cpp, is called from one (a wrapper such as d20's cinematic_drop runs
cutscene_fall), or is listed in NOT_CUED below with the reason it plays as gameplay.

QA client (fresh profiles, real campaign play):
1. Each mission's last section: qa_halo_finale plays its exit scene ("Halo scene: <exit> start"),
   Enter skips it ("... skipped"), and Enter again loads the next mission's first section
   ("Halo: <next mission>, section 1"). The Maw's finale raises the ending instead.
2. a30's extraction: the variant follows the player's position (the river, cliff or rubble site).
3. Scenes a script calls mid-section (Call rows) play there and leave the player where the scene
   does: d20's captain scene at player0_capt_done, d40's bridge scene at player0_playon_base; a50's
   rescue then hands over to ini_post_rescue, whose object_teleport puts the player by the switch
   (prison_player0_teleport). The section's exit then plays no scene (those scenes have no
   Exit row any more), so each starts once.
4. A Call scene skipped at once still runs the rest of its script: b30's cutscene_shaftb_goal
   leaves shafta_lock_door open (it closes it first and opens it later in the scene).
6. Gameplay scripts a scene wakes go on in play (gap 3 of batch 5's critic, not yet run): b30's
   cutscene_insertion wakes music_b30 on its first tick, which waits for the mission's music cues; with
   the scene skipped, it carries on as a gameplay thread ("Halo scene: music_b30, woken by
   cutscene_insertion, goes on in play"; qa_hs_script music_b30 1 answers 1).

Static (gap 3): SCENE_WAKES below lists, per cue-table scene, the gameplay scripts its threads wake
(through static calls and the woken scripts' own wakes), read from the maps; the test fails on any
change, and checks that b40's pelican_up is the one that seats the player (the seat endCue takes in play).

Written while testing was on hold: not yet run.
"""
import json
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

IDS = 'a10 a30 a50 b30 b40 c10 c20 c40 d20 d40'.split()
TITLES = ['The Pillar of Autumn', 'Halo', 'The Truth and Reconciliation', 'The Silent Cartographer',
          'Assault on the Control Room', '343 Guilty Spark', 'The Library', 'Two Betrayals', 'Keyes', 'The Maw']
# Scripts that take the camera but are not cued, and why.
NOT_CUED = {
    ('a10', 'fade_out_in'): 'never called',
    ('a10', 'fast_setup'): 'the cryo tube exit on Heroic and Legendary: gameplay (the engine does not seat the player in the tube)',
    ('a10', 'tutorial_action'): 'the cryo tutorial\'s exit on Normal: gameplay, as fast_setup',
    ('a50', 'game_lost_script'): 'Keyes dying: game_lost reverts the player; its camera is presentation',
    ('b40', 'cutscene_insertion_b'): 'camera_control false: the first-person Pelican ride after cutscene_insertion_a is gameplay',
    ('c10', 'c10_cinematic_placeholder'): 'never called',
    ('c10', 'cinematic_placeholder_endgame'): 'never called',
    ('c10', 'mission_control'): 'the gameplay half of x50 (the Exit cue it calls): the clean-up and the Flood ambush after it',
}

# --- Static: the cue table covers every scene.
source = (root / 'source/src/halo_script.cpp').read_text()
rows = re.findall(r'\{(\d+), (-?\d+), (\d+), "(\w+)", SceneTrigger::(\w+)', source)
assert rows, 'cue table not found'
cued = {}
for mission, section, bsp, name, trigger in rows:
    cued.setdefault(IDS[int(mission)], set()).add(name)
require(HALO_SOURCE / 'a10.map', 'set HALO_SOURCE to the expanded Xbox maps')
calls = json.loads(subprocess.run([sys.executable, str(root / 'tests/tools/hs_conditions.py'), '--all'],
                                  capture_output=True, text=True, check=True).stdout)
uncued = []
for cache, (found, names) in calls.items():
    scenes = set(found.get('cinematic_start', [])) | set(found.get('camera_control', []))

    def reached(script, seen):
        # A cue row's script, or called (through any chain) from one.
        if script in cued.get(cache, ()):
            return True
        for caller in found.get(script, []):
            if caller in names and caller not in seen:
                seen.add(caller)
                if reached(caller, seen):
                    return True
        return False

    for scene in sorted(scenes):
        if not reached(scene, {scene}) and (cache, scene) not in NOT_CUED:
            uncued.append((cache, scene, found.get(scene)))
    for row in cued.get(cache, ()):
        assert row in names, (cache, row, 'cue row names no script of the mission')
assert not uncued, ('scene scripts with no cue row', uncued)
print(f'{sum(len(v) for v in cued.values())} cued scenes cover every camera script of the ten missions '
      f'({len(NOT_CUED)} listed as gameplay).')

# --- Static (gap 3): the gameplay scripts each cued scene wakes. A scene's thread that wakes one of
# these starts it on the scene stage; endCue moves those still running onto the gameplay threads
# (Scenario::sceneWoken), and a seat one of them gives the player (b40's pelican_up) is taken in play.
SCENE_WAKES = {
    ('a10', 'x10'): {'tank_run_1', 'tank_run_2', 'x10_crew_panic_1', 'x10_crew_panic_2', 'x10_crew_panic_3', 'x10_crew_panic_4',
                     'x10_crew_panic_5', 'x10_crew_panic_6', 'x10_crew_salute_1', 'x10_crew_salute_2', 'x10_crew_walk_1',
                     'x10_crew_walk_2', 'x10_crew_walk_3', 'x10_crew_walk_4', 'x10_hog_1', 'x10_hog_2', 'x10_hog_3', 'x10_hog_4',
                     'x10_hog_5', 'x10_tankpel'},
    ('a10', 'x30'): {'approach_setup', 'heavy_fire_1', 'heavy_fire_2', 'pass_pyro', 'safe_pyro'},
    ('a30', 'cutscene_bridge'): {'save_cave_bridge'},
    ('a50', 'cutscene_insertion'): {'dialog_insertion'},
    ('b30', 'cutscene_extraction_exit'): {'extraction_music', 'pelican_fade'},
    ('b30', 'cutscene_insertion'): {'music_b30', 'music_b30_01', 'music_b30_02', 'music_b30_03', 'music_b30_031', 'music_b30_032',
                                    'music_b30_05', 'music_b30_06'},
    ('b30', 'cutscene_ledge'): {'ledge_music'},
    ('b40', 'cutscene_extraction'): {'extraction_music'},
    ('b40', 'cutscene_insertion_a'): {'pelican_up'},
    ('c10', 'cutscene_extraction'): {'extraction_music'},
    # x50 ends section 5: door_open opens the lab's control_door_a and control_infection places Flood
    # (dropped on the scene stage) in BSP 3, both finished inside the scene; none of them moves a door
    # of section 6 (BSP 4), whose route has no door rows.
    ('c10', 'x50'): {'control_infection', 'door_open', 'the_horror'},
    ('c40', 'cutscene_extraction'): {'extraction_music'},
    ('c40', 'cutscene_insertion'): {'animation_fix_1', 'chief_puzzled', 'chip_insert', 'insertion_music_1'},
    ('d20', 'cinematic_captain'): {'captain_music'},
    ('d20', 'cinematic_drop'): {'fall_music'},
    ('d20', 'cinematic_intro'): {'oops'},
    ('d40', 'cinematic_bridge'): {'bridge_music_1', 'chief_duck', 'engine_sentinels_1', 'engine_sentinels_2', 'sentinels_enter_1',
                                  'sentinels_enter_2', 'sentinels_fire'},
    ('d40', 'cinematic_finale'): {'flood_army_pyros', 'flood_chase', 'hangar_pyrotechnics_1', 'hangar_pyrotechnics_2', 'helmet',
                                  'launch_2_pyro', 'sit_pyrotechnics', 'x80_elite_speech'},
    ('d40', 'cinematic_intro'): {'insertion_dialog', 'insertion_music'},
}
SCENE_SEATS = {('b40', 'cutscene_insertion_a'): {'pelican_up'}}
sys.path.insert(0, str(root / 'tools'))
import mmap  # noqa: E402
from import_halo_a30 import HaloMap  # noqa: E402


def scene_wakes(cache):
    """Per cued scene of the mission, the non-cue scripts its threads wake, and those that seat player0."""
    with (HALO_SOURCE / f'{cache}.map').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        h = HaloMap(m)
        t, a = h.tags, h.scenario
        p, s, ln = t.read('I', a + 1152)[0], t.read('I', a + 1172)[0], t.read('I', a + 1160)[0]
        roots = [(t.name(b, 32), *t.read('HHI', b + 32)) for b in t.block(a + 1180, 92)]
        nodes = [t.read('4H3I', p + 56 + 20 * i) for i in range((t.read('I', a + 1140)[0] - 56) // 20)]

        def name(i):
            return t.name(s + i, 256) if i < ln else ''

        def children(i):
            data, out = nodes[i & 65535][6], []
            while data != 0xffffffff:
                out.append(data)
                data = nodes[data & 65535][4]
            return out

        def visit(i, calls, wakes, seats):
            _, op, _, flag, _, _, _ = nodes[i & 65535]
            if flag & 1:
                return
            if flag & 2:
                calls.add(op)
            kids = children(i)
            if not kids:
                return
            head = nodes[kids[0] & 65535]
            head_name = name(head[5]) if head[3] & 1 else ''
            if head_name == 'wake' and len(kids) > 1:
                arg = nodes[kids[1] & 65535]
                if arg[3] & 1 and arg[1] == 10:
                    wakes.add(arg[6] & 65535)
            if head_name == 'unit_enter_vehicle' and len(kids) > 1:
                unit = nodes[kids[1] & 65535]
                if not unit[3] & 1 and unit[3] & 2 and roots[unit[1]][0] == 'player0':
                    seats.add(True)
            for kid in kids[1:]:
                visit(kid, calls, wakes, seats)

        index = {r[0]: k for k, r in enumerate(roots)}
        woken, seating = {}, {}
        for scene in sorted(cued.get(cache, ())):
            if scene not in index:
                continue
            todo, seen = [index[scene]], set()
            while todo:
                k = todo.pop()
                if k in seen:
                    continue
                seen.add(k)
                calls, wakes, seats = set(), set(), set()
                visit(roots[k][3], calls, wakes, seats)
                todo += [c for c in calls if c < len(roots) and roots[c][1] == 3]
                for w in wakes:
                    if roots[w][0] not in cued[cache]:
                        woken.setdefault((cache, scene), set()).add(roots[w][0])
                    todo.append(w)
                if seats and roots[k][0] in woken.get((cache, scene), ()):
                    seating.setdefault((cache, scene), set()).add(roots[k][0])
        return woken, seating


found_wakes, found_seats = {}, {}
for cache in IDS:
    woken, seating = scene_wakes(cache)
    found_wakes.update(woken)
    found_seats.update(seating)
assert found_wakes == SCENE_WAKES, ('the gameplay scripts the scenes wake changed',
                                    {k: found_wakes.get(k) for k in set(found_wakes) | set(SCENE_WAKES) if found_wakes.get(k) != SCENE_WAKES.get(k)})
assert found_seats == SCENE_SEATS, ('a woken script seats the player', found_seats)
print(f'{sum(len(v) for v in SCENE_WAKES.values())} gameplay scripts are woken by {len(SCENE_WAKES)} scenes; '
      'pelican_up alone seats the player.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/maps/halo_d40_08.cgz', 'import the campaign (docs/macos-build.md)')


def run(name, start, schedule, timeout=180):
    profile = scratch(f'exits-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_scripttrace 1; halo_difficulty 1; {start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def player(text, tag='QA_PLAYER'):
    found = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith(tag)]
    assert found, text[-2000:]
    return found[-1]


# 1. Each mission's exit scene, then the next mission (or the ending).
EXIT_SCENES = [
    (1, 7, ('x30',)),
    (2, 3, ('mission_extraction_river_skip', 'mission_extraction_cliff_skip', 'mission_extraction_rubble_skip')),
    (3, 5, ('cutscene_extraction',)),
    (4, 3, ('cutscene_extraction_exit',)),
    (5, 7, ('cutscene_extraction',)),
    (6, 7, ('cutscene_extraction',)),
    (7, 4, ('cutscene_extraction',)),
    (8, 5, ('cutscene_extraction',)),
    (9, 6, ('cinematic_outro',)),
    (10, 8, ('cinematic_finale',)),
]
for mission, section, scenes in EXIT_SCENES:
    # The first Enter skips the exit scene, the second loads the next mission (or, after the
    # finale, ends the credits: the title menu that follows pauses the timers, so quit then).
    schedule = [(300, 'qa_halo_finale'), (1500, 'qa_halo_ending; halo_next')]
    if mission < 10:
        schedule += [(2500, 'halo_next'), (14000, 'qa_halo_where; quit')]
    else:
        schedule += [(2500, 'qa_halo_ending; halo_next; qa_halo_ending; quit')]
    text, log = run(f'm{mission}', f'qa_campaign_play {mission} {section}', schedule, timeout=240)
    started = [s for s in scenes if f'Halo scene: {s} start' in text]
    assert len(started) == 1, (mission, scenes, log)
    assert f'Halo scene: {started[0]} skipped' in text or f'Halo scene: {started[0]} finished' in text, (mission, log)
    if mission < 10:
        assert f'Halo: {TITLES[mission]}, section 1' in text, (mission, 'the next mission did not start', log)
    else:
        endings = [line for line in text.splitlines() if line.startswith('QA_ENDING')]
        assert 'Halo ending reached' in text and 'Halo credits finished' in text, log
        # After the credits the title menu is up.
        assert 'done=1' in endings[-1] and 'title=1' in endings[-1], endings
    print(f'{TITLES[mission - 1]}: {started[0]} played at the exit, then '
          f'{TITLES[mission] + " began" if mission < 10 else "the ending"}.')

# 2. a30: the extraction follows the player's site.
for site, scene in (('river_hide_flag', 'mission_extraction_river_skip'), ('cliff_hide_flag', 'mission_extraction_cliff_skip'),
                    ('rubble_hide_flag', 'mission_extraction_rubble_skip')):
    text, log = run(f'a30-{site}', 'qa_campaign_play 2 3', [(300, f'qa_halo_flag {site}; qa_halo_finale'), (900, 'quit')])
    assert f'QA_FLAG {site} 1' in text, log
    assert f'Halo scene: {scene} chosen for the exit' in text and f'Halo scene: {scene} start' in text, (site, log)
    print(f'a30: standing by {site} chose {scene}.')

# 3. Scenes the scripts call mid-section, and where they leave the player.
CALLS = [
    # (mission, section, scene, flag the scene or the script after it puts the player at, script to wake after)
    (9, 5, 'cinematic_captain', 'player0_capt_done', None),
    (10, 2, 'cinematic_bridge', 'player0_playon_base', None),
    (3, 4, 'cutscene_rescue', 'prison_player0_teleport', 'ini_post_rescue'),
]
for mission, section, scene, flag, after in CALLS:
    schedule = [(300, f'qa_hs_cue {scene}'), (1200, 'qa_halo_where'), (1500, 'halo_next'), (2500, 'qa_halo_where')]
    if after:
        # The script after the scene is static (called by the thread that called the scene, which a
        # QA cue has none of): run it as a thread of its own (qa_hs_script action 3).
        schedule.append((2800, f'qa_hs_script {after} 3'))
    schedule += [(4000, 'qa_halo_where; qa_halo_finale'), (5000, 'qa_halo_where; quit')]
    text, log = run(f'call-{scene}', f'qa_campaign_play {mission} {section}', schedule)
    assert f'QA_CUE {scene} 1' in text and f'Halo scene: {scene} start' in text, log
    assert f'Halo scene: {scene} skipped' in text, log
    assert f'Halo script: player placed at {flag}' in text, (scene, flag, log)
    assert 'has no floor in this section' not in text, log
    lines = [line for line in text.splitlines() if line.startswith('QA_PLAYER')]
    assert 'cinematic=1' in lines[0] and 'cinematic=0' in lines[1], lines
    # The section's exit does not play the scene a second time.
    assert text.count(f'Halo scene: {scene} start') == 1, (scene, log)
    print(f'{TITLES[mission - 1]}: {scene} played on the call and left the player at {flag}.')

# 4. A skipped Call scene still does what it does for the play after it: b30's shaft B scene
#    closes shafta_lock_door, then opens it later on; skipped at once, the door still ends open.
text, log = run('skip-shaftb', 'qa_campaign_play 4 1',
                [(300, 'qa_hs_cue cutscene_shaftb_goal'), (600, 'halo_next'),
                 (9000, 'qa_hs_cmd device_get_position obj:shafta_lock_door; quit')])
assert 'QA_CUE cutscene_shaftb_goal 1' in text and 'Halo scene: cutscene_shaftb_goal skipped' in text, log
assert 'did not run to its end when skipped' not in text, log
positions = [float(line.split()[-1]) for line in text.splitlines() if line.startswith('HS_CMD device_get_position')]
assert positions and positions[-1] > 0.5, (positions, log)
print('The Silent Cartographer: cutscene_shaftb_goal skipped at once still opened shafta_lock_door.')

# 5. A Call scene gives back the gameplay music it cut and finishes the conversation it cut (gap 12,
#    written while testing was on hold, not yet run): d40's bridge scene with one of d40's looping
#    sounds started first, a50's rescue with a conversation being spoken.
def audio(text, label):
    found = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith(f'AUDIO_QA {label} ')]
    assert found, (label, text[-2000:])
    return found[-1]


loop = read_hscb(root / 'packages/halo/d40.hscb')['loops'][-1]
text, log = run('call-music', 'qa_campaign_play 10 2', [
    (300, f'qa_hs_cmd sound_looping_start "{loop} 0 1"; qa_audio_state before'), (500, 'qa_hs_cue cinematic_bridge'),
    (800, 'qa_audio_state during'), (1000, 'halo_next'), (1600, 'qa_audio_state after; quit')])
assert str(loop) in audio(text, 'before')['loops'], (loop, log)
assert audio(text, 'during')['scene'] == 'cinematic_bridge' and str(loop) in audio(text, 'during')['restart'], (loop, log)
# (If the bridge scene stops this loop itself, it is right that it stays stopped: pick another loop.)
assert audio(text, 'after')['scene'] == '-' and str(loop) in audio(text, 'after')['loops'], (loop, log)
conversations = read_hscb(root / 'packages/halo/a50.hscb')['conversations']
talk = conversations[0]
text, log = run('call-talk', 'qa_campaign_play 3 4', [
    (300, f'qa_hs_cmd ai_conversation "{conversations.index(talk)}"; qa_conversation_state {talk}'),
    (500, 'qa_hs_cue cutscene_rescue'), (800, 'halo_next'), (1400, f'qa_conversation_state {talk}; quit')])
states = [line.split()[-1] for line in text.splitlines() if line.startswith(f'QA_CONVERSATION {talk} ')]
assert states[:1] == ['2'] and states[-1] == '5', (talk, states, log)
print(f'A Call scene restarted gameplay loop {loop} after it and finished the conversation {talk} it cut.')
# 6. Gap 3: a gameplay script a scene woke goes on in play (b30's music director, woken on the
#    insertion's first tick). Plain sleeps from launch, so the scene runs a while before qa_scene_skip
#    skips it (qa_after_gameplay skips it at once, possibly before its first tick).
profile = scratch('exits-scene-wakes-')
log = profile / 'run.log'
with log.open('w') as output:
    subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                    '-ehalo_scripttrace 1; halo_difficulty 1; qa_campaign_play 4 1; sleep 12000 [qa_scene_skip] 1; '
                    'sleep 13000 [qa_hs_script music_b30 1; qa_hs_script music_b30 2] 1; sleep 13500 [quit] 1'],
                   cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=120, check=True)
text = log.read_text(errors='replace')
assert 'QA_SKIP 1' in text and 'Halo scene: cutscene_insertion skipped' in text, ('the insertion was playing at 12 s', log)
assert 'Halo scene: music_b30, woken by cutscene_insertion, goes on in play' in text, log
states = [line.split()[-1] for line in text.splitlines() if line.startswith('HS_SCRIPT music_b30 ')]
assert states[:2] == ['1', '0'], ('music_b30 runs as a gameplay thread, waiting for its cues', states, log)
print('The Silent Cartographer: music_b30, woken by the skipped insertion, went on in play.')
print('Mission exits, the finale and script-called scenes behave.')
