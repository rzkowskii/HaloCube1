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

Gap 4 of batch 5's critic (written while testing was on hold, not yet run):
6. Queued vitality: unit_set_current_vitality of captain_keyes in the same breath as its ai_attach (as
   c10 gives marine_suicidal) is queued ("vitality waits for its actor") and the actor spawns with it
   (health 0.40).
7. A Call scene's start finds an attached actor where it stands: during qa_hs_cue cutscene_rescue the
   scripted captain_keyes is visible (the scene's reset would leave it hidden at its placement) at the
   actor's feet (attachgap 0).
8. Carried into the next section: Keyes (hurt to 0.5) and free_marine_1 attached in a50 section 4; the
   section is finished and section 5 loads; both "come along", spawn behind the player ("came along with
   the player into section 5"), alive, Keyes at health 0.5, unit_get_health 0.5, following the player.
9. Saved and continued there: the section-start save (or a later checkpoint) writes `s attached` for
   captain_keyes with the health field; Continue brings him back alive at 0.5 (behind the player again
   when the save was taken before he spawned: the carried flag is 1). The record now ends with his
   start (four more fields, gap 11 of batch 5's critic).

Gap 11 of batch 5's critic and the melee review (written while testing was on hold, not yet run):
10. An attached actor keeps its own start across checkpoints: Keyes attached, then moved 1.5 world units
    (qa_attach_move); a checkpoint, a death and a revert bring him back where he stood at the checkpoint;
    ai_teleport_to_starting_location prison_captain then puts him back where he took over (his spawn),
    not where he stood at the checkpoint. The checkpoint's `s attached` record (qa_checkpoint_save writes
    the progress file) ends with that start, which differs from its point.
11. The kill from behind spares an attached story figure: the player's melee from behind on the calm,
    attached Keyes logs DAMAGE_UNSUSPECTING_SPARED attached=1 and no DAMAGE_UNSUSPECTING (he takes the
    melee's own damage; whether he survives it is printed, not asserted).

Gap 1 of batch 6's critic (the fresh-start batch, written while testing was on hold, not yet run):
12. Carried actors waiting to spawn: Keyes (hurt to 0.5) and free_marine_1 are carried from a50 section 4
    into section 5 with the QA hold on (halo_carriedhold 1), so they wait unspawned. There, qa_attach_state
    lists both as carried= entries in their old squads; ai_living_count prison_captain is 1 and ai_strength
    prison_captain 0.5; ai_migrate prison_captain hangar_captain_halls/squad_switch and ai_migrate_by_unit
    free_marine_1 hangar_marines_halls/squad_switch retarget the entries (the scripttrace line "carried in
    and not spawned yet, migrates to"), so ai_living_count prison_captain is then 0 and
    hangar_captain_halls 1, and an ai_erase of prison_captain and prison_marines kills neither (both still
    carried, in the new squads). With the hold off both come along behind the player, alive, in the new
    squads, Keyes at 0.5.
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
# Version 10 (major upgrades) keeps version 9's tables.
assert all(h['version'] in (9, 10) for h in programs.values()), {k: h['version'] for k, h in programs.items()}


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
        out[fields.get('object') or fields.get('pending') or fields.get('carried') or 'count'] = fields
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

# ---- Gap 4: queued unit commands, a scene's start, Keyes carried on into section 5 and saved there.
text, log = run('queued', [
    (100, 'qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"; '
          'qa_hs_cmd unit_set_current_vitality "obj:captain_keyes 0.4 0"'),
    (700, 'qa_attach_state queued; qa_hs_cmd unit_get_health "obj:captain_keyes"'),
    (900, 'qa_hs_cue cutscene_rescue'),
    (1300, 'qa_hs_object visible obj:captain_keyes; qa_hs_object attachgap obj:captain_keyes'),
    (1500, 'quit'),
])
assert 'vitality waits for its actor' in text and 'takes the vitality queued for it' in text, log
state = attached(text, 'queued')
assert state['captain_keyes']['alive'] == '1' and abs(float(state['captain_keyes']['health']) - 0.4) < 0.02, (state, log)
assert abs(hs(text, 'unit_get_health')[0] - 0.4) < 0.02, log
assert 'QA_CUE cutscene_rescue 1' in text and 'Halo scene: cutscene_rescue start' in text, log
objects = {line.split()[1]: line.split()[-1] for line in text.splitlines() if line.startswith('HS_OBJECT ')}
assert objects.get('visible') == '1' and objects.get('attachgap') == '0', ('the scene finds Keyes where his actor stands', objects, log)
print('A vitality given with ai_attach waited for the actor; the rescue scene found Keyes where he stood.')


def carried_run(name, script, profile=None, timeout=200):
    profile = profile or scratch(f'attach-{name}-')
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600', f'-e{script}'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), profile, log


require(root / 'packages/maps/halo_a50_05.cgz', 'import the campaign (docs/macos-build.md)')
text, profile, log = carried_run('carry', 'halo_scripttrace 1; halo_difficulty 1; qa_campaign_play 3 4; qa_after_gameplay [botsshoot 0; '
                                 'qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"; '
                                 'qa_hs_cmd object_create "obj:free_marine_1"; qa_hs_cmd ai_attach "obj:free_marine_1 prison_marines"; '
                                 'sleep 600 [qa_hs_cmd unit_set_current_vitality "obj:captain_keyes 0.5 0"; qa_attach_state before] 1; '
                                 'sleep 900 [qa_halo_finale] 1; sleep 1400 [halo_next] 1; sleep 2400 [halo_next] 1; '
                                 'sleep 9000 [qa_attach_state carried; qa_hs_cmd unit_get_health "obj:captain_keyes"; qa_halo_where] 1; '
                                 'sleep 9500 [quit] 1]')
before = attached(text, 'before')
assert before['captain_keyes']['alive'] == '1' and before['free_marine_1']['alive'] == '1', (before, log)
assert 'Halo: The Truth and Reconciliation, section 5' in text, ('section 5 loaded', log)
assert re.search(r'captain_keyes \(prison_captain\) comes along into section 5, health 0\.5', text), log
assert 'captain_keyes came along with the player into section 5' in text and 'free_marine_1 came along with the player into section 5' in text, log
state = attached(text, 'carried')
assert state['captain_keyes']['alive'] == '1' and abs(float(state['captain_keyes']['health']) - 0.5) < 0.02, (state, log)
assert state['free_marine_1']['alive'] == '1' and float(state['captain_keyes']['distance']) < 40, (state, log)
assert abs(hs(text, 'unit_get_health')[-1] - 0.5) < 0.02, ('unit_get_health answers from the carried actor', log)
saved = read_progress(profile)
records = [r for r in saved['records']['s'] if r.startswith('attached ') and ' =captain_keyes ' in r]
assert records, ('the section-start save (or a checkpoint since) keeps Keyes', [r for r in saved['records']['s'] if r.startswith('attached')])
fields = records[0].split()
# attached <object> =<name> <encounter> =<name> <squad> =<name> suspended flags gone actor variant x y z yaw health shield
# near, then (gap 11) the start x y z yaw.
assert len(fields) == 23, ('the record ends with the start (gap 11)', records[0])
assert abs(float(fields[16]) - 0.5) < 0.02 and fields[18] in ('0', '1'), ('the health field and the carried flag', records[0])
near = fields[18] == '1'
print(f'Keyes (0.5) and a freed marine came along into a50 section 5 behind the player; saved {"before" if near else "after"} he spawned.')
text, profile, log = carried_run('carry-continue', 'halo_scripttrace 1; halo_title; sleep 300 [halo_continue] 1; '
                                 'qa_after_gameplay [sleep 1500 [qa_attach_state continued; qa_hs_cmd unit_get_health "obj:captain_keyes"] 1; '
                                 'sleep 1800 [quit] 1]', profile)
state = attached(text, 'continued')
assert state['captain_keyes']['alive'] == '1' and abs(float(state['captain_keyes']['health']) - 0.5) < 0.02, (state, log)
if near:
    assert 'captain_keyes came along with the player into section 5' in text, ('a carried record spawns behind the player again', log)
print('Continue in a50 section 5 brought the carried Keyes back at 0.5.')


# ---- 10. An attached actor keeps its own start across checkpoints (gap 11).
def where(text, label):
    row = attached(text, label)['captain_keyes']
    return tuple(float(v) for v in row['pos'].split(','))


def apart(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** .5


text, log = run('start', [
    (100, 'qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"'),
    (600, 'qa_attach_state spawned'),
    (700, 'qa_attach_move captain_keyes 0 1.5'),
    (900, 'qa_attach_state moved; qa_checkpoint_save'),
    (1200, 'qa_halo_damage 100000'),
    (2800, 'qa_halo_revert'),
    (4300, 'qa_attach_state reverted'),
    (4400, 'qa_hs_cmd ai_teleport_to_starting_location "prison_captain"'),
    (4700, 'qa_attach_state home'),
    (4900, 'quit'),
])
assert 'ATTACH_QA move object=captain_keyes ok=1' in text, ('Keyes moved off his spawn', log)
spawned, moved, reverted, home = (where(text, label) for label in ('spawned', 'moved', 'reverted', 'home'))
step = apart(spawned, moved)
assert step > .5, ('the move took him off his spawn', spawned, moved)
assert apart(reverted, moved) < .4 * step, ('the revert brings him back where he stood at the checkpoint', moved, reverted)
assert apart(home, spawned) < .4 * step and apart(home, moved) > .6 * step, ('his start is still his spawn', spawned, moved, home)
saved = read_progress(log.parent)
records = [r.split() for r in saved['records']['s'] if r.startswith('attached ') and ' =captain_keyes ' in r]
assert records and len(records[0]) == 23, ('the checkpoint keeps Keyes with his start', saved['records']['s'])
stood, start = [float(v) for v in records[0][12:15]], [float(v) for v in records[0][19:22]]
assert apart(stood, start) > 1, ('the record keeps where he stood and his start apart (world units)', records[0])
print('captain_keyes came back after a revert where he stood, and ai_teleport_to_starting_location took him to his spawn.')

# ---- 11. The kill from behind spares an attached story figure (the melee review).
text, log = run('melee', [
    (100, 'halo_damagetrace 1; qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"'),
    (700, 'qa_damage_melee_friend keyes'),
    (900, 'qa_attach_state struck'),
    (1100, 'quit'),
])
spared = [line for line in text.splitlines() if line.startswith('DAMAGE_UNSUSPECTING_SPARED ')]
assert spared and 'attached=1' in spared[0], ('Keyes is spared the kill from behind', log)
assert 'DAMAGE_UNSUSPECTING ' not in text, log
struck = attached(text, 'struck')['captain_keyes']
print(f'captain_keyes struck from behind is spared the kill (alive={struck["alive"]}, health {struck["health"]}: the melee\'s own damage).')


# ---- 12. Carried actors waiting to spawn take migrations and count (the fresh-start batch, gap 1 of batch 6's critic).
text, profile, log = carried_run('carry-migrate', 'halo_scripttrace 1; halo_difficulty 1; qa_campaign_play 3 4; qa_after_gameplay [botsshoot 0; '
                                 'qa_hs_cmd object_create "obj:captain_keyes"; qa_hs_cmd ai_attach "obj:captain_keyes prison_captain"; '
                                 'qa_hs_cmd object_create "obj:free_marine_1"; qa_hs_cmd ai_attach "obj:free_marine_1 prison_marines"; '
                                 'sleep 600 [qa_hs_cmd unit_set_current_vitality "obj:captain_keyes 0.5 0"; qa_attach_state before; halo_carriedhold 1] 1; '
                                 'sleep 900 [qa_halo_finale] 1; sleep 1400 [halo_next] 1; sleep 2400 [halo_next] 1; '
                                 'sleep 9000 [qa_attach_state held; qa_hs_cmd ai_living_count "prison_captain"; qa_hs_cmd ai_strength "prison_captain"; '
                                 'qa_hs_cmd ai_migrate "prison_captain hangar_captain_halls/squad_switch"; '
                                 'qa_hs_cmd ai_migrate_by_unit "obj:free_marine_1 hangar_marines_halls/squad_switch"; '
                                 'qa_hs_cmd ai_living_count "prison_captain"; qa_hs_cmd ai_living_count "hangar_captain_halls"; '
                                 'qa_hs_cmd ai_erase "prison_captain"; qa_hs_cmd ai_erase "prison_marines"; qa_attach_state migrated; '
                                 'halo_carriedhold 0] 1; '
                                 'sleep 11000 [qa_attach_state spawned; qa_hs_cmd unit_get_health "obj:captain_keyes"; '
                                 'qa_hs_cmd ai_living_count "hangar_captain_halls"] 1; sleep 11500 [quit] 1]')
before = attached(text, 'before')
assert before['captain_keyes']['alive'] == '1' and before['free_marine_1']['alive'] == '1', (before, log)
assert 'Halo: The Truth and Reconciliation, section 5' in text, ('section 5 loaded', log)
held = attached(text, 'held')
assert 'carried' in held['captain_keyes'] and held['captain_keyes']['squad'].startswith('prison_captain/'), (held, log)
assert abs(float(held['captain_keyes']['health']) - 0.5) < 0.02 and held['free_marine_1']['squad'].startswith('prison_marines/'), (held, log)
counts = hs(text, 'ai_living_count')
assert counts[0] == 1.0 and abs(hs(text, 'ai_strength')[0] - 0.5) < 0.02, ('a pending carried actor counts', counts, log)
assert 'captain_keyes, carried in and not spawned yet, migrates to hangar_captain_halls/squad_switch' in text, log
assert 'free_marine_1, carried in and not spawned yet, migrates to hangar_marines_halls/squad_switch' in text, log
assert counts[1] == 0.0 and counts[2] == 1.0, ('the count moved with the migration', counts, log)
migrated = attached(text, 'migrated')
assert migrated['captain_keyes']['squad'] == 'hangar_captain_halls/squad_switch', ('the erase of the old squad spared Keyes', migrated, log)
assert migrated['free_marine_1']['squad'] == 'hangar_marines_halls/squad_switch', (migrated, log)
assert 'captain_keyes came along with the player into section 5' in text and 'free_marine_1 came along with the player into section 5' in text, log
spawned = attached(text, 'spawned')
assert spawned['captain_keyes']['alive'] == '1' and spawned['captain_keyes']['squad'] == 'hangar_captain_halls/squad_switch', (spawned, log)
assert abs(float(spawned['captain_keyes']['health']) - 0.5) < 0.02 and spawned['free_marine_1']['alive'] == '1', (spawned, log)
assert spawned['free_marine_1']['squad'] == 'hangar_marines_halls/squad_switch', (spawned, log)
assert abs(hs(text, 'unit_get_health')[-1] - 0.5) < 0.02 and counts[-1] == 1.0, (counts, log)
print('Carried Keyes and a freed marine, held unspawned, counted in their squad, migrated, outlived the erase of the old '
      'squads and came along in the new ones.')
