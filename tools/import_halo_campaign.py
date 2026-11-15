#!/usr/bin/env python3
"""Convert retail Xbox campaign BSP sections and authored scenario placements."""
import argparse
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
import math
import mmap
from pathlib import Path
import heapq
import re

from import_halo_a30 import HaloMap, ac_yaw, encode_map, entity, LIGHT, PLAYERSTART, AMMO, GRENADE, HEALTH
from halo_collision import (DoorBlocks, PolygonClearance, floor_samples, static_device_surfaces,
                            structure_surfaces, write_structure_collision)
from halo_map_art import write_world_mesh
from halo_weapons import actor_weapon, weapon_index

# PLAYERSTART team value for scenario weapon placements (100 is the route).
WEAPON_PLACEMENT = 101


@dataclass(frozen=True)
class Mission:
    cache: str
    title: str
    sections: tuple
    end: str


# The two Control Room missions travel in opposite directions. Cinematic-only
# BSPs and optional branches stay outside the campaign route.
MISSIONS = (
    Mission('a10', 'The Pillar of Autumn', (0, 1, 2, 3, 4, 5, 6), 'end_trigger'),
    Mission('a30', 'Halo', (0, 1), 'river_boat_lz'),
    Mission('a50', 'The Truth and Reconciliation', (0, 1, 2, 3, 2), 'shuttle_door_switch_trigger'),
    Mission('b30', 'The Silent Cartographer', (0, 1, 0), 'beach_lz'),
    Mission('b40', 'Assault on the Control Room', (3, 0, 1, 10, 2, 11, 2), 'control_trigger'),
    Mission('c10', '343 Guilty Spark', (0, 1, 2, 1, 3, 4, 5), 'monitor_trigger'),
    Mission('c20', 'The Library', (0, 1, 2, 3), 'finale'),
    Mission('c40', 'Two Betrayals', (2, 10, 1, 0, 5), 'pulse_3_trigger'),
    # d20 has no hangar_exit_trigger: the mission ends when the player boards a Banshee in the
    # hangar (enc7_2), and the Banshees wait on the hangar floor in enc7_6c (banshee_safety_net).
    Mission('d20', 'Keyes', (0, 1, 2, 3, 4, 3), 'enc7_6c'),
    Mission('d40', 'The Maw', (0, 1, 2, 3, 4, 5, 6, 7), 'grand_finale'),
)
# Missions that play on the retail BSP collision. Only a10 (sections 1-4 and 7) and b30
# have been walked start to exit in the game (tests/test_retail_collision.py,
# tests/test_b30_retail_collision.py); the others are import-validated only (a route reaches
# each exit on the collision floors) until they are walked. Their .hcol files are written by
# default; `--collision all` writes them for every section, `--collision none`
# keeps stock AssaultCube geometry everywhere. The engine enables retail
# collision for any section that has a valid packages/maps/<section>.hcol.
VALIDATED_COLLISION = {'a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40'}
# Sections (mission, zero-based part) of those missions that keep stock AssaultCube geometry
# (no .hcol; scripts/package-macos.sh does not require one): their retail floors give no
# way through that the route planner (or the engine) can follow.
# c10 (343 Guilty Spark) crosses its interior on light bridges that the player extends from
# control panels, which the planner does not model (they count as sealed):
# - section 2 (BSP 1, lift_a to the control room hall): bridge_a starts retracted, no script
#   extends it, and its one control (bridge_a_cont_b) is on the far side of the pit from lift_a;
# - section 3 (BSP 2, the control room and the x50 scene): the lower storey of the BSP 1/2
#   doorway leads only to the lower labs, and the upper one through post_helmet_door_4, which
#   ini_post_helmet_replace powers only after the x50 scene that the control room starts. The
#   way up is presumably bridge_b (extended from bridge_b_cont_a/_b), which is not modelled;
# - section 5 (BSP 3, lift_b to the BSP 4 doorway): bridge_c is the flickering bridge
#   (flicker_bridge moves it between 0 and 0.5-1 at random), the only way on from lift_b;
# - section 6 (BSP 4, to lift_d): bridges bridge_d and bridge_e are extended from their panels
#   (mission_int_b waits for bridge_e), and lift_d is called down from lift_d_cont_a.
# c40 (Two Betrayals) flies a Banshee between the pulse generators, and the engine has no
# vehicles; the sections that only a flight (or a lift in a BSP that is not a campaign section)
# leaves stay stock:
# - section 1 (BSP 2, the control room to the c bridge): the way on, door_c1 to BSP 10, is on
#   the c bridge at z 70, 8 units above the canyon floor the walk reaches (z 62) and with no
#   way up but fly_away_1 to pulse generator 1 (BSP 12, at z 104) or lift_d in the c1 tower
#   (BSP 11). Neither is a campaign section, and door_c1 is powered only once pulse_1_trigger
#   (e7_c) has been entered; e1_a powers it off when the mission starts;
# - section 4 (BSP 0, the canyon to the third generator): bsp 0,5 is on the landing pad at
#   z 33.6, which the canyon floor (z -11 to 1) reaches only by c_banshee_1 or by lift_a in BSP 4
#   (the a1 tower) and lift_b down from its z 48 walkway. tun_garage_2 and tun_garage_3, on the
#   way, open only from their control panels, which the planner does not model (sealed).
# d40 (The Maw):
# - section 3 (BSP 2): the lower floor, entered from BSP 1 at bsp 2,1 (z 0), and the upper level
#   (the grate deck at z 1.4 round enc3_3b and enc3_4, the walkway at x -49 rising to z 2, and
#   the room with enc3_5b, enc3_6, section4 and bsp 2,3 at z 2) have no walkable connection in the
#   structure collision (nor in the render geometry): the only floor in between is a fallen beam
#   (at x -49, under burning_flame scenery; 0.24 to 0.74 units up) that ends under the deck's edge, 0.75 units
#   below it, and a standing jump at the indoor scale lifts the feet 0.36 units;
# - sections 6 to 8 (BSPs 5 to 7) are the Warthog run, 1,200 units driven in the original, and
#   the engine has no vehicles. The run starts with a 45 to 65 degree ramp up from the elevator
#   (BSP 5, x -94 to -82 past no_jeep_test, z 107.6 to 119.4) and drops down ramps as steep in
#   BSP 6 (x 358 to 366) and BSP 7 (x 815 to 830); the planner and the engine (retailmove) walk
#   only floors whose normal is at least 0.65 (49 degrees). The sections are 300 to 500 units
#   long, so their outdoor scales (1.98, 3.23, 2.24) make the body 1.6 to 2.6 units tall, more
#   than the Maw's corridors and the elevator landing hold (from section6 only 3 cells of BSP 5
#   are reachable at its scale).
STOCK_SECTIONS = {('c10', 1), ('c10', 2), ('c10', 4), ('c10', 5), ('c40', 0), ('c40', 3),
                  ('d40', 2), ('d40', 5), ('d40', 6), ('d40', 7)}
INTERIOR = {
    'a10': set(range(7)), 'a50': {1, 2, 3}, 'b30': {1}, 'b40': set(range(3, 13)),
    'c10': {1, 2, 3, 4}, 'c20': set(range(4)), 'c40': set(range(3, 13)),
    'd20': {0, 2, 3, 4}, 'd40': set(range(5)),
}
ACTORS = {'grunt': 0, 'elite': 1, 'jackal': 2, 'hunter': 3, 'floodcombat elite': 4,
          'floodcombat_human': 5, 'flood_infection': 6, 'floodcarrier': 7, 'sentinel': 8}
# A standing jump lifts the feet about 2.9 units (tests/test_retail_collision.py measures it):
# routes may jump up ledges of up to 2.25 units and drop down 4.
JUMP_CLIMB, JUMP_DROP = 9, 16
# Route costs, in steps: a jump, a crouched step, and a scripted door the walk must wait at.
JUMP_COST, CROUCH_COST, SCRIPTED_COST = 25, 2, 400
# Sections whose only way on is a walk off a ledge onto a floor further below than a jump
# drops (JUMP_DROP), with the deepest drop allowed in retail units. AssaultCube has no fall
# damage; Halo's harmless falls are a few units. The route steps off the ledge (route flag 8)
# and lands two to four cells on, clear of the ledge face. Other sections never drop, so
# their routes stay as they were walked.
# d20 (Keyes): section 2 (BSP 1) climbs down from the channel at the landing pool (z -23) to
# the canyon at z -25.5 (enc3_4 to enc3_4b, 2.9 units); section 3 (BSP 2) drops from the
# walkway in enc4_6 (z -74.9) to the corridor of enc4_7 and enc4_8 (z -77.3, 2.5 units).
# A drop costs as much as a 300-step detour, so a route drops only where there is no walk round:
# with a cheaper drop, section 3 jumped 3.5 units off the enc4_3 balcony instead of walking down.
# d40 (The Maw): section 1 (BSP 0) starts on the deck where the Banshee lands (z 2.0, the
# intro's player0_intro_done) and the only way on to the bridge (z 0) is the 2-unit drop off its
# edge at x -38.
LEDGE_DROPS = {('d20', 1): 3.0, ('d20', 2): 2.6, ('d40', 0): 2.1}
DROP_COST = 300
TRANSITIONS = {
    # mission_area5 puts the player on the gravity room pad (gravity_teleport0_flag) after the
    # lift. Starting at entr_safe, beside the hangar exit, skipped the gravity room and the
    # muster bay, and left that flag outside the section's projection.
    ('a50', 0, 1): 'gravity_pad_teleport_trigger', ('a50', 1, 0): (-11.05, 14.26, -20.62),
    # The hangar is entered through bsp 1,2, from the muster bay hall.
    ('a50', 2, 1): 'bsp1,2',
    # b40's last section starts where BSP 11 lets the player out, at the bottom doorway
    # (ext_c); the top "bsp 2,11" (#0) is the c bridge's way in (SECTION_EXITS).
    ('b40', 2, 11): 'bsp2,11#1',
    ('d20', 0, 1): '0_to_1_transition_trigger', ('d20', 1, 0): 'grav_lift',
    ('d20', 1, 2): '1_to_2_transition_trigger', ('d20', 2, 1): (-11.6, 11.0, -71.8),
}
# Anchors near the navigable side of scripted lifts, dropship arrivals and
# mission-specific entrances. Coordinates are in the retail BSP's space.
SECTION_ANCHORS = {
    ('a30', 0): ('first_structure_front', 'cave_floor_entrance'),
    # The return starts outside the shaft. The extraction marker is on the
    # ledge beside the hovering Pelican's rear ramp.
    ('b30', 2): ((6.1, -13.4, 4.0), (37.8, -18.6, 9.1)),
    # b40 section 2 ends at the top of the a2 tower, on the far side of the first bridge. The
    # way on is lift_a down the tower to the canyon floor (ext_a), but the tower interior is
    # BSP 4, which is not a campaign section; section 3 starts beyond it, in BSP 1.
    ('b40', 1): (None, (41.5, -82.0, 48.0)),
    ('b40', 2): ((174.0, -299.0, -10.8), (264.0, -558.0, 21.8)),
    ('b40', 4): ((283.0, -622.0, 70.0), (283.0, -685.0, 70.0)),
    # BSP 11 (the c1 tower) is entered at the top from the c bridge and left at the bottom
    # (ext_c). Both doorways have a volume named "bsp 11,2" (and "bsp 2,11"); #0 is the top.
    ('b40', 5): ('bsp11,2#0', 'bsp11,2#1'),
    # Swamp B starts on the landing beside the top of lift_d (bsp 5,4 is half way up the shaft)
    # and ends in monitor_trigger, where mission_swamp_b's last fight and the extraction play.
    ('c10', 6): ((-41.0, 84.9, 2.0), None),
    ('c40', 0): ((362.686, -712.737, 73.818), (286.5, -627.9, 70.3)),
    # c40 section 3 (BSP 1) starts at the bottom of lift_c (BSP 8, not a campaign section) and
    # walks the canyon floor and the tunnel past waypoint5_trigger to bsp 1,0. It used to end at
    # waypoint5_trigger, which left bsp 1,0 (90 units on) outside the section's projection, so
    # the route could not reach the transition. Pulse generator 2 (BSP 6, z 87) is reached only
    # by Banshee and is skipped, as it was on stock geometry.
    ('c40', 2): ((273.9, -551.6, 22.4), None),
    ('c40', 3): ((204.1, -246.0, -11.0), (173.3, -103.6, 1.0)),
    # c40 section 5 (BSP 5, the third generator) starts in bsp 5,0 inside the landing pad door,
    # as a player arriving from section 4 does. It used to start beside the generator, and its
    # 27-step route stopped 6 units short of pulse_3_trigger.
    ('c40', 4): (None, 'pulse_3_trigger'),
    # d20 section 2 (BSP 1, the swamp under the Truth and Reconciliation) starts where
    # cinematic_drop leaves the player after the fall into the coolant (player0_afterfall_base),
    # not in grav_lift, the volume round the gravity lift at the section's far end: starting
    # there skipped the swamp (enc3_1 to enc3_8) and made a 117-step route.
    ('d20', 1): ((42.5, -34.4, -27.6), None),
    ('d40', 7): ((744.0, -8.0, 66.0), 'grand_finale'),
    # The Library climbs four floors (-4.6, 11.4, 27.4, 43.4) on platform1 to platform3, which
    # the platforms script raises while the player stands on them. Each BSP switch volume is
    # half way up a shaft, where no floor is; a section starts on the landing where the player
    # steps off at the top: section3_trigger and section5_trigger, which section2 and section4
    # wait for, are on those landings. The last section starts on the landing west of the top
    # of platform3, the way on to floor4_door2 (the ride's section7_trigger is in the shaft,
    # above bsp 2,3: the engine counts it as passed, see Scenario::noteRide).
    ('c20', 1): ('section3_trigger', None),
    ('c20', 2): ('section5_trigger', None),
    ('c20', 3): ((-50.5, 72.56, 43.4), None),
    # The prison is entered and left through the same doorway: bsp 2,3 lies on the prison
    # side of it and bsp 3,2 on the hangar side, so the section starts in bsp 2,3 (starting in
    # bsp 3,2 would switch straight back to BSP 2) and its return leg ends in bsp 3,2. The
    # last section starts on the hangar side, where that switch leaves the player.
    ('a50', 3): ('bsp2,3', 'bsp3,2'),
    ('a50', 4): ('bsp3,2', None),
}


# Volumes a section's script waits for the player to enter before it opens the way on, in
# order; the route walks through each before heading for the exit.
# An entry may name the cutscene flag the scene then puts the player at (object_teleport),
# where the next leg starts, or give that place as a retail point (where a lift sets the
# player down). An objective given as a retail point (a control the script waits
# for the player to use) is reached when the player stands within reach of it. The section's
# projection covers its objectives as well as its start and exit.
SECTION_OBJECTIVES = {
    ('a10', 1): (('bridge_trigger_3', 'chief_basea10'),),   # Keyes's bridge scene (mission_bridge, cinematic_x20)
    # mission_prison waits for prison_d_switch (cell block D) before the rescue scene, then the
    # player walks back out. ini_post_rescue's teleport to prison_player0_teleport is a player
    # object_teleport, which the engine does not perform, so the return starts at the switch.
    ('a50', 3): (((-15.1, -116.6, -19.0), None),),
    # b40's c1 tower: lift_d waits at the bottom of its shaft. lift_d_control_a, at the top,
    # calls it up and, used again from the platform, rides it down. The platform is not a floor
    # the route knows (the shaft below it drops to the pit at 43.7), so the walk goes on from
    # the landing beside it at the bottom (c1_bottom_save), on the way out to ext_c.
    ('b40', 5): (((263.83, -709.8, 70.87), (266.0, -707.0, 54.4)),),
    # c10: mission_swamp_a waits for enc_swamp2 before it calls lift_a up (int_a_trigger_3, the
    # lift room, is the exit's own room).
    ('c10', 0): (('enc_swamp2', None),),
    # c20 (the Library) opens its floor doors through their device groups once the Monitor's
    # command lists and the fights the trigger volumes start are done:
    # - floor 1: the monitor1_2 .. monitor1_7 chain (each waits for its volume, in order) ends
    #   in floor1_door1_trigger; monitor1_8 then opens floor1_door1, and section1 waits for
    #   "section2" past it;
    # - floor 2: enc3_4_trigger wakes floor2_door1 (open 48 s later), enc3_6_trigger
    #   monitor2_5, which opens floor2_door2 after the Monitor's lists and a Flood wave;
    #   section3 waits for section4_trigger past that door. The way on to platform2 loops east
    #   past x 70, through monitor2_10 and monitor2_11 (the Monitor's own path), which also puts
    #   that loop inside the section's projection;
    # - floor 3: enc5_2_trigger wakes enc5_5; enc5_5_trigger starts floor3_door2's counter and
    #   enc5_6, whose enc5_6_trigger opens floor3_door3; section5 waits for section6_trigger
    #   past it; enc6_3_trigger sets floor3_door4 to 0.6 and enc6_4_trigger, beyond it, closes
    #   it again and opens floor3_door5 after a wave (floor3_door1 never opens);
    # - floor 4: enc7_4_trigger wakes enc7_5, whose enc7_5_trigger starts floor4_door2's
    #   counter; enc7_9_trigger opens floor4_door3, the way to the finale.
    ('c20', 0): tuple((name, None) for name in ('monitor1_2', 'enc3_trigger', 'monitor1_4', 'monitor1_5',
                                                  'monitor1_6', 'floor1_door1_trigger', 'section2')),
    ('c20', 1): tuple((name, None) for name in ('enc3_4_trigger', 'enc3_6_trigger', 'section4_trigger',
                                                  'monitor2_10', 'monitor2_11')),
    ('c20', 2): tuple((name, None) for name in ('enc5_2_trigger', 'enc5_5_trigger', 'enc5_6_trigger',
                                                  'section6_trigger', 'enc6_3_trigger', 'enc6_4_trigger')),
    ('c20', 3): (('enc7_4_trigger', None), ('enc7_5_trigger', None), ('enc7_9_trigger', None)),
    # d20 (Keyes). Each encounter script sleeps until the player enters its volume and then wakes
    # the next one, so the volumes are walked in the scripts' order:
    # - section 2 (BSP 1): enc3_1 to enc3_7 lead down the canyon (enc3_2 is a detour east),
    #   enc3_7 wakes enc3_8, which waits for enc3_8 and then enc3_8b before it watches
    #   1_to_2_transition_trigger: the player must stand in it for three seconds with no Covenant
    #   inside before cinematic_lift switches to BSP 2;
    # - section 3 (BSP 2): section4 starts enc4_1; enc4_2 and enc4_3 follow (enc4_3_door opens
    #   once that fight is done, with the player in enc4_3c); enc4_5 wakes enc4_6, which wakes
    #   enc4_8; enc4_8_spawner opens enc4_8_door (device_set_position_immediate) on the way to
    #   bsp 2,3;
    # - section 5 (BSP 4, the control room): section6 wakes enc6_1 and enc6_2, enc6_2 wakes enc6_3,
    #   and enc6_3 (with no Flood in enc6_3_safe) plays cinematic_captain, after which enc6_5
    #   powers enc6_5_door1 and _door2 and section7 waits in BSP 3. The scene teleports the
    #   player to player0_capt_done, 1.6 units away, which the engine does not do (as for a50's
    #   prison): the walk back to bsp 4,3 starts in enc6_3;
    # - section 6 (BSP 3, back through the hangar): section7 wakes enc7_1, enc7_1 wakes enc7_2
    #   (any of enc7_2, enc7_2b, enc7_2c), which brings in the Banshees and wakes enc7_6.
    ('d20', 1): tuple((name, None) for name in ('enc3_1', 'enc3_2', 'enc3_3', 'enc3_4', 'enc3_5', 'enc3_6',
                                                  'enc3_7', 'enc3_8', 'enc3_8b')),
    ('d20', 2): tuple((name, None) for name in ('section4', 'enc4_2', 'enc4_3', 'enc4_3c', 'enc4_5', 'enc4_6',
                                                  'enc4_8', 'enc4_8_spawner')),
    ('d20', 4): (('section6', None), ('enc6_2', None), ('enc6_3', None)),
    ('d20', 5): (('section7', None), ('enc7_1', None), ('enc7_2', None), ('enc7_6', None)),
    # d40 (The Maw):
    # - section 1 (BSP 0): section1 wakes enc1_1 to enc1_4; enc1_4 opens enc1_4_door, the way to
    #   bsp 0,1;
    # - section 2 (BSP 1): enc2_1 wakes enc2_2, which opens enc2_1_door2 and enc2_1_door1 (the
    #   Hunters' doors, on the way) once the player is in enc2_2, and wakes enc2_5; enc2_5 waits for enc2_6, then
    #   for its Covenant to die and for cinematic_bridge (the x70 bridge scene), and wakes enc2_6,
    #   which waits for enc2_6 again and opens enc2_6_door, the way to bsp 1,2. The scene's teleport
    #   to player0_playon_base, 1.6 units from the volume, is a player object_teleport, which the
    #   engine does not perform;
    # - section 5 (BSP 4, the engine room): section5 wakes enc5_1 and enc5_2. The four manifolds
    #   are opened from their controls (enc5_1_controls1, _controln1, _controln3, _controls3, on
    #   the top floor) and destroyed by shooting them; enc5_2 then waits for enc5_2, opens
    #   enc5_2_door and wakes enc5_3, which raises the elevator once every player is in enc5_3.
    ('d40', 0): (('section1', None), ('enc1_4', None)),
    ('d40', 1): (('enc2_1', None), ('enc2_2', None), ('enc2_6', None), ('cinematic_bridge', None), ('enc2_6', None)),
    ('d40', 4): (('section5', None), ('enc5_1_main', None), ((-86.3, 6.62, 7.82), None),
                 ((-86.28, -10.12, 7.88), None), ((-94.03, -10.14, 7.89), None), ((-94.04, 6.6, 7.89), None),
                 ('enc5_2', None)),
}

# Sections whose exit volume hangs in a shaft past the edge of the last floor: the player walks
# off the edge and falls into it. The .route file ends with one more step, at the volume's
# centre, for the walker to head for; spawns and markers stay on the floor. d20 section 1 ends
# in 0_to_1_transition_trigger, 1 to 4.5 units down the hole Cortana tells the player to jump
# into (objective_jump); section1 then plays cinematic_drop, the fall into the coolant.
EXIT_FALLS = {('d20', 0)}

# Sections whose projection origin is the middle of their height range (section_layout).
MIDDLE_ORIGIN = {('b40', 5)}

# A section's exit volume when it is not the default "bsp<this>,<next>" (or the mission end), or
# a retail point the route ends at (within reach, as for a SECTION_OBJECTIVES control).
SECTION_EXITS = {
    # The a2 tower top (SECTION_ANCHORS): bsp 0,1 is beyond lift_a, in BSP 4.
    ('b40', 1): (41.5, -82.0, 48.0),
    # The c bridge enters BSP 11 at its top doorway; the bottom one is the way out of it.
    ('b40', 4): 'bsp2,11#0',
    ('b40', 5): 'bsp11,2#1',
    # c10's lifts: bsp 0,1 and bsp 1,3 are half way down the shafts. The route ends beside the
    # control the player uses to ride down: lift_a_cont_a at the top of lift_a, and the spot on
    # the landing nearest lift_b_cont_a, which stands on the lift's corner 1.25 units in from it.
    ('c10', 0): (-60.85, -7.93, 42.62),
    ('c10', 3): (-16.4, 21.95, 1.9, .3),
    # c20's sections end beside the lift at the bottom of the next shaft (bsp 0,1, 1,2 and 2,3
    # are half way up): on the floor within 14.5 units of the platform's centre. The platform
    # collides as a 19.8-unit square box, and the corridors reach the round shaft room on its
    # diagonals, where the box corner is 14 units out. Stepping on raises it (platforms).
    ('c20', 0): (-88.6, -18.28, -4.6, 14.5),
    ('c20', 1): (37.85, -83.85, 11.4, 14.5),
    ('c20', 2): (-37.8, 72.56, 27.4, 14.5),
    # d40's engine room ends on the elevator (enc5_3), which enc5_3 raises once the player is on
    # it; bsp 4,5 is 65 units up the shaft.
    ('d40', 4): 'enc5_3',
}

# Machines that move only during a cinematic: they are baked into the .hcol in their first pose
# (static_device_surfaces). c20's index_platform stands at floor 4 under the finale volume
# until cutscene_extraction lowers it; its box would wall off the whole 20-unit platform.
CINEMATIC_STILL = {
    'c20': {'index_platform'},
}


# Scripted doors that only open as a punishment or on another difficulty: a route must not
# wait at them.
NEVER_OPEN = {
    'a10': {'bridge_kill_door_1', 'bridge_kill_door_3'},   # bridge_kill_kill_kill: shooting Keyes
}
# Missions whose closed unnamed doors with no power or position group are walls. Nothing can open
# such a door (no script names it, no control or group drives it, and a manual or unpowered door
# does not open on approach), but the other missions' routes were planned with every closed
# unnamed door as one to wait at, and a10's were walked so: they change only on a re-walk.
# d40 has eight (manual and unpowered): section 1's route used to jump through the hatch on the
# landing deck at (-49, 35.5) instead of walking round by the open door at (-42.3, 34).
UNGROUPED_DOORS_SEALED = {'d40'}


def cutscene_flag(halo, name):
    for p in halo.tags.block(halo.scenario + 1252, 92):
        if halo.tags.name(p + 4, 32) == name:
            return halo.tags.read('3f', p + 36)
    raise ValueError(f'no cutscene flag {name}')


def volume_centers(halo):
    """Trigger volume centres by name. A name used by several volumes (b40's "bsp 11,2", one
    at each of the c1 tower's doorways) maps to the last of them, and "<name>#<n>" to the n-th
    in scenario order."""
    centers, seen = {}, {}
    for offset in halo.tags.block(halo.scenario + 864, 96):
        name = re.sub(r'^bsp\s+', 'bsp', halo.tags.name(offset + 4, 32))
        x, y, z, w, d, h = halo.tags.read('6f', offset + 72)
        centers[name] = centers[f'{name}#{seen.get(name, 0)}'] = (x + w/2, y + d/2, z + h/2)
        seen[name] = seen.get(name, 0) + 1
    return centers


def volume_box(halo, name):
    """An oriented scenario trigger volume as (forward, side, up, origin, extent), or None.

    "<name>#<n>" picks the n-th volume of that name; a bare name the first."""
    name, _, nth = name.partition('#')
    nth = int(nth or 0)
    for offset in halo.tags.block(halo.scenario + 864, 96):
        if re.sub(r'^bsp\s+', 'bsp', halo.tags.name(offset + 4, 32)) != name:
            continue
        if nth:
            nth -= 1
            continue
        f, u, origin, extent = (halo.tags.read('12f', offset + 48)[i:i+3] for i in (0, 3, 6, 9))
        side = (u[1]*f[2]-u[2]*f[1], u[2]*f[0]-u[0]*f[2], u[0]*f[1]-u[1]*f[0])
        return f, side, u, origin, extent
    return None


def exit_target(mission, part):
    """The volume name (or retail point) that ends a section's route."""
    bsp = mission.sections[part]
    following = mission.sections[part+1] if part+1 < len(mission.sections) else None
    if (mission.cache, part) in SECTION_EXITS:
        return SECTION_EXITS[mission.cache, part]
    if following is None:
        return mission.end
    return TRANSITIONS.get((mission.cache, bsp, following), f'bsp{bsp},{following}')


def objective_point(halo, objective):
    """A SECTION_OBJECTIVES target in retail space: a volume's centre or a given point."""
    return volume_centers(halo)[objective] if isinstance(objective, str) else objective


def objective_test(halo, objective, projection):
    """Predicate (x, y, quarter-unit floor) for a player who has reached the objective."""
    if isinstance(objective, str):
        return volume_test(volume_box(halo, objective), projection)
    # A control: the engine's use reach is 1.65 units from the body's centre; stop well inside it.
    # A fourth value is a reach of its own (a stand point beside a control out of that reach).
    px, py, pz = projection.point(objective)
    reach, scale = (objective[3] if len(objective) > 3 else 1.2) * projection.scale, projection.scale
    def inside(x, y, z4):
        return (x - px)**2 + (y - py)**2 <= reach*reach and -1.5*scale < pz - z4/4 < 3*scale
    return inside


def volume_test(box, projection):
    """Predicate (x, y, quarter-unit floor) for a player standing in the volume.

    The engine tests the player at 0.55 of the eye height above the feet.
    """
    f, side, u, origin, extent = box
    def inside(x, y, z4):
        px = (x - 9) / projection.scale + projection.x0 - origin[0]
        py = projection.y0 - (y - 9) / projection.scale - origin[1]
        pz = projection.z0 + (z4 / 4 + .55 * 4.5) / projection.scale - origin[2]
        return all(0 < px*a[0] + py*a[1] + pz*a[2] < e for a, e in zip((f, side, u), extent))
    return inside


def actor_starts(halo, index):
    palette = halo.tags.block(halo.scenario + 1056, 16)
    for encounter in halo.tags.block(halo.scenario + 1068, 176):
        bsp, = halo.tags.read('h', encounter + 126)
        if bsp != index:
            continue
        for squad in halo.tags.block(encounter + 128, 232):
            default, = halo.tags.read('h', squad + 32)
            for start in halo.tags.block(squad + 208, 28):
                variant, = halo.tags.read('h', start + 24)
                variant = default if variant < 0 else variant
                if not 0 <= variant < len(palette):
                    continue
                tag = halo.reference(halo.tags, palette[variant])
                if not tag:
                    continue
                actor = tag.name.split('\\')[1]
                if actor in ACTORS:
                    yield halo.tags.read('4f', start), (ACTORS[actor], actor_weapon(halo, tag) or '')


def weapon_placements(halo):
    """Scenario weapon placements: tag path, retail position and yaw."""
    palette = [halo.reference(halo.tags, entry) for entry in halo.tags.block(halo.scenario + 636, 48)]
    for placement in halo.tags.block(halo.scenario + 624, 92):
        kind, = halo.tags.read('h', placement)
        if 0 <= kind < len(palette) and palette[kind] and palette[kind].group == 'weap':
            x, y, z, yaw = halo.tags.read('4f', placement + 8)
            yield palette[kind].name, (x, y, z), yaw


@dataclass
class Projection:
    x0: float
    y0: float
    z0: float
    scale: float

    def xy(self, x, y):
        return (x - self.x0) * self.scale + 9, (self.y0 - y) * self.scale + 9

    def z(self, z):
        return (z - self.z0) * self.scale

    def point(self, p):
        return (*self.xy(*p[:2]), self.z(p[2]))


def rasterize(halo, bsp, projection, size):
    floors, ceilings = {}, {}
    for shader, triangle in halo.triangles(bsp):
        if shader.group != 'senv' or 'filler' in shader.name:
            continue
        a, b, c = map(projection.point, triangle)
        bx, by, bz = (b[i] - a[i] for i in range(3))
        cx, cy, cz = (c[i] - a[i] for i in range(3))
        area = bx*cy - by*cx
        if abs(area) < 1e-5:
            continue
        if math.hypot(bz*cy-cz*by, bx*cz-cx*bz) > abs(area) * 2.2:
            continue
        layer = floors if area > 0 else ceilings
        for y in range(max(6, math.ceil(min(a[1], b[1], c[1]))), min(size-7, math.floor(max(a[1], b[1], c[1])))+1):
            for x in range(max(6, math.ceil(min(a[0], b[0], c[0]))), min(size-7, math.floor(max(a[0], b[0], c[0])))+1):
                u = ((x-a[0])*cy-(y-a[1])*cx)/area
                v = (bx*(y-a[1])-by*(x-a[0]))/area
                if min(u, v) >= -1e-5 and u + v <= 1.00001:
                    z = round((a[2] + u*bz + v*cz)*4)
                    if -460 <= z <= 468:
                        layer.setdefault((x, y), set()).add(z)
    return {p: sorted(z for z in levels if not any(1 < c-z < 22 for c in ceilings.get(p, ())))
            for p, levels in floors.items()}, ceilings


def component(samples, point, elevation):
    # Represent a single height at each XY, as native AC cells require.
    floor = {point: elevation}
    queue = deque([point])
    while queue:
        x, y = queue.popleft()
        current = floor[x,y]
        for p in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if p in floor:
                continue
            elevations = samples.get(p, ())
            if elevations:
                target = min(elevations, key=lambda z: abs(z-current))
                if abs(target-current) <= 6:
                    floor[p] = target
                    queue.append(p)
    return floor


def clear_square(floor, p, z, span=range(-1, 3)):
    x, y = p
    return all(abs(floor.get((x+dx,y+dy), 10000)-z) <= 13
               for dx in span for dy in span)


def projected_route(samples, start, end, actor_positions, inside=None, span=range(-1, 3)):
    def seed_score(p, z, target):
        return (p[0]-target[0])**2 + (p[1]-target[1])**2 + ((z/4-target[2])*2)**2

    def seed_for(target, limit=35):
        candidates = []
        for x in range(round(target[0])-limit, round(target[0])+limit+1):
            for y in range(round(target[1])-limit, round(target[1])+limit+1):
                for z in samples.get((x,y), ()):
                    if all(any(abs(other-z)<=9 for other in samples.get((x+dx,y+dy),()))
                           for dx in span for dy in span):
                        candidates.append((seed_score((x,y),z,target),(x,y),z))
        return min(candidates, default=None)

    tried, best = set(), None
    for target in (start,end,*actor_positions[:36]):
        seed = seed_for(target)
        if not seed or (seed[1],seed[2]) in tried:
            continue
        _, point, z = seed
        floor = component(samples,point,z)
        # Record multiple trial seeds for the same connected floor.
        for candidate in (start,end,*actor_positions[:36]):
            near = (round(candidate[0]),round(candidate[1]))
            if near in floor:
                tried.add((near,floor[near]))
        tried.add((point,z))
        points = {p for p, level in floor.items() if clear_square(floor,p,level,span)}
        if len(points)<20:
            continue
        first = closest(points,floor,start)
        parents = routes(points,first,floor)
        # Prefer a cell inside the exit trigger volume, so walking the route fires the transition.
        within = [p for p in parents if inside and inside(p[0], p[1], floor[p])]
        last = closest(within or parents,floor,end)
        path = trace_path(parents,last)
        if len(path)<16:
            distances = {first: 0}
            for p, parent in parents.items():
                if parent is not None:
                    distances[p] = distances[parent]+1
            last = max(distances, key=lambda p: (distances[p],p))
            path = trace_path(parents,last)
        if len(path) < 16:
            continue
        score = seed_score(path[0],floor[path[0]],start) + seed_score(path[-1],floor[path[-1]],end)
        if best is None or score < best[0]:
            best = (score,floor,parents,path)
        if score < 42:
            break
    return best


class Cell(tuple):
    """An (x, y) route cell that remembers its own floor height (quarter units)."""
    def __new__(cls, xy, z):
        cell = super().__new__(cls, xy)
        cell.z = z
        return cell


def retail_route(samples, start, end, inside=None, step=5, blocked=None, require_exit=False, low=(), cost=None,
                 tree=None, crossing=None, over=None, drop=0, minimum=16):
    """Route over the retail collision floors, which may stack several levels at one XY.

    Nodes are (x, y, quarter-unit floor). A cheapest-path search: each step costs 1, plus
    `cost(x, y, z)` for entering a node (a scripted door to wait at, a crawl space); `blocked`
    nodes are never entered. `crossing(here, node)` prices a step by where it comes from (a
    one-sided door walked into from its front), None forbidding it. With the collision `tree`, the route may also jump a low
    obstacle or a gap; `over(x, y, z)` prices each cell jumped over (a door panel), None forbidding it. With
    `drop` (quarter units, more than JUMP_DROP), it may also walk off a ledge onto a floor up to that
    far below (LEDGE_DROPS); those steps come back as cells with `fall` set. Returns (floor, cells, path) like projected_route, where `floor` keeps
    one height per XY for the native cube map (the route wins), or None.
    """
    def score(node, target):
        return (node[0]-target[0])**2 + (node[1]-target[1])**2 + ((node[2]/4-target[2])*2)**2
    nodes = [(x, y, z) for (x, y), levels in samples.items() for z in levels]
    if not nodes:
        return None
    # Start standing: crouch-only floors (`low`) are tops of props and crawl spaces.
    first = min((n for n in nodes if n not in low), default=None, key=lambda n: score(n, start))
    if first is None:
        return None
    parents = {first: None}
    best = {first: 0}
    queue = [(0, first)]
    clear, extra = {}, {}
    falls = set()

    def enter(node, here, price, fall=False):
        if blocked is not None:
            if node not in clear:
                clear[node] = not blocked(*node)
            if not clear[node]:
                return
        if cost is not None and node not in extra:
            extra[node] = cost(*node)
        total = best[here] + price + extra.get(node, 0)
        if crossing is not None:
            toll = crossing(here, node)
            if toll is None:
                return
            total += toll
        if total < best.get(node, float('inf')):
            best[node] = total
            parents[node] = here
            if fall:
                falls.add(node)
            else:
                falls.discard(node)
            heapq.heappush(queue, (total, node))

    done = set()
    while queue:
        spent, here = heapq.heappop(queue)
        if here in done:
            continue
        done.add(here)
        x, y, z = here
        for dx, dy in ((1,0), (-1,0), (0,1), (0,-1)):
            for other in samples.get((x+dx,y+dy), ()):
                if abs(other - z) <= step:
                    enter((x+dx, y+dy, other), here, 1)
            if tree is None:
                continue
            # Jump over a low obstacle or a gap (the cryo bay's jump tutorial): land two to
            # four cells on, with open space above whatever is in between.
            for reach in (2, 3, 4):
                for other in samples.get((x+dx*reach, y+dy*reach), ()):
                    fall = z - other > JUMP_DROP
                    if other - z > JUMP_CLIMB or (fall and z - other > drop):
                        continue
                    between = [(x+dx*k, y+dy*k) for k in range(1, reach)]
                    if any(abs(level - z) <= step for cell in between for level in samples.get(cell, ())):
                        continue  # walkable on foot: no jump needed
                    top = max([z, other] + [level for cell in between for level in samples.get(cell, ())
                                            if z < level <= z + JUMP_CLIMB]) / 4
                    if not all(tree.is_open(cx, cy, top + h) for cx, cy in between for h in (.4, 2.6, 4.8)):
                        continue
                    if fall:
                        # Walking off the ledge: the body falls past the ledge face, so the
                        # landing cell must be open all the way down from the ledge height.
                        lx, ly = x+dx*reach, y+dy*reach
                        levels = [other / 4 + h for h in (.4, 2.6, 4.8)]
                        while levels[-1] < top + 4.8:
                            levels.append(levels[-1] + 1.5)
                        if not all(tree.is_open(cx, cy, h) for cx, cy in between + [(lx, ly)] for h in levels
                                   if h >= other / 4 + .4):
                            continue
                        if over is not None and None in [over(cx, cy, round(top * 4)) for cx, cy in between]:
                            continue
                        enter((lx, ly, other), here, reach + DROP_COST, True)
                        continue
                    # Door panels are not in the BSP: `over` keeps a jump from passing through one.
                    toll = 0
                    if over is not None:
                        tolls = [over(cx, cy, round(top * 4)) for cx, cy in between]
                        if None in tolls:
                            continue
                        toll = sum(tolls)
                    enter((x+dx*reach, y+dy*reach, other), here, reach + JUMP_COST + toll)
            # Climb onto a ledge up to the jump height.
            for other in samples.get((x+dx,y+dy), ()):
                if step < other - z <= JUMP_CLIMB:
                    enter((x+dx, y+dy, other), here, 1 + JUMP_COST)
    exits = [n for n in best if inside and inside(n[0], n[1], n[2])]
    if require_exit and not exits:
        return None
    # The cheapest exit cell; without an exit volume, the cell nearest the end anchor.
    last = min(exits, key=lambda n: (best[n], score(n, end))) if exits else min(best, key=lambda n: score(n, end))
    path = [last]
    while parents[path[-1]] is not None:
        path.append(parents[path[-1]])
    path.reverse()
    if len(path) < minimum:
        return None
    floor = {}
    for x, y, z in path:
        floor.setdefault((x, y), z)
    # Extend the native floor around the route so actors and items placed nearby find a cell.
    queue = deque(floor)
    while queue:
        x, y = queue.popleft()
        for p in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if p not in floor and samples.get(p):
                target = min(samples[p], key=lambda z: abs(z-floor[x,y]))
                if abs(target-floor[x,y]) <= 6:
                    floor[p] = target
                    queue.append(p)
    cells = [Cell((x, y), z) for x, y, z in path]
    for cell, node in zip(cells, path):
        cell.fall = node in falls
    return floor, {p: None for p in floor}, cells


def trace_path(parents,end):
    path = [end]
    while parents[path[-1]] is not None:
        path.append(parents[path[-1]])
    return path[::-1]


def closest(points, floor, target):
    x, y, z = target
    return min(points, key=lambda p: ((p[0]-x)**2 + (p[1]-y)**2 + ((floor[p]/4-z)*2)**2, p))


def routes(points, start, floor):
    parents = {start: None}
    queue = deque([start])
    while queue:
        x, y = queue.popleft()
        for p in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if p in points and p not in parents and abs(floor[p]-floor[x,y]) <= 5:
                parents[p] = (x,y)
                queue.append(p)
    return parents


def native_cubes(floor, ceilings, size):
    for y in range(size):
        for x in range(size):
            z = floor.get((x,y))
            if z is None:
                yield b'\x00\x02\x00'
                continue
            corners = [floor.get((x+dx,y+dy)) for dx,dy in ((1,0),(0,1),(1,1))]
            if any(v is None for v in corners):
                yield b'\x00\x02\x00'
                continue
            base = z // 4
            # Keep overhangs above the selected layer as native ceilings.
            roof = min((c//4 for c in ceilings.get((x,y), ()) if c >= z+22), default=125)
            roof = min(125, max(base+6, roof))
            # Native SPACE cells keep consistent elevations across steep or
            # overlapping rooms. Exact render surfaces are drawn separately.
            yield bytes((4, (z//4)&255, roof&255, 2, 2, 0, 0, 2, 0))


def section_layout(halo, mission, part):
    bsp = mission.sections[part]
    region, address = halo.bsp(bsp)
    xmin, xmax, ymin, ymax, _, _ = region.read('6f', address+200)
    volumes = volume_centers(halo)
    actors = list(actor_starts(halo, bsp))
    previous = mission.sections[part-1] if part else None
    def resolve(anchor):
        return volumes.get(anchor) if isinstance(anchor,str) else anchor
    start = next(halo.player_starts())[:3]
    if previous is not None:
        start = resolve(TRANSITIONS.get((mission.cache,bsp,previous), f'bsp{bsp},{previous}'))
        if start is None and actors:
            start = actors[0][0][:3]
        if start is None:
            start = ((xmin+xmax)/2,(ymin+ymax)/2,region.read('f', address+216)[0]+2)
    end = resolve(exit_target(mission, part))
    if end is None and actors:
        end = max((p[:3] for p,_ in actors), key=lambda p: math.dist(start,p))
    if end is None:
        end = start
    start_override, end_override = SECTION_ANCHORS.get((mission.cache,part), (None,None))
    start = resolve(start_override) or start
    end = resolve(end_override) or end
    indoor = bsp in INTERIOR.get(mission.cache, ())
    margin = 28 if indoor else 65
    points = [start, end]
    box = (min(start[0],end[0])-margin, max(start[0],end[0])+margin, min(start[1],end[1])-margin, max(start[1],end[1])+margin)
    # The objectives the route walks through on the way must be on the map too (a50's prison
    # switch, deep inside a section entered and left through one doorway). One just past the
    # box (a10's bridge trigger, 2 units out) is already inside the map's padding, and the
    # layout of such sections stays as it was.
    for name, _ in SECTION_OBJECTIVES.get((mission.cache, part), ()):
        p = objective_point(halo, name)
        if max(box[0]-p[0], p[0]-box[1], box[2]-p[1], p[1]-box[3]) > 8:
            points.append(p)
    xmin, xmax = max(xmin,min(p[0] for p in points)-margin), min(xmax,max(p[0] for p in points)+margin)
    ymin, ymax = max(ymin,min(p[1] for p in points)-margin), min(ymax,max(p[1] for p in points)+margin)
    span = max(xmax-xmin,ymax-ymin,1)
    scale = min(8.0 if indoor else 4.1, 980/span)
    factor = max(7, math.ceil(math.log2(span*scale+24)))
    # Native cube floors hold heights of -115 to 117 units about the projection's origin, which
    # is the start's height. Sections in MIDDLE_ORIGIN drop further than that below their start
    # (b40's c1 tower lift, 16 retail units at the indoor scale): their origin is the middle of
    # the heights the route spans.
    origin = start[2]
    if (mission.cache, part) in MIDDLE_ORIGIN:
        heights = [start[2], end[2]]
        for name, teleport in SECTION_OBJECTIVES.get((mission.cache, part), ()):
            heights.append(objective_point(halo, name)[2])
            if teleport is not None and not isinstance(teleport, str):
                heights.append(teleport[2])
        origin = (max(heights) + min(heights)) / 2
    projection = Projection(xmin, ymax, origin, scale)
    if mission.cache == 'b30' and part == 2:
        # Both playable views of BSP 0 share one coordinate space.
        *_, factor, projection = section_layout(halo, mission, 0)
    return bsp, indoor, (xmin, xmax, ymin, ymax), start, end, actors, factor, projection


def make_section(halo, mission, part, output, collision='validated'):
    weapons = weapon_index(output)
    if not weapons:
        raise ValueError('run tools/halo_weapons.py first to write packages/halo/weapons.txt')
    bsp, indoor, (xmin, xmax, ymin, ymax), start, end, actors, factor, projection = section_layout(halo, mission, part)
    size, scale = 1 << factor, projection.scale
    projected_start, projected_end = projection.point(start), projection.point(end)
    samples, ceilings = rasterize(halo, bsp, projection, size)
    retail = collision == 'all' or (collision == 'validated' and mission.cache in VALIDATED_COLLISION and
                                    (mission.cache, part) not in STOCK_SECTIONS)
    projected = None
    if retail:
        # The engine walks the original collision surfaces. Plan the route, spawns and
        # markers on floors the player can stand on, not on render geometry. The b30 route,
        # checkpoints and QA waypoints were authored against the render raster, so it keeps
        # that layout and only gains its .hcol.
        surfaces, tree = structure_surfaces(halo, bsp, projection)
        machines, baked = static_device_surfaces(halo, bsp, projection, output,
                                                 CINEMATIC_STILL.get(mission.cache, ()))
        if mission.cache != 'b30':
            standing = floor_samples(surfaces + machines, tree, size)
            low = set()
            crouching = floor_samples(surfaces + machines, tree, size, crouch=low)
            # Two-sided sheets (the invisible ramps laid over stairs, grates) are not solid in
            # the BSP's open-space test but the engine collides with them. Drop the floors
            # they cover: the stair steps under a ramp are walked on the ramp itself.
            sheets = PolygonClearance([q for q in surfaces + machines if q[1] & 1], minimum_normal=2,
                                      radius=1.1, bottom=1.45)
            for samples_ in (standing, crouching):
                for xy in list(samples_):
                    kept = [z for z in samples_[xy] if not sheets.blocked(xy[0], xy[1], z)]
                    if kept:
                        samples_[xy] = kept
                    else:
                        del samples_[xy]
            low -= {(x, y, z) for (x, y), levels in standing.items() for z in levels}
            # The BSP open-space test samples a ring of points; thin posts and railings can sit
            # between them. Test the body against the real wall polygons as well.
            walls = PolygonClearance(surfaces + machines, radius=1.25, bottom=1.26 + 1.1)
            exit_name = exit_target(mission, part)
            box = volume_box(halo, exit_name) if isinstance(exit_name, str) else None
            test = (volume_test(box, projection) if box else
                    None if isinstance(exit_name, str) else objective_test(halo, exit_name, projection))
            doors = DoorBlocks(halo, projection, output, baked, NEVER_OPEN.get(mission.cache, ()),
                               mission.cache in UNGROUPED_DOORS_SEALED)
            # Prefer a route that needs no script to open a door; fall back to waiting at one.
            # Open doors still collide with their panels in the open pose.
            def solid(x, y, z):
                return doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or walls.blocked(x, y, z)
            def solid_low(x, y, z):
                return doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or walls.blocked(x, y, z, True)
            def one_sided(here, node):
                # A one-sided door entered from its front stays shut until a script makes it
                # two-sided or opens it (a50's muster bay doors, once the wave is beaten): wait
                # for that like a scripted door. One no script frees is a wall from that side.
                door = doors.one_sided_crossing(here, node)
                return 0 if door is None else SCRIPTED_COST if door[1] else None
            def over_door(x, y, z):
                # A jump may not pass through a sealed door or an open door's panels (a50's
                # gravity_door_lr). A scripted door jumped over stays free, as it always was:
                # a10's cryo jump tutorial lands through cryo_explosion_door_3 once it is open.
                return None if doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) else 0
            def crawl(x, y, z):
                return ((x, y, z) in low or walls.blocked(x, y, z)) * CROUCH_COST + \
                       bool(doors.scripted_at(x, y, z)) * SCRIPTED_COST
            # Walk upright where the section allows it. Some passages (a10's jammed bulkheads)
            # are only passable crouching, and some obstacles only by jumping (the cryo bay's
            # jump tutorial); the route marks those steps for the walker. A scripted door to
            # wait at costs far more than any detour, crouch or jump.
            # Objective volumes the mission script waits for the player to enter before it opens
            # the way on (a10's bridge, where Keyes's scene plays) are walked through in order.
            legs = []
            for name, flag in SECTION_OBJECTIVES.get((mission.cache, part), ()):
                legs.append((objective_test(halo, name, projection), projection.point(objective_point(halo, name)),
                             flag and projection.point(cutscene_flag(halo, flag) if isinstance(flag, str) else flag),
                             flag is not None and not isinstance(flag, str)))
            legs.append((test, projected_end, None, False))
            # Route steps the player reaches by riding a lift from the step before (b40's lift_d).
            rides = set()
            # Ledge drops (LEDGE_DROPS) in quarter units of the section's projection.
            drop = round(LEDGE_DROPS.get((mission.cache, part), 0) * scale * 4)
            route, origin, riding = None, projected_start, False
            names = [name for name, _ in SECTION_OBJECTIVES.get((mission.cache, part), ())] + [exit_target(mission, part)]
            for number, (inside, target, teleport, lift) in enumerate(legs):
                last = number == len(legs) - 1
                # An objective may be a step or two on from the last (d20's section 2 starts inside
                # enc3_1): only the whole route needs 16 steps, as a section's single leg always has.
                minimum = 16 if number == 0 and last else 1
                leg = retail_route(crouching, origin, target, inside, require_exit=True, low=low,
                                   blocked=solid_low, cost=crawl, tree=tree, crossing=one_sided, over=over_door,
                                   drop=drop, minimum=minimum) or \
                    (retail_route(crouching, origin, target, inside, low=low, blocked=solid_low, cost=crawl,
                                  crossing=one_sided, minimum=minimum) if last else None)
                if leg is None:
                    print(f'{mission.cache} section {part+1}: no route to {names[number]}', flush=True)
                    route = None
                    break
                if route is None:
                    route = leg
                else:
                    floor_, cells_, path_ = route
                    for xy, z in leg[0].items():
                        floor_.setdefault(xy, z)
                    # A leg after a ride may be a single cell (the ride sets the player down
                    # inside the next objective): there is no step after the ride to flag.
                    if riding and len(leg[2]) > 1:
                        rides.add(leg[2][1])
                    route = (floor_, {**cells_, **leg[1]}, path_ + leg[2][1:])
                end_cell = route[2][-1]
                origin = teleport or (end_cell[0], end_cell[1], end_cell.z / 4)
                riding = lift
            samples = crouching
            projected = route and (0, *route)
            if projected:
                for previous, cell in zip([None] + projected[3][:-1], projected[3]):
                    cell.crouch = (cell[0], cell[1], cell.z) in low or walls.blocked(cell[0], cell[1], cell.z)
                    cell.ride = any(cell is ride for ride in rides)
                    cell.fall = getattr(cell, 'fall', False)
                    cell.jump = previous is not None and not cell.ride and not cell.fall and (
                        cell.z - previous.z > 5 or abs(cell[0] - previous[0]) + abs(cell[1] - previous[1]) > 1)
            if projected and part:
                # The entry volume often straddles the door from the previous section, which the
                # mission script shuts once the new BSP loads (a10's mission_bsp). Start the
                # player past any door panel that could close across the first steps.
                _, floor_, cells_, path_ = projected
                crossed = [i for i, c in enumerate(path_[:40]) if doors.closable_at(c[0], c[1], c.z)]
                if crossed and len(path_) - crossed[-1] - 2 >= 16:
                    print(f'{mission.cache} section {part+1}: start moved {crossed[-1] + 2} steps past '
                          f'{doors.closable_at(*path_[crossed[-1]], path_[crossed[-1]].z)}', flush=True)
                    projected = (0, floor_, cells_, path_[crossed[-1] + 2:])
            if projected:
                # Diagnostics: the doors the walk waits at, and a route that stops short of its exit
                # (the last leg falls back to the cell nearest the end when no route reaches it).
                path_ = projected[3]
                waits = []
                for previous, c in zip([None] + path_[:-1], path_):
                    # A jump passes over the cells between its ends (d40's enc2_1_door2 threshold).
                    over = [] if previous is None or c.fall else \
                        [(previous[0] + (c[0] - previous[0]) * k // 4, previous[1] + (c[1] - previous[1]) * k // 4)
                         for k in range(1, 4)]
                    for x, y in over + [c]:
                        door = doors.scripted_at(x, y, max(c.z, previous.z if previous is not None else c.z))
                        if door and door not in waits:
                            waits.append(door)
                if waits:
                    print(f'{mission.cache} section {part+1}: route waits at scripted doors {", ".join(waits)}', flush=True)
                if test is not None and not test(path_[-1][0], path_[-1][1], path_[-1].z):
                    print(f'{mission.cache} section {part+1}: ' + (
                        f'route ends at the edge above its exit {exit_name} (EXIT_FALLS)'
                        if (mission.cache, part) in EXIT_FALLS else
                        f'WARNING route ends outside its exit {exit_name}'), flush=True)
    if projected is None and not (retail and mission.cache != 'b30'):
        projected = projected_route(samples, projected_start, projected_end,
                                    [projection.point(p) for p,_ in actors])
    if projected is None:
        raise ValueError(f'no continuous walkable layer at {start}')
    _, floor, parents, path = projected
    body = None
    if retail and mission.cache != 'b30':
        # Spawns, markers and items need room for a body, not only floor: a shelf at chest
        # height can sit between the route planner's clearance samples. Nudge each one along
        # the route to the nearest step with clearance from every wall, shelf and machine.
        body = PolygonClearance(surfaces + machines, radius=1.15, bottom=1.26 + 1.1)
    def roomy(i):
        if body is None:
            return i
        for k in sorted(range(-6, 7), key=abs):
            j = min(max(i + k, 0), len(path) - 1)
            if not body.blocked(path[j][0], path[j][1], path[j].z, getattr(path[j], 'crouch', False)):
                return j
        return i
    def xyz(p):
        return (*p, (p.z if isinstance(p, Cell) else floor[p])/4)
    def facing(a,b):
        return ac_yaw(math.atan2(a[1]-b[1],b[0]-a[0]))
    # The player spawns on the first step with room for the body.
    path = path[roomy(0):]
    halfway = roomy(len(path)//2)
    retail_surface = mission.cache == 'b30' and part == 2
    entries = [entity(tuple(map(round, projected_start)) if retail_surface else xyz(path[0]), PLAYERSTART,
                      900 if retail_surface else facing(path[0],path[min(5,len(path)-1)]), 1),
               entity(xyz(path[halfway]), PLAYERSTART, facing(path[halfway],path[min(halfway+5,len(path)-1)]),1),
               entity(tuple(map(round, projected_end)) if retail_surface else xyz(path[-1]), PLAYERSTART, 0,100)]
    for i in range(10, len(path)-4, 11):
        phase = 1 if i < len(path)//4 else 2 if i < halfway-3 else 3
        entries.append(entity(xyz(path[i]), PLAYERSTART,0,100,phase))
    for i in (0,halfway):
        for kind in (AMMO, HEALTH, GRENADE):
            spot = path[roomy(min(len(path)-1,i+kind*2))]
            entries.append(entity(xyz(spot),kind,0))
    used = []
    for wave, anchor in enumerate((path[len(path)//4],path[3*len(path)//4])):
        eligible = []
        for p, actor in actors:
            point = projection.point(p)
            if math.dist(point[:2],anchor) > 55:
                continue
            # Projection from an authored actor onto a nearby reachable cell.
            close = [(x,y) for x in range(round(point[0])-6,round(point[0])+7)
                           for y in range(round(point[1])-6,round(point[1])+7) if (x,y) in parents]
            if not close:
                continue
            place = closest(close,floor,point)
            if abs(floor[place]/4-point[2]) > 8 or math.dist(place,anchor)>35:
                continue
            eligible.append((math.dist(place,anchor),place,actor,p[3]))
        for _, place, (kind, weapon), angle in sorted(eligible):
            if any(math.dist(place,p) < 5 for p in used):
                continue
            # attr5 holds the carried weapon's packages/halo/weapons.txt index + 1.
            entries.append(entity(xyz(place),PLAYERSTART,ac_yaw(angle),0,wave,kind,weapons.get(weapon,-1)+1))
            used.append(place)
            if sum(e[4]==PLAYERSTART and e[5]==0 and e[6]==wave for e in entries) >= 4:
                break
    placed = 0
    for weapon, point, yaw in weapon_placements(halo):
        if weapon not in weapons or not (xmin <= point[0] <= xmax and ymin <= point[1] <= ymax):
            continue
        projected_point = projection.point(point)
        close = [(x,y) for x in range(round(projected_point[0])-4,round(projected_point[0])+5)
                       for y in range(round(projected_point[1])-4,round(projected_point[1])+5) if (x,y) in parents]
        if not close:
            continue
        place = closest(close,floor,projected_point)
        if abs(floor[place]/4-projected_point[2]) > 8:
            continue
        entries.append(entity(xyz(place),PLAYERSTART,ac_yaw(yaw),WEAPON_PLACEMENT,0,0,weapons[weapon]+1))
        placed += 1
    for p in (path[0],path[halfway],path[-1]):
        x,y,z = xyz(p)
        entries.append(entity((x,y,z+5),LIGHT,19,165,170,182))
    name = f'halo_{mission.cache}_{part+1:02}'
    output = output / 'packages'
    maps = output / 'maps'
    maps.mkdir(parents=True, exist_ok=True)
    (maps / f'{name}.cgz').write_bytes(encode_map(native_cubes(floor,ceilings,size),entries,factor,
                                                  f'{mission.title} | Section {part+1}'.encode()))
    (maps / f'{name}.cfg').write_text('fog 360\nfogcolour 0x617181\n')
    write_world_mesh(halo,output,projection.xy,projection.z,(xmin-8,xmax+8,ymin-8,ymax+8),
                     map_name=name,bsp=bsp,include_sky=not indoor)
    hcol, route_file = maps / f'{name}.hcol', maps / f'{name}.route'
    if retail:
        count = write_structure_collision(halo, bsp, projection, hcol, (machines, baked))
        # The walkable route in feet coordinates, for tests/test_retail_collision.py.
        # x y z [crouch]: the fourth column marks steps only passable crouching.
        # x y z [flags]: 1 marks steps only passable crouching, 2 a ledge to jump up onto, 4 a lift ride,
        # 8 a walk off a ledge onto the floor below (LEDGE_DROPS).
        # 4 marks the first step after a lift ride: use the control at the step before, board the
        # platform when it arrives, use the control on it and step off where it stops.
        def flags(p):
            value = (1 if getattr(p, 'crouch', False) else 0) | (2 if getattr(p, 'jump', False) else 0) | \
                    (4 if getattr(p, 'ride', False) else 0) | (8 if getattr(p, 'fall', False) else 0)
            return f' {value}' if value else ''
        rows = [('%d %d %.2f%s\n' % (*xyz(p), flags(p))) for p in path]
        if (mission.cache, part) in EXIT_FALLS:
            # Walk off the edge towards the exit volume's centre and fall into it (flag 8).
            rows.append('%d %d %.2f 8\n' % (round(projected_end[0]), round(projected_end[1]), projected_end[2]))
        route_file.write_text(''.join(rows))
        crouched = sum(bool(getattr(p, 'crouch', False)) for p in path)
        jumps = sum(bool(getattr(p, 'jump', False)) for p in path)
        lifts = sum(bool(getattr(p, 'ride', False)) for p in path)
        falls = sum(bool(getattr(p, 'fall', False)) for p in path)
        if crouched or jumps or lifts or falls:
            print(f'{name}: {crouched} route steps need crouching, {jumps} jumps' +
                  (f', {lifts} lift rides' if lifts else '') + (f', {falls} ledge drops' if falls else ''), flush=True)
        print(f'{name} BSP {bsp}: {count} retail collision surfaces, {len(baked)} static machines', flush=True)
    else:
        # A stale file would switch the engine to collision this section has not been checked against.
        hcol.unlink(missing_ok=True)
        route_file.unlink(missing_ok=True)
    print(f'{name} BSP {bsp}: {len(path)} steps, {len(used)} actors, {placed} weapons, {len(parents)} cells; '
          f'entry offset {math.dist(xyz(path[0]),projected_start)/scale:.1f}, '
          f'exit offset {math.dist(xyz(path[-1]),projected_end)/scale:.1f} source units',flush=True)


def convert(args):
    source, output, mission, part, collision = args
    with (source / f'{mission.cache}.map').open('rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        if halo.name != mission.cache:
            raise ValueError(f'Wrong cache in {mission.cache}.map')
        try:
            make_section(halo,mission,part,output,collision)
        except (ValueError, IndexError) as error:
            return f'{mission.cache} section {part+1}: {error}'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps',type=Path,help='Directory with the ten expanded Xbox campaign map caches')
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--mission',choices=[m.cache for m in MISSIONS])
    parser.add_argument('--part',type=int,help='One-based section number, used with --mission')
    parser.add_argument('--collision',choices=('validated','all','none'),default='validated',
                        help='Which sections get retail BSP collision (.hcol): validated missions (default), all, or none')
    args = parser.parse_args()
    jobs = [(args.maps,args.output,m,i,args.collision) for m in MISSIONS if not args.mission or m.cache==args.mission
            for i in range(len(m.sections)) if args.part is None or i+1==args.part]
    if not jobs or (args.part is not None and not args.mission):
        parser.error('Choose a valid mission and section')
    with ProcessPoolExecutor(max_workers=min(len(jobs),4)) as pool:
        failures = [error for error in pool.map(convert,jobs) if error]
    for failure in failures:
        print(failure,flush=True)
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
