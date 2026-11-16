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
    # Damage: the immune actors keep their health, the rest lose some; vitality sets fractions; renew refills.
    (800, hs(('damage_object', 's:shock unit:lz_search:0'), ('damage_object', 's:shock unit:lz_search:1'), ('damage_object', 's:shock unit:lz_search:6'))),
    (900, 'qa_actor_state hurt'),
    (1000, hs(('units_set_current_vitality', 'unit:lz_search:3 0.5 0'), ('unit_set_current_vitality', 'unit:lz_search:4 0.25 0'))),
    (1100, 'qa_actor_state vital'),
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
    (3800, hs(('unit_impervious', 'player true'), ('damage_object', 's:shock player')) + '; qa_player_vitals'),
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
assert int(hurt[names[1]]['health']) == int(base[names[1]]['health']), ('impervious actor is unhurt', hurt[names[1]])
# Shielded actors take the damage on their shield first.
assert names[6] not in hurt or int(hurt[names[6]]['health']) < int(base[names[6]]['health']) or \
    float(hurt[names[6]]['shield']) < float(flags[names[6]]['shield']), \
    ('damage_object hurts or kills an unprotected actor', hurt.get(names[6]))
assert float(hurt[names[0]]['shield']) == float(flags[names[0]]['shield']), 'protected actors keep their shield'

vital = by_name('vital')
for n, fraction in ((names[3], 0.5), (names[4], 0.25)):
    expected = max(1, int(int(base[n]['health']) * fraction + .5))
    assert int(vital[n]['health']) == expected, ('unit_set_current_vitality sets a fraction of the starting health', fraction, base[n], vital[n])
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
assert len(vitals) >= 6, (vitals, log)
base_v, no_shield, low, full, protected, hurt_v = vitals[:6]
assert float(no_shield['shield']) == 0.0, ('maximum shield 0 removes the shield', no_shield)
assert int(low['health']) == round(25 / 100 * 75), ('25 of 100 health is a quarter', low)
assert int(full['health']) == 75 and float(full['shield']) > 0.99, ('full vitality refills health and shield', full)
assert protected['protected'] == '1' and int(protected['health']) == 75 and float(protected['shield']) > 0.99, ('impervious player takes no damage', protected)
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
table, variants, current = {}, {}, None
for entry in (root / 'packages/halo/actors.txt').read_text().splitlines():
    key, _, rest = entry.partition(' ')
    if key == 'difficulty':
        index, name, *values = rest.split()
        table[name] = [float(v) for v in values]
    elif key == 'actor':
        current = variants.setdefault(int(rest), {})
    elif current is not None and key in ('fire', 'burst', 'grenade'):
        current[key] = [float(v) for v in rest.split()]
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
        (5000, 'qa_actor_state combat; qa_actor_combat combat'),
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
        for base, got in zip((1600, 2199), smash):
            assert abs(got - round(base * table['melee_delay_scale'][d] + table['melee_delay_base'][d] * 1000)) <= 1, ('melee delay', d, r)
    assert written, ('at least one Covenant actor fired at its scaled delay', d, shots, log)
    summary.append((d, round(float(covenant[0]['scaled_error']), 4), covenant[0]['delay'], marines[0]['scaled_error']))
print(f'Combat scaling: per level (level, a Covenant actor\'s error and delay, a marine\'s error) {summary}.')
