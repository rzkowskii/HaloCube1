#!/usr/bin/env python3
"""Convert retail Xbox campaign BSP sections and authored scenario placements."""
import argparse
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
import math
import mmap
from pathlib import Path
import hashlib
import heapq
import json
import re
import time

from import_halo_a30 import HaloMap, ac_yaw, encode_map, entity, LIGHT, PLAYERSTART, AMMO, GRENADE, HEALTH, MAPMODEL
from halo_collision import (CollisionTree, DoorBlocks, FlightSpace, GrenadeArcs, LightBridges, PolygonClearance, SightLines,
                            VehicleGround, assist_surfaces, euler_rows, floor_samples, lift_ride, machine_boxes, overlap_seals,
                            scenery_raster, scenery_surfaces, static_device_surfaces, structure_surfaces,
                            write_structure_collision)
from halo_map_art import SCENERY_MODELS, write_scenery, write_world_mesh
from halo_weapons import actor_weapon, equipment_index, weapon_index
from halo_vehicles import vehicle_type
from halo_devices import structure_leaves

# PLAYERSTART team value for scenario weapon placements (100 is the route).
WEAPON_PLACEMENT = 101
# PLAYERSTART team value for scenario vehicle placements: attr3 is the halo_vehicles.VEHICLES
# index, attr4 the placement's not-placed flags (1 script-created, 2/4/8 not on easy/normal/hard),
# attr5 the scenario object name index + 1 (0 unnamed), which is the .hscb object index, and
# attr7 the section's scale times 20.
VEHICLE_PLACEMENT = 102
# PLAYERSTART team value for scenario equipment placements (ammo, health packs, grenades, the
# overshield and active camouflage): attr3 is the packages/halo/equipment.txt index, attr4 the
# not-placed flags (as for vehicles), attr5 the scenario object name index + 1 (0 unnamed).
EQUIPMENT_PLACEMENT = 103


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
# The Crash Site: the engine's first a30 section (packages/maps/halo_a30.*, halo_campaign.h), from the
# lifepod (the scenario's player start) up the valley to first_marine_extraction, where the authored
# layout ended too. With CRASH_SITE it is imported from the retail BSP 0 like the other sections, on
# a uniform projection (its own section_layout: the outdoor scale, 4.1), with an .hcol, a walked
# route and metrics (`--mission a30 --part 0`; a full import includes it), and
# tools/import_halo_a30.py only writes its textures and sky. Its tables use CRASH_SITE_KEY, not
# ('a30', 0), which is halo_a30_01. The tracked packages/maps/halo_a30.cfg (texture and mapmodel
# slots) is kept as it is, except that its mapmodel lines must match CRASH_SITE_MAPMODELS (below).
# Not walked in the game. To go back to the authored layout (tools/import_halo_a30.py: a non-uniform
# 2.75 by 2.0 projection, stock geometry, no route metrics), set CRASH_SITE = False, put the
# mapmodel lines of halo_a30.cfg back (git: zoff -64, the 2.75 sizes), delete packages/maps/halo_a30.hcol,
# .route and .metrics.json, and re-run tools/import_halo_a30.py and
# `tools/import_halo_scripts.py "$maps" --mission a30` (the Crash Site's script projection follows).
CRASH_SITE = True
CRASH_SITE_KEY = ('a30', 'crash')
CRASH_SITE_NAME = 'halo_a30'
CRASH_SITE_MISSION = Mission('a30', 'Halo', (0,), 'first_marine_extraction')
# The Crash Site's scenery (halo_map_art.SCENERY_MODELS, mapmodel slots 0 to 4 in that order, as the
# tracked halo_a30.cfg lists them): collision radius and height in AssaultCube units at the retail
# layout's scale (the authored 2.75 by 2.0 sizes times 1.5 across and 2.05 up) and a zero z offset:
# a model stands on the native cube floor under it plus its elevation (attr3 / 5). The importer
# checks halo_a30.cfg against this and refuses a mismatch. The route goes round the models'
# cylinders (a body collides with them, physics.cpp mmcollide), as round parked vehicles.
CRASH_SITE_MAPMODELS = {'tree_pine': (1, 14), 'tree_pine_tall': (1, 20), 'boulder_granite_large': (1, 4),
                        'boulder_granite_gigantic': (3, 8), 'lifepod_crashed': (1, 4)}


def crash_site_cfg_problems(output):
    """How the tracked packages/maps/halo_a30.cfg's mapmodel lines differ from CRASH_SITE_MAPMODELS."""
    cfg = output / 'packages' / 'maps' / f'{CRASH_SITE_NAME}.cfg'
    if not cfg.is_file():
        return [f'{cfg} is missing (it is tracked in git)']
    lines = [line.split() for line in cfg.read_text().splitlines() if line.startswith('mapmodel ')]
    problems = []
    for slot, model in enumerate(SCENERY_MODELS):
        # With the scenery's real collision in the .hcol (SCENERY_COLLISION), the mapmodels only draw:
        # a zero height turns their cylinder off (physics.cpp mmcollide, weapon.cpp).
        rad, height = (0, 0) if CRASH_SITE_KEY in SCENERY_COLLISION else CRASH_SITE_MAPMODELS[model]
        want = ['mapmodel', str(rad), str(height), '0', '1', f'"halo_a30/{model}"']
        if slot >= len(lines) or lines[slot][:6] != want:
            problems.append(f'mapmodel slot {slot}: {" ".join(lines[slot]) if slot < len(lines) else "missing"}, '
                            f'expected {" ".join(want)}')
    return problems


def crash_site_scenery(halo, projection):
    """The Crash Site's scenery models in campaign units: [(slot, (x, y, z), yaw, pitch, roll, scenario
    scenery index)]. A placement the mission does not place when it starts (not-placed flag 1, a
    script's to create) is left out: a mapmodel draws from the map's start and no script can take it
    away or bring it in. a30's only one is intro_tree (a tree_pine); no a30 script creates it (its one
    use is cutscene_intro's object_destroy, and no *_containing call matches it), so Halo never shows
    it in play (the use-aware rule's batch; before, it drew from the start)."""
    found = []
    tags = halo.tags
    palette = tags.block(halo.scenario + 540, 48)
    # halo.scenery()'s walk, keeping each placement's index in the scenario's scenery block.
    for index, placement in enumerate(tags.block(halo.scenario + 528, 72)):
        variant, _, not_placed = tags.read('2hH', placement)
        if not 0 <= variant < len(palette) or not_placed & 1:
            continue
        tag = halo.reference(tags, palette[variant], 'scen')
        x, y, z, yaw, pitch, roll = tags.read('6f', placement + 8)
        model = tag.name.rsplit('\\', 1)[-1]
        if model in SCENERY_MODELS:
            found.append((SCENERY_MODELS.index(model), projection.point((x, y, z)), yaw, pitch, roll, index))
    return found


def mapmodel_angles(yaw, pitch, roll):
    """A MAPMODEL entity's (attr1, attr5, attr6) that draw a scenery placement's retail (yaw, pitch,
    roll) (radians) as Halo turns it, in the campaign frame (y reflected).

    Halo turns an object by halo_collision.euler_rows, R = Rx(roll) Ry(-pitch) Rz(yaw); seen in the
    campaign frame (D = diag(1, -1, 1) on both sides, as the projection and the md3 export reflect y)
    that is T = D R D = Rx(-roll) Ry(-pitch) Rz(-yaw). AssaultCube draws a mapmodel (entities.cpp
    rendermapmodels, md3.h render: quat(-yaw - 180, pitch).roll(roll), fromquat, glMultMatrixf) with
    Rz(attr1 / 10 + 180) Ry(-attr5 / 10) Rx(-attr6): the other order, so the angles are solved for
    from T, not copied (the Crash Site batches copied them, which mirrored the yaw and put tilt on the
    wrong axes for a placement both turned and tilted). attr6 is a signed byte of whole degrees: of
    the two solutions the one whose roll fits (|roll| <= 127) is taken, and after rounding the roll the
    yaw and pitch are solved again for it (left within about half a degree; attr1 and attr5 are
    tenths)."""
    rows = euler_rows(yaw, pitch, roll)
    t = [[rows[i][j] * (-1 if (i == 1) != (j == 1) else 1) for j in range(3)] for i in range(3)]
    # T = Rz(a) Ry(b) Rx(c): T[2][0] = -sin b, T[2][1] = cos b sin c, T[2][2] = cos b cos c,
    # T[1][0] = sin a cos b, T[0][0] = cos a cos b.
    b = -math.asin(max(-1.0, min(1.0, t[2][0])))
    if math.cos(b) > 1e-6:
        a, c = math.atan2(t[1][0], t[0][0]), math.atan2(t[2][1], t[2][2])
    else:
        a, c = math.atan2(-t[0][1], t[1][1]), 0.0
    if abs(math.degrees(c)) > 127:
        # The other solution: Rz(a + pi) Ry(pi - b) Rx(c - pi).
        a, b, c = a + math.pi, math.pi - b, c - math.copysign(math.pi, c)
    c = math.radians(round(math.degrees(c)))
    # U = T Rx(-c) = Rz(a) Ry(b) for the rounded roll; keep the branch's sign of cos b.
    cc, sc = math.cos(c), math.sin(c)
    u = [[t[i][0], t[i][1] * cc + t[i][2] * sc, t[i][2] * cc - t[i][1] * sc] for i in range(3)]
    sign = 1 if math.cos(b) >= 0 else -1
    a = math.atan2(sign * u[1][0], sign * u[0][0])
    b = math.atan2(-u[2][0], u[0][0] * math.cos(a) + u[1][0] * math.sin(a))
    attr1 = round((math.degrees(a) - 180) * 10) % 3600
    attr5 = round(-math.degrees(b) * 10)
    attr5 = (attr5 + 1800) % 3600 - 1800
    return attr1, attr5, -round(math.degrees(c))


def scenery_drawn(halo, baked, as_models):
    """The baked scenery placements (scenery_surfaces' (index, count) pairs) that write_world_mesh
    draws: every one no mapmodel draws, [(scen tag, (x, y, z, yaw, pitch, roll))] in retail units."""
    if not baked:
        return []
    tags = halo.tags
    palette = tags.block(halo.scenario + 540, 48)
    placements = tags.block(halo.scenario + 528, 72)
    drawn = []
    for index, _ in baked:
        if index in as_models:
            continue
        variant, = tags.read('h', placements[index])
        drawn.append((halo.reference(tags, palette[variant], 'scen'), tags.read('6f', placements[index] + 8)))
    return drawn


class SceneryBlocks:
    """Route cells a body cannot stand in beside the Crash Site's scenery models (their mapmodel
    cylinders, CRASH_SITE_MAPMODELS), widened by the body (1.1) and the walker's drift (0.3)."""

    def __init__(self, scenery):
        self.cells = {}
        for slot, (x, y, z), *_ in scenery:
            rad, height = CRASH_SITE_MAPMODELS[SCENERY_MODELS[slot]]
            reach = rad + 1.1 + .3
            for cx in range(math.floor(x - reach), math.ceil(x + reach) + 1):
                for cy in range(math.floor(y - reach), math.ceil(y + reach) + 1):
                    if max(abs(cx - x), abs(cy - y)) <= reach:
                        self.cells.setdefault((cx, cy), []).append((z - 2, z + height))

    def blocked(self, x, y, z4):
        z = z4 / 4
        return any(low <= z + 5.2 and z <= high for low, high in self.cells.get((x, y), ()))
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
# c10 (343 Guilty Spark) crosses its interior on two storeys (z 0 and 1.9 in interior A) and
# light bridges that the player extends from control panels (LightBridges models them):
# - section 2 (BSP 1, lift_a to the BSP 2 doorway) is no longer stock: it walks the upper hall from
#   the bottom of lift_a and drops into the lower one (LEDGE_DROPS) beside bridge_a, which is
#   extended only from its far side. It has not been walked in the game; to put it back on stock
#   geometry add ('c10', 1) here and re-run `import_halo_campaign.py "$maps" --mission c10 --part 2`;
# - section 3 (BSP 2, the control room and the x50 scene) is no longer stock: it walks from the
#   lower doorway (bsp 2,1 #0) across hall C and hall D over two walking assists (ASSISTS) to bay B
#   and the lab, and after x50 out through the upper doorway (bsp 2,1 #1). Not walked in the game;
#   to put it back on stock geometry add ('c10', 2) here and re-run
#   `import_halo_campaign.py "$maps" --mission c10 --part 3`;
# - section 5 (BSP 3, lift_b to the BSP 4 doorway) is no longer stock (the batch after the final
#   integration review): it starts on the landing at the bottom of lift_b (SECTION_ANCHORS; bsp 3,1 is
#   near the top of the shaft), drops 1.9 units to the lower storey (LEDGE_DROPS), climbs the debris
#   pile in its west bay to the upper storey's balcony over one walking assist (ASSISTS), walks the
#   upper storey east and drops 1.9 units again by the lower BSP 3/4 doorway (bsp 3,4 #0, TRANSITIONS),
#   where it ends. The lower storey's own way east, bsp3_device_13 and _14, and the upper doorway's
#   bsp3_device_15 and _16 are off (placement flag 2) and nothing powers them. It does not cross
#   bridge_c (UNCROSSED_BRIDGES): flicker_bridge (continuous, never stopped) moves bridge_c's position
#   while BSP 3 is loaded, (device_set_position bridge_c 0), (sleep (random_range 15 75)),
#   (device_set_position bridge_c (real_random_range 0.5 1)), (sleep (random_range 15 75)), and the
#   deck is the machine's panels, which follow the position (the tag is "bridge fast": 0.0667 a tick),
#   so from the start of an extension to the next retraction is at most 90 ticks (3 s), while the
#   8.5-unit crossing takes 3.8 s at Halo's run (2.25 a second); an assist there would be a permanent
#   bridge, a link Halo does not have. The debris pile (x -47 to -44, y 35 to 41) is BSP rubble, a b40
#   metal sheet, tilted c_storage crates and three invisible door_blast_collision planes
#   (SCENERY_COLLISION); with Halo's own placement rotation (halo_collision.euler_rows, since the final
#   integration review) it is one walkable slope from the rubble's top (z -39.20, its north chamfer
#   -39.30) up the metal sheet (-39.0) and the first plane (normal z 0.88, -38.98 to -38.29) onto the
#   balcony (-38.31). Only the first climb, from the lower floor (-40.00), is beyond the engine's jump:
#   0.72 onto the rubble's chamfer, or 0.46 onto the c_storage lying on its side at the pile's foot
#   (top -39.54) and then 0.28 onto the chamfer. 0.46 is a standing jump in Halo (0.69); the assist is
#   a ramp onto that crate, and the 0.28 hop is the engine's own jump (route flag 2). The second plane
#   (normal z 0.70) is walkable too, unused; the third is too steep to stand on (a guard). The scenery
#   batch's figures (0.72 to 0.84, then 0.38 to 0.50) were measured with the old rotation. Not walked
#   in the game; to put it back on stock geometry add ('c10', 4) here and re-run
#   `import_halo_campaign.py "$maps" --mission c10 --part 5` and `import_halo_scripts.py "$maps"
#   --mission c10`. docs/macos-build.md has the details;
# - section 6 (BSP 4, to lift_d) is no longer stock (gap 5 of batch 5, the scenery batch): it starts at
#   the lower BSP 4/3 doorway (bsp 4,3 #0; TRANSITIONS), walks the lower storey and climbs to the
#   upper one, where bridge_d, bridge_e (mission_int_b waits for it) and the lift_d room are, by the
#   fallen debris under the upper floor's broken edge (x 6.4 to 9, y 79 to 85.8): the debris top is
#   0.80 above the lower floor and a notch in the broken edge 0.70 above the debris (then 0.3 up to
#   the upper floor), two crouch jumps in Halo (0.89) that the engine's jump cannot make, so two
#   walking assists (ASSISTS); the direct climbs (1.31 with 0.4 across, 1.0 with 1.9 across) stay out
#   of reach, as batch 6 found. It crosses bridge_e from brdige_e_cont_a and rides lift_d up
#   (LIFT_RIDES). Not walked in the game; to put it back on stock geometry add ('c10', 5) here and
#   re-run `import_halo_campaign.py "$maps" --mission c10 --part 6` and
#   `import_halo_scripts.py "$maps" --mission c10`. docs/macos-build.md has the details.
# c40 (Two Betrayals) flies a Banshee between the pulse generators. The sections that only a
# flight (or a lift in a BSP that is not a campaign section) leaves are flown now: they are in
# DRIVEN_SECTIONS below (an .hcol, no walking route), no longer here. Why they cannot be walked:
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
# - section 3 (BSP 2) is no longer stock (the d40 section 3 batch; not walked in the game; to put it
#   back add ('d40', 2) here and re-run `import_halo_campaign.py "$maps" --mission d40 --part 3` and
#   `import_halo_scripts.py "$maps" --mission d40`). Why it was: the lower floor, entered from BSP 1 at
#   bsp 2,1 (z 0), and the upper level
#   (the grate deck at z 1.49 round enc3_3b and enc3_4, the walkway at x -49 rising to z 2, and
#   the room with enc3_5b, enc3_6, section4 and bsp 2,3 at z 2) have no walkable connection in the
#   structure collision (nor in the render geometry). The only floor in between is a fallen beam
#   (at x -49, under burning_flame scenery; 0.24 to 0.79 units up, y -2.2 to 2.2). Its ends were
#   thought to be a crouch jump below the deck (0.75), but the BSP walls them in: at y 2.2 to 2.5 a
#   parapet stands from the floor to z 2.79 along the north deck's edge, and along the east deck's
#   edge (x -48.27 to -48.12) a curb and railing rise to 1.74 and 2.49, open only at y 0 to 0.69
#   above the curb, 1.4 above the beam there. No jump Halo's biped makes (0.89 crouched) gets onto
#   the deck from the beam, so no assist. Looked at again with its scenery's collision (the scenery
#   batch; SCENERY_COLLISION): the fallen h computer banks lie on the lower floor with their tops 0.30
#   to 0.72 up, and from each the nearest upper floor is 1.31 to 1.50 higher, beyond Halo's jump, so
#   the scenery is not the link either. enc3_3_door1 and _2 are automatic powered doors (attributes 0,
#   placement flags 0: enc3_3 waits for either to open to 0.9, then places the infection forms behind
#   it), not manual ones; the planner already walks through them, and they join the lower floor only
#   to itself. The one climb in Halo's envelope left is from the 45-degree chamfers at the foot of the
#   ribbed walls beside the grate deck (top 0.8, 0.3-wide ribs) up 0.81 onto the deck's edge, 1.0
#   across; whether Halo's biped stands on those chamfers is not settled statically, so no assist
#   (a link Halo may not have). BSP 2 has no lift or other device between the floors. Since the d40
#   section 3 batch the section is on retail collision all the same (it was the last section on stock
#   geometry): it starts on the upper level where its stock route started (SECTION_ANCHORS), so the
#   lower floor stays out of the route, as it was on stock geometry;
# - sections 6 to 8 (BSPs 5 to 7) are the Warthog run, 1,200 units driven in the original. The
#   run starts with a 45 to 65 degree ramp up from the elevator (BSP 5, x -94 to -82 past
#   no_jeep_test, z 107.6 to 119.4) and drops down ramps as steep in BSP 6 (x 358 to 366) and
#   BSP 7 (x 815 to 830); the planner and the engine (retailmove) walk only floors whose normal
#   is at least 0.65 (49 degrees). The sections are 300 to 500 units long, so their outdoor
#   scales (1.98, 3.23, 2.24) make the body 1.6 to 2.6 units tall, more than the Maw's corridors
#   and the elevator landing hold (from section6 only 3 cells of BSP 5 are reachable at its
#   scale). They are DRIVEN_SECTIONS instead: the player drives them in a Warthog.
# The table is empty since the d40 section 3 batch: every section of a validated mission plays on
# retail collision. To put a section back on stock geometry, add its key and re-run its import.
STOCK_SECTIONS = set()


@dataclass(frozen=True)
class Assist:
    """An authored walking assist: an invisible, two-sided walking surface (a plank or a ramp) baked
    into a section's .hcol (halo_collision.assist_surfaces, the AST1 trailer). `points` is a convex,
    planar polygon in retail units, walkable (at most 49 degrees). Not retail data."""
    points: tuple
    why: str


def ramp_y(x1, x2, y1, z1, y2, z2, why):
    """An Assist ramp across x1..x2 rising (or falling) along Y from (y1, z1) to (y2, z2)."""
    return Assist(((x1, y1, z1), (x2, y1, z1), (x2, y2, z2), (x1, y2, z2)), why)


def ramp_x(y1, y2, x1, z1, x2, z2, why):
    """An Assist ramp across y1..y2 rising (or falling) along X from (x1, z1) to (x2, z2)."""
    return Assist(((x1, y1, z1), (x1, y2, z1), (x2, y2, z2), (x2, y1, z2)), why)


# Walking assists (Assist), by section. The engine's jump is fixed (the movement constants are a hard
# requirement) and lower than Halo's: the cyborg biped's tag gives a jump velocity of 0.07 world
# units a tick (bipd +948) and the globals' falling-damage block was computed with a gravity of
# 0.003565 units a tick squared (its harmful 3 and 6 unit falls are 0.1463 and 0.2068 units a tick:
# v^2 / 2d), so a standing jump lifts the feet v^2/2g = 0.69 units, 0.89 crouched in the air
# (standing collision height 0.7, crouching 0.5, bipd +1060 and +1064), and running (2.25 units a
# second, the globals' player information) a level jump covers about 2.9 units. The engine's jump
# at the indoor scale lifts the feet 0.36 units and covers about 1.3 (JUMP_CLIMB and the planner's
# 2 to 4 cell jumps model less than that). Where the retail player makes a link with that jump and
# the engine cannot, an assist bakes one minimal invisible ramp (or plank) into the .hcol for it,
# never a link Halo does not have. The engine collides bodies (the player, actors) with assists;
# shots, sight lines, probes, grenades and the camera pass through them (halo_collision.cpp), and
# the planner walks them like floor but keeps them out of its sight lines and grenade arcs. Only the
# sections listed here get them; none of them has been walked in the game. To take a section's
# assists out, delete its entry and re-run `import_halo_campaign.py "$maps" --mission <m> --part N`;
# to put it back on stock geometry as well, add it to STOCK_SECTIONS.
ASSISTS = {
    # c10 section 3 (BSP 2): hall C, entered by the lower doorway, is a sunken channel whose only way
    # on is the platform over its south end, in front of post_helmet_door_2 (x -18.65 to -17.4, y
    # -34.2 to -35.6, at the walkways' height, 1.0 above the channel floor). The walkways either side
    # are 1.6 units across the slope from it, a level running jump in Halo (2.9) and more than the
    # engine's (1.3); the slope's struts stand between them except north of the platform (y -33.1
    # to -34.1), where the plank runs from the west walkway to the platform's north-west corner.
    # Beyond the door, hall D's lower floor meets the raised floor that leads up the V-shaped ramp to
    # bay B only at a bench whose tops stand 0.70 and 0.80 above that floor (the ramp lands on the
    # 0.80 one): a crouch jump in Halo (0.89; a standing jump's 0.69 falls just short of either),
    # and the engine's climbs end at 0.36; the second ramp climbs onto the bench.
    # With the section's scenery collision (SCENERY_COLLISION, the scenery batch) both stay: the tilted
    # c_storage leaning into the channel beside the platform gives Halo's player a second way (up the
    # crate's sloped face from the channel floor, then 0.47 from its exposed top onto the platform), and
    # the fallen b40 metal debris in hall D a lower first step (the bench is 0.41 above it), but each
    # still leaves a climb above the engine's 0.36, so an assist stays either way; the plank and the
    # ramp do not depend on the tilted scenery's pose (halo_collision.euler_rows, taken from Halo's own
    # source in the final integration review). The route was re-planned with the scenery in place.
    ('c10', 2): (
        ramp_y(-20.385, -18.323, -33.143, .008, -34.08, .008,
               'c10 hall C: west walkway across the channel to the platform at post_helmet_door_2 (Halo running jump, 1.6)'),
        ramp_y(-13.95, -12.95, -54.33, .006, -55.59, .789,
               'c10 hall D: lower floor up onto the bench before the raised floor (Halo crouch jump, 0.8)'),
    ),
    # c10 section 6 (BSP 4): the lower storey reaches the upper one only up the fallen debris under the
    # upper floor's broken edge, north of hall G's east end. The debris top (z -39.20, x 7.1 to 9.0,
    # y 80.6 to 83.5) is 0.80 above the lower floor (-40.00), and the broken edge has a notch (z -38.50,
    # x 5.8 to 6.4, y 82.3 to 82.9) 0.70 above the debris, 0.3 below the upper floor (-38.20): two
    # crouch jumps in Halo (0.89), beyond the engine's 0.36. The first ramp climbs from the floor south
    # of the debris onto its top, the second from the debris top west into the notch; the step from the
    # notch to the upper floor is the engine's own jump.
    ('c10', 5): (
        ramp_y(8.2, 8.8, 79.6, -39.994, 80.75, -39.194,
               'c10 BSP 4: lower floor up onto the fallen debris below the broken upper floor (Halo crouch jump, 0.80)'),
        Assist(((7.35, 82.42, -39.194), (7.35, 82.85, -39.194), (6.35, 82.85, -38.494), (6.35, 82.42, -38.494)),
               'c10 BSP 4: debris up into the notch in the upper floor\'s broken edge (Halo crouch jump, 0.70)'),
    ),
    # c10 section 5 (BSP 3): the lower storey reaches the upper one only up the debris pile in the west
    # bay (STOCK_SECTIONS' notes: bridge_c never stays extended long enough to cross). The pile is one
    # walkable slope from the BSP rubble's top up to the balcony; the lower floor (-40.00) meets it only
    # 0.72 below the rubble's north chamfer, or 0.46 below the top of the c_storage lying on its side at
    # its foot (scenery 1495: flat top -39.53 to -39.54, x -46.43 to -45.63, y 39.62 to 40.28, bevels out
    # to x -45.47), from which the chamfer is 0.28 higher, 0.375 across (the engine's own jump). The ramp
    # climbs 28 degrees west from the floor east of the crate onto its top (its upper corners on the
    # crate's top and east bevel): a Halo standing jump (0.69), beyond the engine's 0.36.
    ('c10', 4): (
        ramp_x(39.70, 40.10, -44.85, -39.994, -45.72, -39.536,
               'c10 BSP 3 west bay: lower floor up onto the lying c_storage crate at the foot of the debris pile (Halo standing jump, 0.46)'),
    ),
    # No assist for d40 section 3 (STOCK_SECTIONS' notes, above): none of its missing links is one Halo's
    # jump makes that an assist may stand in for (docs/macos-build.md, the notes on d40; batch 5 looked
    # again with its scenery's collision). On retail collision since the d40 section 3 batch, it starts on
    # the upper level (SECTION_ANCHORS) and needs none.
}
# Sections whose .hcol carries their scenery's collision (halo_collision.scenery_surfaces, the SCN1
# trailer): the scenario's scenery placements in the section's BSP and map window, each its coll tag's
# polygons (the permutation its render model draws) at its placement, as ordinary static surfaces:
# bodies, shots, grenades, the camera and vehicles collide with them, as with everything in Halo.
# Scenery a script creates, destroys or moves, or that some difficulty does not place, is left out
# (the .hcol is fixed). The planner walks on and round them like the static machines, and so do the
# vehicle planners (VehicleGround's floors and walls, FlightSpace's sight lines). They are drawn too:
# the Crash Site's trees, boulders and lifepod by their mapmodels where those stand (whose cylinders
# then no longer collide: halo_a30.cfg's zero heights), every other baked placement in the section's
# .mesh (write_world_mesh's `scenery`). The scenery batch started with the Crash Site and c10 sections
# 3 and 6; the batch after the c10 section 5 batch added every other section on retail collision,
# mission by mission, walked, driven and flown (each route still ends in its exit; the per-section
# figures are in docs/macos-build.md, Scenery collision); the d40 section 3 batch added d40 section 3
# (it left STOCK_SECTIONS) and the scenery of the EXTRA_BSPS a section holds (c40 section 1's BSP 12),
# scanned with the same filters and map window. A section on stock geometry gets no .hcol, so its entry
# only matters should it leave STOCK_SECTIONS. Not seen in the game. To take a section's scenery out,
# add its key to SCENERY_LEFT_OUT with the reason and re-run its import (for the Crash Site also put
# halo_a30.cfg's mapmodel lines back to CRASH_SITE_MAPMODELS' sizes).
SCENERY_LEFT_OUT = set()
SCENERY_COLLISION = ({CRASH_SITE_KEY} | {(m.cache, part) for m in MISSIONS if m.cache in VALIDATED_COLLISION
                                         for part in range(len(m.sections))}) - SCENERY_LEFT_OUT
# Scenery a script creates or destroys by name pattern (object_create_containing,
# object_create_anew_containing, object_destroy_containing: Halo matches the pattern anywhere in the
# object's name, strstr in hs_library_external.c of the Halo CE port in ../Halo3DS, as the engine's
# interpreter does) is left out like scenery a script creates, destroys or moves by its full name
# (halo_collision.scenery_surfaces, 'scripted by name pattern'). Except the calls and uses of the
# scripts listed here, which run only once the mission is over (judgement call: the .hcol is fixed,
# and a crate or a flame that goes in the last seconds before the end changes nothing the player can
# do); they count neither by pattern nor by full name (halo_collision.scenery_scripted):
# - a50 extraction_cleanup ("box": box_1 to box_4, c_storage in the hangar, BSP 2, sections 3 and 5):
#   game_win_script calls it after the hangar fight, behind a fade out, just before cutscene_extraction
#   and game_won;
# - d40 test_for_endgame and endgame_cleaner ("bsp_8_1", "_5", "_7", "_8" created, "bsp_8_" destroyed:
#   the flames and oxygen tanks along the Warthog run, BSP 7, section 8): test_for_endgame waits for
#   the player in grand_finale (the drive's end), then destroys every object, re-creates those and wakes
#   endgame_cleaner, which destroys them all after a sleep, while the ending plays;
# - b30 cutscene_extraction_exit (shaft_box_1 to _3: c_storage crates in BSP 0, sections 1 and 3, by
#   full name; the use-aware rule's batch): obj_shafta_goal calls it once the player boards the
#   extraction Pelican and calls game_won after it, so the crates go only in the mission's last scene.
#   Its other uses (the dust, falling_box_ and extraction Pelican objects) are script-created.
# The rule's one new left-out: d40's hangar_fire (x70_finale's hangar_3, the ending cinematic) matches
# hangar_fire_2a to _2c, placed burning_flame emitters in the Longsword's hangar (section 8), which have
# no collision model, so nothing changes. Every other *_containing pattern of the ten scenarios that
# matches a scenery name matches only scenery a script creates (not-placed flag 1, left out already)
# or whose full name a script uses: a10's x10_ dressing, b30's downed_smolder and dust, c10's blood,
# elite_corpse and shotgun, d40's banshee_fire, display, hangar_fire_1, hangar_tank and nipple (the
# d40 section 3 batch's scan; b40's dump, d20's 0_, 1_, 2_ and 2x_ and c40's object_ patterns match
# no scenery).
SCENERY_SCRIPTS_AT_END = {'a50': ('extraction_cleanup',), 'b30': ('cutscene_extraction_exit',),
                          'd40': ('test_for_endgame', 'endgame_cleaner')}
# Named scenery a script handles only outside the section where it stands, baked there all the same
# (scenery_surfaces' `kept`): the .hcol of the section that plays while it stands holds it, as Halo does.
# - c10 section 1: x50_rock (boulder_moss_large in the swamp, BSP 0, placed when the mission starts) is
#   destroyed only by x50's door script (object_destroy_containing x50_rock), which plays in section 3
#   (BSP 2, the control room); the player never comes back to BSP 0. Its full name is the pattern, so
#   the whole-name rule used to leave it out of section 1, where it stands throughout the play.
#   crashed_dropship (pelican_crashed, BSP 0) likewise: the same ini_x50_preclean destroys it, called by
#   mission_control once the player reaches control_lab_trigger in section 3 (the use-aware rule's batch).
# - a10 section 7: field_x30 (c_field_generator, BSP 6) is destroyed only by mission_final after the
#   player reaches end_trigger (the mission's exit), just before the x30 cinematic and game_won: the
#   mission is over (the use-aware rule's batch; judgement call, as for SCENERY_SCRIPTS_AT_END).
SCENERY_KEPT = {('c10', 0): ('x50_rock', 'crashed_dropship'), ('a10', 6): ('field_x30',)}
# Sections played in a vehicle (source/src/halo_vehicle.cpp), not walked: d40's Warthog run and
# c40's Banshee flights. They get an .hcol, which the vehicle drives or flies on (its body is the
# retail vehicle's size at the section's scale, so it fits where the walking body does not), but
# no .route: their spawns, markers and route come from the render raster, as on stock geometry.
# The flown c40 sections are FLOWN_SECTIONS as well. Not driven or flown in the game yet. To put
# a section back on stock AssaultCube geometry, add it to STOCK_SECTIONS as well (STOCK_SECTIONS
# wins) and re-run `import_halo_campaign.py "$maps" --mission <cache> --part N` (for c40 also
# `import_halo_scripts.py "$maps" --mission c40`: FLOWN_EXTENTS widens section 1's projection
# only while it is flown).
DRIVEN_SECTIONS = {('d40', 5), ('d40', 6), ('d40', 7), ('c40', 0), ('c40', 3)}
# Sections on retail collision that keep the route, spawns and markers of the render raster, as the
# Silent Cartographer work authored them: an .hcol and a raster route, no walked plan. None are left:
# b30's return (part 2) has been walked since batch 5, from where the player comes out of the shaft
# (bsp 0,1) to the ledge beside the extraction Pelican's ramp (SECTION_EXITS), and its first two
# sections since the route batch (gap 7 of batch 5's critic): the beach to the shaft A entrance
# (bsp 0,1), and the descent of shaft A from bsp 1,0 through shafta_lock_door (a door row,
# DOOR_WAITS) to the Cartographer's control (a use row, SECTION_OBJECTIVES), then back up to bsp 1,0.
# Their projections are unchanged (no anchor or objective moves their layout), so the .hcol files,
# b30.hscb and tests/test_b30_retail_collision.py's own waypoints (tests/data/b30-collision-qa-route.txt,
# authored on section 2's projection) still apply; that test is now the regression check of the
# section 2 layout the planned route shares. To put a section back on its raster route, add it here
# and re-run `import_halo_campaign.py "$maps" --mission b30 --part N`.
RASTER_ROUTES = set()
# Of those, the sections flown in a Banshee (the engine keeps a Banshee the player may take in
# them, halo.cpp flownSection): c40 section 1 starts in the control room (BSP 2) and walks down
# to fly_away_1 in the canyon, flies up to pulse generator 1 (pulse_1_trigger, BSP 12, which is
# not a campaign section; its collision is baked into the section's .hcol, EXTRA_BSPS, and the
# volume, which powers door_c1, is entered on foot), and lands on the c bridge to walk through
# door_c1; c40 section 4 walks the
# canyon (BSP 0) and flies to the landing pad, the engine's Banshee standing beside the start
# (c_banshee_1 is the script's pilot's, e62_a_ban_1 and _2 appear only at the main base).
FLOWN_SECTIONS = {('c40', 0), ('c40', 3)}
# Points (volume names) a flown section's map must hold beyond its BSP's bounds, so the flight
# reaches them: c40 section 1's pulse generator volumes stand 8 units past BSP 2's edge.
FLOWN_EXTENTS = {('c40', 0): ('pulse_1_trigger', 'e7_trigger_b', 'e8_trigger')}
# Planned vehicle routes for the DRIVEN_SECTIONS (gap 5 of the batch 7 critic; not driven or flown in
# the game yet). Each section's route is a list of legs, planned in order on the section's retail
# collision (plan_vehicle_route) and written to packages/maps/<section>.route for the QA walker
# (tests/native/retail_qa.inc: the vehicle rows, route flags 4096, 8192 and 16384), whose rows also
# become the section's route markers, insertion and checkpoint (the raster's floor still places the
# actors and items). A leg is
#   ('walk', target)   on foot (retail_route on the standing floors, round doors, walls and parked
#                      vehicles) to the target: None (the section's exit), a volume name, a retail point,
#                      ('vehicle', object) (beside that scenario vehicle's driver seat) or 'landing'
#                      (back to where the last flight landed, beside the vehicle);
#   ('board', type)    get into the nearest vehicles.txt `type` (8192 row where the player stands);
#   ('fly', target)    a Banshee flight (FlightSpace) from where the vehicle is to a low hover over a
#                      walkable cell with open sky, joined on foot to the target (as 'walk'), then out
#                      (4096 rows, then a 16384 row at the hover);
#   ('drive', target)  a Warthog drive (VehicleGround: half-width clearance, slope limit, ledge drops
#                      and jumps at speed) to the target (4096 rows; a jump's landing has flag 2 too).
# The engine's own vehicles: d40's escape seats the player at each section start
# (halovehicleescape), c40 section 4's Banshee appears beside the player (halovehicleensuretype).
# c40 section 1 walks from the control room to fly_away_1, flies up to the first pulse generator's
# platform (BSP 2 walls the generator in; the room is BSP 12, EXTRA_BSPS), walks in to
# pulse_1_trigger (which powers door_c1) and back, flies down to the c bridge before door_c1 and
# walks through it into bsp 2,10. To go back to the raster route for a section, delete its entry and
# re-run `import_halo_campaign.py "$maps" --mission <cache> --part N`.
VEHICLE_ROUTES = {
    ('d40', 5): (('drive', None),),
    ('d40', 6): (('drive', None),),
    ('d40', 7): (('drive', None),),
    ('c40', 0): (('walk', ('vehicle', 'fly_away_1')), ('board', 'banshee'), ('fly', 'pulse_1_trigger'),
                 ('walk', 'pulse_1_trigger'), ('walk', 'landing'), ('board', 'banshee'),
                 ('fly', (286.5, -627.9, 70.3)), ('walk', None)),
    ('c40', 3): (('board', 'banshee'), ('fly', None), ('walk', None)),
}
# BSPs a section also holds beyond its own: their structure collision goes into its .hcol and their
# render into its .mesh, in the section's projection (the engine loads one section map). c40 section
# 1's pulse generator room (BSP 12) is entered from BSP 2's generator platform: without it the
# player would walk into the void at pulse_1_trigger. The BSP's machines are not baked. The two BSPs
# overlap in the doorway corridor between them (retail y -621.75 to -634.48), and each closes it with
# a seal, which held_bsps leaves out of the walking and the .hcol (the route batch). Measured (gap 2
# of batch 5's critic, which took the doorway to be narrower than the body): the corridor is 1.2
# retail units wide at its floor, 2.0 at 0.8 up, tapering to 1.75 at 1.27 (the body's top at the
# section's 4.1 scale) and 2.8 high, so the engine's body (0.54 across, 1.27 tall at 4.1) fits with
# room to spare and the section keeps its scale (and c40.hscb its projection).
EXTRA_BSPS = {('c40', 0): (12,)}
# Vehicle route planning knobs: the ground vehicle's slope limit (floor normal z: the engine's car
# climbs steeper faces than the player's 0.65 with a run-up, and treats faces under 0.3 as walls), the
# margins kept round the vehicles' bodies (campaign units), the flight grid, the hover over a landing
# (the engine lets the pilot out within 1.5 Halo units of the floor) and the deepest drop off a ledge
# (Halo units).
VEHICLE_SLOPE, VEHICLE_MARGIN, FLIGHT_MARGIN, FLIGHT_CELL, VEHICLE_DROP = .4, .4, 1.0, 4.0, 4.0
# The deepest a ground vehicle's jump may land below its take-off (Halo units). The engine hurts
# neither a vehicle nor its riders when it lands (and the retail Warthog has no body vitality), so
# this only bounds the search; it covers d40 section 8's drop from the pit room's entry road into
# the lower bridge's bowl (about 23 Halo units), the only way down to the corridor there (judgement
# call: the retail run's way, as the room's geometry has it; not checked against the retail game).
VEHICLE_JUMP_DROP = 25.0
# The speeds a drive is planned at, as fractions of the car's top speed, fastest first: its jumps
# leave the ground at that speed. 0.9 is the speed after a run-up; a drive that arrives only slower
# records its speed (`drive.speed` in the metrics), which the tests hold the car to
# (qa_vehicle_cruise) and a player keeps to at its jumps.
DRIVE_SPEEDS = (.9, .85, .8, .75, .7)
# Jump governors (an engine accommodation; judgement call, docs/macos-build.md Planned vehicle
# routes): for every jump of a drive leg planned slower than DRIVE_SPEEDS[0] (d40 section 8's pit
# room drop clears the hanging bridge segment only below about 0.78 of top speed, and a player at
# full throttle would meet it), the section's map .cfg gets `halo_vehiclegovern X Y Z RADIUS SPEED`:
# the take-off row's point, a sphere of GOVERNOR_SECONDS of travel at the leg's speed (at least
# GOVERNOR_RADIUS units) and the leg's speed. halo_vehicle.cpp holds the player's car at that speed
# inside it (halo_vehiclegovernor, on by default): no throttle at it, the brakes above it. One second
# at 27 units a second leaves the Warthog three times its braking distance from 36 (9.5 units).
GOVERNOR_SECONDS, GOVERNOR_RADIUS = 1.0, 12.0
# The engine's ground vehicles without a `physics` line in vehicles.txt (an older table) fall at
# 40 units a second squared, as they always did (halo_vehicle.cpp legacyGravity).
LEGACY_VEHICLE_GRAVITY = 40.0
# Route flags of the vehicle rows (tests/native/retail_qa.inc): drive or fly to the row's point (the
# vehicle's own position: its floor point when driven, its origin when flown), board the named vehicle
# type from where the row stands, and get out of the vehicle at the row.
ROUTE_VEHICLE, ROUTE_BOARD, ROUTE_GET_OUT = 4096, 8192, 16384
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
# c10 (343 Guilty Spark): section 2 (BSP 1) arrives on interior A's upper storey (z 1.9) at the
# bottom of lift_a; the way on to the BSP 2 doorway is the lower storey (z 0), whose upper hall
# ends at bridge_a, retracted and extended only from its far side (bridge_a_cont_b): the route
# drops the 1.9 units into the lower hall beside its west end.
# The Crash Site (CRASH_SITE_KEY) leaves the lifepod's valley by two steep rocky slopes, too steep to
# stand on (2.1 units down over 1.5 at its north end, then 1.5 down into the marines' basin): a walk
# down them in Halo (falls under 3 units do no harm), slides here (SLIDES).
LEDGE_DROPS = {('c10', 1): 2.1, ('c10', 4): 2.1, ('d20', 1): 3.0, ('d20', 2): 2.6, ('d40', 0): 2.1, CRASH_SITE_KEY: 3.0}
DROP_COST = 300
# Sections whose ledge drops may also go down slopes too steep to stand on (retail_route's `slide`):
# the body slides down them (retailmove treats a face under 0.65 as a wall, and gravity takes it down
# along it), as Halo's biped does. Only these sections, so the other drops' routes are unchanged.
SLIDES = {CRASH_SITE_KEY}
TRANSITIONS = {
    # c10 section 6 starts at the lower BSP 4/3 doorway (the first "bsp 4,3"): the upper doorway's doors
    # (bsp3_device_15 and _16, bsp4_device_13 and _15) are off and nothing powers them.
    ('c10', 4, 3): 'bsp4,3#0',
    # c10 section 5 ends in the same doorway from BSP 3's side, the first "bsp 3,4" (z -40.85 to -38.13);
    # the second is the upper doorway's (z -38.1 to -37.1), whose doors are off.
    ('c10', 3, 4): 'bsp3,4#0',
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
    # The Crash Site starts on the ground just outside the crashed lifepod's open rear end, 3 units from
    # the scenario's player start (inside the pod, where Halo's player stands up after cutscene_intro),
    # in the direction that player faces. Since the use-aware rule's batch the pod (pod_of_death) is in
    # the .hcol, and its inside (about 1 unit high) has no room for the engine's 1.27-unit body, so the
    # planner found no layer at the player start. Judgement call: the first steps Halo's player walks,
    # no new link; mission_a30's wait for the player to leave lz_lifeboat then ends at once.
    CRASH_SITE_KEY: ((31.35, -100.45, 58.7), None),
    # a30 section 2 (BSP 0, after the Crash Site's standalone map) starts on the floor in front of the
    # first structure: the centre of first_structure_front is 7.5 units above it, and the floor nearest
    # that point was a 133-cell fragment (a rock top) cut off from the valley, so the route stopped 22
    # steps in. The layout still spans to cave_floor_entrance; the route ends on the ledge above the
    # cave floor (SECTION_EXITS).
    ('a30', 0): ((2.79, 48.89, 63.84), 'cave_floor_entrance'),
    # The return starts where the player comes out of the shaft (bsp 0,1), and its route ends on the
    # ledge beside the hovering Pelican's rear ramp (SECTION_EXITS), where the player boards with E.
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
    # Interior A starts on the landing at the bottom of lift_a, east of the deck (which is still
    # coming down when the section starts); bsp 1,0 is half way up the shaft, where no floor is
    # (the route used to start on the top landing).
    ('c10', 1): ((-58.6, -6.3, 2.0), None),
    # c10 section 5 (BSP 3) starts on the landing at the bottom of lift_b: bsp 3,1 is near the top of
    # the shaft, 34 units up, where no floor is.
    ('c10', 4): ((-18.0, 23.4, -37.9), None),
    # The control room (BSP 2) is entered by the lower of its two "bsp 2,1" doorways (#0, z -0.5):
    # the upper one's post_helmet_door_4 is off until ini_post_helmet_replace powers it after x50,
    # and it is the way out (SECTION_EXITS).
    ('c10', 2): ('bsp2,1#0', None),
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
    # d40 section 3 (BSP 2) starts on the upper level, on the walkway's floor at z 2.02 (the d40
    # section 3 batch: retail collision since then; STOCK_SECTIONS' notes on its floors). That is the
    # first row of the route it had on stock geometry (the render raster's route started there too,
    # 9.75 units from bsp 2,1, which is on the lower floor): the start a player already got, so no new
    # link. The lower floor, where bsp 2,1 lies, stays unreachable from the route, as on stock
    # geometry (judgement call: no walkable link joins it to the upper level, and none is invented).
    # The window does not change (the BSP's bounds clip it either way); the projection's origin is
    # the start's height, so d40.hscb is regenerated with it.
    ('d40', 2): ((-45.97, -16.31, 2.02), 'bsp2,3'),
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


@dataclass(frozen=True)
class UseControl:
    """A SECTION_OBJECTIVES step: walk within reach of the control at `point` (retail) and use it
    until the machine `device` stands at position `value` (route flag 256, `device@value`)."""
    point: tuple
    device: str
    value: float = 1.0

    def __str__(self):
        return f'use {self.device}'


@dataclass(frozen=True)
class ShootTarget:
    """A SECTION_OBJECTIVES step: walk to a standing spot from which one of `targets` (damage
    targets, halo_cast.damage_target: d40's manifold detectors) is in sight with the machine
    `opened` at position `value` (the piston the control before opened), the line of sight passing
    through the space that machine leaves at its initial position, and shoot it until its health is
    at most `health` (route flag 512, `target@health` and the aim point). Stands are searched within
    `reach` retail units of the target."""
    targets: tuple
    opened: str
    value: float = 1.0
    health: float = .1
    reach: float = 14.0

    def __str__(self):
        return f'sight of {self.targets[0]}'


@dataclass(frozen=True)
class ThrowTarget:
    """A SECTION_OBJECTIVES step for a damage target no walkable spot sees: walk to a standing spot
    from which a thrown frag grenade goes off within its blast radius of one of `targets`, with a
    clear line from the blast to the target (Scenario::targetsNear, haloscenariotargetsnear), with the
    machine `opened` at position `value`, and throw frags there until the target's health is at most
    `health` (route flag 2048, `target@health` and an aim point in the last three columns, one row
    per aim, best first). The throws are simulated on the retail collision (halo_collision.GrenadeArcs)
    from the standing spots the leg can walk to: every other cell each way within `reach` retail units
    of the target (across), every fourth cell out to `far`, from `below` retail units under the target
    to `above` over it (the lower walkways as well as the floor above; walkways more than 1.5 below it
    take every fourth cell throughout). From each spot a first look turns up to 60 degrees either side
    of the target's bearing and pitches from `pitches[0]` to `pitches[1]` degrees (low throws that roll
    in off a floor, lobs from below) 15 degrees apart; round each first throw that goes off within half
    a unit of the frag's outer radius of a target, a coarse pass 5 degrees apart; then round the 16
    best landings, from the cells up to two away and within 6 degrees in 2-degree steps. A throw
    counts once it lands and goes off more than the frag's outer blast radius from the thrower's body
    (feet to eye), so the player who throws it is not caught; the 300 that do the most damage from
    furthest off are ranked by how many of their eight neighbours a degree away count too, then by
    damage, then by the thrower's distance from the blast. The spot whose best aims rank highest is
    chosen and up to `aims` of its aims (2 degrees apart at least) are written (fewer than `least` is
    reported as a warning). Before the route batch (gap 9 of batch 5's critic): one ring of 3.5 on the
    floor above, pitches -30 to 10 in 5-degree steps throughout, no thrower distance, three aims."""
    targets: tuple
    opened: str
    value: float = 1.0
    health: float = .1
    reach: float = 3.5
    aims: int = 5
    far: float = 7.0
    below: float = 5.0
    above: float = 3.0
    pitches: tuple = (-45, 45)
    least: int = 3

    def __str__(self):
        return f'a throw at {self.targets[0]}'


# Volumes a section's script waits for the player to enter before it opens the way on, in
# order; the route walks through each before heading for the exit.
# An entry may name the cutscene flag the scene then puts the player at (object_teleport),
# where the next leg starts, or give that place as a retail point (where a lift sets the
# player down). An objective given as a retail point (a control the script waits
# for the player to use) is reached when the player stands within reach of it. The section's
# projection covers its objectives as well as its start and exit.
SECTION_OBJECTIVES = {
    ('a10', 1): (('bridge_trigger_3', 'chief_basea10'),),   # Keyes's bridge scene (mission_bridge, cinematic_x20)
    # mission_prison waits for prison_d_switch (cell block D) before the rescue scene (a Call cue
    # in halo_script.cpp), then the player walks back out. ini_post_rescue's object_teleport puts
    # the player at prison_player0_teleport, 1.6 units from the switch; the return leg still starts
    # at the switch, a step the walker covers from there.
    ('a50', 3): (((-15.1, -116.6, -19.0), None),),
    # b40's c1 tower: lift_d waits at the bottom of its shaft. lift_d_control_a, at the top,
    # calls it up and, used again from the platform, rides it down. The platform is not a floor
    # the route knows (the shaft below it drops to the pit at 43.7), so the walk goes on from
    # the landing beside it at the bottom (c1_bottom_save), on the way out to ext_c.
    ('b40', 5): (((263.83, -709.8, 70.87), (266.0, -707.0, 54.4)),),
    # c10: mission_swamp_a waits for enc_swamp2 before it calls lift_a up (int_a_trigger_3, the
    # lift room, is the exit's own room).
    ('c10', 0): (('enc_swamp2', None),),
    # b30 section 2 (BSP 1, shaft A) is entered and left through bsp 1,0, at the top of the shaft. The
    # engine opens shafta_lock_door when the section starts (Scenario::startCartographer: the adapted
    # approach skips shaft B's security detour) and holds the switch back to BSP 0 until the map is done
    # (switchBsp, cartographerMapped). obj_shafta_goal waits for the map_position group, which
    # shafta_map_control drives, at the bottom of the shaft; its use plays cutscene_map (the
    # Cartographer scene), which sets holohalo_1 to 1 and leaves the player at player0_map_done, and the
    # walk back up to bsp 1,0 starts there. The lock door is passed on a door row of its own (DOOR_WAITS).
    ('b30', 1): ((UseControl((6.55, -13.06, -14.44), 'holohalo_1'), 'player0_map_done'),),
    # c10 section 3 (BSP 2): mission_control waits for bayb_trigger and then control_lab_trigger,
    # where x50 plays (a Call cue) and leaves the player at player0_playon_c10 (object_teleport); the
    # walk out to the upper doorway starts there. The way from hall C to bay B is over two walking
    # assists (ASSISTS).
    ('c10', 2): (('bayb_trigger', None), ('control_lab_trigger', 'player0_playon_c10')),
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
    #   powers enc6_5_door1 and _door2 and section7 waits in BSP 3. The scene (a Call cue) leaves
    #   the player at player0_capt_done, 2 units away; the walk back to bsp 4,3 still starts in
    #   enc6_3, a step the walker covers from there;
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
    #   which waits for enc2_6 again and opens enc2_6_door, the way to bsp 1,2. The scene (a Call
    #   cue) leaves the player at player0_playon_base, 1.7 units from the volume, where the next
    #   leg is a step on;
    # - section 5 (BSP 4, the engine room): section5 wakes enc5_1 and enc5_2. The four manifolds
    #   are opened from their controls (enc5_1_controls1, _controln1, _controln3, _controls3, on
    #   the top floor: each moves its piston, enc5_1_pistons1 and so on, out of its channel, and
    #   opening one closes the others) and destroyed by shooting a detector in it
    #   (enc5_1_detector_s1 or _s1b and so on, which enc5_1_destroyed_poll reads with
    #   unit_get_health; destroy_manifold_* closes the piston again); enc5_2 then waits for every
    #   manifold and for enc5_2, opens enc5_2_door and wakes enc5_3, which raises the elevator once
    #   every player is in enc5_3. The route uses each control (UseControl) and walks to a spot that
    #   sees a detector through the open channel (ShootTarget), where the walker shoots it. No walkable
    #   spot sees s3's detectors, which sit at the inner end of their channel under the top floor's
    #   walkway: the route throws frags into the channel (ThrowTarget). Since the route batch the search
    #   takes in floor 2's walkway below as well, and the best spot is there (retail -92.3, 7.19, 2.03):
    #   lobs at pitch 48 up through the channel's open stretch, going off by s3 inside the frag's inner
    #   radius, 7.4 units from the thrower, every neighbour a degree away landing too.
    ('d40', 0): (('section1', None), ('enc1_4', None)),
    ('d40', 1): (('enc2_1', None), ('enc2_2', None), ('enc2_6', None), ('cinematic_bridge', None), ('enc2_6', None)),
    ('d40', 4): (('section5', None), ('enc5_1_main', None),
                 *((step, None) for manifold, control in (('s1', (-86.3, 6.62, 7.82)), ('n1', (-86.28, -10.12, 7.88)),
                                                        ('n3', (-94.03, -10.14, 7.89)), ('s3', (-94.04, 6.6, 7.89)))
                   for step in (UseControl(control, f'enc5_1_piston{manifold}'),
                                (ThrowTarget if manifold == 's3' else ShootTarget)(
                                    (f'enc5_1_detector_{manifold}', f'enc5_1_detector_{manifold}b'), f'enc5_1_piston{manifold}'))),
                 ('enc5_2', None)),
}

# Sections whose exit volume hangs in a shaft past the edge of the last floor: the player walks
# off the edge and falls into it. The .route file ends with one more step, at the volume's
# centre, for the walker to head for; spawns and markers stay on the floor. d20 section 1 ends
# in 0_to_1_transition_trigger, 1 to 4.5 units down the hole Cortana tells the player to jump
# into (objective_jump); section1 then plays cinematic_drop, the fall into the coolant.
EXIT_FALLS = {('d20', 0)}

# Sections whose scripted doors the walker waits at on a route row of their own (flag 1024 on the
# last step before the door, with the door's name and DOOR_OPEN): it stands at the door, inside
# its opening radius, clears the Covenant, looks round and uses the controls in reach until the
# door opens, and logs the door's power and position while it waits, instead of backing off and
# retrying as at other doors. a10 section 5 waits at tunnel_door_2 (powered by mission_tunnel
# after the tunnel conversations), section 6 at cryo_door_2 (opened by cryo_search, woken after
# the bridge scene, so only on a walk carried in from the sections before). The rows are in the
# route file only; the steps are where they were, so the section maps are unchanged. b30 section 2
# (the route batch) passes shafta_lock_door on its way down shaft A on one: the door starts open and
# only scripts close it (DoorBlocks' closable doors, which the walk goes through: they get a row only
# in the sections of DOOR_WAITS_CLOSABLE); the engine opens it again when the section starts
# (Scenario::startCartographer) and holds mission_shafta_lock, so the row passes at once unless it
# does not, and then the walker waits there and logs why instead of walking into it.
DOOR_WAITS = {('a10', 4), ('a10', 5), ('b30', 1)}
DOOR_WAITS_CLOSABLE = {('b30', 1)}
# The position at which a door the walker waits at counts as open (it walks on as it opens).
DOOR_OPEN = .5
# Trigger volumes a DOOR_WAITS door's script waits for the player to enter (all players) before it
# opens the door: the importer checks that the route passes through each before the door's row and
# records it in the section's metrics (`door_order`, tests/test_traversal_routes.py checks it).
# a10 section 5: mission_tunnel sleeps until every player is in tunnel_trigger_close, then closes
# tunnel_door_3 behind them (device_set_position 0), waits 45 ticks and for tunnel_door_3 to stand
# above 0.25, for game_all_quiet and the tunnel_2 conversation (each at most delay_dawdle), 4 s
# more, and powers tunnel_door_2. tunnel_door_3 is a blast door (machine kind 1, which never opens
# on approach) that closes at 0.0024 a tick (its tag's powered rate: 1/(30 x 14 s)), so 45 ticks
# after the script starts it, it stands near 0.89 and the wait passes at once; nothing has to
# reopen it. The chain powers tunnel_door_2 at most about 25 s after the player enters
# tunnel_trigger_close, provided mission_tunnel runs: mission_a10 wakes it only after the bridge
# scene, so on a walk carried in from sections 1 and 2 (not on a section started on its own).
DOOR_PREREQUISITES = {('a10', 4): {'tunnel_door_2': ('tunnel_trigger_close',)}}

# Sections whose projection origin is the middle of their height range (section_layout). a30 section 2
# drops 28 units from the first structure to the cave ledge, which at its scale (4.1) is at the edge
# of the native cube heights about a start-height origin.
MIDDLE_ORIGIN = {('a30', 0), ('b40', 5)}

# Lifts a section's route rides. The route file gets one more row, on the lift's deck (flag 16,
# with the lift's name and the position at which its deck is at that landing): the walker waits
# at the step before until the deck is there, calling it from the landing if no script moves it
# (flag 32), walks onto it, uses the control beside it if the deck has one (flag 64), and rides.
# A section whose BSP switch is half way along the shaft (c10's lifts, c20's platforms, d40's
# elevator) ends on the deck; b40's c1 tower rides on, and the step after the ride has flag 4.
# The deck row is in the route file only: spawns, markers and items stay on retail floor.
LIFT_RIDES = {
    ('b40', 5): 'lift_d',
    ('c10', 0): 'lift_a', ('c10', 3): 'lift_b', ('c10', 5): 'lift_d',
    ('c20', 0): 'platform1', ('c20', 1): 'platform2', ('c20', 2): 'platform3',
    ('d40', 4): 'elevator',
}

# A section's exit volume when it is not the default "bsp<this>,<next>" (or the mission end), or
# a retail point the route ends at (within reach, as for a SECTION_OBJECTIVES control).
SECTION_EXITS = {
    # a30 section 2: bsp 0,1, BSP 0's way on to BSP 1 (x -338), is 165 units beyond the section's map
    # window (start and cave_floor_entrance, 65 units round), and the cave floor (z 35) is cut off from
    # the valley by a 5-unit gap over a 4-unit trench, beyond any jump. The route ends on the ledge
    # (z 35.7) above it, 16.9 units from cave_floor_entrance's centre, where the section ends at its
    # end marker as before (halo.cpp's stage machine: no BSP switch is in reach on this map).
    ('a30', 0): (-99.65, -0.62, 35.67, 2.5),
    # b30's return ends on the ledge beside the extraction Pelican's rear ramp (37.8, -18.6, 9.1): the
    # mission's end volume, beach_lz, is the insertion beach 100 units west, and the section ends when
    # the player boards the Pelican there (halo.cpp, E once halopelicanready).
    ('b30', 2): (37.8, -18.6, 9.1, 1.5),
    # The a2 tower top (SECTION_ANCHORS): bsp 0,1 is beyond lift_a, in BSP 4.
    ('b40', 1): (41.5, -82.0, 48.0),
    # b40 section 3 ends at the bottom of the b3 lift, in the doorway into the tower (the first
    # "bsp 1,8", on BSP 1's side of b3_bot_ent_door). lift_c, the b3 lift, is in BSP 8, which is not a
    # campaign section (the engine never loads its collision or runs it in this section), and bsp 1,10
    # is at its top, 48 units up; the section ends at its end marker, and section 4 starts at the top.
    # The route used to aim for bsp 1,10 and stop beside the anchor, outside every exit volume.
    ('b40', 2): 'bsp1,8',
    # The c bridge enters BSP 11 at its top doorway; the bottom one is the way out of it.
    ('b40', 4): 'bsp2,11#0',
    ('b40', 5): 'bsp11,2#1',
    # c10's lifts: bsp 0,1 and bsp 1,3 are half way down the shafts. The route ends beside the
    # control the player uses to ride down: lift_a_cont_a at the top of lift_a, and the spot on
    # the landing nearest lift_b_cont_a, which stands on the lift's corner 1.25 units in from it.
    ('c10', 0): (-60.85, -7.93, 42.62),
    # c10 section 6 ends at lift_d_cont_a, on lift_d's deck at the bottom of its shaft: the walker calls
    # the lift, boards and rides it up (LIFT_RIDES); bsp 4,5 is half way up the shaft.
    ('c10', 5): (-38.74, 86.53, -37.37),
    ('c10', 3): (-16.4, 21.95, 1.9, .3),
    # c10 section 3 leaves the control room by the upper doorway (the second "bsp 2,1", z 1.9), whose
    # post_helmet_door_4 ini_post_helmet_replace powers after x50; section 4 walks on from it.
    ('c10', 2): 'bsp2,1#1',
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
# Missions whose routes walk over door leaves folded flat on the floor (halo_collision.floor_leaf):
# c10's interior doors (the BSP 3/4 doorway's bsp3_device_bsp4_device_1 to _3, BSP 4's
# bsp4_device_12 and _17) fold their leaves down into the doorway when open, 0.05 to 0.23 units
# proud of the floor, more than a step at the indoor scale. Halo's biped walks over them, and the
# engine lets bodies do the same (Scenario::collideDevices). The other missions' routes keep
# treating every panel as solid, as they were planned and walked: switching one over needs a re-walk.
FLOOR_LEAVES = {'c10'}
# Light bridges (LightBridges) a section's route may not cross: their decks are left out of its floor.
# c10 section 5's bridge_c is moved for ever by flicker_bridge (retracted, 0.5 to 2.5 s, extended to a
# random 0.5 to 1, 0.5 to 2.5 s, and so on), so its deck never stays long enough to cross (3 s at most
# from an extension to the next retraction, 3.8 s to cross at Halo's run); the route goes round by the
# west bay's debris pile instead (ASSISTS). The engine still moves and collides with the bridge.
UNCROSSED_BRIDGES = {('c10', 4): {'bridge_c'}}


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


def exit_target(mission, part, key=None):
    """The volume name (or retail point) that ends a section's route. `key` is the section's key in
    the tables (default (cache, part); the Crash Site's is CRASH_SITE_KEY)."""
    key = key or (mission.cache, part)
    bsp = mission.sections[part]
    following = mission.sections[part+1] if part+1 < len(mission.sections) else None
    if key in SECTION_EXITS:
        return SECTION_EXITS[key]
    if following is None:
        return mission.end
    return TRANSITIONS.get((mission.cache, bsp, following), f'bsp{bsp},{following}')


def damage_targets(halo):
    """{name: (retail centre, retail position, yaw, box low, box high)} of the scenario's damage
    targets (halo_cast.damage_target): the objects the walker shoots (ShootTarget)."""
    from import_halo_scripts import named_objects, Program
    from halo_cast import damage_target
    found = {}
    for name, tag, pose in named_objects(Program(halo)):
        target = damage_target(halo, tag)
        if target:
            _, low, high = target
            yaw = pose[3]
            cx, cy, cz = ((a + b) / 2 for a, b in zip(low, high))
            centre = (pose[0] + cx*math.cos(yaw) - cy*math.sin(yaw), pose[1] + cx*math.sin(yaw) + cy*math.cos(yaw), pose[2] + cz)
            found[name] = (centre, pose[:3], yaw, low, high)
    return found


def objective_point(halo, objective):
    """A SECTION_OBJECTIVES target in retail space: a volume's centre or a given point."""
    if isinstance(objective, UseControl):
        return objective.point
    if isinstance(objective, (ShootTarget, ThrowTarget)):
        return damage_targets(halo)[objective.targets[0]][0]
    return volume_centers(halo)[objective] if isinstance(objective, str) else objective


def shoot_stands(halo, objective, projection, samples, low, sights, opened):
    """{(x, y, quarter-unit z): (target name, campaign aim point)} for the standing route cells from
    which a ShootTarget's target is in sight (eye 4.5 units above the feet, as the engine's player)
    through the space its opened machine leaves, the nearest aim point of each target first.
    `sights` is a SightLines with the machines in the pose the objective opens; `opened` the
    machine_boxes entries of the opened machine at its initial position."""
    targets = damage_targets(halo)
    scale = projection.scale
    found = {}
    aims = []
    for name in objective.targets:
        if name not in targets:
            continue
        centre, position, yaw, lo, hi = targets[name]
        cosine, sine = math.cos(yaw), math.sin(yaw)
        half = [(b - a) / 2 for a, b in zip(lo, hi)]
        middle = [(a + b) / 2 for a, b in zip(lo, hi)]
        for fx, fy, fz in ((0, 0, 0), (0, 0, .8), (0, 0, -.8), (.6, .6, 0), (-.6, .6, 0), (.6, -.6, 0), (-.6, -.6, 0)):
            lx, ly, lz = middle[0] + fx*half[0], middle[1] + fy*half[1], middle[2] + fz*half[2]
            point = (position[0] + lx*cosine - ly*sine, position[1] + lx*sine + ly*cosine, position[2] + lz)
            aims.append((name, point, projection.point(point)))
    if not aims:
        return found
    reach = objective.reach * scale
    tx, ty, tz = projection.point(targets[objective.targets[0]][0])
    from halo_collision import _segment_box
    for (x, y), levels in samples.items():
        if (x - tx)**2 + (y - ty)**2 > reach * reach:
            continue
        for z in levels:
            if (x, y, z) in low or abs(z / 4 - tz) > 4 * scale:
                continue
            eye = (x, y, z / 4 + 4.5)
            reye = sights.retail(eye)
            for name, point, aim in aims:
                # Only a sight line the opening makes counts: it passes where the opened machine stood.
                if opened and not any(_segment_box(reye, point, place, box) for _, place, box in opened):
                    continue
                if sights.clear(eye, aim):
                    found[(x, y, z)] = (name, aim)
                    break
    return found


def frag_grenade(output):
    """The frag grenade (grenade type 0) of packages/halo/equipment.txt: (arming ms, fuse ms after the
    first bounce, blast inner radius, outer radius, damage lower bound, full damage), or the retail
    values when the table is missing."""
    path = Path(output) / 'packages/halo/equipment.txt'
    found = None
    if path.is_file():
        lines = path.read_text().splitlines()
        for i, line in enumerate(lines):
            words = line.split()
            if len(words) >= 7 and words[:2] == ['grenade', '0']:
                arming, fuse = float(words[5]), float(words[6])
                for following in lines[i + 1:i + 6]:
                    blast = following.split()
                    if len(blast) >= 6 and blast[0] == 'blast':
                        inner, outer, lower, upper = float(blast[1]), float(blast[2]), float(blast[3]), float(blast[4])
                        found = (arming * 1000, fuse * 1000, inner, outer, min(lower, upper), upper)
                        break
                break
    return found or (1500.0, 500.0, 1.5, 2.5, 80.0, 120.0)


def throw_plan(halo, objective, projection, cells, low, arcs, sights, frag, stats=None):
    """The throws of a ThrowTarget from the standing cells `cells` ((x, y, quarter-unit z) the leg can
    walk to): [(cell, yaw, pitch, campaign aim point, target, damage, neighbours that count, the
    thrower's distance from the blast in retail units)] at the best cell, best first, at most
    objective.aims of them, or [] when no simulated throw counts. `arcs` is a GrenadeArcs and `sights`
    a SightLines, both with the machines in the throw's pose. `stats`, a dict, gets the search's
    figures (stands, throws simulated, landings, landings too close to the thrower)."""
    targets = damage_targets(halo)
    named = [(name, targets[name]) for name in objective.targets if name in targets]
    if not named:
        return []
    scale = projection.scale
    arming, fuse, inner, outer, lower, full = frag
    centres = {name: projection.point(target[0]) for name, target in named}

    def box_distance(r, target):
        # Scenario::targetsNear: the distance from a blast at retail point r to the nearest point of
        # the target's box.
        _, position, yaw, lo, hi = target
        dx, dy, dz = r[0] - position[0], r[1] - position[1], r[2] - position[2]
        c, s = math.cos(yaw), math.sin(yaw)
        local = (dx*c + dy*s, -dx*s + dy*c, dz)
        gap = [max(lo[k] - local[k], local[k] - hi[k], 0.0) for k in range(3)]
        return math.sqrt(sum(g*g for g in gap))

    def blast(o):
        # The blast's damage to the nearest target in its radius, with haloscenariotargetsnear's clear
        # line from the blast to the box's centre.
        r = sights.retail(o)
        best = None
        for name, target in named:
            distance = box_distance(r, target)
            if distance >= outer or not sights.clear(o, centres[name]):
                continue
            falloff = 1.0 if distance <= inner else 1 - (distance - inner) / max(outer - inner, 1e-4)
            damage = lower + (full - lower) * falloff
            if best is None or damage > best[1]:
                best = (name, damage)
        return best

    tx, ty, tz = centres[named[0][0]]
    reach, far = objective.reach * scale, max(objective.far, objective.reach) * scale
    room = (far + (outer + 6) * scale)
    bounds = ((tx - room, ty - room, tz - (objective.below + outer + 1) * scale), (tx + room, ty + room, tz + 10 * scale))

    def ring(x, y, z):
        # Every other cell near the target at its level (the floor above included), every fourth
        # further out and on the walkways more than a unit and a half below it.
        d2 = (x - tx)**2 + (y - ty)**2
        if d2 <= reach * reach and z / 4 - tz >= -1.5 * scale:
            return x % 2 == 0 and y % 2 == 0
        return d2 <= far * far and x % 4 == 0 and y % 4 == 0
    stands = sorted((x, y, z) for x, y, z in cells if (x, y, z) not in low and ring(x, y, z) and
                    -objective.below * scale <= z / 4 - tz <= objective.above * scale)
    figures = {'stands': len(stands), 'throws': 0, 'landed': 0, 'too_close': 0}

    def clearance(cell, o):
        # The blast's distance from the thrower's body, feet to eye (retail units).
        b, feet = sights.retail(o), sights.retail((cell[0], cell[1], cell[2] / 4))
        top = feet[2] + 4.5 / scale
        z = min(max(b[2], feet[2]), top)
        return math.dist(b, (feet[0], feet[1], z))

    def land(cell, eye, yaw, pitch):
        figures['throws'] += 1
        result = arcs.throw(eye, yaw, pitch, fuse=(arming, fuse), bounds=bounds)
        hit = result and blast(result[0])
        if not hit:
            return None
        figures['landed'] += 1
        margin = clearance(cell, result[0])
        if margin <= outer:
            figures['too_close'] += 1
            return None
        return hit[0], hit[1], margin

    raw, tried, neighbours = [], set(), {}

    def attempt(cell, yaw, pitch):
        yaw = round(yaw % 360, 1)
        if (cell, yaw, pitch) in tried:
            return
        tried.add((cell, yaw, pitch))
        eye = (cell[0], cell[1], cell[2] / 4 + 4.5)
        landed = land(cell, eye, yaw, pitch)
        if landed:
            raw.append((landed[1], landed[2], cell, yaw, pitch, landed[0]))

    def near(hit):
        # How many of its neighbours a degree away count too (simulated once, for the hits ranked).
        _, _, cell, yaw, pitch, _ = hit
        if (cell, yaw, pitch) not in neighbours:
            eye = (cell[0], cell[1], cell[2] / 4 + 4.5)
            neighbours[cell, yaw, pitch] = sum(bool(land(cell, eye, yaw + a, pitch + b))
                                               for a in (-1, 0, 1) for b in (-1, 0, 1) if a or b)
        return neighbours[cell, yaw, pitch]

    def ranked(count):
        # The hits with their neighbours, best first: the neighbours are simulated for the `count`
        # hits that do the most damage from furthest off (the rest cannot all be checked: a lob into
        # the open channel lands from thousands of aims).
        best = sorted(raw, key=lambda h: (-h[0], -h[1], h[2], h[3], h[4]))[:count]
        return sorted(((near(h), *h) for h in best), key=rank)

    def rank(hit):
        return (-hit[0], -hit[1], -hit[2], hit[3], hit[4], hit[5])

    low_pitch, high_pitch = objective.pitches

    def screened(cell):
        # A first look from the spot, 15 degrees apart: the (yaw, pitch) of the throws that go off within
        # half a unit of the frag's outer radius of a target, line of sight or not. Only round those is
        # a finer look worth it.
        eye = (cell[0], cell[1], cell[2] / 4 + 4.5)
        bearing = math.degrees(math.atan2(ty - cell[1], tx - cell[0])) + 90
        close = []
        for turn in range(-60, 61, 15):
            for pitch in range(low_pitch, high_pitch + 1, 15):
                yaw = round((bearing + turn) % 360, 1)
                figures['throws'] += 1
                result = arcs.throw(eye, yaw, pitch, fuse=(arming, fuse), bounds=bounds)
                if result and min(box_distance(sights.retail(result[0]), target) for _, target in named) < outer + .5:
                    close.append((yaw, pitch))
        return close
    figures['screened'] = 0
    for cell in stands:
        close = screened(cell)
        figures['screened'] += bool(close)
        # The coarse pass, 5 degrees apart, round each close first look (the 15-degree square about it).
        for yaw, pitch in close:
            for turn in (-5, 0, 5):
                for tilt in (-5, 0, 5):
                    attempt(cell, yaw + turn, pitch + tilt)
    # The landings are narrow: look round the best coarse ones, from the cells up to two away (the
    # coarse pass takes every other cell, or every fourth further out) and within 6 degrees, in
    # 2-degree steps.
    walkable = {cell for cell in cells if cell not in low}
    for _, _, _, (x, y, z), yaw, pitch, _ in ranked(64)[:16]:
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                cell = next(((x + dx, y + dy, z + dz) for dz in (0, 1, -1, 2, -2) if (x + dx, y + dy, z + dz) in walkable), None)
                if cell is None:
                    continue
                for turn in range(-6, 7, 2):
                    for tilt in range(-6, 7, 2):
                        attempt(cell, yaw + turn, pitch + tilt)
    hits = ranked(300)
    figures['hits'] = len(raw)
    if stats is not None:
        stats.update(figures)
    if not hits:
        return []
    # The spot whose best aims count most reliably (neighbours that count, then damage), each aim at
    # least 2 degrees from the others.
    by_cell = {}
    for hit in hits:
        kept = by_cell.setdefault(hit[3], [])
        if not any(abs(hit[4] - other[4]) < 2 and abs(hit[5] - other[5]) < 2 for other in kept):
            kept.append(hit)
    cell = max(sorted(by_cell), key=lambda c: (sum((h[0] + 1) * h[1] for h in by_cell[c][:objective.aims]),
                                                min(h[2] for h in by_cell[c][:objective.aims])))
    chosen = []
    for near, damage, margin, at, yaw, pitch, target in by_cell[cell][:objective.aims]:
        eye = (cell[0], cell[1], cell[2] / 4 + 4.5)
        ya, pa = math.radians(yaw), math.radians(pitch)
        aim = (eye[0] + 64 * math.sin(ya) * math.cos(pa), eye[1] - 64 * math.cos(ya) * math.cos(pa), eye[2] + 64 * math.sin(pa))
        chosen.append((cell, yaw, pitch, aim, target, damage, near, margin))
    return chosen


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


def vehicle_placements(halo):
    """Scenario vehicle placements the engine knows (halo_vehicles.VEHICLES): type index,
    scenario object name index (-1 unnamed), not-placed flags, retail position and yaw."""
    palette = [halo.reference(halo.tags, entry) for entry in halo.tags.block(halo.scenario + 588, 48)]
    for placement in halo.tags.block(halo.scenario + 576, 120):
        kind, name = halo.tags.read('hh', placement)
        not_placed, = halo.tags.read('H', placement + 4)
        if not (0 <= kind < len(palette) and palette[kind] and palette[kind].group == 'vehi'):
            continue
        index = vehicle_type(palette[kind].name)
        if index is None:
            continue
        x, y, z, yaw = halo.tags.read('4f', placement + 8)
        yield index, name, not_placed, (x, y, z), yaw


def section_vehicle_placements(halo, bsp, bounds):
    """The vehicle placements of one section: inside its map bounds (xmin, xmax, ymin, ymax)
    and its BSP. A placement belongs to the BSP whose structure holds it (BSP bounds overlap:
    d40's elevator shaft, BSP 4, reaches the landing where BSP 5's Warthogs are parked); the
    bounds decide only when no BSP's leaves hold it."""
    xmin, xmax, ymin, ymax = bounds
    bsps = [halo.bsp(i) for i in range(len(halo.tags.block(halo.scenario + 1444, 32)))]
    bx0, bx1, by0, by1, bz0, bz1 = bsps[bsp][0].read('6f', bsps[bsp][1] + 200)
    for kind, name_index, not_placed, point, yaw in vehicle_placements(halo):
        if not (xmin <= point[0] <= xmax and ymin <= point[1] <= ymax and bx0 <= point[0] <= bx1 and
                by0 <= point[1] <= by1 and bz0 - 1 <= point[2] <= bz1 + 1):
            continue
        if not structure_leaves(*bsps[bsp], point) and \
                any(structure_leaves(*other, point) for i, other in enumerate(bsps) if i != bsp):
            continue
        yield kind, name_index, not_placed, point, yaw


def vehicle_bounds(output):
    """{halo_vehicles.VEHICLES index: (low, high)} model bounds (Halo units, x forward, y left,
    z up) from packages/halo/vehicles.txt, or {} before tools/halo_vehicles.py has written it."""
    path = output / 'packages/halo/vehicles.txt'
    bounds, index = {}, None
    if not path.is_file():
        return bounds
    for line in path.read_text(encoding='ascii').splitlines():
        words = line.split()
        if len(words) >= 3 and words[0] == 'vehicle':
            index = int(words[1])
        elif len(words) == 7 and words[0] == 'bounds' and index is not None:
            values = [float(v) for v in words[1:]]
            bounds[index] = (tuple(values[:3]), tuple(values[3:]))
    return bounds


class VehicleBlocks:
    """The scenario vehicles parked in a section when it starts, as the engine's bodies collide
    with them (halovehiclecollide in source/src/halo_vehicle.cpp): the retail model's length,
    a width centred on the vehicle's axis and its height above the floor, at the vehicle's
    retail point and heading. Placements a script creates (not-placed flag 1: d40's trench
    jeeps, most of a50's turrets) are not there when the walk passes and are left out, as are
    those no difficulty places. A route cell is blocked when a standing body there (its
    half-width plus the walker's drift, as DoorBlocks) would touch a box it cannot stand on.
    """

    def __init__(self, projection, placements, bounds, names=None):
        self.projection = projection
        self.boxes, self.names = [], []
        scale = projection.scale
        radius = (1.1 + .3) / scale
        for kind, name_index, not_placed, point, yaw in placements:
            if not_placed & 1 or not_placed & 14 == 14 or kind not in bounds:
                continue
            low, high = bounds[kind]
            half_length = max(.2, high[0] - low[0]) / 2 + radius
            half_width = max(.2, high[1] - low[1]) / 2 + radius
            centre = (high[0] + low[0]) / 2
            height = max(.2, high[2] - max(0., low[2]))
            # The engine settles a vehicle onto the floor under its point; the retail point is
            # at or a little above that floor.
            bottom, top = point[2] - .25, point[2] + height
            cosine, sine = math.cos(yaw), math.sin(yaw)
            reach = math.hypot(half_length + abs(centre), half_width) * scale
            # The map stores the placement on the nearest cell (make_section), where the engine
            # puts the vehicle.
            px, py = (round(c) for c in projection.xy(*point[:2]))
            ox, oy = (px - 9) / scale + projection.x0, projection.y0 - (py - 9) / scale
            self.boxes.append((ox, oy, cosine, sine, centre, half_length, half_width, bottom, top,
                               px - reach, px + reach, py - reach, py + reach))
            self.names.append(names[name_index] if names and 0 <= name_index < len(names) else f'#{kind}@{point[0]:.0f},{point[1]:.0f}')

    def blocked(self, x, y, z4):
        """The vehicle a standing body at route cell (x, y, quarter-unit floor) would touch."""
        if not self.boxes:
            return None
        p = self.projection
        rx = (x - 9) / p.scale + p.x0
        ry = p.y0 - (y - 9) / p.scale
        feet = p.z0 + z4 / 4 / p.scale
        body = 5.2 / p.scale
        for i, (ox, oy, c, s, centre, half_length, half_width, bottom, top, x0, x1, y0, y1) in enumerate(self.boxes):
            if not (x0 <= x <= x1 and y0 <= y <= y1):
                continue
            dx, dy = rx - ox, ry - oy
            along, side = dx * c + dy * s, -dx * s + dy * c
            # As the engine: a body whose feet are within 0.42 AssaultCube units of the top
            # stands on it; one wholly below it passes under.
            if abs(along - centre) < half_length and abs(side) < half_width and \
                    feet < top - .42 / p.scale and feet + body > bottom + .1 / p.scale:
                return self.names[i]
        return None


def equipment_placements(halo):
    """Scenario equipment placements: eqip tag path, object name index (-1 unnamed), not-placed
    flags, retail position and yaw."""
    palette = [halo.reference(halo.tags, entry) for entry in halo.tags.block(halo.scenario + 612, 48)]
    for placement in halo.tags.block(halo.scenario + 600, 40):
        kind, name = halo.tags.read('hh', placement)
        not_placed, = halo.tags.read('H', placement + 4)
        if 0 <= kind < len(palette) and palette[kind] and palette[kind].group == 'eqip':
            x, y, z, yaw = halo.tags.read('4f', placement + 8)
            yield palette[kind].name, name, not_placed, (x, y, z), yaw


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
                 tree=None, crossing=None, over=None, drop=0, minimum=16, fill=None, explored=None, slide=False):
    """Route over the retail collision floors, which may stack several levels at one XY.

    Nodes are (x, y, quarter-unit floor). A cheapest-path search: each step costs 1, plus
    `cost(x, y, z)` for entering a node (a scripted door to wait at, a crawl space); `blocked`
    nodes are never entered. `crossing(here, node)` prices a step by where it comes from (a
    one-sided door walked into from its front), None forbidding it. With the collision `tree`, the route may also jump a low
    obstacle or a gap; `over(x, y, z)` prices each cell jumped over (a door panel), None forbidding it. With
    `drop` (quarter units, more than JUMP_DROP), it may also walk off a ledge onto a floor up to that
    far below (LEDGE_DROPS); those steps come back as cells with `fall` set. With `slide` (SLIDES), such a
    drop may also go down a slope too steep to walk, landing up to six cells on, when the body has room
    along the straight line from the ledge to the landing. `explored`, a dict, gets
    every node the search reached with its cost (ThrowTarget picks its throwing spot among them).
    Returns (floor, cells, path) like projected_route, where `floor` keeps
    one height per XY for the native cube map (the route wins), or None. `floor` spreads from the
    route over `fill` (default `samples`): the floors without the light-bridge decks, which are
    floor only while extended, so actors and items are not put on them.
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
            for reach in (2, 3, 4, 5, 6) if slide else (2, 3, 4):
                for other in samples.get((x+dx*reach, y+dy*reach), ()):
                    fall = z - other > JUMP_DROP
                    if other - z > JUMP_CLIMB or (fall and z - other > drop) or (reach > 4 and not fall):
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
                            # Down a slope too steep to stand on (SLIDES): the body slides on it, so it
                            # needs room along the line from the ledge to the landing instead.
                            if not slide or not all(
                                    tree.is_open(x+dx*k, y+dy*k, (z + (other - z) * k / reach) / 4 + h)
                                    for k in range(1, reach + 1) for h in (.4, 2.6, 4.8)):
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
    if explored is not None:
        explored.update(best)
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
    fill = samples if fill is None else fill
    queue = deque(floor)
    while queue:
        x, y = queue.popleft()
        for p in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if p not in floor and fill.get(p):
                target = min(fill[p], key=lambda z: abs(z-floor[x,y]))
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


# The vdelta of the solid cubes the cube-raster stamp writes (native_cubes' `scenery`): the engine lets
# light through them on a campaign section map (haloSceneryVdelta in source/src/halo_campaign.h, keep in
# sync), so models are tinted as before the stamp. Every other solid cube has 0.
RASTER_SCENERY_VDELTA = 83


def native_cube_open(floor, x, y, scenery=(), opened=()):
    """Whether native_cubes writes cube (x, y) as open space: a floor at (x, y) and at the three grid
    points of its square's other corners, or a cube in `opened` with a floor of its own, and not one
    of the `scenery` cubes (scenery_raster's solid ones)."""
    if (x, y) not in floor or (x, y) in scenery:
        return False
    return (x, y) in opened or all((x+dx, y+dy) in floor for dx, dy in ((1,0),(0,1),(1,1)))


def native_cubes(floor, ceilings, size, scenery=(), opened=(), capped=()):
    """The native cube raster. `scenery`: the cubes the cube-raster stamp made solid (scenery_raster),
    written with RASTER_SCENERY_VDELTA so the engine's light passes them. `opened`: cubes kept open
    space even where a corner of their square has no floor (the route's and the placed entities'
    cubes since the cube-raster stamp: they had stood in solid cubes beside walls and on other storeys).
    `capped`: open cubes whose ceiling is one unit over the floor (a mapmodel's cube inside its own
    scenery: the engine draws the mapmodel at the floor, and neither a waypoint nor a body fits)."""
    for y in range(size):
        for x in range(size):
            z = floor.get((x,y))
            if (x, y) in scenery:
                yield bytes((0, 2, RASTER_SCENERY_VDELTA))
                continue
            if z is None or not native_cube_open(floor, x, y, opened=opened):
                yield b'\x00\x02\x00'
                continue
            base = z // 4
            # Keep overhangs above the selected layer as native ceilings.
            roof = min((c//4 for c in ceilings.get((x,y), ()) if c >= z+22), default=125)
            roof = min(127, base + 1) if (x, y) in capped else min(125, max(base+6, roof))
            # Native SPACE cells keep consistent elevations across steep or
            # overlapping rooms. Exact render surfaces are drawn separately.
            yield bytes((4, (z//4)&255, roof&255, 2, 2, 0, 0, 2, 0))


def section_layout(halo, mission, part, key=None):
    key = key or (mission.cache, part)
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
    end = resolve(exit_target(mission, part, key))
    if end is None and actors:
        end = max((p[:3] for p,_ in actors), key=lambda p: math.dist(start,p))
    if end is None:
        end = start
    start_override, end_override = SECTION_ANCHORS.get(key, (None,None))
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
    for name, _ in SECTION_OBJECTIVES.get(key, ()):
        p = objective_point(halo, name)
        if max(box[0]-p[0], p[0]-box[1], box[2]-p[1], p[1]-box[3]) > 8:
            points.append(p)
    xmin, xmax = max(xmin,min(p[0] for p in points)-margin), min(xmax,max(p[0] for p in points)+margin)
    ymin, ymax = max(ymin,min(p[1] for p in points)-margin), min(ymax,max(p[1] for p in points)+margin)
    # A flown section reaches past its BSP (FLOWN_EXTENTS): the map grows to hold those volumes,
    # 20 units round them. Only while the section is flown (not put back in STOCK_SECTIONS).
    if key not in STOCK_SECTIONS:
        for name in FLOWN_EXTENTS.get(key, ()):
            p = volumes.get(name)
            if p is not None:
                xmin, xmax = min(xmin, p[0] - 20), max(xmax, p[0] + 20)
                ymin, ymax = min(ymin, p[1] - 20), max(ymax, p[1] + 20)
    span = max(xmax-xmin,ymax-ymin,1)
    scale = min(8.0 if indoor else 4.1, 980/span)
    factor = max(7, math.ceil(math.log2(span*scale+24)))
    # Native cube floors hold heights of -115 to 117 units about the projection's origin, which
    # is the start's height. Sections in MIDDLE_ORIGIN drop further than that below their start
    # (b40's c1 tower lift, 16 retail units at the indoor scale): their origin is the middle of
    # the heights the route spans.
    origin = start[2]
    if key in MIDDLE_ORIGIN:
        heights = [start[2], end[2]]
        for name, teleport in SECTION_OBJECTIVES.get(key, ()):
            heights.append(objective_point(halo, name)[2])
            if teleport is not None and not isinstance(teleport, str):
                heights.append(teleport[2])
        origin = (max(heights) + min(heights)) / 2
    projection = Projection(xmin, ymax, origin, scale)
    if mission.cache == 'b30' and part == 2:
        # Both playable views of BSP 0 share one coordinate space.
        *_, factor, projection = section_layout(halo, mission, 0)
    return bsp, indoor, (xmin, xmax, ymin, ymax), start, end, actors, factor, projection


# Route metrics (`packages/maps/<section>.metrics.json`, git-ignored with the maps): what the
# stub-route gate (tests/test_route_metrics.py) reads. Written by every import of a section, and
# by `--metrics-only`, which plans the section as an import would but writes nothing else.
# Format 2 (the ownerless-kill batch): every metrics file carries `cfg_current` (format 1 files
# from before the jump governors lack it, and a stale .cfg would pass unseen).
METRICS_FORMAT = 2
# A route is a stub (definition of done, roadmap item 3) when it has fewer steps than this, its
# reachable floor is under this share of the section's floor columns, or it ends more than this
# many source units from the section's exit point; and a walked route (retail collision) that
# does not end inside its exit volume (EXIT_FALLS sections end at the edge above it: they count
# as inside). The reach share is not held against a walked route that ends inside its exit: the
# planner proved that route connects the entry to the exit, and a low share there only means the
# section's map window holds floors the route has no reason to visit (a big outdoor BSP).
STUB_STEPS, STUB_REACH_PCT, STUB_EXIT_OFFSET = 100, 5.0, 25.0


def stub_reasons(metrics, exit_falls=False):
    """Why a section's route counts as a stub (empty: it does not). `exit_falls`: the section is in
    EXIT_FALLS."""
    reasons = []
    # A planned route: walked, or a driven or flown section's vehicle route (VEHICLE_ROUTES).
    walked = metrics['mode'] == 'walked' or bool(metrics.get('vehicle_route'))
    inside = walked and (metrics['ends_in_exit'] or exit_falls)
    if metrics['steps'] < STUB_STEPS:
        reasons.append(f"{metrics['steps']} steps")
    if not inside and metrics['reach_pct'] < STUB_REACH_PCT:
        reasons.append(f"reach {metrics['reach_pct']:.1f}%")
    if metrics['exit_offset'] > STUB_EXIT_OFFSET:
        reasons.append(f"exit offset {metrics['exit_offset']:.1f}")
    if walked and metrics['ends_in_exit'] is False and not exit_falls:
        reasons.append('ends outside its exit volume')
    return reasons


class MergedTree:
    """Open space of a section's BSP and the EXTRA_BSPS it holds: a point is open when it is open in
    the section's BSP, or inside an extra BSP's bounds (campaign units) and open there."""
    RING, CROUCH_TOP = CollisionTree.RING, CollisionTree.CROUCH_TOP
    can_stand, can_crouch = CollisionTree.can_stand, CollisionTree.can_crouch

    def __init__(self, tree, extras):
        self.tree, self.extras, self.projection = tree, extras, tree.projection

    def is_open(self, x, y, z):
        if self.tree.is_open(x, y, z):
            return True
        for (x0, y0, z0), (x1, y1, z1), extra in self.extras:
            if x0 <= x <= x1 and y0 <= y <= y1 and z0 <= z <= z1 and extra.is_open(x, y, z):
                return True
        return False


def bsp_box(halo, bsp, projection, pad=1.0):
    """A BSP's bounds in campaign units, as ((x0, y0, z0), (x1, y1, z1))."""
    region, address = halo.bsp(bsp)
    xmin, xmax, ymin, ymax, zmin, zmax = region.read('6f', address + 200)
    a, b = projection.point((xmin, ymax, zmin)), projection.point((xmax, ymin, zmax))
    return (tuple(min(a[i], b[i]) - pad for i in range(3)), tuple(max(a[i], b[i]) + pad for i in range(3)))


def held_bsps(halo, key, bsp, projection, surfaces, tree):
    """A section's structure surfaces with the EXTRA_BSPS it holds: (its own surfaces, the extra BSPs'
    surfaces, [(box, box, tree)] for MergedTree, how many seals were left out). Where two of the BSPs
    share a doorway, each closes it with a seal (a polygon without a material) inside the other's open
    space: c40 section 1's BSP 2 at y -621.75 and BSP 12 at y -634.48 (retail), across the 1.2-unit
    doorway corridor between pulse generator 1's platform and its room. Halo switches BSPs in the
    corridor, so its player never meets either; the engine holds both BSPs at once and would collide
    with both, which kept the walk (and the player) out of the room. Those seals are left out of the
    walking and of the .hcol (halo_collision.overlap_seals). A section without EXTRA_BSPS gets its
    surfaces back unchanged."""
    own, extra, extras, dropped = list(surfaces), [], [], 0
    held = []
    for number in EXTRA_BSPS.get(key, ()):
        extra_surfaces, extra_tree = structure_surfaces(halo, number, projection)
        held.append((number, extra_surfaces, extra_tree, bsp_box(halo, number, projection)))
    if not held:
        return own, extra, extras, 0
    own_box = bsp_box(halo, bsp, projection)
    for number, extra_surfaces, extra_tree, box in held:
        # The section's seals inside an extra BSP's space, and the extra BSP's inside the section's.
        seals = set(overlap_seals(own, extra_tree, box))
        own = [q for i, q in enumerate(own) if i not in seals]
        theirs = set(overlap_seals(extra_surfaces, tree, own_box))
        extra += [q for i, q in enumerate(extra_surfaces) if i not in theirs]
        extras.append((*box, extra_tree))
        dropped += len(seals) + len(theirs)
    return own, extra, extras, dropped


def vehicle_table(output):
    """{vehicles.txt name: {'index', 'low', 'high', 'forward', 'seats': {name: (at, enter)}}}, and
    'gravity_scale' and 'world_gravity' from the `physics` line when the table has one."""
    path, table, current = output / 'packages/halo/vehicles.txt', {}, None
    if not path.is_file():
        return table
    for line in path.read_text(encoding='ascii').splitlines():
        words = line.split()
        if len(words) >= 3 and words[0] == 'vehicle':
            current = table.setdefault(words[2], {'index': int(words[1]), 'seats': {}})
        elif current is None or not words:
            continue
        elif words[0] == 'bounds' and len(words) == 7:
            values = [float(v) for v in words[1:]]
            current['low'], current['high'] = tuple(values[:3]), tuple(values[3:])
        elif words[0] == 'motion' and len(words) >= 3:
            current['forward'] = float(words[2])
        elif words[0] == 'physics' and len(words) >= 4:
            current['gravity_scale'], current['world_gravity'] = float(words[2]), float(words[3])
        elif words[0] == 'seat' and len(words) >= 9:
            values = [float(v) for v in words[3:9]]
            current['seats'][words[1]] = (tuple(values[:3]), tuple(values[3:]))
    return table


def vehicle_top_speed(d, scale):
    """The engine's Warthog top speed (halo_vehicle.cpp topSpeed for a car): the retail speed at the
    section's scale, at least 36 units a second (the Maw's timer was set for it)."""
    return max(36.0, d.get('forward', .25) * 30 * scale)


def vehicle_gravity(d, scale):
    """The engine's airborne gravity for a ground vehicle (halo_vehicle.cpp vehicleGravity), in
    campaign units a second squared: the world's gravity (vehicles.txt `physics`, Halo units a tick
    squared, 30 ticks a second) times the phys tag's gravity scale at the section's scale, times
    the square of how much faster the engine's vehicle runs than the retail one (its top speed over
    the retail speed at that scale, at least 1). The engine keeps a 36 units a second floor on the
    Warthog's speed (2.1 times the retail speed at d40 section 8's scale); at that pace retail
    gravity alone would carry it 4.4 times as far off every ramp as Halo's Warthog goes. Scaling
    gravity by the square of the speed ratio keeps the retail Warthog's arcs over the same ground,
    flown that much faster. Without the line (an older table) the engine's old 40."""
    world = d.get('world_gravity')
    if not world:
        return LEGACY_VEHICLE_GRAVITY
    retail = d.get('forward', .25) * 30 * scale
    ratio = max(1.0, vehicle_top_speed(d, scale) / retail) if retail > .5 else 1.0
    return max(1.0, min(80.0, world * 900 * d.get('gravity_scale', 1.0) * scale * ratio * ratio))


def nearest_volume_box(halo, name, near):
    """The trigger volume named `name` ("<name>#<n>" picks one) nearest retail point `near`: some names
    repeat (d40's "bsp 5,6" at both ends of BSP 5)."""
    if '#' in name or near is None:
        return volume_box(halo, name)
    boxes, n = [], 0
    while True:
        box = volume_box(halo, f'{name}#{n}')
        if box is None or n > 32:
            break
        boxes.append(box)
        n += 1
    if not boxes:
        return None
    def centre(box):
        f, side, u, origin, extent = box
        return tuple(origin[i] + (f[i]*extent[0] + side[i]*extent[1] + u[i]*extent[2]) / 2 for i in range(3))
    return min(boxes, key=lambda box: math.dist(centre(box), near))


def plan_vehicle_route(halo, mission, part, key, projection, size, bsp, surfaces, tree, machines, baked, start, end,
                       output, placements, say):
    """The planned route of a driven or flown section (VEHICLE_ROUTES): (rows, cells, ended, goal, extras,
    drive) with the .route file's rows, the route's cells (Cell, z in quarter units) for the section's
    markers, whether it ends inside the section's exit, the exit's campaign point, how many EXTRA_BSPS
    it planned over and the drive legs' totals ({'jumps', 'seconds', 'gravity', 'speed', 'top'}: the
    jumps planned, the time the drive takes at the planner's speed, the vehicle gravity, the speed the
    drive is planned at (the slowest leg's), the car's top speed and 'governors', the jump governors
    of the legs planned slower than DRIVE_SPEEDS[0], [x, y, z, radius, speed] each); None when the section
    has no plan or the vehicle table is missing."""
    legs = VEHICLE_ROUTES.get(key)
    table = vehicle_table(output)
    if not legs or not table:
        return None
    scale = projection.scale
    from import_halo_scripts import named_objects, Program
    objects = {name: (tag, pose) for name, tag, pose in named_objects(Program(halo))}
    own, extra_surfaces, extras, seals = held_bsps(halo, key, bsp, projection, surfaces, tree)
    if seals:
        print(f'{key}: {seals} seals between the BSPs it holds left out (held_bsps)', flush=True)
    surfaces = own + extra_surfaces
    tree = MergedTree(tree, extras) if extras else tree
    walking = surfaces + machines
    exit_name = exit_target(mission, part, key)

    def target_of(target):
        """(inside predicate on (x, y, z4) or None, campaign point) of a leg's target."""
        if target is None:
            target = exit_name
        if isinstance(target, str):
            box = nearest_volume_box(halo, target, end if target == exit_name else None)
            if box is None:
                raise ValueError(f'{key}: no volume {target}')
            f, side, u, origin, extent = box
            middle = tuple(origin[i] + (f[i]*extent[0] + side[i]*extent[1] + u[i]*extent[2]) / 2 for i in range(3))
            return volume_test(box, projection), projection.point(middle)
        if len(target) == 4:
            point, reach = target[:3], target[3]
        else:
            point, reach = target, 2.0
        p = projection.point(point)
        return (lambda x, y, z4, p=p, r=reach * scale: math.hypot(x - p[0], y - p[1]) <= r and abs(z4 / 4 - p[2]) <= 3 * scale), p

    # Foot legs: the standing floors with the walking planner's walls, doors and parked vehicles.
    feet_floor = None

    def walk_setup():
        nonlocal feet_floor
        if feet_floor is None:
            standing = floor_samples(walking, tree, size)
            walls = PolygonClearance(walking, radius=1.25, bottom=1.26 + 1.1)
            doors = DoorBlocks(halo, projection, output, list(baked), NEVER_OPEN.get(mission.cache, ()),
                               mission.cache in UNGROUPED_DOORS_SEALED, mission.cache in FLOOR_LEAVES)
            parked = VehicleBlocks(projection, placements, vehicle_bounds(output), list(objects))
            def solid(x, y, z):
                return doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or walls.blocked(x, y, z) or \
                    parked.blocked(x, y, z) is not None
            def over(x, y, z):
                return None if doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or parked.blocked(x, y, z) else 0
            def cost(x, y, z):
                return bool(doors.scripted_at(x, y, z)) * SCRIPTED_COST
            feet_floor = (standing, solid, over, cost, walls)
        return feet_floor

    def walk(origin, inside, point):
        standing, solid, over, cost, _ = walk_setup()
        leg = retail_route(standing, origin, point, inside, blocked=solid, cost=cost, tree=tree, over=over,
                           require_exit=True, minimum=1) or \
            retail_route(standing, origin, point, inside, blocked=solid, cost=cost, tree=tree, over=over, minimum=1)
        return leg[2] if leg else None

    def vehicle_dims(name):
        d = table[name]
        low, high = d['low'], d['high']
        half_length, half_width = (high[0] - low[0]) / 2 * scale, (high[1] - low[1]) / 2 * scale
        height = (high[2] - max(0.0, low[2])) * scale
        return d, half_length, half_width, height

    rows, cells = [], []
    def add(x, y, z, flags=0, name=''):
        rows.append(f'{round(x)} {round(y)} {z:.2f}' + (f' {flags}' if flags or name else '') + (f' {name}' if name else '') + '\n')
        cell = Cell((round(x), round(y)), round(z * 4))
        # In the air: a flight row (or the hover before getting out), not a place to stand.
        cell.air = bool(flags & (ROUTE_VEHICLE | ROUTE_GET_OUT)) and name == 'banshee'
        cells.append(cell)

    here = projection.point(start)           # the player's feet
    vehicle = None                            # (type, campaign point of the vehicle) once known
    landing = None                            # the last flight's landing cell (feet)
    ended = False
    goal = target_of(None)[1]
    drive_seconds, drive_jumps, drive_gravity, drive_speed, drive_top = 0.0, 0, None, None, None
    drive_governors = []                      # [x, y, z, radius, speed] per governed take-off
    for number, leg in enumerate(legs):
        kind, target = leg
        last = number == len(legs) - 1
        if kind == 'walk':
            if target == 'landing':
                inside, point = (lambda x, y, z4, l=landing: math.hypot(x - l[0], y - l[1]) <= 1.5 and abs(z4 / 4 - l[2]) < 2), landing
            elif isinstance(target, tuple) and target[0] == 'vehicle':
                tag, pose = objects[target[1]]
                name = next(n for n in table if table[n]['index'] == vehicle_type(tag.name))
                d = table[name]
                driver = next((v for k, v in d['seats'].items() if 'driver' in k.lower()), None)
                enter = driver[1] if driver else (d['low'][0] - .5, 0.0, 0.0)
                yaw = pose[3]
                entry = (pose[0] + enter[0]*math.cos(yaw) - enter[1]*math.sin(yaw),
                         pose[1] + enter[0]*math.sin(yaw) + enter[1]*math.cos(yaw), pose[2])
                vehicle = (name, projection.point(pose[:3]))
                point = projection.point(entry)
                # The vehicle's box (VehicleBlocks) keeps the walk a body's width off the entry point.
                inside = (lambda x, y, z4, p=point, r=(1.1 + .3) + 2.5: math.hypot(x - p[0], y - p[1]) <= r and abs(z4 / 4 - p[2]) < 3 * scale)
            else:
                inside, point = target_of(target)
            path = walk(here, inside, point)
            if not path:
                say(f'{key}: WARNING vehicle route: no walk to {target}', flush=True)
                break
            arrived = inside(path[-1][0], path[-1][1], path[-1].z)
            if not arrived:
                say(f'{key}: WARNING vehicle route: the walk to {target} stops short', flush=True)
            for previous, c in zip([None] + path[:-1], path):
                if cells and previous is None and math.hypot(c[0] - cells[-1][0], c[1] - cells[-1][1]) < .5:
                    continue
                jumped = previous is not None and (c.z - previous.z > 5 or abs(c[0] - previous[0]) + abs(c[1] - previous[1]) > 1)
                add(c[0], c[1], c.z / 4, 2 if jumped else 0)
            here = (path[-1][0], path[-1][1], path[-1].z / 4)
            if last:
                ended = arrived
            if not arrived:
                break
        elif kind == 'board':
            add(here[0], here[1], here[2], ROUTE_BOARD, target)
            if vehicle is None or vehicle[0] != target:
                # The engine's own (c40 section 4's Banshee beside the player): it starts where they stand.
                vehicle = (target, here)
        elif kind == 'fly':
            name = vehicle[0] if vehicle else 'banshee'
            d, half_length, half_width, height = vehicle_dims(name)
            # The engine's flight capsules (flightBlocked) and a margin.
            radius = max(.3, min(half_width, half_length) * .9) + FLIGHT_MARGIN
            body = max(2 * radius - 2 * FLIGHT_MARGIN, height * .9) + .5
            own = bsp_box(halo, bsp, projection, 0)
            def open_at(x, y, z, own=own):
                # Inside a BSP the section holds, its open space; beyond them all, nothing collides.
                held = [own] + [(a, b) for a, b, _ in extras]
                if any(a[0] <= x <= b[0] and a[1] <= y <= b[1] and a[2] <= z <= b[2] for a, b in held):
                    return tree.is_open(x, y, z)
                return True
            flight = FlightSpace(open_at, SightLines(walking, projection), FLIGHT_CELL, radius, body)
            inside, point = target_of(target)
            # Where to set down: under open sky (a room in reach on foot may hold the body but not let it
            # in), with room for the vehicle hovering low over it, and the walk from it to the target
            # shortest: the walking planner run from the target out to the first such cell. Failing
            # that, the one nearest the target (the walk leg after it then reports how it stops short).
            standing, solid, over, cost, _ = walk_setup()
            hover = (.25, .6, 1.0)
            def sky(n):
                return all(open_at(n[0], n[1], n[2] / 4 + k * scale) for k in (2, 4, 7, 10))
            room = {}
            def pad_ok(x, y, z4):
                if (x, y, z4) not in room:
                    room[x, y, z4] = next((h for h in hover if sky((x, y, z4)) and
                                           flight.body_free((x, y, z4 / 4 + h * scale))), None) if not solid(x, y, z4) else None
                return room[x, y, z4] is not None
            out = retail_route(standing, point, here, pad_ok, blocked=solid, cost=cost, tree=tree, over=over,
                               require_exit=True, minimum=1)
            pad = None
            if out:
                n = out[2][-1]
                pad = ((n[0], n[1], n.z), room[n[0], n[1], n.z])
            else:
                near = sorted(((x, y, z) for (x, y), levels in standing.items() for z in levels
                               if math.hypot(x - point[0], y - point[1]) < 40 * scale and abs(z / 4 - point[2]) < 2.5 * scale),
                              key=lambda n: math.dist((n[0], n[1], n[2] / 4), point))
                pad = next(((n, room[n]) for n in near[:20000] if pad_ok(*n)), None)
                if pad:
                    say(f'{key}: WARNING vehicle route: no walk from {target} out to open sky; landing at the nearest '
                        f'open spot, {math.dist((pad[0][0], pad[0][1], pad[0][2] / 4), point) / scale:.1f} units away', flush=True)
            if pad is None:
                say(f'{key}: WARNING vehicle route: nowhere to land by {target}', flush=True)
                break
            (lx, ly, lz), h = pad
            hover_point = (lx, ly, lz / 4 + h * scale)
            source = vehicle[1] if vehicle else here
            def over_pad(p, reach=FLIGHT_CELL * .75):
                return math.hypot(p[0] - lx, p[1] - ly) <= reach and lz / 4 <= p[2] <= lz / 4 + 1.3 * scale
            # A flight ends on a grid node over the pad. Where no free node is that close (c40 section 1's
            # landing on the c bridge, between its railings), a node within one and a half cells from
            # which the body descends in a straight line to the hover point will do: the get-out row
            # below is at the hover point itself.
            near_nodes = [(nx, ny, nz) for nx in range(math.floor((lx - FLIGHT_CELL) / FLIGHT_CELL), math.ceil((lx + FLIGHT_CELL) / FLIGHT_CELL) + 1)
                          for ny in range(math.floor((ly - FLIGHT_CELL) / FLIGHT_CELL), math.ceil((ly + FLIGHT_CELL) / FLIGHT_CELL) + 1)
                          for nz in range(math.floor(lz / 4 / FLIGHT_CELL), math.ceil((lz / 4 + 1.3 * scale) / FLIGHT_CELL) + 1)]
            if any(over_pad(flight.point(n)) and flight.free(n) for n in near_nodes):
                inside_pad = over_pad
            else:
                inside_pad = lambda p: over_pad(p, FLIGHT_CELL * 1.5) and flight.clear(p, hover_point)
                print(f'{key}: no flight node over the pad by {target}; ending within {FLIGHT_CELL * 1.5:.0f} units '
                      f'in sight of the hover point', flush=True)
            path, arrived = flight.route(source, hover_point, inside_pad)
            print(f'{key}: flight to {target}: {len(path)} points, {flight.explored} nodes explored, landing at '
                  f'{lx},{ly},{lz / 4:.2f} (hover {h:.2f} Halo units)' +
                  ('' if arrived else f', did not arrive: ends at {", ".join(f"{v:.1f}" for v in path[-1])}'), flush=True)
            if not arrived:
                say(f'{key}: WARNING vehicle route: no flight to {target}', flush=True)
            # Resample the straightened flight every two cells for the walker.
            points = []
            for a, b in zip(path, path[1:]):
                # At most 2 * FLIGHT_CELL - 1 apart, so the rows stay within two cells and a half of
                # each other once rounded to whole cells (tests/test_traversal_routes.py); rounding the
                # count down left legs of 8 to 16 units as one long row (final integration review).
                steps = max(1, math.ceil(math.dist(a, b) / (2 * FLIGHT_CELL - 1)))
                points += [tuple(a[i] + (b[i] - a[i]) * k / steps for i in range(3)) for k in range(1, steps + 1)]
            for p in points:
                add(p[0], p[1], p[2], ROUTE_VEHICLE, name)
            if not arrived:
                break
            add(hover_point[0], hover_point[1], hover_point[2], ROUTE_GET_OUT, name)
            vehicle = (name, hover_point)
            landing = (lx, ly, lz / 4)
            here = landing
            add(lx, ly, lz / 4)
        elif kind == 'drive':
            name = vehicle[0] if vehicle else 'warthog'
            d, half_length, half_width, height = vehicle_dims(name)
            step = min(1.3, max(.3, height * .3))
            chassis = half_width * .95
            ground = VehicleGround(walking, tree, size, chassis + VEHICLE_MARGIN, max(2 * chassis, height - step) + step, step,
                                   half_length, VEHICLE_SLOPE)
            first = ground.nearest(here)
            if first is None:
                say(f'{key}: WARNING vehicle route: no floor for the {name} at the start', flush=True)
                break
            inside, point = target_of(target)
            # The engine's car: its top speed (at least the Maw's 36 units a second), most of it at a
            # jump (the speed after a run-up), and its retail-derived gravity (vehicle_gravity). Where
            # no drive arrives at that speed, slower cruising speeds (DRIVE_SPEEDS): a jump that only
            # works slower (d40 section 8's drop under the pit room's hanging bridge segment) is
            # driven at the leg's speed, recorded in the metrics (`drive.speed`) for the tests'
            # cruise cap (qa_vehicle_cruise) and for the player to keep to.
            top = vehicle_top_speed(d, scale)
            gravity = vehicle_gravity(d, scale)
            for fraction in DRIVE_SPEEDS:
                speed = fraction * top
                path, arrived = ground.route(first, inside, point, drop=VEHICLE_DROP * scale, speed=speed, gravity=gravity,
                                             jump_drop=VEHICLE_JUMP_DROP * scale)
                if arrived:
                    break
                if fraction == DRIVE_SPEEDS[0]:
                    fallback = (speed, path)
            else:
                speed, path = fallback
            if arrived and speed < DRIVE_SPEEDS[0] * top - 1e-6:
                print(f'{key}: the drive to {target or exit_name} arrives only at {speed:.1f} units a second '
                      f'({speed / top:.2f} of top speed): its jumps need the slower take-off', flush=True)
            drive_speed = speed if drive_speed is None else min(drive_speed, speed)
            drive_top = top
            jumps = sum(1 for a, b in zip(path, path[1:]) if math.hypot(b[0] - a[0], b[1] - a[1]) > 1.5)
            # How long the drive takes at that speed: the cells' length, and each jump's flight (the
            # escape countdown of d40's run, 180 s for sections 6 to 8, is checked against it).
            seconds = sum(math.dist((a[0], a[1], a[2] / 4), (b[0], b[1], b[2] / 4)) if math.hypot(b[0] - a[0], b[1] - a[1]) <= 1.5
                          else math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(path, path[1:])) / speed
            drive_seconds += seconds
            drive_jumps += jumps
            drive_gravity = gravity
            if arrived and speed < DRIVE_SPEEDS[0] * top - 1e-6:
                # The take-off of every jump of a leg that only arrives slower (GOVERNOR_SECONDS).
                for a, b in zip(path, path[1:]):
                    if math.hypot(b[0] - a[0], b[1] - a[1]) > 1.5:
                        drive_governors.append([a[0], a[1], round(a[2] / 4, 2),
                                                round(max(GOVERNOR_RADIUS, speed * GOVERNOR_SECONDS), 2), round(speed, 2)])
            print(f'{key}: drive to {target or exit_name}: {len(path)} cells, {len(ground.samples)} floor columns, '
                  f'{jumps} jumps, gravity {gravity:.1f}, about {seconds:.0f} s at {speed:.1f} units a second'
                  f'{"" if arrived else ", did not arrive"}', flush=True)
            for previous, n in zip([None] + path[:-1], path):
                jumped = previous is not None and math.hypot(n[0] - previous[0], n[1] - previous[1]) > 1.5
                add(n[0], n[1], n[2] / 4, ROUTE_VEHICLE | (2 if jumped else 0), name)
            if path:
                here = (path[-1][0], path[-1][1], path[-1][2] / 4)
            if last:
                ended = arrived
            if not arrived:
                say(f'{key}: WARNING vehicle route: the drive stops {math.dist(here, point) / scale:.1f} units short of '
                    f'{target or exit_name}', flush=True)
                break
    drive = {'jumps': drive_jumps, 'seconds': round(drive_seconds, 1), 'gravity': round(drive_gravity, 2),
             'speed': round(drive_speed, 2), 'top': round(drive_top, 2),
             'governors': drive_governors} if drive_gravity is not None else None
    return rows, cells, ended, goal, len(extras), drive


def make_section(halo, mission, part, output, collision='validated', metrics_only=False, key=None):
    # `key`: the section's key in the tables, (cache, part) except for the Crash Site (CRASH_SITE_KEY).
    key = key or (mission.cache, part)
    # Warnings the route planner prints (a route that ends outside its exit, an objective it cannot
    # reach) also go into the section's metrics.
    notes = []
    # The planned frag throws of a ThrowTarget (throw_plan), for the metrics' `throws`.
    planned_throws = []
    def say(text, **_):
        print(text, flush=True)
        if 'WARNING' in text or 'no route to' in text:
            notes.append(text)
    write = not metrics_only
    weapons = weapon_index(output)
    if not weapons:
        raise ValueError('run tools/halo_weapons.py first to write packages/halo/weapons.txt')
    bsp, indoor, (xmin, xmax, ymin, ymax), start, end, actors, factor, projection = section_layout(halo, mission, part, key)
    size, scale = 1 << factor, projection.scale
    projected_start, projected_end = projection.point(start), projection.point(end)
    samples, ceilings = rasterize(halo, bsp, projection, size)
    retail = collision == 'all' or (collision == 'validated' and mission.cache in VALIDATED_COLLISION and
                                    key not in STOCK_SECTIONS)
    # Driven sections keep the raster's route and spawns (the body does not fit their retail
    # floors) but get the .hcol the vehicle drives on; b30's first two sections keep their authored
    # raster layout (RASTER_ROUTES).
    driven = retail and key in DRIVEN_SECTIONS
    walked = retail and key not in RASTER_ROUTES and not driven
    projected, bridges, doors, test = None, None, None, None
    assists = []
    scenery_polygons, scenery_baked = [], []
    section_placements = list(section_vehicle_placements(halo, bsp, (xmin, xmax, ymin, ymax)))
    # Parked vehicles (turrets, Ghosts, Wraiths, Banshees) stand in the walk's way: the walked
    # route, and the actors and items placed beside it, go round them (VehicleBlocks).
    parked = None
    if walked:
        from import_halo_scripts import named_objects, Program
        parked = VehicleBlocks(projection, section_placements, vehicle_bounds(output),
                               [name for name, _, _ in named_objects(Program(halo))])
        if parked.names:
            print(f'{mission.cache} section {part+1}: route avoids parked vehicles {", ".join(parked.names)}',
                  flush=True)
    crash_site = key == CRASH_SITE_KEY
    scenery = crash_site_scenery(halo, projection) if crash_site else []
    # With their real collision in the .hcol (SCENERY_COLLISION), the route goes round and over their
    # polygons, not the mapmodels' cylinders.
    trees = SceneryBlocks(scenery) if crash_site and walked and key not in SCENERY_COLLISION else None
    if crash_site and write:
        problems = crash_site_cfg_problems(output)
        if problems:
            raise ValueError(f'{CRASH_SITE_NAME}.cfg does not match CRASH_SITE_MAPMODELS: ' + '; '.join(problems))
    def parked_at(x, y, z4):
        return (parked is not None and parked.blocked(x, y, z4) is not None) or (trees is not None and trees.blocked(x, y, z4))
    if retail:
        # The engine walks the original collision surfaces. Plan the route, spawns and
        # markers on floors the player can stand on, not on render geometry. The routes,
        # checkpoints and QA waypoints of b30's first two sections were authored against the render
        # raster, so they keep that layout and only gain their .hcol (RASTER_ROUTES).
        surfaces, tree = structure_surfaces(halo, bsp, projection)
        machines, baked = static_device_surfaces(halo, bsp, projection, output,
                                                 CINEMATIC_STILL.get(mission.cache, ()))
        # Scenery collision (SCENERY_COLLISION): its polygons are walked, seen and thrown at like the
        # static machines' (`fixed`), and written to the .hcol's SCN1 trailer. With the scenery of the
        # EXTRA_BSPS the section holds (c40 section 1's BSP 12, the d40 section 3 batch), in the same
        # map window; less what a script creates or destroys during play by name or name pattern
        # (SCENERY_SCRIPTS_AT_END, SCENERY_KEPT).
        if key in SCENERY_COLLISION:
            scenery_polygons, scenery_baked, scenery_left = scenery_surfaces(
                halo, bsp, projection, window=(xmin - 8, xmax + 8, ymin - 8, ymax + 8),
                extra_bsps=EXTRA_BSPS.get(key, ()), at_end=SCENERY_SCRIPTS_AT_END.get(mission.cache, ()),
                kept=SCENERY_KEPT.get(key, ()))
            print(f'{mission.cache} section {part+1}: {len(scenery_baked)} scenery placements baked '
                  f'({len(scenery_polygons)} surfaces)' + (', left out: ' + ', '.join(
                      f'{count} {why}' for why, count in sorted(scenery_left.items())) if scenery_left else ''), flush=True)
        fixed = machines + scenery_polygons
        # Authored walking assists (ASSISTS): floor for the walk and the engine's bodies, never for
        # sight lines or grenades (SightLines, GrenadeArcs), as there is nothing there in Halo.
        assists = assist_surfaces(ASSISTS.get(key, ()), projection) if walked else []
        if assists:
            print(f'{mission.cache} section {part+1}: {len(assists)} walking assists (ASSISTS, not retail): ' +
                  '; '.join(a.why for a in ASSISTS[key]), flush=True)
        walking = surfaces + fixed + assists
        if walked:
            standing = floor_samples(walking, tree, size)
            low = set()
            crouching = floor_samples(walking, tree, size, crouch=low)
            # Two-sided sheets (the invisible ramps laid over stairs, grates) are not solid in
            # the BSP's open-space test but the engine collides with them. Drop the floors
            # they cover: the stair steps under a ramp are walked on the ramp itself.
            sheets = PolygonClearance([q for q in walking if q[1] & 1], minimum_normal=2,
                                      radius=1.1, bottom=1.45)
            # A walking assist's own floor is kept: on one steeper than 40 degrees the body's lowest
            # test point is within its radius of the sheet it stands on.
            own = floor_samples(assists, tree, size) if assists else {}
            for samples_ in (standing, crouching):
                for xy in list(samples_):
                    kept = [z for z in samples_[xy] if z in own.get(xy, ()) or not sheets.blocked(xy[0], xy[1], z)]
                    if kept:
                        samples_[xy] = kept
                    else:
                        del samples_[xy]
            low -= {(x, y, z) for (x, y), levels in standing.items() for z in levels}
            # The BSP open-space test samples a ring of points; thin posts and railings can sit
            # between them. Test the body against the real wall polygons as well.
            walls = PolygonClearance(walking, radius=1.25, bottom=1.26 + 1.1)
            # Light bridges (c10) are floor once extended: the route may cross one, waiting at
            # its end for a script to extend it or using the control there (LightBridges).
            bridges = LightBridges(halo, projection, output, SCRIPTED_COST, bsp)
            bridgeless = {xy: list(levels) for xy, levels in crouching.items()}
            bridges.add_floors(crouching, tree, size, walls, UNCROSSED_BRIDGES.get(key, ()))
            exit_name = exit_target(mission, part, key)
            box = volume_box(halo, exit_name) if isinstance(exit_name, str) else None
            test = (volume_test(box, projection) if box else
                    None if isinstance(exit_name, str) else objective_test(halo, exit_name, projection))
            doors = DoorBlocks(halo, projection, output, list(baked) + [b['device'] for b in bridges.bridges],
                               NEVER_OPEN.get(mission.cache, ()), mission.cache in UNGROUPED_DOORS_SEALED,
                               mission.cache in FLOOR_LEAVES)
            # Prefer a route that needs no script to open a door; fall back to waiting at one.
            # Open doors still collide with their panels in the open pose.
            def solid(x, y, z):
                return doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or walls.blocked(x, y, z) or \
                    parked_at(x, y, z)
            def solid_low(x, y, z):
                return doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or walls.blocked(x, y, z, True) or \
                    parked_at(x, y, z)
            def one_sided(here, node):
                # A one-sided door entered from its front stays shut until a script makes it
                # two-sided or opens it (a50's muster bay doors, once the wave is beaten): wait
                # for that like a scripted door. One no script frees is a wall from that side.
                door = doors.one_sided_crossing(here, node)
                toll = 0 if door is None else SCRIPTED_COST if door[1] else None
                # Stepping onto a light bridge that has to be extended first is a wait; one
                # whose controls are all elsewhere (the far side) cannot be extended from here.
                extended = bridges.entry(here, node)
                return None if toll is None or extended is None else toll + extended
            def over_door(x, y, z):
                # A jump may not pass through a sealed door or an open door's panels (a50's
                # gravity_door_lr), nor through a parked vehicle. A scripted door jumped over stays
                # free, as it always was: a10's cryo jump tutorial lands through
                # cryo_explosion_door_3 once it is open.
                return None if doors.sealed_at(x, y, z) or doors.opened_at(x, y, z) or parked_at(x, y, z) else 0
            def crawl(x, y, z):
                return ((x, y, z) in low or walls.blocked(x, y, z)) * CROUCH_COST + \
                       bool(doors.scripted_at(x, y, z)) * SCRIPTED_COST
            # Walk upright where the section allows it. Some passages (a10's jammed bulkheads)
            # are only passable crouching, and some obstacles only by jumping (the cryo bay's
            # jump tutorial); the route marks those steps for the walker. A scripted door to
            # wait at costs far more than any detour, crouch or jump.
            # Objective volumes the mission script waits for the player to enter before it opens
            # the way on (a10's bridge, where Keyes's scene plays) are walked through in order.
            legs, actions, extra = [], [], []
            # Machines in a pose a step leaves them in (ShootTarget's opened piston), for sight lines.
            sight_cache = {}
            def sights_for(device, value):
                if (device, value) not in sight_cache:
                    sight_cache[device, value] = SightLines(surfaces + fixed, projection,
                                                            machine_boxes(halo, output, bsp, {device: value}, baked))
                return sight_cache[device, value]
            def pose_blocker(device, value):
                # A body may not stand in a machine's boxes at that pose (the opened piston's).
                boxes = [entry for entry in machine_boxes(halo, output, bsp, {device: value}, baked) if entry[0] == device]
                radius, height = (1.1 + .3) / scale, 5.2 / scale
                def inside(x, y, z4):
                    rx, ry = (x - 9) / scale + projection.x0, projection.y0 - (y - 9) / scale
                    rz = projection.z0 + z4 / 4 / scale
                    for _, (ox, oy, oz, c, s_), (x1, y1, z1, x2, y2, z2) in boxes:
                        dx, dy, dz = rx - ox, ry - oy, rz - oz
                        lx, ly = dx*c + dy*s_, -dx*s_ + dy*c
                        if x1 - radius < lx < x2 + radius and y1 - radius < ly < y2 + radius and dz < z2 and dz + height > z1:
                            return True
                    return False
                return inside
            for name, flag in SECTION_OBJECTIVES.get(key, ()):
                if isinstance(name, ThrowTarget):
                    # The throwing spot depends on where the leg can walk: chosen when it is routed.
                    legs.append((None, projection.point(objective_point(halo, name)), None, False))
                    actions.append(('throw', name, None))
                    extra.append((name.opened, pose_blocker(name.opened, name.value)))
                    continue
                if isinstance(name, ShootTarget):
                    initial = [entry for entry in machine_boxes(halo, output, bsp, {}, baked) if entry[0] == name.opened]
                    stands = shoot_stands(halo, name, projection, crouching, low, sights_for(name.opened, name.value), initial)
                    print(f'{mission.cache} section {part+1}: {len(stands)} cells see {" or ".join(name.targets)} '
                          f'through {name.opened}', flush=True)
                    legs.append((lambda x, y, z, stands=stands: (x, y, z) in stands, projection.point(objective_point(halo, name)),
                                 None, False))
                    actions.append(('shoot', name, stands))
                    extra.append((name.opened, pose_blocker(name.opened, name.value)))
                    continue
                legs.append((objective_test(halo, name.point if isinstance(name, UseControl) else name, projection),
                             projection.point(objective_point(halo, name)),
                             flag and projection.point(cutscene_flag(halo, flag) if isinstance(flag, str) else flag),
                             flag is not None and not isinstance(flag, str)))
                actions.append(('use', name, None) if isinstance(name, UseControl) else None)
                extra.append(None)
            legs.append((test, projected_end, None, False))
            actions.append(None)
            extra.append(None)
            # Route steps the player reaches by riding a lift from the step before (b40's lift_d).
            rides = set()
            # Ledge drops (LEDGE_DROPS) in quarter units of the section's projection.
            drop = round(LEDGE_DROPS.get(key, 0) * scale * 4)
            route, origin, riding = None, projected_start, False
            names = [str(name) for name, _ in SECTION_OBJECTIVES.get(key, ())] + [exit_target(mission, part, key)]
            for number, (inside, target, teleport, lift) in enumerate(legs):
                last = number == len(legs) - 1
                # An objective may be a step or two on from the last (d20's section 2 starts inside
                # enc3_1): only the whole route needs 16 steps, as a section's single leg always has.
                minimum = 16 if number == 0 and last else 1
                # A ShootTarget leg walks with its opened machine moved: out of its first pose (not a
                # wall there any more) and into the one the step left it in.
                blocked_here = solid_low if extra[number] is None else \
                    (lambda x, y, z, moved=extra[number]: doors.sealed_except(x, y, z, moved[0]) or doors.opened_at(x, y, z) or
                     walls.blocked(x, y, z, True) or parked_at(x, y, z) or moved[1](x, y, z))
                if actions[number] and actions[number][0] == 'throw':
                    # A ThrowTarget: simulate frags from the cells this leg can walk to, then walk to
                    # the best throwing spot.
                    objective = actions[number][1]
                    explored = {}
                    retail_route(crouching, origin, target, None, low=low, blocked=blocked_here, cost=crawl, tree=tree,
                                 crossing=one_sided, over=over_door, drop=drop, minimum=1, fill=bridgeless, explored=explored, slide=key in SLIDES)
                    started = time.time()
                    arcs = GrenadeArcs(surfaces + fixed, projection,
                                       machine_boxes(halo, output, bsp, {objective.opened: objective.value}, baked))
                    search = {}
                    plan = throw_plan(halo, objective, projection, explored, low, arcs,
                                      sights_for(objective.opened, objective.value), frag_grenade(output), search)
                    print(f'{mission.cache} section {part+1}: {len(plan)} frag throws land by {" or ".join(objective.targets)} '
                          f'({time.time() - started:.0f} s; {search.get("stands", 0)} spots, {search.get("throws", 0)} throws '
                          f'simulated, {search.get("landed", 0)} landing, {search.get("too_close", 0)} of them too close to the '
                          f'thrower)' + (
                              f'; from {plan[0][0][0]},{plan[0][0][1]},{plan[0][0][2] / 4:.2f}: ' +
                              ', '.join(f'yaw {yaw:.0f} pitch {pitch:.0f} ({target} {damage:.0f}, {near} of 8 neighbours, '
                                        f'{margin:.2f} from the thrower)'
                                        for _, yaw, pitch, _, target, damage, near, margin in plan) if plan else ''), flush=True)
                    if not plan:
                        say(f'{mission.cache} section {part+1}: WARNING no simulated frag throw lands by {objective.targets[0]}: '
                            f'the walker cannot destroy {" or ".join(objective.targets)}', flush=True)
                        continue
                    if len(plan) < objective.least:
                        say(f'{mission.cache} section {part+1}: WARNING only {len(plan)} frag aims at {objective.targets[0]} '
                            f'(wanted {objective.least} to {objective.aims})', flush=True)
                    actions[number] = ('throw', objective, plan)
                    planned_throws.extend({'target': target, 'yaw': yaw, 'pitch': pitch, 'damage': round(damage, 1),
                                           'neighbours': near, 'thrower_distance': round(margin, 2),
                                           'blast_outer': frag_grenade(output)[3]}
                                          for _, yaw, pitch, _, target, damage, near, margin in plan)
                    inside = (lambda x, y, z, stand=plan[0][0]: (x, y, z) == stand)
                leg = retail_route(crouching, origin, target, inside, require_exit=True, low=low,
                                   blocked=blocked_here, cost=crawl, tree=tree, crossing=one_sided, over=over_door,
                                   drop=drop, minimum=minimum, fill=bridgeless, slide=key in SLIDES) or \
                    (retail_route(crouching, origin, target, inside, low=low, blocked=blocked_here, cost=crawl,
                                  crossing=one_sided, minimum=minimum, fill=bridgeless) if last else None)
                if leg is None and actions[number] and actions[number][0] in ('shoot', 'throw'):
                    # No standing spot in sight of the target can be walked to: the walk goes on
                    # without shooting it (the section then stays blocked where the script waits).
                    say(f'{mission.cache} section {part+1}: WARNING no route to {names[number]}: '
                          f'the walker cannot {actions[number][0]} {" or ".join(actions[number][1].targets)}', flush=True)
                    continue
                if leg is None:
                    say(f'{mission.cache} section {part+1}: no route to {names[number]}', flush=True)
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
                if actions[number]:
                    # The walker acts at the leg's last step: uses the control, or shoots the target.
                    kind, objective, stands = actions[number]
                    if kind == 'use':
                        step = ('use', f'{objective.device}@{objective.value:.2f}', None)
                    elif kind == 'throw':
                        if (end_cell[0], end_cell[1], end_cell.z) != stands[0][0]:
                            say(f'{mission.cache} section {part+1}: WARNING the route stops off the throwing spot for {objective}', flush=True)
                        step = tuple(('throw', f'{target}@{objective.health:.2f}', aim) for _, _, _, aim, target, *_ in stands)
                    else:
                        found = stands.get((end_cell[0], end_cell[1], end_cell.z))
                        if found is None:
                            say(f'{mission.cache} section {part+1}: WARNING the route stops out of sight of {objective}', flush=True)
                            found = (objective.targets[0], projection.point(objective_point(halo, objective)))
                        step = ('shoot', f'{found[0]}@{objective.health:.2f}', found[1])
                    end_cell.actions = getattr(end_cell, 'actions', ()) + (step if kind == 'throw' else (step,))
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
                    say(f'{mission.cache} section {part+1}: ' + (
                        f'route ends at the edge above its exit {exit_name} (EXIT_FALLS)'
                        if key in EXIT_FALLS else
                        f'WARNING route ends outside its exit {exit_name}'), flush=True)
    if projected is None and not walked:
        projected = projected_route(samples, projected_start, projected_end,
                                    [projection.point(p) for p,_ in actors])
    if projected is None:
        raise ValueError(f'no continuous walkable layer at {start}')
    _, floor, parents, path = projected
    # Driven and flown sections with a planned vehicle route (VEHICLE_ROUTES): its rows are the
    # section's route file and its cells the route markers, insertion and checkpoint. The raster's
    # floor still holds the native cubes, the actors and the items.
    vehicle_plan = None
    if driven and key in VEHICLE_ROUTES:
        vehicle_plan = plan_vehicle_route(halo, mission, part, key, projection, size, bsp, surfaces, tree, fixed, baked,
                                          start, end, output, section_placements, say)
        if vehicle_plan is None:
            say(f'{mission.cache} section {part+1}: WARNING no vehicle route (run tools/halo_vehicles.py first)', flush=True)
        elif len(vehicle_plan[1]) >= 2:
            path = vehicle_plan[1]
    # The cube-raster stamp (scenery_raster): the baked scenery goes into the native cube raster too,
    # which the engine's flood waypoints, AssaultCube's sight checks on stock geometry and the stock
    # fallback read (the .hcol, the planner and the route are as they were). Tall scenery makes its
    # cubes solid, a low walkable top raises the floor onto it. The cubes the route walks, the vehicles
    # stand in and the Crash Site's mapmodels are drawn on keep their floor (the engine draws a mapmodel
    # at its cube's floor); actors, weapons and equipment are placed off the solid cubes below. A
    # mapmodel's cube the stamp would make solid (a tree's trunk) is capped instead: open space one unit
    # high over its floor, so the mapmodel stands where it did and no waypoint goes in it (the flood
    # needs the body's radius between floor and ceiling) and bodies on stock geometry do not fit.
    raster_solid, raster_raised, raster_kept, raster_capped = set(), {}, {}, set()
    if scenery_polygons:
        walked_cubes = {(p[0], p[1]) for p in path}
        if retail:
            walked_cubes |= {(round(projection.point(point)[0]), round(projection.point(point)[1]))
                             for _, _, _, point, _ in section_placements}
        modelled_cubes = {(round(x), round(y)) for _, (x, y, _), *_ in scenery} if crash_site else set()
        raster_solid, raster_raised, raster_kept = scenery_raster(scenery_polygons, scenery_baked, floor,
                                                                  walked_cubes | modelled_cubes)
        raster_capped = {cube for cube, level in raster_kept.items()
                         if level is None and cube in modelled_cubes and cube not in walked_cubes}
        floor = {**floor, **raster_raised}
    body = None
    if walked:
        # Spawns, markers and items need room for a body, not only floor: a shelf at chest
        # height can sit between the route planner's clearance samples. Nudge each one along
        # the route to the nearest step with clearance from every wall, shelf and machine.
        body = PolygonClearance(walking, radius=1.15, bottom=1.26 + 1.1)
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
    if vehicle_plan:
        # The checkpoint marker stands on a floor, not in the middle of a flight.
        halfway = min((i for i, p in enumerate(path) if not getattr(p, 'air', False)), key=lambda i: abs(i - halfway),
                      default=halfway)
    entries = [entity(xyz(path[0]), PLAYERSTART, facing(path[0],path[min(5,len(path)-1)]), 1),
               entity(xyz(path[halfway]), PLAYERSTART, facing(path[halfway],path[min(halfway+5,len(path)-1)]),1),
               entity(xyz(path[-1]), PLAYERSTART, 0,100)]
    for i in range(10, len(path)-4, 11):
        phase = 1 if i < len(path)//4 else 2 if i < halfway-3 else 3
        entries.append(entity(xyz(path[i]), PLAYERSTART,0,100,phase))
    used = []
    def off_scenery(cells):
        # The cube-raster stamp: off its solid cubes, unless every cell in reach is one (the placement's
        # cube is then opened again below, as the route's are).
        return [p for p in cells if p not in raster_solid] or cells
    # Native actor spawns: two encounters, each the actors (at most four) whose retail position is
    # within 55 units (2D) of an anchor on the route, placed on the nearest reachable raster cell
    # within 35 of it; the anchors stand a quarter and three quarters along the route.
    spots = []
    for p, actor in actors:
        point = projection.point(p)
        # Projection from an authored actor onto a nearby reachable cell.
        close = [(x,y) for x in range(round(point[0])-6,round(point[0])+7)
                       for y in range(round(point[1])-6,round(point[1])+7)
                 if (x,y) in parents and not parked_at(x, y, floor[x,y])]
        close = off_scenery(close)
        if not close:
            continue
        place = closest(close,floor,point)
        if abs(floor[place]/4-point[2]) > 8:
            continue
        spots.append((point, place, actor, p[3]))
    def anchored(anchor):
        return sorted((math.dist(place,anchor),place,actor,angle) for point, place, actor, angle in spots
                      if math.dist(point[:2],anchor) <= 55 and math.dist(place,anchor) <= 35 and
                      not any(math.dist(place,u) < 5 for u in used))
    first_anchor = -1
    for wave, at in enumerate((len(path)//4, 3*len(path)//4)):
        eligible = anchored(path[at])
        if not eligible and vehicle_plan and key not in FLOWN_SECTIONS:
            # A driven section's anchor that finds nobody (d40 section 8's, after its drive grew to
            # reach grand_finale) moves to the route cell nearest it from which actors not yet placed
            # are in reach: the first encounter's within the route's first half, the second's past
            # the first's anchor, so the encounters keep their order along the drive. The planned
            # drive goes past the retail encounters, not through the raster's quarter points.
            steps = range(0, len(path)//2) if wave == 0 else range(first_anchor + 1, len(path))
            for i in sorted(steps, key=lambda i: (abs(i - at), i)):
                eligible = anchored(path[i])
                if eligible:
                    print(f'{mission.cache} section {part+1}: encounter {wave+1} anchored at route step {i} '
                          f'({path[i][0]},{path[i][1]}), not {at}: no actor in reach there', flush=True)
                    at = i
                    break
        if wave == 0:
            first_anchor = at
        for _, place, (kind, weapon), angle in eligible:
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
                       for y in range(round(projected_point[1])-4,round(projected_point[1])+5)
                       if (x,y) in parents and not parked_at(x, y, floor[x,y])]
        close = off_scenery(close)
        if not close:
            continue
        place = closest(close,floor,projected_point)
        if abs(floor[place]/4-projected_point[2]) > 8:
            continue
        entries.append(entity(xyz(place),PLAYERSTART,ac_yaw(yaw),WEAPON_PLACEMENT,0,0,weapons[weapon]+1))
        placed += 1
    # Scenario equipment on the route's floor, as the weapons are. Script-created pieces (flag 1)
    # wait in the engine for the script's object_create.
    equipment, items = equipment_index(output), 0
    for tag, name_index, not_placed, point, yaw in equipment_placements(halo):
        if tag not in equipment or not (xmin <= point[0] <= xmax and ymin <= point[1] <= ymax):
            continue
        projected_point = projection.point(point)
        close = [(x,y) for x in range(round(projected_point[0])-4,round(projected_point[0])+5)
                       for y in range(round(projected_point[1])-4,round(projected_point[1])+5)
                       if (x,y) in parents and not parked_at(x, y, floor[x,y])]
        close = off_scenery(close)
        if not close:
            continue
        place = closest(close,floor,projected_point)
        if abs(floor[place]/4-projected_point[2]) > 8:
            continue
        entries.append(entity(xyz(place),PLAYERSTART,ac_yaw(yaw),EQUIPMENT_PLACEMENT,equipment[tag],not_placed & 15,
                              name_index + 1 if name_index >= 0 else 0))
        items += 1
    if not items:
        # Without retail equipment in reach (or before tools/halo_weapons.py wrote equipment.txt),
        # the old ammunition, health and grenade boxes at the route's start and middle.
        for i in (0,halfway):
            for kind in (AMMO, HEALTH, GRENADE):
                spot = path[roomy(min(len(path)-1,i+kind*2))]
                entries.append(entity(xyz(spot),kind,0))
    # Scenario vehicles inside this section's BSP. On retail collision they stand at their retail
    # point (the engine settles them onto the floor); on stock geometry on the nearest raster
    # floor. Script-created ones (flag 1) wait in the engine for the script's object_create.
    vehicles = 0
    for kind, name_index, not_placed, point, yaw in section_placements:
        projected_point = projection.point(point)
        if retail:
            spot = (round(projected_point[0]), round(projected_point[1]), projected_point[2])
        else:
            close = [(x,y) for x in range(round(projected_point[0])-6,round(projected_point[0])+7)
                           for y in range(round(projected_point[1])-6,round(projected_point[1])+7) if (x,y) in floor]
            if not close:
                continue
            place = closest(close,floor,projected_point)
            if abs(floor[place]/4-projected_point[2]) > 8:
                continue
            spot = (round(projected_point[0]), round(projected_point[1]), floor[place]/4)
        x, y, z = spot
        # attr7 carries the section's scale (x20) for the engine, which sizes the vehicle by it
        # even when the section runs without its mission script (a section launched on its own).
        entries.append(entity((x, y, math.ceil(z)), PLAYERSTART, ac_yaw(yaw), VEHICLE_PLACEMENT, kind,
                              not_placed & 15, name_index + 1 if name_index >= 0 else 0, 0,
                              min(255, max(1, round(scale * 20)))))
        vehicles += 1
    for p in (path[0],path[halfway],path[-1]):
        x,y,z = xyz(p)
        entries.append(entity((x,y,z+5),LIGHT,19,165,170,182))
    name = CRASH_SITE_NAME if crash_site else f'halo_{mission.cache}_{part+1:02}'
    # Scenery placements a mapmodel draws (the Crash Site's); the rest of the baked scenery is drawn in the mesh.
    as_models = set()
    if crash_site:
        # The scenery models (trees, boulders, the lifepod) on the native cube floor under each: the
        # engine stands a mapmodel at the cube floor plus its elevation (attr3 / 5; the cfg's z offset
        # is 0). One beside no cube floor, or more than 51 units above it, is left out.
        modelled = 0
        for slot, (x, y, z), yaw, pitch, roll, index in scenery:
            cell = (round(x), round(y))
            if cell not in floor or any((cell[0] + dx, cell[1] + dy) not in floor for dx, dy in ((1, 0), (0, 1), (1, 1))):
                continue
            elevation = round((z - floor[cell] // 4) * 5)
            if not -5 <= elevation <= 255:
                continue
            # Turned as Halo turns it (mapmodel_angles), as its collision is (scenery_surfaces).
            attr1, attr5, attr6 = mapmodel_angles(yaw, pitch, roll)
            entries.append(entity((cell[0], cell[1], z), MAPMODEL, attr1, slot, max(0, elevation), 0, attr5, attr6))
            modelled += 1
            as_models.add(index)
        print(f'{name}: {modelled} of {len(scenery)} scenery models placed', flush=True)
        if key in SCENERY_COLLISION:
            # Mapmodels whose placement is not in the .hcol (another BSP's, or one a script handles)
            # draw without colliding: halo_a30.cfg's zero heights turn every cylinder off.
            loose = as_models - {index for index, _ in scenery_baked}
            if loose:
                print(f'{name}: {len(loose)} mapmodels draw scenery with no collision (not baked: '
                      f'{", ".join(map(str, sorted(loose)))})', flush=True)
    output = output / 'packages'
    maps = output / 'maps'
    maps.mkdir(parents=True, exist_ok=True)
    title = f'{mission.title} | The Crash Site' if crash_site else f'{mission.title} | Section {part+1}'
    # The route's and the placed entities' cubes are open space even where a corner of the square has no
    # floor (beside a wall, on another storey): nothing the route walks or the map places stands in a
    # solid cube of the raster (the cube-raster stamp's static check, below and in the metrics' `raster`).
    # An actor, weapon or equipment with nothing but scenery cubes in reach (b40 section 3's script-created
    # items under a snow pine's low boughs) keeps its cell, and the stamp gives that cube back.
    raster_reopened = {(e[0], e[1]) for e in entries} & raster_solid
    raster_solid -= raster_reopened
    opened = {(p[0], p[1]) for p in path} | {(e[0], e[1]) for e in entries}
    opened = {cube for cube in opened if cube in floor and cube not in raster_solid}
    cgz = encode_map(native_cubes(floor, ceilings, size, raster_solid, opened, raster_capped), entries, factor, title.encode())
    cgz_path = maps / f'{name}.cgz'
    cgz_current = cgz_path.is_file() and cgz_path.read_bytes() == cgz
    # The section's map .cfg (git-ignored; halo_a30.cfg, the Crash Site's, is tracked and not written
    # here): the fog, and a driven section's jump governors (GOVERNOR_SECONDS).
    cfg_path, cfg_text = maps / f'{name}.cfg', None
    if not crash_site:
        cfg_text = 'fog 360\nfogcolour 0x617181\n'
        governors = ((vehicle_plan[5] or {}).get('governors') or []) if vehicle_plan and driven else []
        if governors:
            cfg_text += ('// Jump governors (tools/import_halo_campaign.py GOVERNOR_SECONDS; halo_vehiclegovernor 0 turns them off):\n'
                         '// take-off x y z, radius, speed (units, units a second)\n' +
                         ''.join('halo_vehiclegovern %d %d %.2f %.2f %.2f\n' % tuple(g) for g in governors))
            print(f'{name}: {len(governors)} jump governors at {governors[0][4]:.1f} units a second '
                  f'(take-offs {", ".join(f"{g[0]},{g[1]},{g[2]:.2f}" for g in governors)})', flush=True)
    cfg_current = cfg_text is None or (cfg_path.is_file() and cfg_path.read_text() == cfg_text)
    route_text = None
    if write:
        cgz_path.write_bytes(cgz)
        if crash_site:
            # halo_a30.cfg is the tracked texture and mapmodel list (checked above); the models are
            # exported at this layout's scale, the same across and up.
            write_scenery(halo, output / 'models' / 'mapmodels' / 'halo_a30', scale, scale)
        else:
            cfg_path.write_text(cfg_text)
        write_world_mesh(halo,output,projection.xy,projection.z,(xmin-8,xmax+8,ymin-8,ymax+8),
                         map_name=name,bsp=bsp,include_sky=not indoor,
                         extra_bsps=EXTRA_BSPS.get(key, ()) if retail else (),
                         scenery=scenery_drawn(halo, scenery_baked, as_models))
    hcol, route_file = maps / f'{name}.hcol', maps / f'{name}.route'
    if driven:
        # The vehicle route (VEHICLE_ROUTES) for the QA walker's vehicle rows; without one, no route.
        # The extra BSPs' surfaces, and the section's own, without the seals between them (held_bsps).
        own, extra, _, _ = held_bsps(halo, key, bsp, projection, surfaces, tree)
        count = write_structure_collision(halo, bsp, projection, hcol, (list(machines) + extra, baked),
                                          scenery=(scenery_polygons, scenery_baked),
                                          structure=own if EXTRA_BSPS.get(key) else None) if write else '-'
        if vehicle_plan:
            route_text = ''.join(vehicle_plan[0])
            if write:
                route_file.write_text(route_text)
            print(f'{name}: vehicle route of {len(vehicle_plan[0])} rows' + ('' if vehicle_plan[2] else ', not reaching its exit') +
                  (f'; BSPs {", ".join(map(str, EXTRA_BSPS[key]))} baked in ({len(extra)} surfaces)' if extra else ''), flush=True)
        elif write:
            route_file.unlink(missing_ok=True)
        print(f'{name} BSP {bsp}: {count} retail collision surfaces, {len(baked)} static machines (driven)', flush=True)
    elif retail:
        count = write_structure_collision(halo, bsp, projection, hcol, (machines, baked), assists,
                                          (scenery_polygons, scenery_baked)) if write else '-'
        # The walkable route in feet coordinates, for tests/test_retail_collision.py.
        # x y z [crouch]: the fourth column marks steps only passable crouching.
        # x y z [flags]: 1 marks steps only passable crouching, 2 a ledge to jump up onto, 4 a lift ride,
        # 8 a walk off a ledge onto the floor below (LEDGE_DROPS).
        # 4 marks the first step after a lift ride: use the control at the step before, board the
        # platform when it arrives, use the control on it and step off where it stops.
        # 128 marks a step on a light bridge that has to be extended (LightBridges), with the
        # bridge's name and extended position as a fifth column (name@position); 32 on the first
        # of them means a control of the bridge is in reach of the step before: use it there.
        # 16, 32 and 64 on a lift's deck row: see LIFT_RIDES.
        crossed = []
        def flags(previous, p):
            value = (1 if getattr(p, 'crouch', False) else 0) | (2 if getattr(p, 'jump', False) else 0) | \
                    (4 if getattr(p, 'ride', False) else 0) | (8 if getattr(p, 'fall', False) else 0)
            bridge = bridges.at(p) if bridges is not None else None
            if bridge is None or bridge['always']:
                return f' {value}' if value else ''
            value |= 128
            if previous is not None and bridges.at(previous) is not bridge:
                if bridges.control_near(bridge, (previous[0], previous[1], previous.z)):
                    value |= 32
                crossed.append(bridge['name'])
            return f' {value} {bridge["name"]}@{bridge["value"]:.2f}'
        # Scripted doors the walker waits at on a row of its own (DOOR_WAITS): the last step before the
        # door's closed panels, flag 1024 with the door and the position at which it counts as open.
        door_waits, door_order = {}, []
        if key in DOOR_WAITS and doors is not None:
            def waited_at(q):
                return doors.scripted_at(q[0], q[1], q.z) or (key in DOOR_WAITS_CLOSABLE and doors.closable_at(q[0], q[1], q.z))
            for previous, p in zip(path[:-1], path[1:]):
                door = waited_at(p)
                # A door without a script name (DoorBlocks labels it '#kind...') opens on approach; the
                # walker can only wait for a named one (it reads the door's position by name).
                if door and not door.startswith('#') and not waited_at(previous) and door not in door_waits.values():
                    door_waits[id(previous)] = door
        # The volumes a door's script waits for before it opens the door (DOOR_PREREQUISITES): the
        # route must pass through each before the door's row, or the walker waits there for ever.
        for door, volumes in DOOR_PREREQUISITES.get(key, {}).items():
            at = next((i for i, p in enumerate(path) if door_waits.get(id(p)) == door), None)
            if at is None:
                say(f'{name}: WARNING no door row for {door} (DOOR_PREREQUISITES)', flush=True)
                continue
            passed = [volume for volume in volumes
                      if any(volume_test(volume_box(halo, volume), projection)(c[0], c[1], c.z) for c in path[:at + 1])]
            door_order.append({'door': door, 'step': at, 'passed': passed})
            for volume in volumes:
                if volume not in passed:
                    say(f'{name}: WARNING the route reaches {door} without passing {volume}', flush=True)
            if passed:
                print(f'{name}: route passes {", ".join(passed)} before waiting at {door} (step {at})', flush=True)
        rows, row_of = [], []
        for previous, p in zip([None] + path[:-1], path):
            row_of.append(len(rows))
            # Actions at a step (UseControl: 256 with device@position; ShootTarget: 512 with
            # target@health and the aim point, x y z; ThrowTarget: 2048 with target@health and the
            # point to aim a frag at from the eye, one row per aim, best first) follow the step's own
            # row, one row each at the same place; so does a door wait (1024).
            rows.append('%d %d %.2f%s\n' % (*xyz(p), flags(previous, p)))
            if id(p) in door_waits:
                rows.append('%d %d %.2f 1024 %s@%.2f\n' % (*xyz(p), door_waits[id(p)], DOOR_OPEN))
            for kind, column, aim in getattr(p, 'actions', ()):
                if kind == 'use':
                    rows.append('%d %d %.2f 256 %s\n' % (*xyz(p), column))
                else:
                    rows.append('%d %d %.2f %d %s %.2f %.2f %.2f\n' % (*xyz(p), 2048 if kind == 'throw' else 512, column, *aim))
        if door_waits:
            print(f'{name}: route waits on its own rows at doors {", ".join(door_waits.values())}', flush=True)
        acted = sum(len(getattr(p, 'actions', ())) for p in path)
        if acted:
            print(f'{name}: route uses {sum(1 for p in path for a in getattr(p, "actions", ()) if a[0] == "use")} controls, '
                  f'shoots {sum(1 for p in path for a in getattr(p, "actions", ()) if a[0] == "shoot")} targets and '
                  f'throws frags on {sum(1 for p in path for a in getattr(p, "actions", ()) if a[0] == "throw")} aims', flush=True)
        if crossed:
            print(f'{name}: route crosses light bridges {", ".join(crossed)}', flush=True)
        # The end of the path's rows (with their action rows), before an exit fall's row: a lift
        # boarded from the route's end goes here, as it did before action rows existed.
        path_end = len(rows)
        if key in EXIT_FALLS:
            # Walk off the edge towards the exit volume's centre and fall into it (flag 8).
            rows.append('%d %d %.2f 8\n' % (round(projected_end[0]), round(projected_end[1]), projected_end[2]))
        lift = LIFT_RIDES.get(key)
        if lift:
            # Board the lift from the step before the ride (b40's c1 tower), or from the route's end.
            index = next((i for i, p in enumerate(path) if getattr(p, 'ride', False)), None)
            before = path[index - 1] if index else path[-1]
            near = ((before[0] - 9) / scale + projection.x0, projection.y0 - (before[1] - 9) / scale,
                    before.z / 4 / scale + projection.z0)
            found = lift_ride(halo, projection, output.parent, lift, near)
            if found is None:
                say(f'{name}: WARNING no lift deck for {lift}', flush=True)
            else:
                board, value, send, call = found
                bx, by, bz = projection.point(board)
                rows.insert(path_end if index is None else row_of[index],
                            '%d %d %.2f %d %s@%.2f\n' % (round(bx), round(by), bz, 16 | (32 if call else 0) | (64 if send else 0),
                                                         lift, value))
                print(f'{name}: route boards {lift} at {board[0]:.2f},{board[1]:.2f},{board[2]:.2f} (position {value:.2f}'
                      f'{", called from the landing" if call else ""}{", sent from its control" if send else ""})', flush=True)
        route_text = ''.join(rows)
        if write:
            route_file.write_text(route_text)
        crouched = sum(bool(getattr(p, 'crouch', False)) for p in path)
        jumps = sum(bool(getattr(p, 'jump', False)) for p in path)
        lifts = sum(bool(getattr(p, 'ride', False)) for p in path)
        falls = sum(bool(getattr(p, 'fall', False)) for p in path)
        if crouched or jumps or lifts or falls:
            print(f'{name}: {crouched} route steps need crouching, {jumps} jumps' +
                  (f', {lifts} lift rides' if lifts else '') + (f', {falls} ledge drops' if falls else ''), flush=True)
        print(f'{name} BSP {bsp}: {count} retail collision surfaces, {len(baked)} static machines' +
              (f', {len(scenery_baked)} scenery placements' if scenery_baked else '') +
              (f', {len(assists)} walking assists' if assists else ''), flush=True)
    elif write:
        # A stale file would switch the engine to collision this section has not been checked against.
        hcol.unlink(missing_ok=True)
        route_file.unlink(missing_ok=True)
    # The cube-raster stamp's static check: route rows and entities in a solid cube of the raster written
    # above. The route's own cubes and the placed entities' are open (`opened`), so what is left is rows
    # with no native floor at their cube (a lift's deck, a flight over a drop, an exit fall's last step,
    # a drive or flight leg off the raster's floor) and entities beside none (a vehicle at its retail point).
    raster_rows, raster_entities = {}, {}
    for line in (route_text or '').splitlines():
        x, y = (int(v) for v in line.split()[:2])
        if not native_cube_open(floor, x, y, raster_solid, opened):
            why = 'scenery' if (x, y) in raster_solid else 'no floor' if (x, y) not in floor else 'corner'
            raster_rows[why] = raster_rows.get(why, 0) + 1
    labels = {0: 'actor', 1: 'start', 100: 'marker', WEAPON_PLACEMENT: 'weapon', VEHICLE_PLACEMENT: 'vehicle',
              EQUIPMENT_PLACEMENT: 'equipment'}
    for e in entries:
        if not native_cube_open(floor, e[0], e[1], raster_solid, opened):
            label = labels.get(e[5], 'start') if e[4] == PLAYERSTART else {LIGHT: 'light', MAPMODEL: 'mapmodel'}.get(e[4], 'item')
            raster_entities[label] = raster_entities.get(label, 0) + 1
    if raster_solid or raster_raised or raster_rows or raster_entities:
        print(f'{name}: cube raster: {len(raster_solid)} cubes solid under scenery, {len(raster_raised)} floors raised onto it'
              f'{f" ({len(raster_kept)} cubes of the route, vehicles and mapmodels kept as they were" if raster_kept else ""}'
              f'{f", {len(raster_capped)} mapmodel cubes capped)" if raster_capped else ")" if raster_kept else ""}'
              f'{f", {len(raster_reopened)} reopened under actors or items with no other cell in reach" if raster_reopened else ""}; '
              f'{sum(raster_rows.values())} route rows in solid cubes' +
              (' (' + ', '.join(f'{n} {why}' for why, n in sorted(raster_rows.items())) + ')' if raster_rows else '') +
              (', entities in solid cubes: ' + ', '.join(f'{n} {what}' for what, n in sorted(raster_entities.items()))
               if raster_entities else ''), flush=True)
    entry_offset = math.dist(xyz(path[0]), projected_start) / scale
    # A vehicle route is measured to its exit volume's middle (c40 section 4's anchor is the
    # raster's end on the canyon floor, its exit the landing pad).
    exit_offset = math.dist(xyz(path[-1]), vehicle_plan[3] if vehicle_plan else projected_end) / scale
    print(f'{name} BSP {bsp}: {len(path)} steps, {len(used)} actors, {placed} weapons, {items} equipment, {vehicles} vehicles, {len(parents)} cells; '
          f'entry offset {entry_offset:.1f}, '
          f'exit offset {exit_offset:.1f} source units',flush=True)
    mode = ('stock' if not retail else 'flown' if key in FLOWN_SECTIONS else 'driven' if driven
            else 'walked' if walked else 'raster')
    route_path = maps / f'{name}.route'
    metrics = {
        'format': METRICS_FORMAT, 'name': name, 'mission': mission.cache, 'section': 0 if crash_site else part + 1, 'bsp': bsp,
        'mode': mode,
        'steps': len(path), 'route_rows': route_text.count('\n') if route_text is not None else None,
        'reached_cells': len(parents), 'floor_cells': len(samples),
        'reach_pct': round(100.0 * len(parents) / max(1, len(samples)), 2),
        'entry_offset': round(entry_offset, 2), 'exit_offset': round(exit_offset, 2),
        'ends_in_exit': (bool(test(path[-1][0], path[-1][1], path[-1].z)) if walked and test is not None else
                         vehicle_plan[2] if vehicle_plan else None),
        # A planned vehicle route (VEHICLE_ROUTES): its rows by kind and the extra BSPs baked in.
        'vehicle_route': ({'rows': len(vehicle_plan[0]),
                           'vehicle_rows': sum(1 for r in vehicle_plan[0] if len(r.split()) > 3 and int(r.split()[3]) & ROUTE_VEHICLE),
                           'boards': sum(1 for r in vehicle_plan[0] if len(r.split()) > 3 and int(r.split()[3]) & ROUTE_BOARD),
                           'get_outs': sum(1 for r in vehicle_plan[0] if len(r.split()) > 3 and int(r.split()[3]) & ROUTE_GET_OUT),
                           'extra_bsps': list(EXTRA_BSPS.get(key, ())),
                           'drive': vehicle_plan[5]} if vehicle_plan else None),
        'actors': len(used), 'weapons': placed, 'equipment': items, 'vehicles': vehicles,
        'warnings': notes,
        # Authored walking assists baked into the section's .hcol (ASSISTS; 0 for retail-only).
        'assists': len(assists),
        # Scenery placements baked into the section's .hcol (SCENERY_COLLISION; 0 elsewhere).
        'scenery': len(scenery_baked),
        # The cube-raster stamp (scenery_raster): the native cubes its scenery made solid (written with
        # RASTER_SCENERY_VDELTA) and those whose floor it raised onto a low top, the cubes of the route,
        # vehicles and mapmodels it left as they were, and the route rows and entities still in a solid
        # cube of the raster, by reason and by kind (none from scenery).
        'raster': {'scenery_solid': len(raster_solid), 'scenery_raised': len(raster_raised), 'scenery_kept': len(raster_kept),
                   'scenery_capped': len(raster_capped), 'scenery_reopened': len(raster_reopened),
                   'route_rows_solid': raster_rows, 'entities_solid': raster_entities},
        # A ThrowTarget's planned aims (the route batch): the target, yaw, pitch, damage, neighbours that
        # count, and the blast's distance from the thrower's body against the frag's outer radius
        # (retail units). Empty elsewhere.
        'throws': planned_throws,
        # DOOR_PREREQUISITES: for each door, the route step of its door row and the volumes passed before it.
        'door_order': door_order if walked else [],
        'cgz_sha1': hashlib.sha1(cgz).hexdigest(),
        'route_sha1': hashlib.sha1(route_text.encode()).hexdigest() if route_text is not None else None,
        # Whether the files on disk are what this importer writes (always, after a real import;
        # with --metrics-only, false means the section needs re-importing).
        'cgz_current': True if write else cgz_current,
        # The map .cfg likewise (the fog and a driven section's jump governors; the Crash Site's
        # tracked halo_a30.cfg always counts as current).
        'cfg_current': True if write else cfg_current,
        'route_current': (True if write else route_path.is_file() and route_path.read_text() == route_text)
                         if route_text is not None else (write or not route_path.exists()),
    }
    metrics['stub'] = stub_reasons(metrics, key in EXIT_FALLS)
    (maps / f'{name}.metrics.json').write_text(json.dumps(metrics, indent=1, sort_keys=True) + '\n')


def convert(args):
    source, output, mission, part, collision, metrics_only, key = args
    with (source / f'{mission.cache}.map').open('rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        if halo.name != mission.cache:
            raise ValueError(f'Wrong cache in {mission.cache}.map')
        try:
            make_section(halo,mission,part,output,collision,metrics_only,key)
        except (ValueError, IndexError) as error:
            return f'{mission.cache} section {part+1}: {error}'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps',type=Path,help='Directory with the ten expanded Xbox campaign map caches')
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--mission',choices=[m.cache for m in MISSIONS])
    parser.add_argument('--part',type=int,help='One-based section number, used with --mission; '
                        'a30 --part 0 is the Crash Site (halo_a30, CRASH_SITE)')
    parser.add_argument('--collision',choices=('validated','all','none'),default='validated',
                        help='Which sections get retail BSP collision (.hcol): validated missions (default), all, or none')
    parser.add_argument('--metrics-only', action='store_true',
                        help='Plan each section as the import would but write only packages/maps/<section>.metrics.json '
                             '(the stub-route gate, tests/test_route_metrics.py); the maps, meshes, .hcol and .route stay as they are')
    args = parser.parse_args()
    jobs = [(args.maps,args.output,m,i,args.collision,args.metrics_only,None) for m in MISSIONS if not args.mission or m.cache==args.mission
            for i in range(len(m.sections)) if args.part is None or i+1==args.part]
    if CRASH_SITE and args.mission in (None, 'a30') and args.part in (None, 0):
        jobs.insert(0, (args.maps, args.output, CRASH_SITE_MISSION, 0, args.collision, args.metrics_only, CRASH_SITE_KEY))
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
