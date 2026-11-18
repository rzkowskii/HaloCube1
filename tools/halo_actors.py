#!/usr/bin/env python3
"""Export the retail actor (actr) and actor variant (actv) behaviour tables.

Every `actv` tag in the ten Xbox campaign caches becomes one record in
packages/halo/actors.txt with the perception, firing, burst, grenade, defence,
morale and vitality values its actr and actv tags carry. Distances are in Halo
world units, times in seconds and angles in radians. source/src/halo_ai.cpp
reads it; tools/import_halo_scripts.py stores record indices per squad start.
The player's own unit (the globals' player_information unit, characters\\cyborg\\cyborg)
goes to packages/halo/player.txt (PLAYER_FIELDS): its coll's materials, shield
material type and shield stun and recharge, which halodamage's player branch takes
in place of the engine's built-in rule when the file is there, and the globals'
falling damage block (the `falling` lines), which the engine's falling damage reads.
"""
import argparse
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN, weapon_index

FORMAT = 3                        # 2: team, unitmelee and material lines; 3: major and difficulty lines
# Format 3 also carries the optional unit, spawn, pop, destroyed, resistance and shieldtime lines and the
# material lines' fifth field, the shield leak, the berserk line's fourth (the actr melee delay) and the
# grenade line's seventh (the encounter grenade timeout; below), the unitmeleematerials, melee and special lines. The format number stays 3:
# an engine that predates them skips keys it does not know and fields past a line's last (halo_ai.cpp
# parse), so a newer table still loads there, and an older table (without them) loads in a newer
# engine, which keeps its built-in carrier burst, has no infection pop or speed stagger, puts
# every shield over the whole body and recharges it on the Elite's 12 s stun and 10 s recharge, and
# scales its own melee and grenade waits.
# Actor families the engine has bodies for, matching ActorKind in halo.cpp (monitor and engineer
# fly and never fight).
KINDS = {'grunt': 0, 'elite': 1, 'jackal': 2, 'hunter': 3, 'floodcombat elite': 4,
         'floodcombat_human': 5, 'flood_infection': 6, 'floodcarrier': 7, 'sentinel': 8,
         'marine': 9, 'marine_armored': 9, 'crewman': 9, 'marine_suicidal': 9, 'captain': 9,
         'johnson': 9, 'bisenti': 9, 'mendoza': 9, 'fitzgerald': 9, 'jenkins': 9,
         'marine_wounded': 9, 'captain_ingame': 9, 'monitor': 10, 'engineer': 11}
# Inside the actv tag's unit (bipd): its default team (h: 0 default, 1 player, 2 human,
# 3 covenant, 4 flood, 5 sentinel, 6-9 unused), the collision model and the melee jpt!.
UNIT_REFERENCE = 20               # actv: the unit tag reference
# actv: the major upgrade actv reference (../Halo3DS source/ai/actor_definitions.h:408, after the
# flags, actor and unit references): the variant Halo places instead when a squad's major upgrade
# roll succeeds (encounters.c:3040, actors.c:2687).
MAJOR_REFERENCE = 36
# matg: difficulty_information (game_globals.h:246), one element of 35 values by 4 difficulties
# (Easy, Normal, Hard, Impossible) then 21 pad floats (game_globals.h:189-193); the value names
# follow the enum at game_globals.h:63-99.
GLOBALS_DIFFICULTY, DIFFICULTY_STRIDE = 284, 35 * 16 + 21 * 4
DIFFICULTY_VALUES = (
    'enemy_damage', 'enemy_vitality', 'enemy_shield', 'enemy_recharge', 'friend_damage',
    'friend_vitality', 'friend_shield', 'friend_recharge', 'infection_forms', 'unused9',
    'rate_of_fire', 'projectile_error', 'burst_error', 'new_target_delay', 'burst_separation',
    'target_tracking', 'target_leading', 'overcharge_chance', 'special_fire_delay',
    'guidance_vs_player', 'melee_delay_base', 'melee_delay_scale', 'unused22',
    'grenade_chance_scale', 'grenade_timer_scale', 'unused25', 'unused26', 'unused27',
    'major_upgrade', 'major_upgrade_1', 'major_upgrade_2', 'unused31', 'unused32', 'unused33',
    'unused34')
UNIT_FLAGS = 380                  # unit: flags (../Halo3DS units/unit_definitions.h:21-48; bit 19 is_inconsequential)
UNIT_TEAM = 384                   # unit: default team
UNIT_COLLISION = 112              # object: coll reference
# object: acceleration_scale (../Halo3DS objects/object_definitions.h:134-141: after type, flags,
# bounding radius, bounding and origin offsets): what scales a jpt!'s instantaneous acceleration on
# the object (objects/damage.c:1005-1016). Checked against the cache bytes: cyborg 0.7, elite 0.6,
# grunt 0.9, hunter 0.5, carrier 1.2, engineer 2, marine and jackal 1 (bounding radius at +4 0.45,
# 0.44, 0.48, 0.7 alongside).
OBJECT_ACCELERATION_SCALE = 32
UNIT_MELEE = 648                  # unit: melee damage jpt! reference
# unit: the actors a unit releases when it is destroyed (unit_definitions.h:152-155): the
# spawned_actor_variant actv reference, spawn_actor_lower_bound and _upper_bound (h, h) and
# spawn_throw_velocity (f, world units a second). Halo spawns random_range(lower, upper + 1) of them
# and throws them at velocity / 30 a tick (units.c:1666-1693, actors.c:2844).
UNIT_SPAWN_VARIANT, UNIT_SPAWN_BOUNDS, UNIT_SPAWN_VELOCITY = 588, 604, 608
# bipd: the biped section starts at +752 (its jump_velocity, +196 in _biped_definition,
# biped_definitions.h:53-87, is the +948 tools/import_halo_campaign.py reads); its flags follow the
# moving turning speed (bit 11 random_speed_increase, biped_definitions.h:25-37).
BIPED_FLAGS = 756
# coll: the body destroyed effect reference (after the localized, area, body damaged and body
# depleted effects); the infection forms' is the effe whose jpt! pops its neighbours.
COLL_BODY_DESTROYED_EFFECT = 188
JPT_INFECTION_FORM_POP = 1 << 12  # jpt! damage flag _damage_infection_form_pop_bit (objects/damage.c:207-209)
UNIT_DESTROYED_AFTER_DYING = 1 << 1  # unit flag _unit_is_destroyed_after_dying_bit (units/unit_definitions.h:24)
# coll materials: name[32], flags u32 (1 head), material type h, pad, shield leak f at 40,
# shield damage multiplier f at 44, body damage multiplier f at 60 (../Halo3DS
# objects/damage_resistances.h:21-41, which asserts the 0x48 size and the 0x28 and 0x3C offsets).
COLL_MATERIALS, MATERIAL_STRIDE = 564, 72
# coll damage_resistance (damage_resistances.h:43-87): the indirect damage material index (h) at 4,
# maximum body vitality at 8, maximum shield vitality at 204, the shield material type (h) at 210,
# the shield failure function (h) at 236, threshold and maximum failure at 240 and 244, the minimum
# shield stun damage, stun time and recharge time (seconds) at 264, 268 and 272 and the runtime
# shield recharge velocity (a fraction of the shield per tick) at 448.
COLL_INDIRECT_MATERIAL, COLL_BODY_VITALITY, COLL_SHIELD_VITALITY, COLL_SHIELD_MATERIAL = 4, 8, 204, 210
COLL_SHIELD_FAILURE, COLL_SHIELD_STUN, COLL_RECHARGE_VELOCITY = 236, 264, 448
# actr: berserk.melee_attack_delay_timer (../Halo3DS ai/actor_definitions.h:222), the first field of
# actor_berserk_properties at +888 (defensive at +720, 108 bytes; pursuit at +828, 60), whose ninth,
# damage_berserk_amount, is the +920 read below. actv: grenade_combat.encounter_grenade_timeout
# (:375) at +424 (grenade_combat at +384: four shorts, then six floats to the throw chance at +416,
# throw_grenade_delay at +420; items follow at +448). Both checked against the cache bytes (the
# padding around them is zero in every actv and actr of the ten caches). The optional last field of
# the `berserk` (only for a variant with an actr) and `grenade` lines; older engines read the rest.
ACTR_MELEE_DELAY, ACTV_GRENADE_TIMEOUT = 888, 424
# The melee charge (the `melee` line; ../Halo3DS ai/actions.c:2534-2610, ai/action_charge.c:269-706).
# actv ranged_combat (actor_definitions.h:352-357, ranged_combat at +100): melee_range and
# melee_abort_range at +352 and +356, berserk_melee_range and berserk_melee_abort_range at +368 and
# +372 (after the berserk firing range bounds at +360). actr berserk (+888, :220-236):
# melee_attack_range +892, melee_attack_charge_timer +896, suicide_sensing_distance +932. The
# carried weapon's flags (weapon section +0, items/weapon_definitions.h:41: bit 15
# non_players_use_melee_damage, only the energy sword's) choose its melee jpt! over the unit's for an
# actor's melee (units/units.c:8597-8612). The unit's melee animation (object +56, the antr): the
# 'stand' seat (+12, 100 bytes) and the weapon class whose weapon types (+176, 60 bytes) hold the
# weapon's label (weapon section +4; an empty type label for no weapon, units.c:7918-7921), its
# animation slot 39 (melee_attack, units/units.h:274; class animations at +152), and that animation's
# frame count (+34), key frame (+52), frame info type (+38) and frame info (+72): Halo's x offsets
# at the key frame and at the end (models/model_animations.c:416-457; unit_get_melee_range_and_ticks,
# units.c:1391-1459). Every offset checked against the cache bytes (padding zero in every actv).
ACTV_MELEE_RANGE, ACTV_BERSERK_MELEE_RANGE = 352, 368
ACTR_MELEE_ATTACK_RANGE, ACTR_MELEE_CHARGE_TIMER, ACTR_SUICIDE_SENSING = 892, 896, 932
WEAPON_SECTION, WEAPON_NON_PLAYERS_USE_MELEE = 776, 1 << 15
# Infantry special fire (the optional `special` line; ../Halo3DS ai/actor_combat.c:1765-1840, 1339-1348).
# actv ranged_combat (+100, actor_definitions.h:318-358): special_damage_modifier and
# special_projectile_error at +248 and +252 (after the 44-byte burst geometry at +204),
# special_fire_mode and special_fire_situation (h, h) at +340 and +342, special_fire_chance and
# special_fire_delay at +344 and +348 (just before melee_range at +352). Checked against the cache
# bytes: the Jackals' plasma pistol variants mode 1 (overcharge), situation 1 (enemy visible), chance
# 0.2 minor and 0.3 major, delay 5 s, modifier 0.4, error 0; the Banshee pilot mode 2 (secondary),
# situation 3 (strafing), chance 1, delay 10; every other actv mode 0 (the Grunts' plasma pistol
# variants carry a chance and delay with mode 0, which Halo never reads). Written only for mode > 0.
ACTV_SPECIAL_DAMAGE, ACTV_SPECIAL_FIRE = 248, 340
OBJECT_ANIMATION_GRAPH, ANTR_UNIT_SEATS, SEAT_STRIDE, SEAT_WEAPON_CLASSES = 56, 12, 100, 88
CLASS_STRIDE, CLASS_ANIMATIONS, CLASS_WEAPON_TYPES, TYPE_STRIDE = 188, 152, 176, 60
ANTR_ANIMATIONS, ANIMATION_STRIDE, MELEE_ATTACK_SLOT = 116, 180, 39
# Halo's danger zones and dives (the optional `danger` and `dive` lines; ../Halo3DS ai/actor_perception.c:
# 4932-5026, 5028-5319, 5511-5600, 6286-6486; ai/actions.c:3761-3952, 4265-4449, 4699-5054). actr
# perception (+24, actor_definitions.h:84-106): notice_projectile_chance and notice_vehicle_chance at +80
# and +84 (after the hearing distance at +76); actr moving (+128, :108-119): cover_dive_chance +128,
# grenade_dive_chance +136; actr berserk (+888): grenade_attached_chance +936 (:234, after the suicide
# sensing distance at +932); actv ranged_combat berserk_firing_range_upper_bound +364 (:355, after the
# lower bound at +360). The unit (unit_definitions.h:146-163): dive_distance +568 (actions.c:398-400
# asserts 0x238) and ai_danger_radius +644 (the carrier's suicide radius, actor_perception.c:5423).
# Checked against the cache bytes: Grunt minor 0.5, 0.75, 0.3, 0.3; marine 1, 1, 0, 0.75; Hunter 1, 1,
# 0, 0; the Grunt, Elite, Hunter and flood units 1, the Jackal's 0.5, the marine's 1.5, the carrier's
# danger radius 2; Elite minor and major attached berserk 0.3 and 0.45 within 4 world units, the
# Hunter's 0.7 within 2; the unused fields after the notice chances (+88) and the attached chance (+940)
# and the unit's movement_unused after the dive distance (+572) zero in every tag of the ten caches.
ACTR_NOTICE_CHANCES, ACTR_COVER_DIVE, ACTR_GRENADE_DIVE, ACTR_ATTACHED_BERSERK = 80, 128, 136, 936
ACTV_BERSERK_FIRING_UPPER, UNIT_DIVE_DISTANCE, UNIT_AI_DANGER_RADIUS = 364, 568, 644
# The dive animations Halo's actor_action_try_to_dive picks from, in its global_dive_animation_table
# order (actions.c:939-948: diving left, evade left, diving right, evade right, diving front, diving
# back), as the weapon class animation slots unit_animation_impulse_get_index maps the impulses 10, 6,
# 11, 7, 8 and 9 to (units/units.c:8210-8263; slots in units/units.h:233-288: diving front 4, back 5,
# left 6, right 7, evade left 30, evade right 31). The marines' and crewmen's diving left and right
# slots hold their evade-left and evade-right animations; no unit has a diving back; the Flood have none.
DIVE_SLOTS = (6, 30, 7, 31, 4, 5)
FRAME_INFO_SIZES = {1: 8, 2: 12, 3: 16}         # dx dy, dx dy dyaw, dx dy dz dyaw (model_animations.c:428-441)
# matg: player_information (../Halo3DS game/game_globals.h:253, the eleventh block after the
# difficulty block at 284: 368), one 244-byte element (game_globals.h:143-174) whose first field
# is the player_unit reference: the unit every player spawns as (characters\cyborg\cyborg in all
# ten caches; checked against the cache bytes).
GLOBALS_PLAYER_INFORMATION, PLAYER_INFORMATION_STRIDE = 368, 244
# matg: falling_damage (game_globals.h:255, the block after first_person_interface at 380: 392), one
# 152-byte element (../Halo3DS units/bipeds.c:368-385 and objects/damage.c:262-286, which asserts
# the 0x98 size and the falling jpt!'s index at 0x1C): falling distance lower and upper bounds at
# +8 and +12, the falling jpt! at +16, maximum distance at +40, the maximum distance jpt! at +44
# and the runtime maximum falling, minimum damage and maximum damage velocities (units a tick) at
# +0x8C. Checked against the cache bytes: 3, 6 and 16.5 units, 0.3430, 0.1463 and 0.2068 (the
# sqrt(2 g d) of 16.5, 3 and 6 with physics.c:279's gravity), globals\falling (10, 155-155,
# category 1 falling) and globals\distance (105-105, falling), the same in all ten caches.
GLOBALS_FALLING, FALLING_STRIDE = 392, 152
FALLING_BOUNDS, FALLING_JPT, FALLING_MAXIMUM, FALLING_DISTANCE_JPT, FALLING_RUNTIME = 8, 16, 40, 44, 0x8C
PLAYER_FORMAT = 1

FIELDS = '''# difficulty <value index> <name> <easy> <normal> <hard> <impossible>   the globals (matg) difficulty table, before the actors
# actor <index>
# path <actv tag>
# family <folder> <kind> <actr type> <actr flags> <actv flags>
# weapon <weapons.txt index>|-1
# vision <distance> <central angle> <max angle> <peripheral angle> <peripheral distance> <hearing>
# fire <max range> <rate> <error> <first delay min> <max> <combat range min> <max> <lead> <damage scale>
# burst <duration min> <max> <separation min> <max>
# grenade <type> <chance> <range min> <max> <count min> <max> <encounter timeout s>   older engines read the first six
# vitality <body> <shield>
# defense <hide shield fraction> <emerge to attack> <emerge to pursue>
# morale <casualty trigger> <friend killed chance> <leader killed chance>
# berserk <damage amount> <threshold> <melee range> <melee attack delay s>   the last only with an actr; older engines read three
# positions <guard min> <max> <combat min> <max>
# team <unit default team>          retail numbering: 0 default, 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel
# unitmelee <lower> <upper min> <upper max>   the unit's own melee jpt! (a Hunter's smash)
# unitmeleematerials <33 modifiers>  that jpt!'s damage modifier per retail material type (absent: none; older engines skip it)
# unitmeleekind <category> <acceleration>  that jpt!'s damage category and instantaneous acceleration (weapons.txt
#                                    `jptkind`: the unit dialogue's death lines; older engines skip it)
# material <type> <flags> <body multiplier> <shield multiplier> <shield leak>   each collision material (flags 1: head); older engines read the first four
# resistance <indirect material> <coll body> <coll shield> <shield material type> <failure function> <failure threshold> <maximum failure>   the coll's (absent: none)
# shieldtime <minimum stun damage> <stun s> <recharge s> <runtime recharge velocity>   the coll's shield stun and recharge, velocity per tick (absent: none)
# major <actv tag>                  the variant a major upgrade places instead (absent: none)
# unit <unit flags> <biped flags>   the unit tag's flags (bit 19: inconsequential) and a biped's (bit 11: random speed increase; 0 for a vehicle)
# accelscale <scale>                the unit object's acceleration scale on a jpt!'s instantaneous acceleration (the death push; older engines skip it)
# spawn <actor index> <lower> <upper> <throw velocity>   the actors the unit releases when destroyed (absent: none), velocity in world units a second
# pop <inner> <outer> <lower> <upper min> <upper max> <jpt flags>   the body destroyed effect's infection-form pop damage (absent: none)
# destroyed <effe tag>              the body destroyed effect a unit destroyed after dying sets off, when it is not the pop (absent: none; effects.txt holds its damage)
# melee <range> <abort> <berserk range> <berserk abort> <attack range> <charge timer s> <suicide sensing> <weapon melee 0|1> <frames> <key frame> <key x> <total x>
#                                   the melee charge: the actv's ranges, the actr's attack range, charge timer and suicide sensing distance
#                                   (0 without an actr), 1 when the carried weapon's melee jpt! replaces the unit's, and the unit's melee
#                                   animation for that weapon (frames, key frame, x offsets in world units; 0 0 0 0 without one)
# special <mode> <situation> <chance> <delay s> <damage modifier> <projectile error>
#                                   the actv's special fire (mode 1 overcharge, 2 secondary trigger; situation 0 never,
#                                   1 enemy visible, 2 enemy out of sight, 3 strafing), only for mode > 0; older engines skip it
# danger <notice projectile> <notice vehicle> <grenade dive> <cover dive> <dive distance> <suicide radius> <attached berserk> <berserk firing range>
#                                   Halo's danger zones: the actr's notice and dive chances, the unit's dive distance and ai
#                                   danger radius (world units), the actr's grenade attached berserk chance and the actv's
#                                   berserk firing range upper bound; only with an actr; older engines skip it
# dive <frames> <dx> <dy> x6         the unit's dive animations for its weapon in Halo's table order (diving left, evade left,
#                                   diving right, evade right, diving front, diving back): frames and root motion (world
#                                   units, x forward, y left; 0 0 0 none); older engines skip it
'''

PLAYER_FIELDS = '''# player <unit tag>                 the globals' player_information unit (game_globals.h:143-145)
# material, resistance, shieldtime   its coll, as in actors.txt (the cyborg: every material type 21,
#                                    cyborg armor, leak 0; shield material type 22, cyborg energy shield;
#                                    indirect material 1; stun 6 s, recharge 4 s)
# unit <unit flags> <biped flags>    as in actors.txt (the cyborg's bit 22: shields fry infection forms)
# falling <lower> <upper> <maximum distance> <max falling v> <min damage v> <max damage v> <falling min> <lower> <upper> <distance min> <lower> <upper> <falling category> <distance category> <falling jpt flags> <distance jpt flags>
#                                    the globals' falling_damage block (distances in world units, velocities
#                                    in units a tick) and its two jpt!s' damage (optional; older engines skip it)
# fallingmaterials, distancematerials <33 modifiers>   those jpt!s' damage modifier per retail material type
# end
'''


def actor_record(halo, tag, weapons):
    t, a = halo.tags, tag.address
    actr = halo.reference(t, a + 4)
    weapon = halo.reference(t, a + 100)
    family = tag.name.split('\\')[1] if '\\' in tag.name else tag.name
    record = {'path': tag.name, 'family': family.replace(' ', '_'), 'kind': KINDS.get(family, -1),
              'weapon': weapons.get(weapon.name, -1) if weapon and weapon.group == 'weap' else -1,
              'actv_flags': t.read('I', a)[0]}
    record['fire'] = (*t.read('3f', a + 116), *t.read('2f', a + 128), *t.read('2f', a + 156),
                      *t.read('2f', a + 192))
    record['burst'] = (*t.read('2f', a + 224), *t.read('2f', a + 232))
    record['grenade'] = (t.read('h', a + 384)[0], t.read('f', a + 416)[0], *t.read('2f', a + 404),
                         *t.read('2h', a + 464), t.read('f', a + ACTV_GRENADE_TIMEOUT)[0])
    record['vitality'] = t.read('2f', a + 512)
    major = halo.reference(t, a + MAJOR_REFERENCE)
    record['major'] = major.name if major and major.group == 'actv' else ''
    record['melee'] = t.read('f', a + 352)[0]
    mode, situation = t.read('2h', a + ACTV_SPECIAL_FIRE)
    if mode > 0:
        record['special'] = (mode, situation, *t.read('2f', a + ACTV_SPECIAL_FIRE + 4), *t.read('2f', a + ACTV_SPECIAL_DAMAGE))
    unit = halo.reference(t, a + UNIT_REFERENCE)
    record.update(unit_record(halo, unit))
    charge = (0.0, 0.0, 0.0)
    if actr and actr.group == 'actr':
        r = actr.address
        record['type'] = t.read('h', r + 20)[0]
        record['actr_flags'] = t.read('I', r)[0]
        record['vision'] = (*t.read('3f', r + 24), *t.read('2f', r + 40), t.read('f', r + 76)[0])
        record['defense'] = t.read('3f', r + 732)
        record['morale'] = (t.read('h', r + 632)[0], t.read('f', r + 672)[0], t.read('f', r + 680)[0])
        record['berserk'] = (*t.read('2f', r + 920), record['melee'], t.read('f', r + ACTR_MELEE_DELAY)[0])
        record['positions'] = t.read('4f', r + 952)
        charge = (*t.read('2f', r + ACTR_MELEE_ATTACK_RANGE), t.read('f', r + ACTR_SUICIDE_SENSING)[0])
    else:
        record.update(type=-1, actr_flags=0, vision=(0,) * 6, defense=(0,) * 3, morale=(0, 0.0, 0.0),
                      berserk=(0.0, 0.0, record['melee']), positions=(0,) * 4)
    carried = weapon if weapon and weapon.group == 'weap' else None
    weapon_flags = t.read('I', carried.address + WEAPON_SECTION)[0] if carried else 0
    record['meleecharge'] = (*t.read('2f', a + ACTV_MELEE_RANGE), *t.read('2f', a + ACTV_BERSERK_MELEE_RANGE), *charge,
                             1 if weapon_flags & WEAPON_NON_PLAYERS_USE_MELEE else 0,
                             *melee_animation(halo, unit, carried))
    if actr and actr.group == 'actr':
        r = actr.address
        biped = unit if unit and unit.group == 'bipd' else None
        record['danger'] = (*t.read('2f', r + ACTR_NOTICE_CHANCES), t.read('f', r + ACTR_GRENADE_DIVE)[0],
                            t.read('f', r + ACTR_COVER_DIVE)[0],
                            t.read('f', biped.address + UNIT_DIVE_DISTANCE)[0] if biped else 0.0,
                            t.read('f', biped.address + UNIT_AI_DANGER_RADIUS)[0] if biped else 0.0,
                            t.read('f', r + ACTR_ATTACHED_BERSERK)[0], t.read('f', a + ACTV_BERSERK_FIRING_UPPER)[0])
        dives = dive_animations(halo, unit, carried)
        if dives:
            record['dive'] = dives
    return record


def weapon_class(halo, unit, weapon):
    """(antr, weapon class) of the unit's 'stand' seat (else its first) for the carried weapon: the class
    whose weapon types carry the weapon's label, an empty label for no weapon (units.c:7912-7921); or None."""
    t = halo.tags
    if not unit or unit.group != 'bipd':
        return None
    antr = halo.reference(t, unit.address + OBJECT_ANIMATION_GRAPH)
    if not antr or antr.group != 'antr':
        return None
    label = t.name(weapon.address + WEAPON_SECTION + 4, 32) if weapon else 'unarmed'
    seats = t.block(antr.address + ANTR_UNIT_SEATS, SEAT_STRIDE)
    seat = next((s for s in seats if t.name(s, 32) == 'stand'), seats[0] if seats else None)
    if seat is None:
        return None
    for candidate in t.block(seat + SEAT_WEAPON_CLASSES, CLASS_STRIDE):
        types = [t.name(w, 32) for w in t.block(candidate + CLASS_WEAPON_TYPES, TYPE_STRIDE)]
        if any((label == 'unarmed' and not name) or name.lower() == label.lower() for name in types):
            return antr, candidate
    return None


def animation_motion(halo, clip):
    """(frames, frame info x and y sums) of an antr animation: its root motion in world units
    (models/model_animations.c:416-457, the frame info sizes of :428-441)."""
    t = halo.tags
    frames, = t.read('h', clip + 34)
    info_type, = t.read('h', clip + 38)
    size, _, _, pointer = t.read('4I', clip + 72)
    stride = FRAME_INFO_SIZES.get(info_type, 0)
    x = y = 0.0
    for frame in range(max(0, frames)):
        if stride and (frame + 1) * stride <= size:
            dx, dy = t.read('2f', pointer + frame * stride)
            x += dx
            y += dy
    return max(0, frames), x, y


def dive_animations(halo, unit, weapon):
    """The unit's dive animations for the carried weapon (DIVE_SLOTS order) as (frames, dx, dy) each,
    (0, 0, 0) for a slot without one (unit_test_animation_impulse fails, units.c:8382-8420); None
    when the unit has no animation graph or class for the weapon, or no dive at all."""
    t = halo.tags
    found = weapon_class(halo, unit, weapon)
    if not found:
        return None
    antr, klass = found
    slots = t.block(klass + CLASS_ANIMATIONS, 2)
    animations = t.block(antr.address + ANTR_ANIMATIONS, ANIMATION_STRIDE)
    values = []
    for slot in DIVE_SLOTS:
        index = t.read('h', slots[slot])[0] if len(slots) > slot else -1
        values.extend(animation_motion(halo, animations[index]) if 0 <= index < len(animations) else (0, 0.0, 0.0))
    return tuple(values) if any(values[i] > 0 for i in range(0, len(values), 3)) else None


def melee_animation(halo, unit, weapon):
    """The unit's melee attack animation for the carried weapon, as Halo's actors find it
    (unit_get_melee_range_and_ticks, ../Halo3DS units/units.c:1391-1459): (frames, key frame, x
    offset at the key frame, total x offset), the offsets in world units summed over the frame
    info as animation_get_x_offsets does (models/model_animations.c:416-457); (0, 0, 0.0, 0.0)
    without one. The seat is the antr's 'stand' (else its first), the weapon class the one whose
    weapon types carry the weapon's label, an empty label for no weapon (units.c:7912-7921)."""
    none = (0, 0, 0.0, 0.0)
    t = halo.tags
    if not unit or unit.group != 'bipd':
        return none
    antr = halo.reference(t, unit.address + OBJECT_ANIMATION_GRAPH)
    if not antr or antr.group != 'antr':
        return none
    label = t.name(weapon.address + WEAPON_SECTION + 4, 32) if weapon else 'unarmed'
    seats = t.block(antr.address + ANTR_UNIT_SEATS, SEAT_STRIDE)
    seat = next((s for s in seats if t.name(s, 32) == 'stand'), seats[0] if seats else None)
    if seat is None:
        return none
    for weapon_class in t.block(seat + SEAT_WEAPON_CLASSES, CLASS_STRIDE):
        types = [t.name(w, 32) for w in t.block(weapon_class + CLASS_WEAPON_TYPES, TYPE_STRIDE)]
        if not any((label == 'unarmed' and not name) or name.lower() == label.lower() for name in types):
            continue
        slots = t.block(weapon_class + CLASS_ANIMATIONS, 2)
        index = t.read('h', slots[MELEE_ATTACK_SLOT])[0] if len(slots) > MELEE_ATTACK_SLOT else -1
        animations = t.block(antr.address + ANTR_ANIMATIONS, ANIMATION_STRIDE)
        if not 0 <= index < len(animations):
            return none
        clip = animations[index]
        frames, = t.read('h', clip + 34)
        info_type, = t.read('h', clip + 38)
        key, = t.read('h', clip + 52)
        size, _, _, pointer = t.read('4I', clip + 72)
        stride = FRAME_INFO_SIZES.get(info_type, 0)
        x = key_x = 0.0
        for frame in range(max(0, frames)):
            if stride and (frame + 1) * stride <= size:
                x += t.read('f', pointer + frame * stride)[0]
            if frame == key:
                key_x = x
        return max(0, frames), max(0, key), key_x, x
    return none


def unit_record(halo, unit):
    """The unit's default team, flags, melee jpt! damage, collision materials, the actors it
    releases when destroyed and its infection-form pop."""
    from halo_weapons import jpt_damage, jpt_kind, jpt_materials, referenced
    t = halo.tags
    record = {'team': -1, 'unitmelee': (0.0, 0.0, 0.0), 'materials': []}
    if not unit or unit.group not in ('bipd', 'vehi'):
        return record
    record['team'], = t.read('h', unit.address + UNIT_TEAM)
    unit_flags, = t.read('I', unit.address + UNIT_FLAGS)
    biped_flags = t.read('I', unit.address + BIPED_FLAGS)[0] if unit.group == 'bipd' else 0
    record['unit'] = (unit_flags, biped_flags)
    record['accelscale'], = t.read('f', unit.address + OBJECT_ACCELERATION_SCALE)
    spawned = halo.reference(t, unit.address + UNIT_SPAWN_VARIANT)
    if spawned and spawned.group == 'actv':
        lower, upper = t.read('2h', unit.address + UNIT_SPAWN_BOUNDS)
        velocity, = t.read('f', unit.address + UNIT_SPAWN_VELOCITY)
        record['spawn'] = (spawned.name, lower, upper, velocity)
    melee = halo.reference(t, unit.address + UNIT_MELEE)
    if melee and melee.group == 'jpt!':
        d = jpt_damage(halo, melee)
        record['unitmelee'] = (d.lower, d.upper_min, d.upper_max)
        record['unitmeleematerials'] = jpt_materials(halo, melee)
        record['unitmeleekind'] = jpt_kind(halo, melee)
    coll = halo.reference(t, unit.address + UNIT_COLLISION)
    if coll and coll.group == 'coll':
        c = coll.address
        for m in t.block(c + COLL_MATERIALS, MATERIAL_STRIDE):
            flags, = t.read('I', m + 32)
            kind, = t.read('h', m + 36)
            leak, shield = t.read('2f', m + 40)
            body, = t.read('f', m + 60)
            record['materials'].append((kind, flags, body, shield, leak))
        # Halo's shield step (objects/damage.c:800-988): the hit's material leaks that fraction past
        # the shield (842-843), the failure curve lets more through below the threshold (844-857),
        # the coll's shield material type picks the jpt! modifier (898-899), and a blast lands on the
        # indirect material (1623-1630); the stun and recharge (978-982, 1925-1932) follow.
        record['resistance'] = (t.read('h', c + COLL_INDIRECT_MATERIAL)[0], t.read('f', c + COLL_BODY_VITALITY)[0],
                                t.read('f', c + COLL_SHIELD_VITALITY)[0], t.read('h', c + COLL_SHIELD_MATERIAL)[0],
                                t.read('h', c + COLL_SHIELD_FAILURE)[0], *t.read('2f', c + COLL_SHIELD_FAILURE + 4))
        record['shieldtime'] = (*t.read('3f', c + COLL_SHIELD_STUN), t.read('f', c + COLL_RECHARGE_VELOCITY)[0])
        # The infection forms' body destroyed effect (characters\flood_infection\body destroyed):
        # its jpt! carries the pop flag, which only hurts inconsequential units (damage.c:2343-2375).
        effect = halo.reference(t, coll.address + COLL_BODY_DESTROYED_EFFECT)
        pop = referenced(halo, effect, 'jpt!') if effect and effect.group == 'effe' else None
        if pop:
            d = jpt_damage(halo, pop)
            if d.flags & JPT_INFECTION_FORM_POP:
                record['pop'] = (d.inner, d.outer, d.lower, d.upper_min, d.upper_max, d.flags)
        destroyed = destroyed_effect(halo, unit)
        if destroyed:
            record['destroyed'] = destroyed
    return record


def destroyed_effect(halo, unit):
    """The effe tag a unit sets off when it is destroyed, or None: its coll's body destroyed effect
    (coll +188) when the unit is destroyed after dying (unit +380 bit 1) and the effect is not the
    infection-form pop (written as the `pop` line instead). Halo's unit_destroy (../Halo3DS
    units/units.c:2711-2716), reached at the end of dying for such a unit (:6679, :10043), calls
    object_destroy, which creates the coll's body destroyed effect on the object (objects/damage.c:
    606-632), then releases the unit's spawned actors. In the ten caches: the carrier's
    (characters\\floodcarrier\\effects\\body destroyed), the Sentinel's and the Engineer's."""
    from halo_weapons import jpt_damage, referenced
    t = halo.tags
    if not unit or unit.group not in ('bipd', 'vehi'):
        return None
    flags, = t.read('I', unit.address + UNIT_FLAGS)
    coll = halo.reference(t, unit.address + UNIT_COLLISION)
    if not flags & UNIT_DESTROYED_AFTER_DYING or not coll or coll.group != 'coll':
        return None
    effect = halo.reference(t, coll.address + COLL_BODY_DESTROYED_EFFECT)
    if not effect or effect.group != 'effe':
        return None
    damage = referenced(halo, effect, 'jpt!')
    if damage and jpt_damage(halo, damage).flags & JPT_INFECTION_FORM_POP:
        return None
    return effect.name


def number(value):
    if isinstance(value, int):
        return str(value)
    text = f'{value:.6g}'
    return '0' if text == '-0' else text


def difficulty_rows(halo):
    """The globals tag's difficulty table: 35 rows of (Easy, Normal, Hard, Impossible) values,
    or None when the cache has no matg tag or no difficulty_information element. Halo reads it in
    game_difficulty_get_value (../Halo3DS source/game/game_globals.c:167-202)."""
    t = halo.tags
    matg = next((tag for tag in halo.directory if tag.group == 'matg'), None)
    if not matg:
        return None
    elements = t.block(matg.address + GLOBALS_DIFFICULTY, DIFFICULTY_STRIDE)
    if not elements:
        return None
    values = t.read(f'{len(DIFFICULTY_VALUES) * 4}f', elements[0])
    return [tuple(values[i * 4:i * 4 + 4]) for i in range(len(DIFFICULTY_VALUES))]


def write(records, path, difficulty=None):
    lines = [f'format {FORMAT}', f'# {len(records)} actor variants from the Xbox campaign caches', FIELDS.rstrip()]
    for index, row in enumerate(difficulty or ()):
        lines.append(f'difficulty {index} {DIFFICULTY_VALUES[index]} ' + ' '.join(map(number, row)))
    # A spawned actor variant names its record by index (every actv of the caches has one).
    index_of = {r['path']: i for i, r in enumerate(records)}
    for index, r in enumerate(records):
        lines += [f'actor {index}', f'path {r["path"]}',
                  f'family {r["family"]} {r["kind"]} {r["type"]} {r["actr_flags"]} {r["actv_flags"]}',
                  f'weapon {r["weapon"]}']
        for key in ('vision', 'fire', 'burst', 'grenade', 'vitality', 'defense', 'morale', 'berserk', 'positions'):
            lines.append(key + ' ' + ' '.join(map(number, r[key])))
        lines.append(f'team {r.get("team", -1)}')
        lines.append('unitmelee ' + ' '.join(map(number, r.get('unitmelee', (0.0, 0.0, 0.0)))))
        if r.get('unitmeleematerials'):
            lines.append('unitmeleematerials ' + ' '.join(map(number, r['unitmeleematerials'])))
        if r.get('unitmeleekind'):
            lines.append('unitmeleekind ' + ' '.join(map(number, r['unitmeleekind'])))
        lines += ['material ' + ' '.join(map(number, m)) for m in r.get('materials', [])]
        if r.get('major'):
            lines.append(f'major {r["major"]}')
        if 'unit' in r:
            lines.append('unit ' + ' '.join(map(number, r['unit'])))
        if 'accelscale' in r:
            lines.append(f'accelscale {number(r["accelscale"])}')
        spawn = r.get('spawn')
        if spawn and spawn[0] in index_of:
            lines.append('spawn ' + ' '.join(map(number, (index_of[spawn[0]], *spawn[1:]))))
        if r.get('pop'):
            lines.append('pop ' + ' '.join(map(number, r['pop'])))
        if r.get('destroyed'):
            lines.append(f'destroyed {r["destroyed"]}')
        for key in ('resistance', 'shieldtime'):
            if r.get(key):
                lines.append(key + ' ' + ' '.join(map(number, r[key])))
        if r.get('meleecharge'):
            lines.append('melee ' + ' '.join(map(number, r['meleecharge'])))
        if r.get('special'):
            lines.append('special ' + ' '.join(map(number, r['special'])))
        if r.get('danger'):
            lines.append('danger ' + ' '.join(map(number, r['danger'])))
        if r.get('dive'):
            lines.append('dive ' + ' '.join(map(number, r['dive'])))
        lines.append('end')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n')


def player_record(halo):
    """The globals' player unit (matg player_information[0].player_unit) and its unit record, or None."""
    t = halo.tags
    matg = next((tag for tag in halo.directory if tag.group == 'matg'), None)
    if not matg:
        return None
    elements = t.block(matg.address + GLOBALS_PLAYER_INFORMATION, PLAYER_INFORMATION_STRIDE)
    unit = halo.reference(t, elements[0]) if elements else None
    if not unit or unit.group != 'bipd':
        return None
    record = unit_record(halo, unit)
    if not record.get('materials') or not record.get('resistance'):
        return None
    return {'path': unit.name, **record, **falling_record(halo, matg)}


def falling_record(halo, matg):
    """The globals' falling_damage block (biped_falling_damage, ../Halo3DS units/bipeds.c:792-845)
    as the player.txt `falling`, `fallingmaterials` and `distancematerials` values, or {} when the
    cache has no element or either jpt! is missing."""
    from halo_weapons import jpt_damage, jpt_materials
    t = halo.tags
    elements = t.block(matg.address + GLOBALS_FALLING, FALLING_STRIDE)
    if not elements:
        return {}
    e = elements[0]
    falling, distance = halo.reference(t, e + FALLING_JPT), halo.reference(t, e + FALLING_DISTANCE_JPT)
    if not falling or falling.group != 'jpt!' or not distance or distance.group != 'jpt!':
        return {}
    f, d = jpt_damage(halo, falling), jpt_damage(halo, distance)
    return {'falling': (*t.read('2f', e + FALLING_BOUNDS), t.read('f', e + FALLING_MAXIMUM)[0], *t.read('3f', e + FALLING_RUNTIME),
                        f.lower, f.upper_min, f.upper_max, d.lower, d.upper_min, d.upper_max, f.category, d.category,
                        f.flags, d.flags),
            'fallingmaterials': jpt_materials(halo, falling), 'distancematerials': jpt_materials(halo, distance)}


def write_player(record, path):
    lines = [f'format {PLAYER_FORMAT}', '# The player\'s unit from the Xbox campaign caches (tools/halo_actors.py)',
             PLAYER_FIELDS.rstrip(), f'player {record["path"]}']
    lines += ['material ' + ' '.join(map(number, m)) for m in record['materials']]
    for key in ('resistance', 'shieldtime'):
        if record.get(key):
            lines.append(key + ' ' + ' '.join(map(number, record[key])))
    if 'unit' in record:
        lines.append('unit ' + ' '.join(map(number, record['unit'])))
    for key in ('falling', 'fallingmaterials', 'distancematerials'):
        if record.get(key):
            lines.append(key + ' ' + ' '.join(map(number, record[key])))
    lines.append('end')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n')


def actor_index(root):
    """actv tag path to record index, read back from an existing actors.txt."""
    index, current = {}, None
    path = Path(root) / 'packages/halo/actors.txt'
    if not path.exists():
        return index
    for line in path.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'actor':
            current = int(rest)
        elif key == 'path' and current is not None:
            index[rest] = current
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    weapons = weapon_index(args.output)
    if not weapons:
        raise SystemExit('run tools/halo_weapons.py first to write packages/halo/weapons.txt')
    records, tables, players = {}, {}, {}
    for cache in CAMPAIGN:
        with (args.maps / f'{cache}.map').open('rb') as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            tables[cache] = difficulty_rows(halo)
            players[cache] = player_record(halo)
            for tag in halo.directory:
                if tag.group == 'actv' and tag.name not in records:
                    records[tag.name] = actor_record(halo, tag, weapons)
    ordered = [records[name] for name in sorted(records)]
    # The ten caches carry the same globals tag; a disagreement would make the table depend on the
    # mission, which the engine does not model: keep the first and say so.
    present = [table for table in tables.values() if table]
    difficulty = present[0] if present else None
    if any(table != difficulty for table in present):
        print('warning: the caches disagree on the globals difficulty table; using the first cache\'s')
    target = args.output / 'packages/halo/actors.txt'
    write(ordered, target, difficulty)
    # The player's unit: the same in every cache (else the first cache's, as for the difficulty table).
    found = [p for p in players.values() if p]
    player_target = args.output / 'packages/halo/player.txt'
    if found:
        if any(p != found[0] for p in found):
            print('warning: the caches disagree on the player unit; using the first cache\'s')
        write_player(found[0], player_target)
        print(f'player unit {found[0]["path"]} -> {player_target}')
    else:
        print('warning: no cache has a player unit with a collision model; player.txt not written')
    bodies = sum(r['kind'] >= 0 for r in ordered)
    majors = sum(bool(r.get('major')) for r in ordered)
    spawns = sum(bool(r.get('spawn')) for r in ordered)
    pops = sum(bool(r.get('pop')) for r in ordered)
    blasts = sum(bool(r.get('destroyed')) for r in ordered)
    print(f'{len(ordered)} actor variants ({bodies} with engine bodies, {majors} with a major upgrade, '
          f'{spawns} releasing actors, {pops} popping, {blasts} with a body destroyed effect), '
          f'{len(difficulty or ())} difficulty values -> {target}')


if __name__ == '__main__':
    main()
