"""Actor brains in the QA client.

Crash Site wave: unaware actors notice the player's gunfire, alert their squad
and fire in bursts. Spawned marines fight a Grunt. a30 section 2 encounters:
squads hold guard positions, take combat positions from their squad's group
masks once they see the player, and the script AI commands (ai_living_count,
ai_living_fraction, ai_status, ai_actors with list_count/list_get,
ai_magically_see_players, ai_kill, ai_command_list and its status) work.
The unit and object-state commands (berserk, grenades, flags, impervious and
damage immunity, vitality, damage, follow, migration, attach, renew, teleport,
suspend; object hover, collision, open, doors) change what the actors and
objects do; the player's vitality and protection follow the same commands.
Combat by difficulty (item 6, the combat-scaling work, not yet run): on each
level the Covenant fire with the globals table's rate_of_fire, projectile_error,
burst_separation, grenade_chance_scale and the lead gate, the marines with
Normal's, and the melee delays and guidance_vs_player follow the level for all.
Script vitality in Halo's points (item 7, the script-vitality change, not yet run):
unit_set_current_vitality counts points of the stored maximum, unit_set_maximum_vitality
sets new maxima at full vitality that ai_renew and a revert keep, and 0 points kill.
ai_playfight (item 8, the playfight change, not yet run): the flag is the encounter's, members placed or
migrated in later take it, a squad reference sets the whole encounter, the brain fires with Halo's
playfighting values, a revert and the progress file keep it, and the damage is 0.3 of itself.
unit_impervious no longer protects (the unit-commands item): only object_cannot_take_damage does.
The scripts' effect damage on actors (item 9, the effect-damage default change, not yet run): a10's
containment_1 doom_crewman, set to 1 point as the script does, lives through the containment_1_flag_4
plasma blast with halo_effectdamage 0 and dies to it, ownerless, by default.
Halo's actor melee (item 10, the actor-melee work, not yet run): an armed Elite minor charges inside its
melee_range and strikes with the unit's melee jpt! (30 to 35); the stealth sword Elite with the sword's 151.
Special fire (item 11, the special-fire change, not yet run): actors.txt's `special` lines match the tags, a
Jackal major rolls its special fire 5 to 6.5 s apart, releases the charged bolt after a 0.6 s hold and its
charged bolt hits for 70 x 0.5 x 0.4; a Grunt has none, and halo_specialfire 0 turns it off.
Danger zones and dives (item 12, the danger-zone work, not yet run): the `danger`, `dive` and `flags` lines match
the tags; a Grunt notices an enemy frag after 30 ticks and dives by its animation before it goes off, steps out of
a plasma grenade's radius when it may not dive, panics with one stuck to it (an Elite goes berserk), and a
marine dives from the player's Warthog and notices a carrier's fatal melee.
"""
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403

require(QA_BIN, 'run scripts/test-macos.sh, or build with -DHALO_BUILD_QA=ON')


def run(name, start, schedule, timeout=90):
    profile = scratch(f'actor-{name}-')
    body = '; '.join(f'sleep {delay} [{action}] 1' for delay, action in schedule)
    log = profile / 'run.log'
    with log.open('w') as output:
        subprocess.run([QA_BIN, '--halo', f'--home={profile}', f'--mod={root}', '-t0', '-w800', '-h600',
                        f'-e{start}; qa_after_gameplay [{body}]'],
                       cwd=RUNTIME, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=True)
    return log.read_text(errors='replace'), log


def actors(text, label):
    rows = []
    for line in text.splitlines():
        if line.startswith(f'ACTOR_QA {label} '):
            rows.append(dict(field.split('=', 1) for field in re.findall(r'(\w+=\S+)', line)))
    return rows


def probes(text, kind):
    return [line.split()[1:] for line in text.splitlines() if line.startswith(kind)]


# 1. The Crash Site wave: blinded actors stay unaware until they hear gunfire,
#    then fight in bursts once they can see again. HALO_AI_BLIND is 1.
text, log = run('wave', 'halo_preview', [
    (200, 'qa_campaign_reach'),
    # Blind them as soon as the wave spawns, before an idle glance can find the player.
    (250, 'qa_actor_flag 1 1'), (300, 'qa_actor_flag 1 1'), (500, 'qa_actor_flag 1 1'),
    (2500, 'qa_actor_state quiet'),
    (3000, 'qa_halo_fire 1'), (3400, 'qa_halo_fire 0'),
    (3800, 'qa_actor_state heard; qa_actor_flag 1 0'),
    (7500, 'qa_actor_state fight'),
    (8000, 'quit'),
])
quiet, heard, fight = actors(text, 'quiet'), actors(text, 'heard'), actors(text, 'fight')
assert len(quiet) >= 3 and all(a['awareness'] == 'unaware' for a in quiet), ('blind actors stay unaware', quiet, log)
assert all(a['awareness'] == 'alert' for a in heard), ('gunfire within hearing range alerts the squad', heard, log)
shots = sorted(int(a['shots']) for a in fight)
assert shots and shots[-1] >= 3, ('the squad returns fire once it can see', shots, log)
print(f'Wave: {len(quiet)} actors heard the gunfire and returned fire, shots {shots}.')

# 2. Marines kill a Grunt.
text, log = run('marines', 'halo_preview', [
    (0, 'qa_actor_spawn 9 1 1 2; qa_actor_spawn 9 1 1 -2; qa_actor_spawn 0 0 9 0'),
    (8000, 'qa_actor_state after'),
    (8500, 'quit'),
])
# With 12 health a marine can lose the duel; either way the two sides fight.
kills = re.findall(r'(Marine \d+ \w+ Grunt \d+|Grunt \d+ \w+ Marine \d+|you were \w+ by Grunt)', text)
assert any(k.startswith('Marine') or k.startswith('Grunt') for k in kills), ('marines and the grunt fight each other', kills, log)
print(f'Marines and a Grunt fought: {kills}.')

# 3. a30 section 2: positions, script commands and a command list.
text, log = run('scripted', 'qa_campaign_resume 2 2', [
    (0, 'qa_actor_place lz_search; qa_actor_place first_marine'),
    (300, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_status lz_search 0; '
          'qa_hs_ai ai_living_fraction lz_search 0; qa_hs_list lz_search; qa_hs_command first_marine; qa_actor_state idle'),
    (600, 'qa_hs_ai ai_magically_see_players lz_search 0; qa_hs_ai ai_command_list first_marine 0'),
    (1000, 'qa_hs_command first_marine'),
    (7000, 'qa_hs_ai ai_status lz_search 0; qa_actor_state seen'),
    (7200, 'qa_hs_ai ai_kill lz_search 0'),
    (7600, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_living_fraction lz_search 0'),
    (8000, 'quit'),
])
values = [(cmd, float(v)) for cmd, enc, v in probes(text, 'HS_AI') if enc == 'lz_search']
count = values[0][1]
# Normal places the first 17 starts of lz_search; some have no floor, leaving 7.
assert values[0][0] == 'ai_living_count' and count >= 5, values
assert values[1] == ('ai_status', 2.0) and values[2] == ('ai_living_fraction', 1.0), values
listed = probes(text, 'HS_LIST')[0]
assert listed[2] == f'count={int(count)}' and listed[3] == f'units={int(count)}', listed
statuses = [int(v) for _, v in probes(text, 'HS_COMMAND')]
assert statuses[:2] == [0, 1], ('ai_command_list starts the list', statuses)
assert ('ai_status', 6.0) in values, ('magic sight puts the squad in combat', values)
assert values[-2:] == [('ai_living_count', 0.0), ('ai_living_fraction', 0.0)], values

# Team 0 is the Covenant; the marine encounter's marines and crew are team 1.
idle = [a for a in actors(text, 'idle') if a['team'] == '0']
seen = [a for a in actors(text, 'seen') if a['team'] == '0']
def in_mask(a):
    return a['dest'] == '1' and int(a['group']) >= 0 and (int(a['mask']) == 0 or int(a['mask']) >> int(a['group']) & 1)
assert idle and sum(in_mask(a) for a in idle) >= len(idle) // 2, ('unaware squads take guard positions', idle)
assert seen and all(a['awareness'] == 'combat' for a in seen) and all(in_mask(a) for a in seen), \
    ('combat positions come from the squad group masks', seen)
marines = actors(text, 'seen')
assert any(a['team'] == '1' and a['command'] in ('1', '2') for a in marines), 'marines run the command list'
print(f'Scripted: {int(count)} actors placed, script AI commands correct, positions follow squad masks, command list runs.')


# 4. Unit and object-state script commands on a30 section 2's marines and Covenant.
def hs(*items):
    return '; '.join(f'qa_hs_cmd {cmd} "{args}"' for cmd, args in items)


text, log = run('unitcmds', 'qa_campaign_resume 2 2', [
    (0, 'qa_actor_place lz_search; qa_actor_place first_marine'),
    (400, 'qa_actor_state base; qa_player_vitals'),
    # Encounter switches: berserk, grenades off, dormancy and maneuver off, immunity, drops, braindead.
    (500, hs(('ai_berserk', 'lz_search true'), ('ai_grenades', 'false'), ('ai_allow_dormant', 'lz_search false'),
             ('ai_maneuver_enable', 'lz_search false'), ('object_cannot_take_damage', 'unit:lz_search:0'),
             ('unit_impervious', 'unit:lz_search:1 true'), ('unit_doesnt_drop_items', 'list:lz_search'),
             ('ai_braindead_by_unit', 'unit:lz_search:2 true'), ('ai_prefer_target', 'players true'),
             ('ai_follow_distance', 'lz_search 6'), ('ai_try_to_fight_nothing', 'first_marine'),
             ('ai_link_activation', 'lz_search first_marine'), ('ai_automatic_migration_target', 'lz_search true'),
             ('ai_dialogue_triggers', 'false'), ('ai_allow_dormant', 'first_marine false'))),
    (600, 'qa_actor_state flags'),
    # Damage: the immune actors keep their health, the rest lose some; vitality sets Halo's points of
    # the stored maximum (the script-vitality change); renew refills.
    (800, hs(('damage_object', 's:shock unit:lz_search:0'), ('damage_object', 's:shock unit:lz_search:1'), ('damage_object', 's:shock unit:lz_search:6'))),
    (900, 'qa_actor_state hurt'),
    (1000, hs(('units_set_current_vitality', 'unit:lz_search:3 10 0'), ('unit_set_current_vitality', 'unit:lz_search:4 0.25 0'))),
    (1100, 'qa_actor_state vital; qa_actor_vitality vital'),
    (1200, hs(('ai_renew', 'lz_search'))),
    (1300, 'qa_actor_state renewed'),
    # Berserk and the other switches clear again.
    (1400, hs(('ai_berserk', 'lz_search false'), ('ai_grenades', 'true'), ('ai_braindead_by_unit', 'unit:lz_search:2 false'),
              ('ai_allow_dormant', 'lz_search true'), ('unit_impervious', 'unit:lz_search:1 false'))),
    (1500, 'qa_actor_state cleared'),
    # Following: the marines trail a Covenant actor until told to stop.
    (1600, hs(('ai_follow_target_ai', 'first_marine lz_search'))),
    (1700, 'qa_actor_state follow'),
    (1800, hs(('ai_follow_target_disable', 'first_marine'))),
    (1900, 'qa_actor_state unfollow'),
    # Suspension stops an actor thinking.
    (2000, hs(('unit_suspended', 'unit:lz_search:5 true')) + '; qa_actor_state asleep_a'),
    (2600, 'qa_actor_state asleep_b'),
    (2700, hs(('unit_suspended', 'unit:lz_search:5 false'))),
    # Migration and attaching move actors between encounters.
    (2800, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_living_count first_marine 0'),
    (2900, hs(('ai_migrate_by_unit', 'unit:lz_search:0 first_marine'), ('ai_attach', 'unit:lz_search:0 first_marine'))),
    (3000, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_living_count first_marine 0'),
    (3100, hs(('ai_migrate_and_speak', 'lz_search first_marine s:advance'))),
    (3200, 'qa_hs_ai ai_living_count lz_search 0; qa_hs_ai ai_living_count first_marine 0'),
    (3300, hs(('ai_detach', 'unit:first_marine:0'), ('ai_free', 'first_marine'), ('ai_exit_vehicle', 'first_marine'),
              ('ai_free_units', 'list:first_marine'), ('ai_teleport_to_starting_location', 'first_marine'),
              ('ai_teleport_to_starting_location_if_unsupported', 'first_marine'), ('unit_set_emotion', 'unit:first_marine:1 1'),
              ('units_set_desired_flashlight_state', 'list:first_marine true'), ('ai_conversation_advance', '0'),
              ('ai_conversation_line', '0'), ('ai_allegiance_broken', '0 1'), ('ai_allegiance_remove', '0 1'),
              ('ai_reconnect', ''), ('ai_timer_expire', 'first_marine'), ('ai_vehicle_enterable_disable', 'first_marine'),
              ('ai_vehicle_enterable_actor_type', 'first_marine s:grunt'))),
    (3400, 'qa_actor_state end'),
    # The player: vitality, protection and damage.
    (3500, hs(('unit_set_maximum_vitality', 'player 100 0')) + '; qa_player_vitals'),
    (3600, hs(('unit_set_current_vitality', 'player 25 0')) + '; qa_player_vitals'),
    (3700, hs(('unit_set_maximum_vitality', 'player 100 75'), ('unit_set_current_vitality', 'player 100 75')) + '; qa_player_vitals'),
    # object_cannot_take_damage protects the player; unit_impervious does not (Halo's impervious bit
    # stops only knocks and pings: ../Halo3DS units/unit_scripting_commands.c:206-222, the
    # unit_impervious change, not yet run).
    (3800, hs(('object_cannot_take_damage', 'player'), ('damage_object', 's:shock player')) + '; qa_player_vitals'),
    (3850, hs(('object_can_take_damage', 'player'), ('unit_impervious', 'player true'), ('damage_object', 's:shock player')) +
           '; qa_player_vitals'),
    (3900, hs(('unit_impervious', 'player false'), ('damage_object', 's:shock player')) + '; qa_player_vitals'),
    # Scripted objects: hover, collision, open, enterable, permutation, damage.
    (4000, hs(('vehicle_hover', 'obj:foehammer_cliff false'), ('object_set_collideable', 'obj:back_cship false'),
              ('unit_open', 'obj:rubble_boat_cship'), ('unit_set_enterable_by_player', 'obj:foehammer_rubble false'),
              ('object_cannot_take_damage', 'obj:enc5_1_detector_s3'), ('unit_suspended', 'obj:foehammer_rubble true'),
              ('object_set_permutation', 'obj:foehammer_cliff s:door_on'), ('object_set_ranged_attack_inhibited', 'obj:foehammer_cliff true'),
              ('object_set_melee_attack_inhibited', 'obj:foehammer_cliff true'), ('ai_attach', 'obj:foehammer_cliff first_marine'))),
    (4100, '; '.join(f'qa_hs_object {what} obj:{name}' for what, name in (
        ('hovering', 'foehammer_cliff'), ('collideable', 'back_cship'), ('open', 'rubble_boat_cship'),
        ('enterable', 'foehammer_rubble'), ('suspended', 'foehammer_rubble'), ('permutation', 'foehammer_cliff'),
        ('ranged', 'foehammer_cliff'), ('melee', 'foehammer_cliff'), ('squad', 'foehammer_cliff')))),
    (4200, hs(('unit_close', 'obj:rubble_boat_cship'), ('ai_detach', 'obj:foehammer_cliff')) +
           '; qa_hs_object open obj:rubble_boat_cship; qa_hs_object squad obj:foehammer_cliff'),
    (4300, 'quit'),
], timeout=240)


def by_name(label):
    # Actors are keyed by client number: names such as "Grunt 3" contain a space.
    rows = {a['cn']: a for a in actors(text, label)}
    for line in text.splitlines():
        if line.startswith(f'ACTOR_EXTRA {label} '):
            fields = dict(re.findall(r'(\w+)=(\S+)', line))
            rows[fields['cn']].update(fields)
    return rows


FLAGS = dict(braindead=4, nodamage=512, impervious=1024, nodrops=2048, nodormant=4096, nomaneuver=8192)
base = {a['cn']: a for a in actors(text, 'base') if a['team'] == '0'}
names = sorted(base, key=int)
assert len(names) >= 7, ('a30 section 2 places enough Covenant actors for the unit commands', names, log)

flags = by_name('flags')
for i, name in enumerate(names[:6]):
    a = flags[name]
    assert a['morale'] == 'berserk', ('ai_berserk puts the encounter into a berserk charge', name, a)
    assert a['grenades_on'] == '0', ('ai_grenades false', a)
    bits = int(a['flags'])
    assert bits & FLAGS['nodormant'] and bits & FLAGS['nomaneuver'] and bits & FLAGS['nodrops'], ('encounter flags and drops', name, bits)
    assert bool(bits & FLAGS['nodamage']) == (i == 0), ('object_cannot_take_damage targets the first actor', name, bits)
    assert bool(bits & FLAGS['impervious']) == (i == 1), ('unit_impervious targets the second actor', name, bits)
    assert bool(bits & FLAGS['braindead']) == (i == 2), ('ai_braindead_by_unit targets the third actor', name, bits)

hurt = by_name('hurt')
assert int(hurt[names[0]]['health']) == int(base[names[0]]['health']), ('cannot-take-damage actor is unhurt', hurt[names[0]])
# Shielded actors take the damage on their shield first. unit_impervious does not protect (the
# unit_impervious change, not yet run): the impervious actor is hurt like the unprotected one.
for n, why in ((names[1], 'damage_object hurts an impervious actor'), (names[6], 'damage_object hurts or kills an unprotected actor')):
    assert n not in hurt or int(hurt[n]['health']) < int(base[n]['health']) or float(hurt[n]['shield']) < float(flags[n]['shield']), \
        (why, hurt.get(n))
assert float(hurt[names[0]]['shield']) == float(flags[names[0]]['shield']), 'protected actors keep their shield'

vital = by_name('vital')
points = {f['cn']: f for f in (dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('ACTOR_VITALITY vital '))}
for n, given in ((names[3], 10), (names[4], 0.25)):
    # Points over the stored (unscaled) maximum, capped at 1, of the full health; 0.25 point keeps 1.
    fraction = min(1, given / float(points[n]['body_points']))
    expected = max(1, int(int(base[n]['health']) * fraction + .5))
    assert int(vital[n]['health']) == expected, ('unit_set_current_vitality sets points of the stored maximum', given, base[n], vital[n], points[n])
    assert float(points[n]['shield']) == 0, ('0 shield points empty the shield', points[n])
renewed = by_name('renewed')
for n in names[3:5]:
    assert int(renewed[n]['health']) == int(base[n]['health']), ('ai_renew restores full health', renewed[n])

cleared = by_name('cleared')
for n in names[:6]:
    a = cleared[n]
    assert a['morale'] != 'berserk' or a['alive'] == '0', ('ai_berserk false', a)
    assert a['grenades_on'] == '1' and not int(a['flags']) & (FLAGS['braindead'] | FLAGS['impervious'] | FLAGS['nodormant']), ('switches clear', a)
    assert int(a['flags']) & FLAGS['nodamage'] or n != names[0], 'object_cannot_take_damage stays until lifted'

marines = {n: a for n, a in by_name('follow').items() if a['team'] == '1'}
assert marines and all(a['follow'] == '1' for a in marines.values()), ('ai_follow_target_ai', marines)
assert all(a['follow'] == '0' for a in by_name('unfollow').values() if a['team'] == '1'), 'ai_follow_target_disable'

view = {}
for label in ('asleep_a', 'asleep_b'):
    for line in text.splitlines():
        if line.startswith(f'ACTOR_VIEW {label} '):
            fields = dict(re.findall(r'(\w+)=(\S+)', line))
            view.setdefault(label, {})[fields['cn']] = int(fields['thinks'])
suspended = names[5]
assert view['asleep_b'][suspended] == view['asleep_a'][suspended], ('unit_suspended stops the actor thinking', view)
assert any(view['asleep_b'][n] > view['asleep_a'][n] for n in names[:5]), ('other actors keep thinking', view)

counts = [(c, float(v)) for c, enc, v in probes(text, 'HS_AI') if c == 'ai_living_count']
lz = [v for (c, v), (_, enc, _) in zip(counts, [p for p in probes(text, 'HS_AI') if p[0] == 'ai_living_count']) if enc == 'lz_search']
fm = [v for (c, v), (_, enc, _) in zip(counts, [p for p in probes(text, 'HS_AI') if p[0] == 'ai_living_count']) if enc == 'first_marine']
assert len(lz) >= 3 and len(fm) >= 3, (lz, fm, log)
# Each command moves the then-first living actor of lz_search, so two actors change sides.
assert lz[1] == lz[0] - 2 and fm[1] == fm[0] + 2, ('ai_migrate_by_unit and ai_attach move one actor each', lz, fm)
assert lz[2] == 0 and fm[2] == fm[0] + lz[0], ('ai_migrate_and_speak moves the rest', lz, fm)

vitals = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('QA_VITALS')]
assert len(vitals) >= 7, (vitals, log)
base_v, no_shield, low, full, protected, impervious, hurt_v = vitals[:7]
assert float(no_shield['shield']) == 0.0, ('maximum shield 0 removes the shield', no_shield)
assert int(low['health']) == round(25 / 100 * 75), ('25 of 100 health is a quarter', low)
assert int(full['health']) == 75 and float(full['shield']) > 0.99, ('full vitality refills health and shield', full)
assert protected['protected'] == '1' and int(protected['health']) == 75 and float(protected['shield']) > 0.99, ('a protected player takes no damage', protected)
assert impervious['protected'] == '0' and (int(impervious['health']) < 75 or float(impervious['shield']) < 0.99), \
    ('unit_impervious leaves the player hurt by damage_object', impervious)
assert hurt_v['protected'] == '0' and (int(hurt_v['health']) < 75 or float(hurt_v['shield']) < 0.99), ('damage_object hurts the player', hurt_v)

# The first nine probes read the state after the commands; the last two read it after the reversals.
rows = probes(text, 'HS_OBJECT')
named_objects = {(w, n.replace('obj:', '')): float(v) for w, n, v in rows[:9]}
assert named_objects[('hovering', 'foehammer_cliff')] == 0 and named_objects[('collideable', 'back_cship')] == 0, named_objects
assert named_objects[('open', 'rubble_boat_cship')] == 1 and named_objects[('enterable', 'foehammer_rubble')] == 0, named_objects
assert named_objects[('suspended', 'foehammer_rubble')] == 1 and named_objects[('permutation', 'foehammer_cliff')] == 7, named_objects
assert named_objects[('ranged', 'foehammer_cliff')] == 1 and named_objects[('melee', 'foehammer_cliff')] == 1, named_objects
assert named_objects[('squad', 'foehammer_cliff')] >= 0, ('ai_attach records the squad of a scripted unit', named_objects)
closed = [float(v) for w, n, v in probes(text, 'HS_OBJECT') if w == 'open'][-1]
freed = [float(v) for w, n, v in probes(text, 'HS_OBJECT') if w == 'squad'][-1]
assert closed == 0 and freed == -1, ('unit_close and ai_detach reverse the state', closed, freed)
print('Unit commands: switches, immunity, vitality, follow, suspend, migration, attach, player vitality and object state all apply.')


# 5. a50 prison doors: object_set_permutation door_off removes the door.
text, log = run('doors', 'qa_campaign_resume 3 1', [
    (0, 'qa_hs_object door obj:cell_a_door_a'),
    (200, hs(('object_set_permutation', 'obj:cell_a_door_a s:door_off-100')) + '; qa_hs_object door obj:cell_a_door_a'),
    (400, 'quit'),
], timeout=240)
doors = [float(v) for w, n, v in probes(text, 'HS_OBJECT')]
assert len(doors) == 2 and doors[0] == 1 and doors[1] == 0, ('a door set to door_off stops drawing and blocking', doors, log)
print('Doors: object_set_permutation door_off removes the cell door.')


# 6. Combat by difficulty (the combat-scaling work; written while testing was on hold, NOT YET RUN).
#    Halo scales hostile actors' firing by the globals' (matg) difficulty table on their team
#    (game_difficulty_get_team_value, ../Halo3DS game/game_globals.c:204-225: the player's side
#    reads Normal): rate_of_fire divides the trigger delay (ai/actor_combat.c:2394), projectile_error
#    multiplies the aim error (:1270), burst_separation the pause between bursts (:603),
#    target_leading only gates the lead (:2196), grenade_chance_scale the throw chance
#    (ai/actions.c:3645) and grenade_timer_scale the wait after a throw (:2356); melee_delay_scale
#    and _base (:2565) and guidance_vs_player (items/projectiles.c:802) are read for every team.
#    qa_actor_combat prints the team factors (COMBAT_DIFFICULTY and _MORE) and, per living actor, the values
#    its brain fires with (ACTOR_COMBAT); they must follow actors.txt's `difficulty` lines and the
#    variant's own `fire`, `burst` and `grenade` lines.
#    Since the melee-delay and grenade-timeout export (NOT YET RUN): the smash interval is the engine's
#    attack (1.6 or 2.199 s) plus scale x the `berserk` line's actr melee_attack_delay_timer (0 while
#    berserk) plus base (actions.c:2563-2568), or the whole interval scaled for a swarm actor or a table
#    without the timer; ACTOR_TIMERS carries the `grenade` line's encounter timeout, and after
#    qa_actor_grenade_note every member of the noted encounter has its scaled timeout left in full
#    (timeout x grenade_timer_scale in whole ticks, actions.c:2350-2373).
table, variants, current = {}, {}, None
for entry in (root / 'packages/halo/actors.txt').read_text().splitlines():
    key, _, rest = entry.partition(' ')
    if key == 'difficulty':
        index, name, *values = rest.split()
        table[name] = [float(v) for v in values]
    elif key == 'actor':
        current = variants.setdefault(int(rest), {})
    elif current is not None and key in ('fire', 'burst', 'grenade', 'berserk'):
        current[key] = [float(v) for v in rest.split()]
    elif current is not None and key == 'family':
        current['actr_flags'] = int(rest.split()[3])
for name in ('rate_of_fire', 'projectile_error', 'burst_separation', 'target_leading', 'grenade_chance_scale',
             'grenade_timer_scale', 'melee_delay_scale', 'melee_delay_base', 'guidance_vs_player'):
    assert name in table, ('actors.txt has no difficulty table (format 3); rerun tools/halo_actors.py', name)
# The retail values (the same in the ten caches; tests/test_difficulty.py checks the export).
assert [round(v, 2) for v in table['rate_of_fire']] == [0.8, 1, 1.2, 1.5], table['rate_of_fire']
assert [round(v, 2) for v in table['guidance_vs_player']] == [0.5, 0.5, 0.75, 1], table['guidance_vs_player']
FACTORS = {'rof': 'rate_of_fire', 'perror': 'projectile_error', 'burst_error': 'burst_error', 'new_target': 'new_target_delay',
           'separation': 'burst_separation', 'tracking': 'target_tracking', 'leading': 'target_leading',
           'overcharge': 'overcharge_chance', 'special': 'special_fire_delay', 'grenade_chance': 'grenade_chance_scale',
           'grenade_timer': 'grenade_timer_scale'}
GLOBAL = {'melee_scale': 'melee_delay_scale', 'melee_base': 'melee_delay_base', 'guidance': 'guidance_vs_player'}


def near(a, b, tolerance=.0015):
    return abs(a - b) <= tolerance   # printed with three decimals


summary = []
for d in range(4):
    text, log = run(f'combat-{d}', f'halo_difficulty {d}; qa_campaign_resume 2 2', [
        (0, 'qa_actor_place lz_search; qa_actor_place first_marine'),
        (300, 'qa_hs_ai ai_magically_see_players lz_search 0'),
        (5000, 'qa_actor_state combat; qa_actor_combat combat; qa_actor_grenade_note note; qa_actor_combat noted'),
        (5300, 'quit'),
    ], timeout=150)
    teams = {}
    for line in text.splitlines():
        # Two lines per team (the console cuts lines at 260 characters), merged by team.
        if line.startswith('COMBAT_DIFFICULTY combat ') or line.startswith('COMBAT_DIFFICULTY_MORE combat '):
            fields = dict(re.findall(r'(\w+)=(\S+)', line))
            teams.setdefault(fields['team'], {}).update(fields)
    assert sorted(teams) == ['covenant', 'flood', 'human', 'none', 'player', 'sentinel'], (sorted(teams), log)
    for team, fields in teams.items():
        assert int(fields['level']) == d and fields['table'] == '1', ('the level and the table', d, fields, log)
        # Hostile teams (and no team) the level's value; the player and the marines Normal's.
        level = 1 if team in ('human', 'player') else d
        for field, name in FACTORS.items():
            assert near(float(fields[field]), table[name][level]), ('team value', d, team, field, fields[field], table[name][level])
        for field, name in GLOBAL.items():
            assert near(float(fields[field]), table[name][d]), ('every team reads the level', d, team, field, fields[field])
    shots = {a['cn']: int(a['shots']) for a in actors(text, 'combat')}
    rows = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith('ACTOR_COMBAT combat ')]
    timer_rows = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith('ACTOR_TIMERS combat ')]
    covenant = [r for r in rows if r['team'] == 'covenant']
    marines = [r for r in rows if r['team'] == 'human']
    assert covenant and marines, ('lz_search and first_marine are placed and alive', len(covenant), len(marines), log)
    written = 0
    for r in rows:
        v = variants[int(r['variant'])]
        level = 1 if r['team'] in ('human', 'player') else d
        rate, error, lead = v['fire'][1], v['fire'][2], v['fire'][7]
        assert near(float(r['scaled_error']), error * table['projectile_error'][level], .0002), ('projectile_error', d, r)
        assert r['leads'] == ('1' if min(1.0, lead + table['target_leading'][level]) > 0 else '0'), ('lead gate', d, r)
        separation = [s * table['burst_separation'][level] for s in v['burst'][2:4]]
        assert all(near(float(a), b) for a, b in zip(r['separation'].split(','), separation)), ('burst_separation', d, r)
        assert near(float(r['grenade_scaled']), v['grenade'][1] * table['grenade_chance_scale'][level]), ('grenade_chance_scale', d, r)
        if int(r['melee']) == 0 and rate > 0:
            want = max(66, int(1000 / (rate * table['rate_of_fire'][level])))
            assert abs(int(r['delay']) - want) <= 1, ('rate_of_fire on the trigger delay', d, r, want)
            # The brain writes the delay into the slot on each shot (an actor whose only noise
            # was a grenade still has its spawn delay, so only count the matches; on Normal the
            # two are the same). Only the hostile actors count: a marine reads Normal, so its slot
            # matches from spawn whether or not fire() wrote it.
            if r['team'] not in ('human', 'player') and shots.get(r['cn'], 0) > 0 and int(r['slot']) == int(r['delay']):
                written += 1
        smash = [int(x) for x in r['smash'].split(',')]
        timers = next(t for t in timer_rows if t['cn'] == r['cn'])
        berserk = v.get('berserk', [])
        timer = berserk[3] if len(berserk) > 3 else -1
        assert near(float(timers['melee_timer']), timer), ('the actr melee delay', d, timers, berserk)
        swarm = (v.get('actr_flags', 0) >> 26) & 1
        assert int(timers['swarm']) == swarm, ('swarm bit', d, timers)
        for base, got in zip((1600, 2199), smash):
            if timer < 0 or swarm:
                want = round(base * table['melee_delay_scale'][d] + table['melee_delay_base'][d] * 1000)
            else:
                held = 0 if timers['berserk'] == '1' else timer
                want = base + round((table['melee_delay_scale'][d] * held + table['melee_delay_base'][d]) * 1000)
            assert abs(got - want) <= 1, ('melee delay', d, r, timers, want)
        grenade = v['grenade']
        assert near(float(timers['grenade_timeout']), grenade[6] if len(grenade) > 6 else -1), ('the actv grenade timeout', d, timers)
    assert written, ('at least one Covenant actor fired at its scaled delay', d, shots, log)
    # The encounter timeout after a (noted) throw, for every member of that encounter.
    note = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith('GRENADE_NOTE note ')]
    assert note and 'encounter' in note[0], ('an lz_search actor has an encounter and a grenade timeout', note, log)
    noted = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines() if line.startswith('ACTOR_TIMERS noted ')]
    team_of = {r['cn']: r['team'] for r in rows}
    members = [t for t in noted if t['encounter'] == note[0]['encounter'] and float(t['grenade_timeout']) >= 0]
    assert members, ('the noted encounter has members', note, noted)
    for t in members:
        level = 1 if team_of.get(t['cn']) in ('human', 'player') else d
        want = int(float(t['grenade_timeout']) * table['grenade_timer_scale'][level] * 30) * 1000 // 30
        assert abs(int(t['grenade_wait']) - want) <= 34, ('grenade_timer_scale on the encounter timeout', d, t, want)
        assert int(t['grenade_left']) == int(t['grenade_wait']), ('the whole timeout is left right after the throw', d, t)
    summary.append((d, round(float(covenant[0]['scaled_error']), 4), covenant[0]['delay'], marines[0]['scaled_error']))
print(f'Combat scaling: per level (level, a Covenant actor\'s error and delay, a marine\'s error) {summary}.')


# 7. Script vitality in Halo's points (the script-vitality change; written while testing was on hold,
#    NOT YET RUN). unit_set_current_vitality makes its points a fraction of the unit's stored maximum,
#    capped at 1 (../Halo3DS units/unit_scripting_commands.c:85-133); unit_set_maximum_vitality
#    re-initialises the maxima at full vitality (:54-65 with objects/damage.c:398-427), which ai_renew
#    then refills to and a checkpoint keeps (the `s squad` record's optional ` max <body> <shield>`);
#    a body set to 0 dies even under object_cannot_take_damage (object_deplete_body, :127-128).
text, log = run('vitality', 'halo_difficulty 1; qa_campaign_play 2 2', [
    (0, 'botsshoot 0; qa_actor_place lz_search; qa_actor_place first_marine'),
    (300, hs(('ai_braindead', 'lz_search true'), ('ai_braindead', 'first_marine true')) + '; qa_actor_vitality base'),
    (400, hs(('unit_set_maximum_vitality', 'unit:lz_search:0 1 0')) + '; qa_actor_vitality max'),
    (500, hs(('unit_set_maximum_vitality', 'unit:lz_search:1 40 0'), ('unit_set_current_vitality', 'unit:lz_search:1 10 0')) +
          '; qa_actor_vitality quarter'),
    (600, hs(('unit_set_current_vitality', 'unit:lz_search:2 100000 100000')) + '; qa_actor_vitality full'),
    (700, hs(('ai_renew', 'lz_search')) + '; qa_actor_vitality renewed'),
    (800, 'qa_hs_ai ai_living_count lz_search 0; ' +
          hs(('object_cannot_take_damage', 'unit:lz_search:3'), ('unit_set_current_vitality', 'unit:lz_search:3 0 0'))),
    (900, 'qa_hs_ai ai_living_count lz_search 0; qa_actor_vitality depleted; qa_checkpoint_save'),
    (1200, 'qa_halo_damage 100000'),
    (3000, 'qa_halo_revert'),
    (3300, 'qa_actor_vitality reverted'),
    (3400, 'quit'),
], timeout=200)


def vitality_rows(label):
    return [dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith(f'ACTOR_VITALITY {label} ')]


def scripted(label):
    return sorted((r for r in vitality_rows(label) if r['scripted'] == '1'), key=lambda r: float(r['body_points']))


one = scripted('max')
assert len(one) == 1 and float(one[0]['body_points']) == 1 and float(one[0]['shield_points']) == 0, ('maximum 1 0', one, log)
assert one[0]['health'] == '1' and one[0]['start'] == '1' and float(one[0]['shield_max']) == 0, ('one point, no shield', one)
pair = scripted('quarter')
assert len(pair) == 2 and float(pair[1]['body_points']) == 40, ('maximum 40 0 on a second actor', pair, log)
assert pair[1]['start'] == '40' and pair[1]['health'] == '10', ('10 of 40 points on Normal is a quarter of 40 health', pair[1])
covenant_full = [r for r in vitality_rows('full') if r['team'] == '0' and r['scripted'] == '0']   # HALO_TEAM_COVENANT
assert covenant_full and all(r['health'] == r['start'] for r in covenant_full), ('points above the maximum cap at full', covenant_full)
renewed = scripted('renewed')
assert [r['health'] for r in renewed] == ['1', '40'], ('ai_renew refills to the script-set maxima', renewed)
counts = [float(v) for c, enc, v in probes(text, 'HS_AI') if c == 'ai_living_count' and enc == 'lz_search']
assert len(counts) >= 2 and counts[1] == counts[0] - 1, ('0 body points kill a protected actor', counts, log)
reverted = scripted('reverted')
assert [(float(r['body_points']), r['start'], r['health']) for r in reverted] == [(1.0, '1', '1'), (40.0, '40', '40')], \
    ('a revert keeps the script-set maxima', reverted, log)
saved = read_progress(log.parent)
maxima = sorted(r.split()[-2:] for r in saved['records']['s'] if r.startswith('squad ') and ' max ' in r)
assert maxima == [['1', '0'], ['40', '0']], ('the checkpoint records end with the maxima', maxima)
print('Script vitality: points of the stored maximum, a script maximum kept by ai_renew and a revert, depletion kills.')


# 8. ai_playfight (the playfight change; written while testing was on hold, NOT YET RUN). Halo's flag
#    is the encounter's (ai_scripting_playfight, ../Halo3DS ai/ai_script.c:1323-1339: a squad
#    reference sets its whole encounter) and every member takes it, later ones included
#    (encounters.c:3734); a playfighting actor aims with 2 x error + 1 degree (actor_combat.c:
#    1278-1281), pauses 1.7 x as long between bursts of 0.6 x the length (612-615, 1259-1262), waits
#    2 x the encounter grenade timeout (actions.c:2361-2362), and its damage is 0.3 of itself
#    (ai.c:671-675). The checkpoint keeps the encounters' flags (the optional `s playfight` record).
hscb = read_hscb(root / 'packages/halo/a30.hscb')
encounter_index = {name: i for i, (name, squads) in enumerate(hscb['encounters'])}
LZ, FM = encounter_index['lz_search'], encounter_index['first_marine']
fm_squad = hscb['encounters'][FM][1][0]
text, log = run('playfight', 'halo_difficulty 1; qa_campaign_play 2 2', [
    (0, 'botsshoot 0; qa_combat_protect 1; qa_actor_place first_marine'),
    (100, hs(('ai_playfight', 'lz_search true'))),
    (200, 'qa_actor_place lz_search'),
    (400, 'qa_actor_state joined; qa_actor_combat joined'),
    (500, hs(('ai_migrate_by_unit', 'unit:lz_search:0 first_marine'))),
    (600, 'qa_actor_state migrated; qa_actor_combat migrated'),
    (700, hs(('ai_playfight', f'first_marine/{fm_squad} true'))),
    (800, 'qa_actor_state squadref; qa_actor_combat squadref'),
    (900, 'qa_checkpoint_save'),
    (1000, hs(('ai_playfight', 'lz_search false'), ('ai_playfight', 'first_marine false'))),
    (1100, 'qa_actor_state off; qa_actor_combat off'),
    (1200, 'qa_combat_protect 0; qa_halo_damage 100000'),
    (3000, 'qa_halo_revert'),
    (3300, 'qa_actor_state reverted; qa_actor_combat reverted'),
    (3400, 'qa_combat_protect 1; halo_damagetrace 1; botsshoot 1; ' +
           hs(('ai_magically_see_encounter', 'lz_search first_marine'), ('ai_magically_see_encounter', 'first_marine lz_search'))),
    (9400, 'quit'),
], timeout=200)


def members(label):
    """{cn: (encounter, playfight flag, ACTOR_COMBAT fields, ACTOR_TIMERS fields)} of the living actors."""
    flags = {a['cn']: int(a['flags']) for a in actors(text, label) if a['alive'] == '1'}
    combat = {f['cn']: f for f in (dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith(f'ACTOR_COMBAT {label} '))}
    timers = {f['cn']: f for f in (dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith(f'ACTOR_TIMERS {label} '))}
    return {cn: (int(timers[cn]['encounter']), bool(bits & 8), combat[cn], timers[cn]) for cn, bits in flags.items() if cn in timers}


def check_values(row, on):
    """The brain's values for a member that plays at fighting (on) or not, against actors.txt."""
    _, flag, c, t = row
    v = variants[int(c['variant'])]
    assert flag == on and t['playfight'] == ('1' if on else '0'), ('the playfight flag', on, row)
    error = v['fire'][2] * table['projectile_error'][1]
    assert near(float(c['scaled_error']), 2 * error + 0.0174533 if on else error, .0002), ('2 x error + 1 degree', on, c)
    separation = [s * table['burst_separation'][1] * (1.7 if on else 1) for s in v['burst'][2:4]]
    assert all(near(float(a), b) for a, b in zip(c['separation'].split(','), separation)), ('1.7 x the separation', on, c)
    burst = [b * (.6 if on else 1) for b in v['burst'][0:2]]
    assert all(near(float(a), b) for a, b in zip(t['burst'].split(','), burst)), ('0.6 x the burst', on, t)
    if float(t['grenade_timeout']) >= 0 and t['encounter'] != '-1':
        want = int(float(t['grenade_timeout']) * table['grenade_timer_scale'][1] * (2 if on else 1) * 30) * 1000 // 30
        assert abs(int(t['grenade_wait']) - want) <= 34, ('2 x the grenade timeout', on, t, want)


joined = members('joined')
assert any(r[0] == LZ for r in joined.values()) and any(r[0] == FM for r in joined.values()), ('both encounters placed', joined, log)
for row in joined.values():
    check_values(row, row[0] == LZ)   # placed after the ai_playfight: the joiners take it
migrated = members('migrated')
moved = [cn for cn, r in migrated.items() if r[0] == FM and joined.get(cn, (None,))[0] == LZ]
assert len(moved) == 1 and not migrated[moved[0]][1], ('a member migrated out leaves the playfight behind', moved, migrated)
squadref = members('squadref')
assert all(r[1] == (r[0] in (LZ, FM)) for r in squadref.values()), ('a squad reference sets its whole encounter', squadref)
assert squadref[moved[0]][1], ('the migrated member takes its new encounter flag', squadref[moved[0]])
assert not any(r[1] for r in members('off').values()), ('ai_playfight false clears every member', log)
reverted = members('reverted')
assert reverted and all(check_values(r, r[0] in (LZ, FM)) is None for r in reverted.values()), ('the revert restores the flags', reverted)
saved = read_progress(log.parent)
records = [r.split()[1:] for r in saved['records']['s'] if r.startswith('playfight ')]
assert records == [[str(2)] + [str(i) for i in sorted((LZ, FM))]], ('the checkpoint keeps both encounters', records)
hits = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('DAMAGE_PLAYFIGHT ')]
assert hits, ('the playfighting encounters hurt each other (or the player)', log)
for h in hits:
    assert near(float(h['factor']), .3) and near(float(h['scaled']), float(h['amount']) * .3, .002), ('0.3 of the damage', h)
print(f'Playfight: encounter-wide, joiners and migrations, the burst and grenade values, kept by a revert; {len(hits)} hits at 0.3.')


# 9. The scripts' effect damage on actors (the effect-damage default change; written while testing was
#    on hold, NOT YET RUN). hs_effect_new creates its effect with owner NONE (../Halo3DS
#    hs/hs_library_external.c:699-724) and the effect's jpt! part deals area damage whatever the owner
#    (effects/effects.c:1760-1781). a10's mission_containment_1 sets the doom_crewman to 1 point, lifts
#    his protection once he is in containment_1_slam and sets off the plasma grenade's explosion at
#    containment_1_flag_4 (inside that volume): in Halo he dies to it. Static part: the flag lies in the
#    slam volume and tools/halo_effect_reach.py finds the call after the player enters
#    containment_1_trigger_4. Run: placed, braindead, at 1 point and stood at the flag, he lives
#    through the blast with halo_effectdamage 0 and dies to it by default, with no killer.
sys.path.insert(0, str(root / 'tools'))
import halo_effect_reach  # noqa: E402
a10 = halo_effect_reach.read_program(root / 'packages/halo/a10.hscb')
flag4 = next(pose[:3] for name, pose in a10['flags'] if name == 'containment_1_flag_4')
slam = next(v for v in a10['volumes'] if v[0] == 'containment_1_slam')
assert halo_effect_reach.inside_volume(slam, flag4), ('containment_1_flag_4 lies in containment_1_slam', flag4, slam)
reach = [e for e in halo_effect_reach.analyse(root / 'packages', ['a10'])
         if e['script'] == 'mission_containment_1' and e['where'] == 'flag containment_1_flag_4']
assert len(reach) == 1 and reach[0]['effect'] == r'weapons\plasma grenade\effects\explosion', ('the flag_4 blast', reach)
assert reach[0]['trigger'] == 'containment_1_trigger_4' and reach[0]['outer'] == 2, ('after containment_1_trigger_4, 2 wu', reach[0])
DOOM = 'containment_1/doom_crewman'
PLASMA = r'weapons\plasma grenade\effects\explosion'
text, log = run('effect-blast', 'halo_difficulty 2; halo_damagetrace 1; halo_scripttrace 1; qa_campaign_play 1 1', [
    (4000, 'botsshoot 0; halo_effectdamage; qa_actor_vitality pre'),
    (4300, hs(('ai_place', DOOM), ('ai_braindead', f'{DOOM} true'))),
    (4600, hs(('units_set_current_vitality', f'list:{DOOM} 1 0'), ('object_can_take_damage', f'list:{DOOM}'),
              ('object_teleport', f'unit:{DOOM}:0 flag:containment_1_flag_4')) + '; qa_actor_vitality doom'),
    (5000, f'qa_hs_ai ai_living_count {DOOM} 0; halo_effectdamage 0; qa_fx_script_effect "{PLASMA}" containment_1_flag_4'),
    (5400, f'qa_hs_ai ai_living_count {DOOM} 0; halo_effectdamage 1; qa_fx_script_effect "{PLASMA}" containment_1_flag_4'),
    (5800, f'qa_hs_ai ai_living_count {DOOM} 0'),
    (5900, 'quit'),
], timeout=200)
assert re.search(r'halo_effectdamage = 1\b', text), ('effect damage is on by default', log)


# Actor names have a space ("<profile> <n>"): read them whole, and leave out the actors placed before
# the doom crewman (a10's other crew may be at 1 point too).
def vitality_names(label, health=None):
    return {m.group(1) for l in text.splitlines() if l.startswith(f'ACTOR_VITALITY {label} ')
            for m in [re.search(r' name=(.+?) kind=', l)] if m and (health is None or f' health={health} ' in l)}


doom = vitality_names('doom', 1) - vitality_names('pre')
assert doom, ('the doom crewman at 1 point (1 health)', log)
counts = [float(v) for c, enc, v in probes(text, 'HS_AI') if c == 'ai_living_count' and enc == DOOM]
assert len(counts) == 3 and counts[0] >= 1, ('three counts of the placed squad', counts, log)
assert counts[1] == counts[0], ('halo_effectdamage 0: the blast is only drawn', counts, log)
assert counts[2] == counts[1] - 1, ('the blast at the flag kills the crewman stood on it (the other start is 3.5 wu away)', counts, log)
deaths = [dict(re.findall(r'(\w+)=(\S+)', l), victim=m.group(1)) for l in text.splitlines() if l.startswith('DEATH_TRACE ')
          for m in [re.search(r'victim=(.+?) killer=', l)] if m]
blasted = [d for d in deaths if d['victim'] in doom]
assert blasted and blasted[-1]['killer'] == '-' and blasted[-1]['ownerless'] == '1', ('an ownerless kill, as in Halo', deaths, log)
# The mission's own scripts may set off their explosions meanwhile (tutorial_asst_kill): only the plasma's count.
dealt = [d for d in re.findall(r'HaloScript: (.+?) (deals its jpt! damage|has no jpt! damage)', text) if d[0] == PLASMA]
assert dealt == [(PLASMA, 'deals its jpt! damage')], ('only the second blast deals damage', dealt)
print(f'Effect damage: the doom crewman lives through the flag_4 blast with halo_effectdamage 0 and dies to it by default, '
      f'ownerless ({counts}).')


# 10. Halo's melee on armed actors and the sword Elites (written while testing was on hold, NOT YET RUN).
#     An Elite minor with a plasma rifle 1.2 world units off charges inside its melee_range (1.75), not in
#     the middle of a burst, and strikes the player with its unit's melee jpt! (elite melee: 30 to 35; the
#     plasma rifle's own melee is for players, units.c:8597-8612), unmodified by its 0.5 (ai.c:654-675).
#     The stealth sword Elite (stalking, no ranged weapon: its melee_range, 4) strikes with the sword's
#     jpt! (151: the sword's non_players_use_melee_damage flag), where the engine's slot gave 75.
def melee_strikes(text, phase):
    return [(m.group(1), float(m.group(2))) for m in re.finditer(rf'ACTOR_MELEE {phase} (.+?) variant=\d+ target=player distance=\S+ value=([\d.]+)', text)]


for variant, phase, low, high in (('elite minor plasma rifle', 'strike_unit', 30, 35), ('stealth elite major energy sword', 'strike_weapon', 151, 151)):
    text, log = run('melee-' + variant.split()[-1], 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
        (0, f'qa_combat_protect 1; qa_variant_spawn "{variant}" 0 {1.2 if phase == "strike_unit" else 2} 0'),
        (300, 'qa_actor_state armed'),
        (8000, 'quit'),
    ])
    state = next((dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('ACTOR_MELEE_QA armed ')), None)
    assert state and state['model'] == '1', ('Halo\'s melee applies', state, log)
    assert state['ranged'] == ('1' if phase == 'strike_unit' else '0'), state
    strikes = melee_strikes(text, phase)
    assert strikes and all(low <= v <= high for _, v in strikes), (variant, strikes, log)
    charge = re.search(r'ACTOR_MELEE charge .+? target=player distance=(\S+) value=(\S+)', text)
    assert charge and float(charge.group(1)) <= float(charge.group(2)) + .01, ('a charge inside the range', charge and charge.group(0), log)
    print(f'Melee: {variant} struck {len(strikes)} time(s) for {[round(v, 1) for _, v in strikes]}.')


# 11. Special fire (the special-fire change; written while testing was on hold, NOT YET RUN). The
#     actv's special fire (../Halo3DS ai/actor_combat.c:1765-1840): outside a burst, once its delay
#     has run out and its situation holds, the delay restarts at the tag's plus 0 to 1.5 s and the
#     chance is rolled; an overcharge (mode 1) holds the plasma pistol's trigger through the pause and
#     the next burst's first pull releases it, the second trigger's charged bolt (EMP, side 3) when it
#     was held 0.6 s (items/weapons.c:3070-3110). A special burst's damage takes the variant's 0.4
#     (actor_combat.c:1339-1345) on top of its 0.5: the charged bolt (70 flat) deals 14.
#     Static: actors.txt's `special` lines (actv +340/+342/+344/+348, +248/+252) are on the two
#     Jackal plasma pistol variants and the Banshee pilot only, with the tag values; against the
#     tag bytes too when HALO_SOURCE has the caches.
specials, current = {}, None
for entry in (root / 'packages/halo/actors.txt').read_text().splitlines():
    key, _, rest = entry.partition(' ')
    if key == 'path':
        current = rest
    elif key == 'special' and current:
        specials[current] = [float(v) for v in rest.split()]
expected = {r'characters\jackal\jackal major plasma pistol': [1, 1, .3, 5, .4, 0],
            r'characters\jackal\jackal minor plasma pistol': [1, 1, .2, 5, .4, 0],
            r'vehicles\banshee\banshee pilot': [2, 3, 1, 10, 0, 0]}
assert set(specials) == set(expected), ('special lines on exactly these variants; rerun tools/halo_actors.py', sorted(specials))
for path, values in expected.items():
    assert all(abs(a - b) < 1e-4 for a, b in zip(specials[path], values)), (path, specials[path])
if (HALO_SOURCE / 'a30.map').exists():
    import mmap  # noqa: E402
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_weapons import CAMPAIGN  # noqa: E402
    seen = {}
    for cache in CAMPAIGN:
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group == 'actv' and tag.name not in seen:
                    t, a = halo.tags, tag.address
                    mode, situation = t.read('2h', a + 340)
                    seen[tag.name] = [mode, situation, *t.read('2f', a + 344), *t.read('2f', a + 248)]
    for path, values in seen.items():
        if values[0] > 0:
            assert path in specials and all(abs(x - y) < 1e-4 for x, y in zip(specials[path], values)), (path, values, specials.get(path))
        else:
            assert path not in specials, path
    print(f'Special fire: actors.txt matches the tag bytes ({sum(v[0] > 0 for v in seen.values())} variants with special fire).')


def special_events(text, event):
    return [(m.group(1), int(m.group(2)), float(m.group(3)), int(m.group(4))) for m in
            re.finditer(rf'ACTOR_SPECIAL_FIRE {event} (.+?) variant=\d+ mode=\d value=(-?\d+) extra=(\S+) ms=(\d+)', text)]


# On the preview at Normal, protected, a Jackal major 6 units off fights the player; halo_specialfire 2
# makes every roll succeed, so the delays alone space the specials.
for mode in (2, 0):
    text, log = run(f'special-{mode}', f'halo_damagetrace 1; halo_difficulty 1; halo_specialfire {mode}; halo_preview', [
        (0, 'qa_combat_protect 1; qa_variant_spawn "jackal major plasma pistol" -1 6 0; qa_variant_spawn "grunt major plasma pistol" -1 6 3'),
        (30000, 'qa_actor_combat special'),
        (30500, 'quit'),
    ], timeout=120)
    spawned = re.findall(r'^FACTION_SPAWN (.+?) kind=', text, re.M)
    assert len(spawned) == 2 and '-' not in spawned, ('both variants spawned', spawned, log)
    jackal, grunt = spawned
    rolls = [e for e in special_events(text, 'roll') if e[0] == jackal]
    releases = [e for e in special_events(text, 'release') if e[0] == jackal]
    bursts = [e for e in special_events(text, 'burst') if e[0] == jackal]
    charged_hits = [float(m.group(1)) for m in re.finditer(rf'DAMAGE_TRACE attacker={re.escape(jackal)} target=player exact=1 amount=(\S+) side=3 ', text)]
    rows = {f['cn']: f for f in (dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('ACTOR_SPECIAL special '))}
    timers = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in text.splitlines() if l.startswith('ACTOR_TIMERS special ')]
    if mode == 0:
        assert 'ACTOR_SPECIAL_FIRE' not in text and not charged_hits, ('halo_specialfire 0: no special fire', log)
        print('Special fire: off with halo_specialfire 0.')
        continue
    # The Grunt's variant has mode 0 (its chance is never read): no special fire, no ACTOR_SPECIAL row.
    assert not any(e[0] == grunt for e in special_events(text, 'roll')), ('the Grunt has no special fire', log)
    assert rows and all(r['variant'] != '-1' for r in rows.values()), ('an ACTOR_SPECIAL row for the Jackal', log)
    row = next(iter(rows.values()))
    assert (row['mode'], row['situation'], row['chance'], row['delay'], row['modifier']) == ('1', '1', '0.300', '5.000', '0.400'), row
    assert len(rows) == 1, ('only the Jackal has an ACTOR_SPECIAL row', rows)
    assert any(t['special_left'] != '-1' for t in timers) and any(t['special_left'] == '-1' for t in timers), timers
    assert rolls and all(e[1] == 1 for e in rolls), ('the forced rolls succeed', rolls, log)
    gaps = [b[3] - a[3] for a, b in zip(rolls, rolls[1:])]
    assert all(g >= 5000 for g in gaps), ('no special roll within 5 s of another', gaps)
    assert all(5 <= e[2] <= 6.5 for e in rolls), ('the delay is the tag\'s plus 0 to 1.5 s', rolls)
    assert releases, ('an overcharge is released by a burst', log)
    assert all((e[1] == 1) == (e[2] >= .6) for e in releases), ('the charged bolt only after a 0.6 s hold', releases)
    assert bursts and all(e[1] == 0 and abs(e[2] - .4) < 1e-3 for e in bursts), ('a special burst takes the 0.4 (on the 0.5)', bursts)
    assert all(abs(v - 14) < .01 for v in charged_hits), ('the charged bolt deals 70 x 0.2', charged_hits, log)
    assert int(row['specials']) >= 1 and int(row['charged']) <= sum(e[1] == 1 for e in releases), row
    print(f'Special fire: {len(rolls)} roll(s), {sum(e[1] == 1 for e in releases)} charged release(s), '
          f'{len(charged_hits)} charged hit(s) of {sorted(set(charged_hits))}.')


# 12. Danger zones and dives (the danger-zone work; written while testing was on hold, NOT YET RUN).
#     Halo's actors keep one danger (../Halo3DS ai/actor_perception.c:4932-5600, 6286-6486): a live
#     grenade within its danger radius + 10, a moving vehicle within its bounding radius + 10, a carrier
#     in its fatal melee; an enemy's after 30, 20 or 6 ticks perceived and the actr's notice chance, its
#     own or a friend's at once; a dive allowed on an enemy grenade by grenade_dive_chance, always on
#     the rest. The dive (actions.c:3761-3952, 4699-5054) moves the body by the unit's dive animation;
#     a grenade stuck to a Grunt makes it panic (actions.c:1443-1479), to an Elite in combat within its
#     berserk firing range roll its attached berserk chance every tick (1938-1970).
#     Static: actors.txt's `danger` and `dive` lines, equipment.txt's and weapons.txt's `danger` lines and
#     vehicles.txt's `flags` against the tag bytes when HALO_SOURCE has the caches.
def records(path, keys):
    out, current = {}, None
    for entry in (root / path).read_text().splitlines():
        key, _, rest = entry.partition(' ')
        if key in ('path', 'vehicle'):
            current = rest
        elif key in keys and current:
            out.setdefault(current, {})[key] = [float(v) for v in rest.split()]
    return out


dangers = records('packages/halo/actors.txt', ('danger', 'dive'))
grunt = dangers.get(r'characters\grunt\grunt minor plasma pistol', {})
assert grunt.get('danger') and all(abs(a - b) < 1e-4 for a, b in zip(grunt['danger'], [.5, .75, .3, .3, 1, 0, 0, 0])), grunt
assert grunt.get('dive') and grunt['dive'][0] == 47 and abs(grunt['dive'][2] - 1.5824) < 1e-3, ('the Grunt dives left 1.58 over 47 frames', grunt)
carrier = dangers.get(r'characters\floodcarrier\floodcarrier', {})
assert carrier.get('danger') and abs(carrier['danger'][5] - 2) < 1e-4 and 'dive' not in carrier, ('the carrier: suicide radius 2, no dive', carrier)
assert not any('dive' in v for k, v in dangers.items() if 'flood' in k), ('the Flood have no dive animations', [k for k, v in dangers.items() if 'flood' in k and 'dive' in v])
equipment = [line for line in (root / 'packages/halo/equipment.txt').read_text().splitlines() if line.startswith('danger ')]
assert equipment == ['danger 2.25 8', 'danger 2.25 8'], ('both grenades: danger radius 2.25, perception 8; rerun tools/halo_weapons.py', equipment)
weapons = records('packages/halo/weapons.txt', ('danger',))
assert weapons == {r'vehicles\wraith\wraith mortar': {'danger': [2.0, 0.0]}}, ('only the Wraith mortar has a danger radius (2, perceived nowhere)', weapons)
flags = records('packages/halo/vehicles.txt', ('flags',))
assert len(flags) == 6 and all(int(v['flags'][0]) & 0x80 for v in flags.values()), ('every engine vehicle causes collision damage', flags)
if (HALO_SOURCE / 'a30.map').exists():
    import mmap  # noqa: E402
    sys.path.insert(0, str(root / 'tools'))
    from import_halo_a30 import HaloMap  # noqa: E402
    from halo_weapons import CAMPAIGN  # noqa: E402
    expected = {}
    for cache in CAMPAIGN:
        with (HALO_SOURCE / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group != 'actv' or tag.name in expected:
                    continue
                t, a = halo.tags, tag.address
                actr, unit = halo.reference(t, a + 4), halo.reference(t, a + 20)
                if not actr or actr.group != 'actr':
                    expected[tag.name] = None
                    continue
                r, u = actr.address, unit.address if unit and unit.group == 'bipd' else None
                expected[tag.name] = [*t.read('2f', r + 80), t.read('f', r + 136)[0], t.read('f', r + 128)[0],
                                      t.read('f', u + 568)[0] if u else 0, t.read('f', u + 644)[0] if u else 0,
                                      t.read('f', r + 936)[0], t.read('f', a + 364)[0]]
    for path, values in expected.items():
        if values is None:
            assert 'danger' not in dangers.get(path, {}), path
        else:
            assert all(abs(x - y) < 1e-4 for x, y in zip(dangers[path]['danger'], values)), (path, values, dangers[path])
    print(f'Danger zones: actors.txt matches the tag bytes ({sum(v is not None for v in expected.values())} variants with a danger line).')


def danger_events(text, event):
    return [(m.group(1), dict(re.findall(r'(\w+)=(\S+)', m.group(2)))) for m in re.finditer(rf'^ACTOR_DANGER {event} (.+?) (variant=.*)$', text, re.M)]


def danger_rows(text, kind, label):
    rows = []
    for line in text.splitlines():
        if line.startswith(f'{kind} {label} '):
            head, _, name = line.partition(' name=')
            rows.append({**dict(re.findall(r'(\w+)=(\S+)', head)), 'name': name})
    return rows


# a. An enemy frag 0.8 world units in front of a Grunt minor (halo_dangeravoid 2: every roll succeeds):
#    assessed at once (hostility 0, acknowledgement 1000 ms), noticed about 1 s later with a dive
#    allowed, and since no side is clear of a still grenade 0.8 units off, the dive comes once the frag
#    has under 20 ticks left (urgent): the Grunt's diving animation, 1.58 units, before the blast.
text, log = run('danger-dive', 'halo_damagetrace 1; halo_difficulty 1; halo_dangeravoid 2; halo_preview', [
    (0, 'qa_combat_protect 1; botsshoot 0; qa_variant_spawn "grunt minor plasma pistol" -1 6 0'),
    (1500, 'qa_grenade_place 0 -1 0.8 0 -1'),
    (2800, 'qa_actor_danger mid'),
    (4000, 'quit'),
])
placed = re.search(r'COMBAT_QA grenade-place actor=(.+?) owner=player type=0 ok=1', text)
assert placed, ('the frag was placed', log)
name = placed.group(1)
assess = [f for n, f in danger_events(text, 'assess') if n == name and f['type'] == '2']
notice = [f for n, f in danger_events(text, 'notice') if n == name]
dives = [f for n, f in danger_events(text, 'dive') if n == name]
assert assess and assess[0]['hostility'] == '0' and float(assess[0]['value']) == 1000, ('assessed as an enemy grenade', assess, log)
assert notice and notice[0]['value'] == '1.000', ('noticed with a dive allowed', notice, log)
assert int(notice[0]['ms']) - int(assess[0]['ms']) >= 950, ('noticed after the 30-tick acknowledgement', assess[0], notice[0])
assert dives and abs(float(dives[0]['value']) - 1.582) < .01, ('the Grunt dives by its animation', dives, log)
assert 0 <= int(dives[0]['left']) < 667, ('the dive waits until the frag is under 20 ticks from going off', dives[0])
print(f'Danger: the Grunt noticed the frag {int(notice[0]["ms"]) - int(assess[0]["ms"])} ms after it and dove with {dives[0]["left"]} ms left.')

# b. halo_dangeravoid 3 (notices succeed, dives fail): an enemy plasma grenade 1.5 units in front of a
#    fighting Grunt without squad positions: no dive; it steps out of the 2.25 radius before the blast.
text, log = run('danger-step', 'halo_damagetrace 1; halo_difficulty 1; halo_dangeravoid 3; halo_preview', [
    (0, 'qa_combat_protect 1; botsshoot 0; qa_variant_spawn "grunt minor plasma pistol" -1 6 0'),
    (1500, 'qa_grenade_place 1 -1 1.5 0 -1'),
    (3300, 'qa_actor_danger late'),
    (4500, 'quit'),
])
late = danger_rows(text, 'ACTOR_DANGER_QA', 'late')
state = danger_rows(text, 'ACTOR_DANGER_STATE', 'late')
steps = danger_events(text, 'step')
assert late and state and late[0]['type'] == '2' and late[0]['noticed'] == '1' and late[0]['dive_allowed'] == '0', (late, log)
assert state[0]['dives'] == '0' and steps, ('no dive; a step out instead', state, steps, log)
assert float(late[0]['line']) > 2.25, ('out of the danger radius before the plasma grenade goes off', late, log)
print(f'Danger: without a dive the Grunt stepped {late[0]["line"]} units from the plasma grenade.')

# c. A plasma grenade stuck to a Grunt minor: attached (no dive), it panics; a second Grunt 2 units to
#    its side takes the stuck grenade for its own danger. Stuck to an Elite minor 3 units off, in combat:
#    its attached berserk chance (0.3 a tick within 4 units) makes it berserk instead.
text, log = run('danger-stuck', 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
    (0, 'qa_combat_protect 1; botsshoot 0; qa_variant_spawn "grunt minor plasma pistol" -1 6 0; '
        'qa_variant_spawn "grunt minor plasma pistol" -1 6 2'),
    (1500, 'qa_grenade_place 1 -1 0 1 -1'),
    (2300, 'qa_actor_danger stuck'),
    (4500, 'quit'),
])
stuck = danger_rows(text, 'ACTOR_DANGER_QA', 'stuck')
attached = [r for r in stuck if r['attached'] == '1']
others = [r for r in stuck if r['attached'] == '0']
assert attached and attached[0]['type'] == '2' and attached[0]['dive_allowed'] == '0', ('the stuck Grunt: its own danger, no dive', stuck, log)
assert any(n == attached[0]['name'] for n, _ in danger_events(text, 'panic')), ('the stuck Grunt panics', log)
panic = {r['cn']: r for r in danger_rows(text, 'ACTOR_DANGER_STATE', 'stuck')}
assert panic[attached[0]['cn']]['morale'] == 'panicked' and panic[attached[0]['cn']]['panic'] == '1', panic
assert others and others[0]['type'] == '2', ('the other Grunt fears the stuck grenade', others, log)
# halo_dangeravoid 2 makes the berserk roll succeed at its first tick: a failed roll would panic it first.
text, log = run('danger-berserk', 'halo_damagetrace 1; halo_difficulty 1; halo_dangeravoid 2; halo_preview', [
    (0, 'qa_combat_protect 1; botsshoot 0; qa_variant_spawn "elite minor plasma rifle" -1 3 0'),
    (1500, 'qa_grenade_place 1 -1 0 1 -1'),
    (3800, 'quit'),
])
assert danger_events(text, 'berserk') and not danger_events(text, 'panic'), ('the stuck Elite goes berserk', log)
print('Danger: a stuck Grunt panics and its neighbour fears the grenade; a stuck Elite goes berserk.')

# d. The player drives a Warthog at a marine (d40 section 3 on stock geometry, as test_damage_model.py's
#    EMP drive): a friend's vehicle (hostility 1) is noticed at once and the marine dives (an urgent
#    impact: under 30 ticks along the Warthog's line) by its evade animation (or its dive-front).
text, log = run('danger-vehicle', 'halo_damagetrace 1; halo_difficulty 1; qa_stock_geometry 1; halo_section 10 3', [
    (100, 'botsshoot 0; qa_combat_protect 1'),
    (300, 'qa_vehicle_spawn 0 8; qa_variant_spawn "marine assault rifle" 1 9 0'),
    (600, 'qa_vehicle_enter 0 0'),
    (900, 'qa_vehicle_input 1 0 2500'),
    (3800, 'qa_actor_danger after'),
    (4200, 'quit'),
], timeout=120)
marine = re.search(r'^FACTION_SPAWN (.+?) kind=9 ', text, re.M)
assert marine, ('the marine spawned', log)
seen = [f for n, f in danger_events(text, 'assess') if n == marine.group(1) and f['type'] == '3']
dove = [f for n, f in danger_events(text, 'dive') if n == marine.group(1) and f['type'] == '3']
assert seen and seen[0]['hostility'] == '1', ('the player\'s Warthog, a friend\'s vehicle', seen, log)
# Its evade-left or evade-right (1.574, 1.588) or, facing across the Warthog's way, its dive-front (2.175).
assert dove and any(abs(float(dove[0]['value']) - v) < .01 for v in (1.574, 1.588, 2.175)), ('the marine dives aside', dove, log)
print(f'Danger: the marine dove {dove[0]["value"]} units from the player\'s Warthog.')

# e. A carrier in its fatal melee beside a marine: a suicide danger (radius 2) noticed after 6 ticks,
#    a dive allowed, taken with a clear side or under a second before the burst.
text, log = run('danger-carrier', 'halo_damagetrace 1; halo_difficulty 1; halo_preview', [
    (0, 'qa_combat_protect 1; qa_variant_spawn "floodcarrier" -1 2 0; qa_variant_spawn "marine assault rifle" 1 1 1.5'),
    (6000, 'quit'),
])
marine = re.search(r'^FACTION_SPAWN (.+?) kind=9 ', text, re.M)
if re.search(r'^ACTOR_MELEE suicide ', text, re.M):
    seen = [f for n, f in danger_events(text, 'assess') if marine and n == marine.group(1) and f['type'] == '1']
    assert seen and float(seen[0]['value']) == 200, ('the carrier is a suicide danger, acknowledged after 6 ticks', seen, log)
    assert any(marine and n == marine.group(1) and f['type'] == '1' for n, f in danger_events(text, 'notice')), log
    print(f'Danger: the marine noticed the carrier\'s melee; dives: {len(danger_events(text, "dive"))}.')
else:
    print('Danger: the carrier made no fatal melee in the run; item 12e checked nothing.')
