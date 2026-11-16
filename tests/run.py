#!/usr/bin/env python3
"""Run the macOS verification suite.

    python3 tests/run.py              quick tier (10 to 15 minutes)
    python3 tests/run.py --slow       quick plus full-speed and keystroke tests
    python3 tests/run.py --release    quick plus the release tier: damaged-asset robustness, 55-section soak
    python3 tests/run.py devices b30  only tests whose names contain these words (any tier)

Needs build/macos/halo-ac-qa (configure with -DHALO_BUILD_QA=ON) and the packaged
app; scripts/test-macos.sh builds both first. Logs go to build/qa/.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

here = Path(__file__).resolve().parent
QUICK = [
    'test_campaign_tables.py',        # C++ and importer mission tables agree (no Mac needed)
    'test_stock_smoke.py',            # AssaultCube baseline still boots
    'test_mission_openings.py',       # packaged app: ten opening cinematics start and skip
    'test_section_loads.py',          # every one of the 55 sections loads with a live player
    'test_campaign_progression.py',   # completing a section advances and saves progress
    'test_combat_replay.py',          # fire, reload, shields, death, revert, extraction
    'test_devices.py',                # door collision, BSP switch, lift carries the player
    'test_library_platform.py',       # Library elevated platform
    'test_b30_retail_collision.py',   # Silent Cartographer on retail BSP collision
    'test_retail_collision.py',       # a10 (its default) on retail collision: active, spawn on floor, route walks to the exit
    'test_retail_recovery.py',        # falls below or past the retail mesh and wedges put the player back on floor; teleport grace
    'test_traversal_routes.py',       # lift deck and light-bridge route rows are consistent (route files only)
    'test_retail_assists.py',         # walking assists (ASSISTS, .hcol AST1): loaded, rays and grenades pass, bodies stand on them
    'test_retail_scenery.py',         # scenery collision (SCENERY_COLLISION, .hcol SCN1): trailer matches metrics, loaded, hit by rays, grenades, bodies
    'test_scenery_uses.py',          # the use-aware scenery rule: named scenery a script only reads stays baked (SCN1), intro_tree undrawn (caches, no game)
    'test_scene_meshes.py',          # every halo_scene_*.mesh and its textures match a fresh export, cast exports on disk and in the .hscb, no orphans (--check-scenes; caches, no game)
    'test_md3_range.py',             # MD3 exports past the 16-bit range: per-frame origins (halo_origin), larger mdlscale, failed exports and orphans removed, restored clips in the .hscb (no game, no caches)
    'test_scenery_raster.py',         # the cube-raster stamp: scenery in the .cgz cube raster (metrics, route rows and entities clear of it), flood waypoints out of it
    'test_route_metrics.py',          # stub-route gate: importer metrics, the known-bad table may not grow (no game)
    'test_content_currency.py',       # map and scene-texture currency compare content (gunzipped .cgz, decoded PNG), write_png skips unchanged files, metrics carry cgz_raw_sha1 (no game, no caches)
    'test_campaign_replay.py',        # all 55 sections complete in one run
    'test_weapon_tags.py',            # retail weapon table imported and parsed by the engine
    'test_weapon_fire.py',            # rate, zoom, heat, charge, swap, revert, enemy weapons
    'test_combat_equipment.py',       # grenade types, sticky plasma, equipment pickups, overshield, camouflage, weapon sounds
    'test_damage_model.py',           # jpt! range and blast falloff, actor grenade rolls, exact actor shots, EMP, time to kill, the cyborg coll
    'test_melee_materials.py',        # melee from behind (allies spared), Hunter head and belly (cannon-fire window), material model cvar, needle supercombine, AOE core, shield leak and timing
    'test_damage_targets.py',         # d40's manifold detectors: HSCB 8 targets, placed whole, damage rules, a real shot from the route
    'test_frag_throw.py',             # d40's s3 manifold: the importer's planned frag throws (2048 rows) thrown in the game destroy it
    'test_actor_tags.py',             # retail actor behaviour table imported and parsed by the engine (shield leak, resistance, shieldtime)
    'test_actor_ai.py',               # perception, bursts, marines, firing positions, script AI commands, combat by difficulty, playfight, effect damage, melee, special fire
    'test_factions.py',               # allegiance matrix and commands, betrayal, Hunters, stealth Elites, flyers, Monitor, Keyes
    'test_ai_attach.py',              # HSCB 9 story-figure flags; ai_attach makes a50's Keyes and marines actors (health, revert, destroy, carried into section 5, own start across checkpoints, spared the kill from behind)
    'test_starting_profiles.py',      # HSCB 9 starting profiles: a10 unarmed, player_add_equipment, a30/c20 loadouts, carry, empty slots saved
    'test_campaign_ending.py',        # game_won/game_lost raise the ending, credits, beaten flag
    'test_mission_exits.py',          # every mission's exit scene and the next mission, a30 variants, script-called scenes
    'test_frontend_progress.py',      # title menu, Continue with the script's state (v3), fallbacks, loadout carry, backup, corruption, squad members where they stood
    'test_nav_points.py',             # script nav points and HUD show/blink commands, waypoint arrows in hud.txt
    'test_script_coverage.py',        # every sleep_until/if condition command is bound; a10 tutorial, d40 timer
    'test_script_seats.py',           # a10's cryotube as a scripted seat (held, released by the script, guards), b40's Pelican ride, marker effects, the Monitor spared
    'test_difficulty.py',             # halo_difficulty: squad sizes, game_difficulty_get, enemy scaling
    'test_falling_damage.py',         # falling damage: player.txt's matg block, the planned-drop scan, the player's and an actor's falls on a30
    'test_script_damage.py',          # damage_object/damage_new by their jpt!: effects.txt records, pulse generator only on the shield, oven, a10 zapper 0
    'test_major_upgrades.py',         # major upgrades at squad placement (chance table, error diffusion, revert, Continue), Halo's squad sizes
    'test_flood.py',                  # squad cap counts living actors; carrier burst from the tag, corpse infection, leap, pop, stagger; forms in the carrier's squad, blast
    'test_vehicles.py',               # vehicle table and placements, Warthog in/out/drive/flip/gunner, turret, seat commands
    'test_enemy_vehicles.py',         # Ghost, Wraith, Scorpion, flown Banshee, AI pilots, dropship cargo, Pelican boarding, c40 flown
    'test_vehicle_damage.py',         # vehicle body vitality: the Wraith destroyed with its pilot, occupied-only Ghost, vehicle gun variants, covered riders, death explosion, tilted hit box, wreck
    'test_vehicle_jump.py',           # d40 section 8's planned Warthog jumps flown by the engine (launch along the ramp, retail-derived gravity), capped and governed
    'test_presentation.py',           # script effects, shake, screen effects, flashlight, damage indicator, class gains, lunge, engine loops, effect_new damage cvar
    'test_package_relocatable.py',    # relocatable signed package, retail-leak check, strict asset verification
]
# Release tier (--release, scripts/ci-macos.sh --release): about 10 and 8 minutes; the ASan/UBSan
# runs (--asan on each, scripts/ci-macos.sh --asan) take about three times as long.
RELEASE = [
    'test_asset_robustness.py',       # missing and damaged tables, .hcol, .hscb, .mesh, .route and progress files: fallback, no crash
    'test_soak.py',                   # all 55 sections loaded twice in one process: no crash, no sanitizer report, bounded memory
]
SLOW = [
    'test_autumn_opening.py',         # four-minute Autumn opening at normal speed
    'test_package_input.py',          # real keystrokes into the packaged app
    'test_maw_drive.py',              # Warthog escape driven by the QA autopilot (section 8: the vehicle walker, jump governors) to the finale, plus keystrokes
]

parser = argparse.ArgumentParser()
parser.add_argument('filters', nargs='*')
parser.add_argument('--slow', action='store_true')
parser.add_argument('--release', action='store_true')
args = parser.parse_args()
tests = QUICK + (SLOW if args.slow or args.filters else []) + (RELEASE if args.release or args.filters else [])
if args.filters:
    tests = [t for t in tests if any(f in t for f in args.filters)]
if not tests:
    raise SystemExit('no tests match')

results = []
for test in tests:
    start = time.time()
    print(f'\n=== {test}', flush=True)
    code = subprocess.run([sys.executable, str(here / test)]).returncode
    results.append((test, code, time.time() - start))

print('\n=== Summary')
for test, code, seconds in results:
    print(f'{"PASS" if code == 0 else "FAIL"}  {seconds:6.1f}s  {test}')
failed = sum(code != 0 for _, code, _ in results)
print(f'{len(results) - failed} passed, {failed} failed')
sys.exit(1 if failed else 0)
