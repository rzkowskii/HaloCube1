#!/usr/bin/env python3
"""Run the macOS verification suite.

    python3 tests/run.py              quick tier (10 to 15 minutes)
    python3 tests/run.py --slow       quick plus full-speed and keystroke tests
    python3 tests/run.py devices b30  only tests whose names contain these words

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
    'test_campaign_replay.py',        # all 55 sections complete in one run
    'test_weapon_tags.py',            # retail weapon table imported and parsed by the engine
    'test_weapon_fire.py',            # rate, zoom, heat, charge, swap, revert, enemy weapons
    'test_actor_tags.py',             # retail actor behaviour table imported and parsed by the engine
    'test_actor_ai.py',               # perception, bursts, marines, firing positions, script AI commands
    'test_campaign_ending.py',        # game_won/game_lost raise the ending, credits, beaten flag
    'test_script_coverage.py',        # every sleep_until/if condition command is bound; a10 tutorial, d40 timer
    'test_difficulty.py',             # halo_difficulty: squad sizes, game_difficulty_get, enemy scaling
    'test_flood.py',                  # squad cap counts living actors; carrier burst, corpse infection, infection leap
    'test_package_relocatable.py',    # relocatable signed package, retail-leak check, strict asset verification
]
SLOW = [
    'test_autumn_opening.py',         # four-minute Autumn opening at normal speed
    'test_package_input.py',          # real keystrokes into the packaged app
    'test_maw_drive.py',              # Warthog escape with keystrokes
]

parser = argparse.ArgumentParser()
parser.add_argument('filters', nargs='*')
parser.add_argument('--slow', action='store_true')
args = parser.parse_args()
tests = QUICK + (SLOW if args.slow or args.filters else [])
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
