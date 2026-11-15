"""ai_attach turning scripted bipeds into actors, and the story-figure flags (HSCB version 9).

Written while testing was on hold: not yet run (see docs/macos-build.md, "Not yet verified in the
game", and Factions and special enemies).

Static (assets, no game): a50.hscb flags captain_keyes, the freed and jailed marines and keyes_prison
as story figures (OBJECT_STORY_FIGURE, tools/import_halo_scripts.py story_figures), d40.hscb its
cortana, d20.hscb keyes_flood (a character posed as scenery) and a10.hscb keyesa10; corpses placed
dead (a30's dead_marine_1, b40's a2_dump_unit_1) and c20's scripted monitor objects are not flagged;
the squads a50 attaches Keyes to (prison_captain/mig_cap_u, prison_marines_ini/captain) default to the
captain_ingame variant.

QA client, a50 section 4 (the cell block, started on its own, Normal):
1. object_create and ai_attach of captain_keyes (to prison_captain, as ini_post_rescue does): two ticks
   later an actor of the squad's variant stands there (ATTACH_QA alive=1, drawn=0: the scripted model is
   no longer drawn), ai_living_count prison_captain is 1, unit_get_health captain_keyes is its health (1).
2. A checkpoint, a death and a revert: the actor comes back (alive=1) where it stood at the checkpoint.
3. damage_object (a lethal effect) on captain_keyes kills the actor: unit_get_health is -1, the entry is
   kept with no actor; another checkpoint and revert keep it gone (no second Keyes, gone=1).
4. enc_spawn_marines (woken): the jailed marines are created, attached and posed sitting (pending, posed=1,
   drawn=1, no actor); unit_stop_custom_animation on one makes it an actor half a second later.
5. object_destroy of that marine erases its actor at once, with no death (the ATTACH_QA count drops and
   no FACTION or death line names it).
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(root / 'packages/halo/a50.hscb', 'run tools/import_halo_scripts.py')

# ---- Static: the flags and the squads' default variants.
programs = {p.stem: read_hscb(p) for p in sorted((root / 'packages/halo').glob('*.hscb'))}
assert all(h['version'] == 9 for h in programs.values()), {k: h['version'] for k, h in programs.items()}


def figures(cache):
    h = programs[cache]
    return {name for name, flags in zip(h['objects'], h['object_flags']) if flags & 1}


a50 = figures('a50')
assert {'captain_keyes', 'free_marine_1', 'free_marine_2', 'free_marine_3', 'jailed_marine_1', 'keyes_prison'} <= a50, a50
assert 'cortana' in figures('d40') and 'keyes_flood' in figures('d20') and 'keyesa10' in figures('a10'), \
    (figures('d40'), figures('d20'), figures('a10'))
assert 'dead_marine_1' not in figures('a30') and 'a2_dump_unit_1' not in figures('b40'), 'corpses are not story figures'
assert not any(name.startswith('monitor') for name in figures('c20')), figures('c20')
actors = {}
current = None
for line in (root / 'packages/halo/actors.txt').read_text().splitlines():
    key, _, rest = line.partition(' ')
    if key == 'actor':
        current = int(rest)
    elif key == 'path' and current is not None:
        actors[rest] = current
captain = actors['characters\\captain_ingame\\captain_ingame']
h = programs['a50']
defaults = {(e[0], s): d for e, ds in zip(h['encounters'], h['squad_defaults']) for s, d in zip(e[1], ds)}
assert defaults[('prison_captain', 'mig_cap_u')][1] == captain and defaults[('prison_marines_ini', 'captain')][1] == captain, defaults
print(f'Story figures flagged ({len(a50)} in a50, cortana, keyes_flood, keyesa10), corpses not; Keyes\'s squads default to captain_ingame.')

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')
require(root / 'packages/maps/halo_a50_04.cgz', 'import the campaign (docs/macos-build.md)')


def run(name, schedule, timeout=150):
    profile = scratch(f'attach-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-ehalo_difficulty 1; qa_campaign_resume 3 4; qa_after_gameplay [botsshoot 0; {body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def attached(text, label):
    out = {}
    for line in text.splitlines():
        if not line.startswith(f'ATTACH_QA {label} '):
            continue
        fields = dict(re.findall(r'(\w+)=(\S+)', line))
        out[fields.get('object') or fields.get('pending') or 'count'] = fields
    assert out, (label, text[-3000:])
    return out


def hs(text, command):
    found = [line.split()[-1] for line in text.splitlines() if line.startswith(f'HS_CMD {command} ')]
    assert found, (command, text[-3000:])
    return [float(v) for v in found]


text, log = run('keyes', [
    (100, 'qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"'),
    (600, 'qa_attach_state attached; qa_hs_cmd unit_get_health "obj:captain_keyes"; qa_hs_cmd ai_living_count "prison_captain"'),
    (900, 'qa_checkpoint_save'),
    (1200, 'qa_halo_damage 100000'),
    (2800, 'qa_halo_revert'),
    (4300, 'qa_attach_state reverted'),
    (4500, 'qa_hs_cmd damage_object "s:death obj:captain_keyes"'),
    (4800, 'qa_attach_state killed; qa_hs_cmd unit_get_health "obj:captain_keyes"'),
    (5000, 'qa_checkpoint_save'),
    (5300, 'qa_halo_damage 100000'),
    (6900, 'qa_halo_revert'),
    (8400, 'qa_attach_state gone; qa_hs_cmd unit_get_health "obj:captain_keyes"'),
    (8600, 'quit'),
])
state = attached(text, 'attached')
assert state['captain_keyes']['alive'] == '1' and state['captain_keyes']['drawn'] == '0', (state, log)
assert 'captain_keyes attached to prison_captain' in text and 'is an actor now' in text, log
health = hs(text, 'unit_get_health')
assert health[0] == 1.0 and hs(text, 'ai_living_count')[0] == 1.0, (health, log)
state = attached(text, 'reverted')
assert state['captain_keyes']['alive'] == '1' and state['count']['count'] == '1', (state, log)
state = attached(text, 'killed')
assert state['captain_keyes']['alive'] == '0' and health[1] == -1.0, (state, health, log)
state = attached(text, 'gone')
assert state['captain_keyes']['alive'] == '0' and state['captain_keyes']['gone'] == '1' and state['count']['count'] == '1', (state, log)
assert health[2] == -1.0, ('a revert does not bring back the dead Keyes', health, log)
print('captain_keyes attached became an actor (health, living count), came back after a revert, died, and stayed dead.')

text, log = run('marines', [
    (100, 'qa_hs_script enc_spawn_marines 0'),
    (1200, 'qa_attach_state posed'),
    (1400, 'qa_hs_cmd unit_stop_custom_animation "obj:jailed_marine_1"'),
    (2000, 'qa_attach_state free'),
    (2200, 'qa_hs_cmd object_destroy "obj:jailed_marine_1"'),
    (2500, 'qa_attach_state destroyed'),
    (2700, 'quit'),
])
state = attached(text, 'posed')
assert state['jailed_marine_1']['posed'] == '1' and state['jailed_marine_1']['drawn'] == '1' and state['count']['count'] == '0', (state, log)
state = attached(text, 'free')
assert state['jailed_marine_1']['alive'] == '1' and state['count']['count'] == '1', (state, log)
state = attached(text, 'destroyed')
assert 'jailed_marine_1' not in state and state['count']['count'] == '0', (state, log)
print('The jailed marines stayed posed figures until their animation stopped; object_destroy erased the actor.')
