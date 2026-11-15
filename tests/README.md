# Tests

Verification for the macOS build. Each `test_*.py` is a standalone script that
exits non-zero on failure. Logs and throwaway profiles go to `build/qa/`.

```bash
./scripts/test-macos.sh            # build game + QA client, package, run the quick tier
./scripts/test-macos.sh --slow     # add the full-speed opening and keystroke tests
python3 tests/run.py devices b30   # rerun by name once built
```

The quick tier takes 10 to 15 minutes. It needs the imported campaign assets
(`docs/macos-build.md`) and the AssaultCube runtime staged in `build/runtime`.

Nothing has been run since commit `e0806ab`: the tests marked "Not yet run" below
are new, and the existing tests have not been re-run against the uncommitted work
since then (recovery, lifts and bridges, vehicles, grenades and equipment, the title
menu and progress file version 2, nav points, the vehicle fixes: bodies pushed out of
vehicles, routes round parked vehicles, vehicle stuck recovery, the Maw's escape taking over a
scenario Warthog, vehicles on lift decks; the recovery's reset on every move that is not walking
(`haloplayermoved`) and its one-second grace; progress file version 3, with which Continue
restores the mission script, vehicles, ground items and powerups; and the corrected `jpt!` damage
layout with weapons.txt and equipment.txt format 2; see "Not yet verified in the game" in
`docs/macos-build.md`). Expect first runs to need fixes in the tests as well as the
game. `test_campaign_progression.py`, `test_campaign_ending.py`, `test_maw_drive.py`,
`test_retail_collision.py`, `test_weapon_tags.py` and `qa_common.py` were changed for that work
and are equally unrun. Every test that reads `weapons.txt` or `equipment.txt` needs them
regenerated as format 2 (`tools/halo_weapons.py`); the game refuses format 1 tables and falls
back to the stock AssaultCube weapons.

| Test | Checks |
| --- | --- |
| `test_campaign_tables.py` | C++ mission table and cinematic cues agree with the importer. No Mac or assets needed |
| `test_stock_smoke.py` | Unmodified AssaultCube still boots a stock map |
| `test_mission_openings.py` | Packaged app starts and skips all ten mission openings |
| `test_section_loads.py` | All 55 sections load with a live player |
| `test_campaign_progression.py` | Finishing a section advances the map and saves progress |
| `test_combat_replay.py` | Fire, reload, shield hit and recharge, death, revert, extraction |
| `test_devices.py` | Autumn door collision, first BSP switch, Control Room lift |
| `test_library_platform.py` | The Library's elevated platform carries the player |
| `test_b30_retail_collision.py` | Silent Cartographer route on retail BSP collision |
| `test_retail_collision.py` | Generic retail-collision walk (default `a10`), one playthrough per mission: for each section with a `packages/maps/<section>.hcol` the log says `loaded N retail collision surfaces`, the player spawns standing on retail floor, every spawn, marker, weapon, equipment placement and item has floor under it (vehicle placements, team 102, settle themselves and are skipped), the importer's `.route` is walked at full speed to the exit, and movement feels like stock geometry (speed, acceleration, jump within 10%). Driven sections (d40 6 to 8) are skipped. See below |
| `test_retail_recovery.py` | On `halo_a10_01`: a drop 20 units below the retail mesh, a fall in the open past its edge and feet sunk into the floor each put the player back, alive and standing, within 4 units of where they stood (`Halo recovery:` in the log), with no revert. Then the teleport rule: `qa_halo_moved` (what every non-walking move calls, `haloplayermoved`) drops the safe floors (`QA_HS_STATE moved safe=0 grace=1`, `Halo moved: QA teleport`), a wedge right after it is still in the floor 1.1 s later (the one-second grace, then 0.75 s wedged) and nudged out by 3.3 s, and safe floors are recorded again. Not yet run (written while testing was on hold) |
| `test_traversal_routes.py` | Route files only, no game: rows parse, lift deck rows (flag 16) and light-bridge rows (flag 128) name their device, each `LIFT_RIDES` section boards its lift on one deck row (the last, or the one before the first flag-4 row), `STOCK_SECTIONS` have no `.hcol` or `.route`, and `DRIVEN_SECTIONS` (d40's Warthog run) have an `.hcol` and no `.route`; no route row stands inside a vehicle parked at the section start (the map's team-102 placements without the script-created flag, boxed as `halo_vehicle.cpp` sizes them from `vehicles.txt`, widened by the body's radius). Not yet run |
| `test_campaign_replay.py` | All 55 sections complete in one run |
| `test_weapon_tags.py` | Retail weapon table matches known tag values and the engine's parsed copy. Format 2 damage columns (`jpt!` lower bound, upper bound min and max): rocket 80/300/330 high explosive, shotgun pellet 8/18/25, plasma rifle bolt 10/12/14, AR and sniper bullets flat (10, 101), human weapon melee 40/50/60, lower ≤ upper min ≤ upper max on every projectile. The column checks are new and not yet run |
| `test_weapon_fire.py` | Rate of fire, zoom, overheat, needler ramp, charged shot, swap, revert, enemy weapons and drops |
| `test_combat_equipment.py` | `equipment.txt` has both grenade types (the plasma grenade sticks, its fuse starts at rest, its blast is EMP; both blasts 80 lower, 120-120 upper in the format 2 columns), the overshield, camouflage, health pack and grenade pickups and the AR ammo tied to its weapon; eight player weapons name a fire sound; section maps carry team-103 equipment placements and no stand-in boxes beside them. On a30 section 2: every table sound is decoded, the grenade switch needs grenades of the other type, a plasma grenade is thrown and explodes, the last one switches back to frag, dropped plasma grenade, health pack, overshield (charges to about three times the shield, takes the next hit, drains), camouflage and AR ammo are picked up, a revert restores two frags and no powerups. On a30 section 3: the Covenant carry and throw plasma grenades, a plasma grenade sticks to an actor, killed actors drop grenades, actors lose track of the camouflaged player. Not yet run (written while testing was on hold) |
| `test_actor_tags.py` | Retail actor table matches known tag values and the engine's parsed copy |
| `test_actor_ai.py` | Hearing and squad alerts, burst fire, marines against the Covenant, squad positions, script AI commands, command lists; the unit and object-state commands (berserk, grenades, damage immunity, vitality, damage, follow, migrate, attach, renew, suspend, player vitality, hover, open, collideable, a50 cell doors) |
| `test_campaign_ending.py` | game_won on The Maw and the finale cue raise the ending and mark progress beaten (no game left to continue); credits end at the menu; early game_won is ignored; game_lost reverts |
| `test_frontend_progress.py` | The title menu (`halo_title`) on a fresh profile; New Game on Heroic saves the game at a10's start (version 3, no state block); a checkpoint saves place, loadout, difficulty and the mission script's state (a state block whose count and hash hold), and a second launch's Continue restores them and the script where it was (`cryo_door_1`'s position, the global `delay_lost` set to 1234, a script clock no earlier than the checkpoint's), keeping the previous file as `.bak`; finishing a10 section 1 carries the loadout into section 2 and the save at its start keeps the carried script, which Continue resumes; a corrupt progress file is replaced by its backup (or reset without one) with a warning in the log and the title menu; version 1 files still load and unlock the missions reached; a state block with another `.hscb` hash (the Continue entry says the scripts changed) or a damaged record falls back to restarting the script at the checkpoint with a warning; a version 2 file with a checkpoint still continues there; a save at the wheel of b30's beach Warthog continues at the wheel (skipped without b30 or `vehicles.txt`). Not yet run (written while testing was on hold) |
| `test_nav_points.py` | `hud.txt` has the waypoint bitmap and its ten arrow types; on a30 section 2 `activate_team_nav_point_flag` shows a nav point (raised by its offset), a second call replaces it, deactivating removes it; `hud_show_health` and `hud_blink_shield` hide and flash HUD parts; a revert brings back the checkpoint's nav points and HUD state. Not yet run (written while testing was on hold) |
| `test_script_coverage.py` | Every command used in a mission's sleep_until/if conditions is bound, and so is every ai_, unit_, object_ and vehicle_ command any script calls; a10 looking tutorial finishes; d40 countdown follows the escape deadline and holds off endgame_cinematics |
| `test_difficulty.py` | halo_difficulty picks each squad's Easy to Impossible start count (a30 lz_search matches the importer table), game_difficulty_get returns it, enemy health and damage scale, the choice persists |
| `test_flood.py` | The squad cap counts only living actors (c10 swarm encounters place behind 128 dead entries); a killed Carrier Form bursts into 4 to 8 Infection Forms; an Infection Form on a dead marine raises a Flood combat form; Infection Forms leap |
| `test_vehicles.py` | `packages/halo/vehicles.txt` has the six vehicles (Warthog seats and gun, turret gunner seat, models); the section maps carry the scenario vehicle placements (d40's escape Warthogs in section 6 and not the engine room, the enc7_7 turrets, a50's 14 turrets, b30's unnamed Warthog); on b30 (retail collision) E by the driver door gets in, throttle drives, E gets out onto floor, a flip throws the driver out and E flips it back, the gunner seat fires; a50's turret is manned and fires; b30's script seat commands (`object_create`, `vehicle_load_magic`, `vehicle_test_seat_list`, `unit_in_vehicle`, `unit_exit_vehicle`) act on its jeep; a Warthog drives on stock geometry (d40 section 3). Vehicle fixes: a body put part way into, or into the middle of, b30's Warthog is pushed out; a Warthog rolled into the player never has the body inside it; `object_create jeep` over the player pushes them out; an actor put inside an a50 turret is pushed out alive; at the wheel, a drop below the mesh and a Warthog sunk into the beach are recovered to a safe spot with the driver still in it; a driver thrown out upside down with no room is put back in the seat by the fall recovery (no `player left in place`); a Warthog on c20's platform1 rises with it. Not yet run (written while testing was on hold) |
| `test_package_relocatable.py` | check-no-retail passes clean and fails on staged or stray retail files; packages/ is ignored by default; package-macos.sh rejects a stub assets dir; packaged app has no Homebrew paths, is signed, carries licences and launches from a copied location |
| `test_autumn_opening.py` | Slow: four-minute Autumn opening at normal speed |
| `test_package_input.py` | Slow: real keystrokes into the packaged app |
| `test_maw_drive.py` | Slow: The Maw's escape in a driven Warthog. Section 6 starts the countdown, at the wheel on retail collision or on foot by a Warthog on stock geometry; E gets in, out and in again; the QA autopilot (`qa_vehicle_autopilot`) drives section 6 into section 7 at the wheel with the countdown carried over, and section 8 to the finale before the deadline; killed at the wheel, the revert puts the player in a Warthog again; on retail collision section 6 seats the player in one of the scenario's parked Warthogs (object bound, no second Warthog on it) and the scripts' seat tests for `trench_jeep1` to `4` and `asspain_1` to `3` see the player at the wheel in sections 6, 7 and 8; then E and W keystrokes into the packaged app exit cleanly. Not yet run (written while testing was on hold) |

## Retail collision walk

`python3 tests/test_retail_collision.py [mission ...] [--section a10:3]` walks each mission as one
playthrough in the QA client (`qa_retail_start` in `tests/native/retail_qa.inc`): it starts the
first section and, at each transition, carries on into the next with the mission script's state,
as a player arrives there. `--section` walks one section on its own; the mission script then
starts afresh, so doors that earlier sections' scripts open stay shut (see
`docs/macos-build.md`). The walk sets `halo_difficulty 2` because Normal runs the Autumn's scripted
cryo tutorial, which keeps its doors shut until the player has performed it. The player cannot be
hurt; contact-stage enemies, and Covenant that hold the walk up, are removed (as the player would
fight them), so only collision and the exit are measured.

The route follower holds forward at full speed. It crouches where the importer's route marks a
crawl space, jumps where it marks a ledge, obstacle or gap, and squeezes through the gap of a part
open door. At a closed scripted door it clears the Covenant in the room, steps back, looks around
(tutorial scripts wait for the player to look at the crewman or the door) and tries again, for up
to `HALOQA_RETAIL_WAIT` ms per door (default 150000). Failures print the waypoint, the surface or
device in the way and the log path under `build/qa/`.

Movement feel: on the first clear, flat straight of 22 or more route steps, the walk stops and runs
the straight twice from a standstill with a fixed heading, once on retail collision and once on the
same map's stock cube geometry, then jumps from a standstill. `RETAIL_QA feel` reports sustained
ground speed (about 16 u/s), the time to cover the first 4 units from a standstill, and the jump's apex and airtime. The test
fails a section whose retail speed is more than 10% below stock (or below 16 u/s when the stock
layout has no room for the straight), whose acceleration or jump falls short of stock by more than
10%, or where the route follower's speed on any straight it walks is more than 10% below stock.

Lifts and light bridges: a route row with flag 16 is a lift's deck, with the lift's name and the
position at which the deck is at that landing as a fifth column (`lift_a@0.00`). The walker stops on
the landing row before it until the lift is there and still, uses the control in reach to call it
if the row has flag 32 and the lift has stood still elsewhere for 3 s, walks onto the deck point,
uses the control beside it if the row has flag 64, and rides: to the section's end (the deck row
is the last), or, when more rows follow (b40 section 6), until the lift stops at the other end,
then walks on from the next row (flag 4). Rows with flag 128 cross a light bridge (`bridge_c@1.00`):
two rows before the first of them the walker stops until the bridge is extended, using its control
first if that row has flag 32. If the engine puts the player back after a fall or a wedge (a jump
of more than 6 units in one tick), the walker carries on from the nearest route row behind it and
waits for any bridge again (`RETAIL_QA recovered`, `RETAIL_QA recoveries N`). Ride, bridge and
recovery lines are printed under each section's result.

Sections in `KNOWN_BLOCKED` (a10 sections 5 and 6, which wait at script-gated doors; b40 section
6, c10 sections 1 and 4 and c20 sections 1 to 3, which end on or include a lift ride; d40 section
5, whose manifolds must be shot; see `docs/macos-build.md`) are walked and printed as `KNOWN` with
the reason instead of failing; the test fails once one of them passes, so the list stays current.
`PENDING_WALK` names the sections the lift and bridge machinery is expected to make pass, which
have not been walked yet: b40 section 6, c10 sections 1, 2 (new on retail collision) and 4, and c20
sections 1 to 3. They are printed as `expected to pass, pending walk`; a pending section that is
not in `KNOWN_BLOCKED` (c10 section 2) fails the test like any other when its walk fails.

The importer writes the route next to each `.hcol` (`tools/import_halo_campaign.py --collision
validated|all|none`; the default `validated` writes both for the missions in
`VALIDATED_COLLISION`). Route rows are `x y z [flags [device@position]]`, with flag 1 for a
crouched step, 2 for a jump, 4 for the first step after a lift ride, 8 for a walk off a ledge onto
the floor below (`LEDGE_DROPS`; the walker walks it like any other step, and d20 section 1's last
row, over the shaft its exit volume hangs in, `EXIT_FALLS`, has the flag too), 16 for a lift deck
(32: call the lift from the landing, 64: send it from the control on its deck) and 128 for a light
bridge (32 on its first row: use the bridge's control from the row before).

## QA client

`halo-ac-qa` is the game plus console commands for injected positions, damage
and scripted outcomes. `tests/native/retail_qa.inc` has the route walker and adds
`qa_retail_where` and `qa_retail_recover fall|void|wedge` for the recovery test.
`tests/native/vehicle_qa.inc` has the `qa_vehicle_*` hooks (the vehicle fixes add
`qa_vehicle_place_player`, `qa_vehicle_place_actor`, `qa_vehicle_inside`, `qa_vehicle_push`,
`qa_vehicle_watch`, `qa_vehicle_stuck` and `qa_vehicle_throw`) and `tests/native/combat_qa.inc`
the grenade, item and sound hooks (both listed in `docs/macos-build.md`).
`tests/native/frontend_qa.inc` adds `qa_progress_state` (with a `QA_PROGRESS_NOTE` line for the
progress warning, kept apart because `conoutf` cuts lines at 260 characters, and a `QA_STATE` line:
the state block's header and its record counts), `qa_progress_reload`, `qa_loadout_state`, `qa_loadout_mark`,
`qa_title_state`, `qa_nav_state`, `qa_checkpoint_save`, `qa_hs_state` (the script's tick, started
threads, visible objects, the recovery's safe floors and grace, whether at a checkpoint or
seated) and `qa_hs_global NAME [VALUE]` for the title menu, saved games and nav points;
`retail_qa.inc` adds `qa_halo_moved`. Every QA hook that teleports the player calls
`haloplayermoved` as the engine's own moves do, except `qa_retail_recover`, which stands in for a
fall or a wedge. `qa_common.read_progress()` reads version 1 to 3 progress files (a version 3 state
block comes back as `state` and `records`, its record count and FNV-1a checked) and
`write_progress()` writes version 2 by default (`version=3` and `state_block()` for a state block).
All of these compile (`-DHALO_BUILD_QA=ON`); the new ones have not been run. Configure with `-DHALO_BUILD_QA=ON`. The hooks live in
`native/` and compile into `source/src/halo.cpp` under `HALO_QA`, because they
read that file's internal campaign state. The shipping `halo-ac` never contains
them.

## Tools

- `tools/compile-check-linux.sh` compiles and link-checks every target on Linux
  with public headers and stubbed macOS frameworks. Use it when no Mac is handy.
- `tools/b30_collision_route.py` regenerates the b30 routes in `data/` from the
  retail cache. Set `HALO_SOURCE` to the expanded Xbox map directory.
- `tools/hs_conditions.py` lists the calls inside script conditions per mission (`--all` for every call).
- `tools/inspect_hs.py` prints a mission's compiled HaloScript.
- `tools/capture_scenes.py` and `tools/capture_autumn_flyby.py` take screenshot
  sequences for comparison with Azahar captures.
- `tools/qa-key.swift` sends keystrokes to the app. `qa_common.qa_key()` builds it.
