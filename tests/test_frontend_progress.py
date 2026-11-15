"""Title menu, saved games, loadout carry-over and progress file backups.

QA client, throwaway profiles. NOT YET RUN: written while testing was on hold.

1. A fresh profile launched with halo_title shows the title (menu "Halo"), nothing to continue,
   one mission unlocked.
2. New Game on Heroic starts The Pillar of Autumn at Heroic and saves the game at its start
   (progress file version 3, valid check line, `game 0 0 2 0 0`, no state block: the mission's
   first section starts its script afresh anyway).
3. A checkpoint saves position, loadout and difficulty, and (version 3) the mission script's state:
   a state block for this section with script, ground-item and player records whose count and
   hash hold. Before the checkpoint the test opens cryo_door_1 and sets the script global
   delay_lost to 1234. A second launch's Continue restores the loadout (frags, plasma grenades,
   rounds, place within 1.5 units, difficulty) and the script where it was: the door's position,
   the global, and a script clock no earlier than the checkpoint's (a restarted script would be
   back near tick 0); it saves again, keeping the previous good file as halo-progress.txt.bak.
4. Finishing a section carries the loadout into the next section of the same mission, and the
   save at that section's start keeps the script carried in (a state block with script and
   player records only); Continue from it resumes that script on a10 section 2.
5. A corrupt progress file with a good backup: the backup is restored, the corrupt file is kept as
   .corrupt, the log and the title menu warn, and the progress file is rewritten valid.
6. A corrupt progress file without a backup: progress is reset, with the warning.
7. A version 1 file ("1 3 1") still loads: furthest b30 section 2, four missions unlocked, and the
   mission select greys out the rest.
8. The checkpoint save of item 3 with another .hscb hash in its state header (as after
   re-importing the scripts): the Continue entry says the scripts changed, and Continue falls back
   to restarting the script at the checkpoint (the old behaviour) with a warning in the log and the
   menus; the global is back to its initial value.
9. The same save with one script record altered (the block's hash no longer holds): Continue
   warns that the saved state is damaged and falls back the same way.
10. A version 2 file with a checkpoint (no state block) still continues at the checkpoint, the
   script restarted, as before.
11. A save taken in a vehicle seat (b30 section 1, the beach Warthog at the wheel) continues in
   that seat. Skipped when b30 or vehicles.txt is not imported.
12. Live play: a death before the first checkpoint of a carried-in section (a10 section 2,
   entered from section 1) reverts to the script as it came in, as Continue from that section's
   start does: the script is not loaded again and its clock does not go back to zero.
13. The warning about a corrupt progress file (item 5) is cleared once New Game starts a game.
14. The state block's optional records (batch 8, not yet run): a checkpoint on a30 section 3 with
   foehammer_cliff created and made enterable writes `s board` (Continue keeps it boardable, so the
   scripts' seat test asks whether the player boarded instead of falling back to true) and, after
   sound_class_set_gain music 0.25, `s gain` (Continue keeps the music class at 0.25); a checkpoint on
   a50 section 4 after captain_keyes is attached writes `s attached` and Continue brings the actor back.
"""
import shutil
import struct
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/maps/halo_a10_01.cgz', 'import the campaign (docs/macos-build.md)')


def run(name, script, profile=None, timeout=120):
    profile = profile or scratch(f'frontend-{name}-')
    log = profile / f'{name}.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600', f'-e{script}'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), profile


def rows(text, tag):
    return [line for line in text.splitlines() if line.startswith(tag)]


def field(row, key):
    for part in row.split():
        if part.startswith(key + '='):
            return part[len(key) + 1:]
    raise AssertionError(f'{key} missing in {row}')


progress_file = 'config/halo-progress.txt'


def hscb_globals(path):
    """The global names of a compiled mission script (packages/halo/<cache>.hscb, see Program::load)."""
    data = Path(path).read_bytes()
    assert data[:4] == b'HSCB', path
    offset = 8

    def number(fmt):
        nonlocal offset
        value = struct.unpack_from(fmt, data, offset)[0]
        offset += struct.calcsize(fmt)
        return value

    def text():
        nonlocal offset
        size = number('<H')
        offset += size
        return data[offset - size:offset].decode('latin-1')

    offset += 4 + 20 * number('<I')          # nodes
    offset += number('<I')                   # strings
    for _ in range(number('<I')):            # scripts
        text(); number('<H'); number('<H'); number('<I')
    names = []
    for _ in range(number('<I')):            # globals
        names.append(text()); number('<H'); number('<I')
    return names


def rewrite_progress(profile, edit):
    """Rewrites a profile's progress file through edit(lines), with a valid outer check line."""
    path = Path(profile) / progress_file
    lines = path.read_text().splitlines()
    assert lines[0].startswith('halo-progress ') and lines[-1].startswith('check '), path
    body = edit(lines[1:-1])
    write_progress(path, body, version=int(lines[0].split()[1]))


assert 'delay_lost' in hscb_globals(root / 'packages/halo/a10.hscb'), 'a10.hscb has no delay_lost global'

# 1. Title on a fresh profile.
text, _ = run('title', 'halo_title; sleep 600 [qa_title_state; qa_progress_state] 1; sleep 900 [quit] 1')
title = rows(text, 'QA_TITLE')
assert title and 'title=1' in title[0] and 'menu=Halo ' in title[0] and 'continue=0' in title[0] and 'unlocked=1' in title[0], title
print('The title menu opens on a fresh profile with nothing to continue.')

# 2. New Game on Heroic.
text, profile = run('newgame', 'halo_title; sleep 300 [halo_new_game 2] 1; '
                    'qa_after_gameplay [qa_loadout_state start; sleep 300 [qa_progress_state; quit] 1]')
start = rows(text, 'QA_LOADOUT')
assert start and 'map=halo_a10_01' in start[0] and 'difficulty=2' in start[0], start
saved = read_progress(profile)
assert saved and saved['furthest'] == ['0', '0'] and saved['game'] == ['0', '0', '2', '0', '0'], saved
assert saved['version'] == ['3'] and 'state' not in saved, ('the first section of a mission keeps no script state', saved.get('state'))
print('New Game on Heroic started a10 and saved the game at its start.')

# 3. Checkpoint, then Continue in a second launch.
SCRIPT_PROBES = 'qa_hs_state {0}; qa_hs_global delay_lost; qa_hs_cmd device_get_position "obj:cryo_door_1"'
text, profile = run('checkpoint', 'halo_scripttrace 1; halo_title; sleep 300 [halo_new_game 2] 1; '
                    'qa_after_gameplay [qa_loadout_mark 3 2 17 40; qa_halo_move 1; sleep 700 [qa_halo_move 0] 1; '
                    'sleep 2300 [qa_hs_cmd device_set_position_immediate "obj:cryo_door_1 1"; qa_hs_global delay_lost 1234] 1; '
                    'sleep 2400 [qa_checkpoint_save; qa_loadout_state saved; ' + SCRIPT_PROBES.format('saved') + '; qa_progress_state] 1; '
                    'sleep 2700 [quit] 1]')
saved_row = rows(text, 'QA_LOADOUT saved')
assert saved_row and 'checkpoint=1' in saved_row[0], rows(text, 'QA_LOADOUT')
saved = read_progress(profile)
assert saved['game'][3] == '1' and 'checkpoint' in saved, saved
assert saved['grenades'][:2] == ['3', '2'], saved
# The state block: this section, at the checkpoint, with the script, ground and player records.
assert saved['version'] == ['3'] and saved['state'][:4] == ['1', '0', '0', '1'], saved.get('state')
records = saved['records']
assert records['s'] and records['w'] and len(records['p']) == 2, {k: len(v) for k, v in records.items()}
assert any(r.startswith('global ') and ' =delay_lost ' in r and r.split()[3] == '1234' for r in records['s']), 'delay_lost not saved'
assert any(r.startswith('items ') for r in records['w']), records['w'][:5]
saved_tick = int(field(rows(text, 'QA_HS_STATE saved')[0], 'tick'))
saved_door = rows(text, 'HS_CMD device_get_position')[-1].split()[-1]
assert saved_tick > 30, ('the script ran before the checkpoint', saved_tick)
text, profile = run('continue', 'halo_scripttrace 1; halo_title; sleep 300 [qa_title_state; halo_continue] 1; '
                    'qa_after_gameplay [qa_loadout_state continued; ' + SCRIPT_PROBES.format('continued') + '; sleep 200 [quit] 1]',
                    profile)
assert 'continue=1' in rows(text, 'QA_TITLE')[0], rows(text, 'QA_TITLE')
assert 'Halo: continued from the checkpoint' in text and 'with the mission script where it was' in text, text[-2000:]
assert 'could not be restored' not in text, text[-2000:]
assert rows(text, 'QA_HS_GLOBAL delay_lost')[-1].split()[-1] == '1234.0000', rows(text, 'QA_HS_GLOBAL')
assert rows(text, 'HS_CMD device_get_position')[-1].split()[-1] == saved_door, (saved_door, rows(text, 'HS_CMD'))
assert int(field(rows(text, 'QA_HS_STATE continued')[0], 'tick')) >= saved_tick, (saved_tick, rows(text, 'QA_HS_STATE'))
assert field(rows(text, 'QA_HS_STATE continued')[0], 'atcheckpoint') == '1', rows(text, 'QA_HS_STATE')
cont = rows(text, 'QA_LOADOUT continued')[0]
assert field(cont, 'frags') == '3' and field(cont, 'plasmas') == '2' and field(cont, 'difficulty') == '2', cont
assert '17/40' in field(cont, 'w0' if field(cont, 'current') == '0' else 'w1'), cont
was = [float(v) for v in field(saved_row[0], 'pos').split(',')]
now = [float(v) for v in field(cont, 'pos').split(',')]
assert sum((a - b) ** 2 for a, b in zip(was, now)) ** .5 < 1.5, (was, now)
assert (profile / (progress_file + '.bak')).exists(), 'the previous good file is kept as the backup'
resaved = read_progress(profile)
assert resaved['state'][:4] == ['1', '0', '0', '1'], 'the save after Continue keeps the script state'
checkpoint_profile = scratch('frontend-checkpoint-copy-')
shutil.copytree(profile / 'config', checkpoint_profile / 'config')
print('Continue restored the checkpoint position, loadout, difficulty and the mission script; the save kept a backup.')

# 4. The loadout carries into the next section of the mission.
text, profile = run('carry', 'halo_scripttrace 1; halo_difficulty 2; qa_campaign_play 1 1; '
                    'qa_after_gameplay [qa_loadout_mark 4 1 21 50; qa_loadout_state before; qa_halo_finale; '
                    # A second Enter skips an exit scene if the first one started it.
                    'sleep 500 [halo_next] 1; sleep 1500 [halo_next] 1; sleep 7000 [qa_loadout_state after; quit] 1]', timeout=150)
after = rows(text, 'QA_LOADOUT after')
assert after and 'map=halo_a10_02' in after[0], rows(text, 'QA_LOADOUT')
assert field(after[0], 'frags') == '4' and field(after[0], 'plasmas') == '1', after
assert 'Halo: game saved on The Pillar of Autumn, section 2 (section start, with the mission script)' in text, \
    ('the save at section 2\'s start keeps the script carried in', text[-2000:])
saved = read_progress(profile)
assert saved['game'][:2] == ['0', '1'], saved
# The section's scripts may have taken a checkpoint since its start; the file holds the latest save.
at_start = saved['state'][:4] == ['1', '0', '1', '0']
if at_start:
    assert saved['records']['s'] and not saved['records']['w'] and not saved['records']['v'] and len(saved['records']['p']) == 2, \
        {k: len(v) for k, v in saved['records'].items()}
else:
    assert saved['state'][:4] == ['1', '0', '1', '1'], saved.get('state')
text, profile = run('carry-continue', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [qa_hs_state continued; '
                    'qa_loadout_state continued; sleep 200 [quit] 1]', profile)
assert ('with the mission script carried in from the section before' if at_start else 'with the mission script where it was') in text, \
    text[-2000:]
assert 'map=halo_a10_02' in rows(text, 'QA_LOADOUT continued')[0], rows(text, 'QA_LOADOUT')
print('The loadout carried from a10 section 1 into section 2, and Continue resumed the script carried in.')

# 5. Corrupt file with a good backup.
profile = scratch('frontend-corrupt-')
write_progress(profile / (progress_file + '.bak'), ['furthest 2 1', 'beaten 0', 'game 2 1 1 0 0', 'vitals 60 40.000',
                                                   'grenades 2 0 0', 'current 0', 'stock 1 60 180 12 48'])
(profile / progress_file).write_text('halo-progress 2\nfurthest 2 1\nbeaten 0\ncheck deadbeef\n')
text, profile = run('corrupt', 'halo_title; sleep 500 [qa_title_state; qa_progress_state] 1; sleep 800 [quit] 1', profile)
assert 'campaign progress was corrupt' in text and 'restored from the backup' in text, text[-2000:]
state = rows(text, 'QA_PROGRESS')[0]
assert 'furthest=2,1' in state and 'game=1' in state, state
assert 'restored from the backup' in rows(text, 'QA_PROGRESS_NOTE')[0], rows(text, 'QA_PROGRESS_NOTE')
assert 'restored from the backup' in rows(text, 'QA_TITLE')[0]
assert (profile / (progress_file + '.corrupt')).exists()
assert read_progress(profile)['furthest'] == ['2', '1']
print('A corrupt progress file was replaced by its backup, with a warning in the log and the menu.')

# 6. Corrupt file and no usable backup.
profile = scratch('frontend-corrupt-nobak-')
(profile / 'config').mkdir(parents=True)
(profile / progress_file).write_text('garbage\n')
text, profile = run('nobackup', 'halo_title; sleep 500 [qa_progress_state] 1; sleep 800 [quit] 1', profile)
state = rows(text, 'QA_PROGRESS')[0]
assert 'furthest=0,0' in state and 'game=0' in state, state
assert 'progress was reset' in rows(text, 'QA_PROGRESS_NOTE')[0], rows(text, 'QA_PROGRESS_NOTE')
assert (profile / (progress_file + '.corrupt')).exists()
print('A corrupt progress file without a backup reset progress, with a warning.')

# 7. Version 1 files still load; mission select offers the missions reached.
profile = scratch('frontend-v1-')
(profile / 'config').mkdir(parents=True)
(profile / progress_file).write_text('1 3 1\n')
text, profile = run('legacy', 'halo_title; sleep 500 [qa_progress_state; echo MISSIONS (halo_mission_state 4) (halo_mission_state 5)] 1; '
                    'sleep 800 [quit] 1', profile)
state = rows(text, 'QA_PROGRESS')[0]
assert 'furthest=3,1' in state and 'game=1' in state and 'at=3,1' in state and 'unlocked=4' in state, state
assert any(line.strip().endswith('MISSIONS 1 0') for line in text.splitlines()), text[-1500:]
print('A version 1 progress file loaded; mission select unlocks the four missions reached.')

# 8. The state block was saved with another .hscb: Continue falls back to restarting the script.
profile = scratch('frontend-stale-')
shutil.copytree(checkpoint_profile / 'config', profile / 'config')
rewrite_progress(profile, lambda lines: [' '.join(l.split()[:5] + ['deadbeef'] + l.split()[6:]) if l.startswith('state ') else l
                                         for l in lines])
text, profile = run('stale', 'halo_scripttrace 1; halo_title; sleep 300 [echo LABEL (halo_continue_label); halo_continue] 1; '
                    'qa_after_gameplay [qa_hs_global delay_lost; qa_loadout_state continued; qa_progress_state; sleep 200 [quit] 1]',
                    profile)
assert any('LABEL' in line and 'scripts changed' in line for line in text.splitlines()), 'the Continue entry warns the scripts changed'
assert 'could not be restored' in text and 'mission scripts were re-imported' in text, text[-2000:]
assert 'Halo: continued from the checkpoint on' in text and 'with the mission script where it was' not in text, text[-2000:]
assert rows(text, 'QA_HS_GLOBAL delay_lost')[-1].split()[-1] != '1234.0000', 'the script restarted: delay_lost is back to its initial value'
assert 'could not be restored' in rows(text, 'QA_PROGRESS_NOTE')[0], ('the menus show the warning', rows(text, 'QA_PROGRESS_NOTE'))
cont = rows(text, 'QA_LOADOUT continued')[0]
assert field(cont, 'checkpoint') == '1' and field(cont, 'frags') == '3', cont
print('A state block from another .hscb was refused with a warning; Continue restarted the script at the checkpoint.')

# 9. One script record altered: the block's own hash no longer holds.
profile = scratch('frontend-damaged-')
shutil.copytree(checkpoint_profile / 'config', profile / 'config')


def damage(lines):
    out, done = [], False
    for line in lines:
        if not done and line.startswith('s global '):
            line, done = line + '0', True
        out.append(line)
    assert done
    return out


rewrite_progress(profile, damage)
text, profile = run('damaged', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [qa_loadout_state continued; sleep 200 [quit] 1]',
                    profile)
assert 'could not be restored' in text and 'the saved state is damaged' in text, text[-2000:]
assert 'Halo: continued from the checkpoint on' in text and 'checkpoint=1' in rows(text, 'QA_LOADOUT continued')[0], text[-2000:]
print('A damaged state block was refused with a warning; Continue restarted the script at the checkpoint.')

# 10. A version 2 file with a checkpoint (no state block) still continues there.
profile = scratch('frontend-v2-')
saved = read_progress(checkpoint_profile)
write_progress(profile / progress_file, ['furthest 0 0', 'beaten 0', 'game 0 0 2 1 0', 'checkpoint ' + ' '.join(saved['checkpoint']),
                                         'vitals 60 40.000', 'grenades 3 2 0', 'current 0', 'stock 1 60 180 12 48'])
text, profile = run('v2', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [qa_loadout_state continued; sleep 200 [quit] 1]',
                    profile)
assert 'holds no mission script state' in text and 'Halo: continued from the checkpoint on' in text, text[-2000:]
assert 'checkpoint=1' in rows(text, 'QA_LOADOUT continued')[0], rows(text, 'QA_LOADOUT')
assert read_progress(profile)['version'] == ['3'], 'the save after Continue is version 3'
print('A version 2 save continued at its checkpoint with the script restarted, and was saved again as version 3.')

# 11. A save in a vehicle seat continues in that seat (b30 section 1, the beach Warthog).
if not (root / 'packages/maps/halo_b30_01.cgz').exists() or not (root / 'packages/halo/vehicles.txt').exists():
    print('SKIP seated save: b30 or packages/halo/vehicles.txt is not imported.')
else:
    def vehicle_rows(text, label):
        found = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith(f'VEHICLE_QA {label} ')]
        assert found, (label, text[-3000:])
        return found[0], found[1:]

    text, profile = run('seat-find', 'halo_vehicletrace 1; qa_campaign_play 4 1; qa_after_gameplay [qa_vehicle_state find; sleep 300 [quit] 1]')
    _, vehicles = vehicle_rows(text, 'find')
    hog = next(int(v['vehicle']) for v in vehicles if v.get('type') == 'warthog' and v.get('exists') == '1')
    text, profile = run('seat-save', f'halo_vehicletrace 1; qa_campaign_play 4 1; qa_after_gameplay [qa_vehicle_enter {hog} 0; '
                        'sleep 600 [qa_vehicle_state saved; qa_checkpoint_save] 1; sleep 900 [quit] 1]', timeout=150)
    head, _ = vehicle_rows(text, 'saved')
    assert head['seated'] == '1', head
    saved = read_progress(profile)
    seats = [int(r.split()[34]) for r in saved['records']['v']]   # the player's seat, -1 for none
    assert seats.count(-1) == len(seats) - 1, ('one vehicle record holds the player\'s seat', seats)
    text, profile = run('seat-continue', 'halo_vehicletrace 1; halo_title; sleep 300 [halo_continue] 1; '
                        'qa_after_gameplay [qa_vehicle_state continued; sleep 200 [quit] 1]', profile, timeout=150)
    head, _ = vehicle_rows(text, 'continued')
    assert head['seated'] == '1' and head['vehicle'] == str(hog) and head['role'] == '0', head
    print('A save at the wheel of the beach Warthog continued at the wheel.')

# 12. Live play: a death before a carried-in section's first checkpoint keeps the carried script.
text, profile = run('carried-revert', 'halo_scripttrace 1; halo_difficulty 2; qa_campaign_play 1 1; '
                    'qa_after_gameplay [qa_halo_finale; sleep 500 [halo_next] 1; sleep 1500 [halo_next] 1; '
                    'sleep 7000 [qa_hs_state carried; qa_loadout_state carried] 1; sleep 7200 [qa_halo_damage 100000] 1; '
                    'sleep 8800 [qa_halo_revert] 1; sleep 10500 [qa_hs_state reverted; qa_loadout_state reverted; quit] 1]',
                    timeout=150)
assert 'map=halo_a10_02' in rows(text, 'QA_LOADOUT carried')[0], rows(text, 'QA_LOADOUT')
assert 'HaloScript: a10 resumed on section 2' in text, text[-2000:]
# A restart on the section would log the script loading again and set its clock back near zero.
assert text.count('HaloScript: a10 loaded') == 1, ('the revert restarted the script', text[-3000:])
carried_tick = int(field(rows(text, 'QA_HS_STATE carried')[0], 'tick'))
reverted = rows(text, 'QA_HS_STATE reverted')[0]
assert int(field(reverted, 'tick')) > 30, ('the revert went back to the carried script, not tick 0', carried_tick, reverted)
assert 'map=halo_a10_02' in rows(text, 'QA_LOADOUT reverted')[0], rows(text, 'QA_LOADOUT')
print('A death in a carried-in section reverted to the script as it came in.')

# 13. The corrupt-file warning is cleared once a game starts.
profile = scratch('frontend-note-')
write_progress(profile / (progress_file + '.bak'), ['furthest 2 1', 'beaten 0', 'game 2 1 1 0 0', 'vitals 60 40.000',
                                                   'grenades 2 0 0', 'current 0', 'stock 1 60 180 12 48'])
(profile / progress_file).write_text('halo-progress 2\nfurthest 2 1\nbeaten 0\ncheck deadbeef\n')
text, profile = run('note', 'halo_title; sleep 500 [qa_title_state] 1; sleep 800 [halo_new_game 2] 1; '
                    'qa_after_gameplay [qa_title_state; sleep 200 [quit] 1]', profile)
titles = rows(text, 'QA_TITLE')
assert 'restored from the backup' in titles[0], titles
assert titles[-1].endswith('note=[]'), ('New Game clears the warning', titles)
print('The corrupt-file warning was cleared when New Game started.')

# 14. Optional state records: boardable Pelicans, sound class gains, attached actors.
text, profile = run('board-save', 'qa_campaign_play 2 3; qa_after_gameplay [qa_hs_cmd object_create "obj:foehammer_cliff"; '
                    'qa_hs_cmd unit_set_enterable_by_player "obj:foehammer_cliff true"; qa_hs_cmd sound_class_set_gain "s:music 0.25 0"; '
                    'sleep 400 [qa_checkpoint_save] 1; sleep 700 [quit] 1]')
saved = read_progress(profile)
cliff = read_hscb(root / 'packages/halo/a30.hscb')['objects'].index('foehammer_cliff')
board = [r for r in saved['records']['s'] if r.startswith('board ')]
assert board and str(cliff) in board[0].split()[2:], (cliff, board)
gain = [r for r in saved['records']['s'] if r.startswith('gain ')]
assert gain and abs(float(gain[0].split()[2 + 32]) - 0.25) < 1e-3, gain   # class 32: music
text, profile = run('board-continue', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [qa_hs_object boardable obj:foehammer_cliff; '
                    'qa_fx_state continued; sleep 200 [quit] 1]', profile)
assert rows(text, 'HS_OBJECT boardable obj:foehammer_cliff')[-1].split()[-1] == '1', rows(text, 'HS_OBJECT')
fx = rows(text, 'FX_QA continued')
assert fx and abs(float(field(fx[-1], 'music')) - 0.25) < 1e-3, fx
text, profile = run('attach-save', 'halo_difficulty 1; qa_campaign_play 3 4; qa_after_gameplay [botsshoot 0; '
                    'qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"; '
                    'sleep 600 [qa_checkpoint_save] 1; sleep 900 [quit] 1]', timeout=150)
saved = read_progress(profile)
assert any(r.startswith('attached ') and ' =captain_keyes ' in r for r in saved['records']['s']), \
    [r for r in saved['records']['s'] if r.startswith('attached')]
text, profile = run('attach-continue', 'halo_title; sleep 300 [halo_continue] 1; qa_after_gameplay [sleep 500 [qa_attach_state continued] 1; '
                    'sleep 700 [quit] 1]', profile, timeout=150)
keyes = [line for line in rows(text, 'ATTACH_QA continued') if 'object=captain_keyes' in line]
assert keyes and 'alive=1' in keyes[0], rows(text, 'ATTACH_QA')
print('Continue kept a boardable Pelican, the music class gain and an attached Keyes.')
