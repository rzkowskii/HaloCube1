"""game_won and game_lost drive the campaign ending.

QA client, fresh profiles, real campaign play (not previews):
- The Maw's last section: game_won raises the ending, the campaign is marked beaten
  in config/halo-progress.txt (with no game left to continue) and the credits finish
  into the title menu (menu "Halo", the title screen behind it).
- The same ending arrives when the finale cue plays out on that section.
- game_won on an earlier section is ignored; game_lost reverts the player.
"""
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=120):
    profile = scratch(f'ending-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), profile


def ending_rows(text):
    return [line for line in text.splitlines() if line.startswith('QA_ENDING')]


# 1. game_won on the last section of The Maw.
text, profile = run('won', 'qa_campaign_play 10 8', [
    (300, 'qa_hs_call game_won 0 0'), (1500, 'qa_halo_ending'),
    (2000, 'halo_next; qa_halo_ending; quit')])  # the menu that follows pauses timers
rows = ending_rows(text)
assert 'Halo ending reached' in text, text[-2000:]
assert len(rows) == 2 and 'ending=1 done=0 beaten=1' in rows[0], rows
assert 'ending=1 done=1' in rows[1] and 'Halo credits finished' in text, rows
assert 'title=1' in rows[1], ('the credits return to the title menu', rows)
progress = read_progress(profile)
assert progress['furthest'] == ['9', '7'] and progress['beaten'] == ['1'] and 'game' not in progress, progress
print('game_won on The Maw raised the ending, marked the campaign beaten and finished the credits.')

# 2. The finale cue playing out on the last section ends the campaign too.
text, profile = run('finale', 'qa_campaign_play 10 8', [
    (300, 'qa_halo_finale'), (600, 'qa_halo_ending'),
    (900, 'halo_next'), (1500, 'qa_halo_ending'), (1800, 'halo_next; qa_halo_ending; quit')])
assert 'Halo scene: cinematic_finale start' in text, text[-2000:]
assert 'Halo ending reached' in text, text[-2000:]
progress = read_progress(profile)
assert progress['furthest'] == ['9', '7'] and progress['beaten'] == ['1'], progress
rows = ending_rows(text)
assert 'ending=0' in rows[0] and 'cinematic=1' in rows[0], rows
assert 'ending=1 done=0 beaten=1' in rows[1] and 'ending=1 done=1' in rows[2], rows
assert 'title=1' in rows[2], ('the credits return to the title menu', rows)
print('The finale cue ending raised the ending state.')

# 3. game_won before the last section is ignored; game_lost reverts the player.
text, profile = run('early', 'qa_campaign_play 1 1', [
    (300, 'qa_hs_call game_won 0 0'), (900, 'qa_halo_ending'),
    (1200, 'qa_hs_call game_lost 0 0'), (1800, 'qa_halo_ending'), (2400, 'quit')])
rows = ending_rows(text)
assert 'game_won ignored' in text and 'ending=0' in rows[0], rows
assert 'Halo game over' in text and 'alive=0' in rows[1], rows
# The section start saves the game (progress file version 2), but never as beaten.
progress = read_progress(profile)
assert progress is None or progress['beaten'] == ['0'], 'early game_won must not mark the campaign beaten'
print('game_won is ignored mid-campaign and game_lost kills the player for a revert.')
