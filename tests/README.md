# Tests

Verification for the macOS build. Each `test_*.py` is a standalone script that
exits non-zero on failure. Logs and throwaway profiles go to `build/qa/`.

```bash
./scripts/test-macos.sh            # build game + QA client, package, run the quick tier
./scripts/test-macos.sh --slow     # add the full-speed opening and keystroke tests
./scripts/test-macos.sh --release  # add the release tier: damaged assets, 55-section soak
python3 tests/run.py devices b30   # rerun by name once built (any tier)
./scripts/ci-macos.sh              # static checks, import check, build, package, quick tier
./scripts/ci-macos.sh --release --asan   # plus the release tier, also under ASan/UBSan
```

The quick tier takes 10 to 15 minutes. It needs the imported campaign assets
(`docs/macos-build.md`) and the AssaultCube runtime staged in `build/runtime`. The
release tier (`test_asset_robustness.py`, about 10 minutes, and `test_soak.py`, about 8)
adds damaged-asset fallbacks and memory over 110 section loads; with `--asan` each takes
about three times as long and needs `./scripts/build-macos.sh --asan` first (the
`HALO_SANITIZE` client in `build/macos-asan`). `scripts/ci-macos.sh` logs each step under
`build/ci/<time>/` (`docs/macos-build.md`, Robustness, sanitizers and CI).

Nothing has been run since commit `e0806ab`: the tests marked "Not yet run" below
are new, and the existing tests have not been re-run against the uncommitted work
since then (recovery, lifts and bridges, vehicles, grenades and equipment, the title
menu and progress file version 2, nav points, the vehicle fixes: bodies pushed out of
vehicles, routes round parked vehicles, vehicle stuck recovery, the Maw's escape taking over a
scenario Warthog, vehicles on lift decks; the recovery's reset on every move that is not walking
(`haloplayermoved`) and its one-second grace; progress file version 3, with which Continue
restores the mission script, vehicles, ground items and powerups; the corrected `jpt!` damage
layout with weapons.txt and equipment.txt format 2; and the damage fidelity work, weapons.txt
format 3 with range and blast falloff to the lower bound, actors' grenade rolls, EMP and exact
actor shots; factions and special enemies: the allegiance matrix, Hunters, stealth Elites, flyers,
the Monitor, Engineers and Keyes, actors.txt format 2 and the `materials` lines of weapons.txt and
equipment.txt; enemy and flying vehicles: vehicles.txt format 2, the Ghost, Wraith and Scorpion
driven, the flown Banshee, actors at the controls and in gun seats, `ai_go_to_vehicle` walkers,
dropship cargo, a30's Pelican boarding and c40 sections 1 and 4 flown on retail collision;
script fidelity: player `object_teleport`, `object_set_facing` and `volume_teleport_players_not_inside`,
scenes the scripts call mid-section (Call cue rows) and where they leave the player, a30's
extraction variant by the player's site, real `game_all_quiet`, `game_safe_to_speak` and volume
tests of actors, the carried-in section's in-memory checkpoint, the cleared progress warning and the
credits returning to the title menu; the blocked-sections batch: damage targets (HSCB version 8,
d40's manifold detectors), the walker's control, shoot and door rows (route flags 256, 512, 1024),
d40 section 5's manifold route and door leaves folded on the floor (c10); the presentation
batch: script effects, camera shake, cinematic screen effects, the flashlight, the
damage-direction indicator, sound class gains, vehicle engine and impact sounds, weapon zoom
sounds, the flamethrower's firing loop, the melee lunge, the rocket launcher's placement and the
fallback reticles; and the robustness and release batch: the ASan/UBSan build, the soak, the
damaged-asset test and the loader fixes behind it, the route metrics and stub-route gate and
`scripts/ci-macos.sh` (`test_route_metrics.py` alone has been run once, statically, to fill its
known-bad table; nothing else of that batch has run); see
"Not yet verified in the game" in
`docs/macos-build.md`). Expect first runs to need fixes in the tests as well as the
game. `test_campaign_progression.py`, `test_campaign_ending.py`, `test_maw_drive.py`,
`test_retail_collision.py`, `test_weapon_tags.py`, `test_flood.py` (its Flood spawns are on the
Flood team now) and `qa_common.py` were changed for that work and are equally unrun, as are the script fidelity changes to `test_campaign_tables.py` (Call and variant cue rows), `test_campaign_ending.py` (the title menu after the credits), `test_frontend_progress.py` (items 12 and 13), `test_script_coverage.py` (`set` values, fixed answers, item 3) and `tools/hs_conditions.py` (`set` values), and the new `test_mission_exits.py`. Every test that reads `weapons.txt` or `equipment.txt` needs them
regenerated (`tools/halo_weapons.py`: weapons.txt format 3, equipment.txt format 2); the game
refuses older tables and falls back to the stock AssaultCube weapons.

| Test | Checks |
| --- | --- |
| `test_campaign_tables.py` | C++ mission table and cinematic cues agree with the importer (exit variants with their vehicle, volume and site; Call rows may name any section, -1). No Mac or assets needed. The Call and variant rows are new and not yet run |
| `test_stock_smoke.py` | Unmodified AssaultCube still boots a stock map |
| `test_mission_openings.py` | Packaged app starts and skips all ten mission openings |
| `test_section_loads.py` | All 55 sections load with a live player |
| `test_campaign_progression.py` | Finishing a section advances the map and saves progress |
| `test_combat_replay.py` | Fire, reload, shield hit and recharge, death, revert, extraction |
| `test_devices.py` | Autumn door collision, first BSP switch, Control Room lift |
| `test_library_platform.py` | The Library's elevated platform carries the player |
| `test_b30_retail_collision.py` | Silent Cartographer route on retail BSP collision |
| `test_retail_collision.py` | Generic retail-collision walk (default `a10`), one playthrough per mission: for each section with a `packages/maps/<section>.hcol` the log says `loaded N retail collision surfaces`, the player spawns standing on retail floor, every spawn, marker, weapon, equipment placement and item has floor under it (vehicle placements, team 102, settle themselves and are skipped), the importer's `.route` is walked at full speed to the exit, and movement feels like stock geometry (speed, acceleration, jump within 10%). Driven sections (d40 6 to 8, and c40 1 and 4, which are flown) are skipped. See below |
| `test_retail_recovery.py` | On `halo_a10_01`: a drop 20 units below the retail mesh, a fall in the open past its edge and feet sunk into the floor each put the player back, alive and standing, within 4 units of where they stood (`Halo recovery:` in the log), with no revert. Then the teleport rule: `qa_halo_moved` (what every non-walking move calls, `haloplayermoved`) drops the safe floors (`QA_HS_STATE moved safe=0 grace=1`, `Halo moved: QA teleport`), a wedge right after it is still in the floor 1.1 s later (the one-second grace, then 0.75 s wedged) and nudged out by 3.3 s, and safe floors are recorded again. Not yet run (written while testing was on hold) |
| `test_route_metrics.py` | Stub-route gate, static (no game, no Mac): every imported section has `packages/maps/<section>.metrics.json` (written by every import, or by `tools/import_halo_campaign.py "$maps" --metrics-only`) in the current format, its mode (walked, raster, driven, flown, stock) matches the importer's tables, the maps and routes on disk are what the importer writes, and the sections whose route is a stub (under 100 steps, reach under 5% unless a walked route ends in its exit, exit offset over 25 source units, a walked route ending outside its exit volume) are exactly `KNOWN_STUBS` (8: a30 section 2, b30 section 3, b40 section 3, c10 sections 3, 5 and 6, c40 section 1, d40 section 8). A new stub fails (the table must not grow), and so does a listed one that is fixed. `--table` prints every section. Run once while writing it, to fill the table |
| `test_traversal_routes.py` | Route files only, no game: rows parse, lift deck rows (flag 16) and light-bridge rows (flag 128) name their device, each `LIFT_RIDES` section boards its lift on one deck row (the last, or the one before the first flag-4 row), `STOCK_SECTIONS` have no `.hcol` or `.route`, and `DRIVEN_SECTIONS` (d40's Warthog run, c40's flown sections 1 and 4) have an `.hcol` and no `.route`; no route row stands inside a vehicle parked at the section start (the map's team-102 placements without the script-created flag, boxed as `halo_vehicle.cpp` sizes them from `vehicles.txt`, widened by the body's radius); action rows (256 use, 512 shoot with an aim point, 1024 door) name their device and repeat the step row before them, d40 section 5 uses its four manifold controls and shoots the s1, n1 and n3 detectors, a10 sections 5 and 6 wait at `tunnel_door_2` and `cryo_door_2` on door rows. Not yet run |
| `test_campaign_replay.py` | All 55 sections complete in one run |
| `test_weapon_tags.py` | Retail weapon table matches known tag values and the engine's parsed copy. Damage columns (`jpt!` lower bound, upper bound min and max): rocket 80/300/330 high explosive, shotgun pellet 8/18/25, plasma rifle bolt 10/12/14, AR and sniper bullets flat (10, 101), human weapon melee 40/50/60, lower ≤ upper min ≤ upper max on every projectile. Format 3 columns: rocket blast inner radius 0.5 of 2, air damage range shotgun 1.5-3, plasma pistol and rifle 20-50, AR 5-15, sniper 1000-1000, the charged plasma bolt 5-20 with side effect 3 (EMP). The column checks are new and not yet run |
| `test_weapon_fire.py` | Rate of fire, zoom, overheat, needler ramp, charged shot, swap, revert, enemy weapons and drops |
| `test_combat_equipment.py` | `equipment.txt` has both grenade types (the plasma grenade sticks, its fuse starts at rest, its blast is EMP; both blasts 80 lower, 120-120 upper in the format 2 columns), the overshield, camouflage, health pack and grenade pickups and the AR ammo tied to its weapon; eight player weapons name a fire sound; section maps carry team-103 equipment placements and no stand-in boxes beside them. On a30 section 2: every table sound is decoded, the grenade switch needs grenades of the other type, a plasma grenade is thrown and explodes, the last one switches back to frag, dropped plasma grenade, health pack, overshield (charges to about three times the shield, takes the next hit, drains), camouflage and AR ammo are picked up, a revert restores two frags and no powerups. On a30 section 3: the Covenant carry and throw plasma grenades, a plasma grenade sticks to an actor, killed actors drop grenades, actors lose track of the camouflaged player. Not yet run (written while testing was on hold) |
| `test_damage_targets.py` | Damage targets. Every `.hscb` is version 8; with `HALO_SOURCE`, d40's eight manifold detectors are the only damage targets (vitality 125, a 1.1 x 1.6 x 0.24 box); `halo_d40_05.route` uses the four manifold controls (256 rows) and shoots the s1, n1 and n3 detectors (512 rows). On d40 section 5: a detector starts whole (`unit_get_health` 1), `object_cannot_take_damage` stops damage and `object_can_take_damage` lets it in, 50 points leave 0.6, a trace meets the target, a b detector shot to 0 destroys its manifold once `enc5_1_manager` runs (`manifold_s1_destroyed` 1) and the s1 detector is immune with its piston closed; then the player stands on the route's n1 use row, uses the control, stands on its shoot row, aims at the aim point and fires, and the detector loses health. Not yet run (written while testing was on hold) |
| `test_damage_model.py` | Damage fidelity. Tables: weapons.txt format 3, grenade blasts 80 lower, and every hostile actor variant's time to kill the player on Normal (shots from full shield and health) within 16% of the old integer slot damage at close range (`docs/macos-build.md`, Damage fidelity, has the table). On a30 section 2: `qa_damage_table` shows the shotgun pellet at 18/18/13/8/8 at 0, 1.5, 2.25, 3 and 7 world units, the plasma rifle bolt 12/11/10 at 20, 35 and 101, the pistol flat; both grenades 120 at the centre, 100 halfway, 80 at the outer radius; actors' grenade rolls inside [80, 120] and spread, the player's 120; `halofalloff` on fixed cases; exact hits carry their fraction (three 0.5 hits take one point and leave 0.5); a hit is taken by the shield first; EMP drains the shield, then the overshield and shield. On d40 section 3 (stock): an EMP holds a driven Warthog for its stun (under 1.5 units moved under full throttle), then it drives over 5 units. On a30 section 3: with `halo_damagetrace 1` and the player protected, every actor shot on the player is exact and inside its weapon's [lower, upper max] times the variant's modifier (at least 1), some fractional; actor grenade blasts on the player deal 80 to 240; the player's EMP hit drains a shielded actor's shield. Not yet run (written while testing was on hold) |
| `test_actor_tags.py` | Retail actor table matches known tag values and the engine's parsed copy |
| `test_actor_ai.py` | Hearing and squad alerts, burst fire, marines against the Covenant, squad positions, script AI commands, command lists; the unit and object-state commands (berserk, grenades, damage immunity, vitality, damage, follow, migrate, attach, renew, suspend, player vitality, hover, open, collideable, a50 cell doors) |
| `test_campaign_ending.py` | game_won on The Maw and the finale cue raise the ending and mark progress beaten (no game left to continue); credits end at the title menu (`QA_ENDING ... title=1`, new and not yet run); early game_won is ignored; game_lost reverts |
| `test_mission_exits.py` | Static (needs `HALO_SOURCE`, no Mac): every retail script that takes the camera (`cinematic_start` or `camera_control`) is a cue row of `halo_script.cpp`, is called from one, or is in its `NOT_CUED` list with the reason it plays as gameplay. QA client: each mission's last section plays its exit scene on `qa_halo_finale` (`Halo scene: <exit> start`), Enter skips it and Enter again starts the next mission's first section (`Halo: <mission>, section 1`); The Maw's finale raises the ending and the credits end at the title menu. a30's extraction follows the player's site: standing by `river_hide_flag`, `cliff_hide_flag` or `rubble_hide_flag` (`qa_halo_flag`) the exit plays that variant (`... chosen for the exit`). Scenes the scripts call mid-section (`qa_hs_cue`, as the script's call does): d20's `cinematic_captain` leaves the player at `player0_capt_done`, d40's `cinematic_bridge` at `player0_playon_base`, a50's `cutscene_rescue` then `ini_post_rescue` at `prison_player0_teleport` (`Halo script: player placed at <flag>`), and the section's exit does not replay them; a Call scene skipped at once still runs the rest of its script (b30's `cutscene_shaftb_goal` leaves `shafta_lock_door` open). Not yet run (written while testing was on hold) |
| `test_frontend_progress.py` | The title menu (`halo_title`) on a fresh profile; New Game on Heroic saves the game at a10's start (version 3, no state block); a checkpoint saves place, loadout, difficulty and the mission script's state (a state block whose count and hash hold), and a second launch's Continue restores them and the script where it was (`cryo_door_1`'s position, the global `delay_lost` set to 1234, a script clock no earlier than the checkpoint's), keeping the previous file as `.bak`; finishing a10 section 1 carries the loadout into section 2 and the save at its start keeps the carried script, which Continue resumes; a corrupt progress file is replaced by its backup (or reset without one) with a warning in the log and the title menu; version 1 files still load and unlock the missions reached; a state block with another `.hscb` hash (the Continue entry says the scripts changed) or a damaged record falls back to restarting the script at the checkpoint with a warning; a version 2 file with a checkpoint still continues there; a save at the wheel of b30's beach Warthog continues at the wheel (skipped without b30 or `vehicles.txt`); in live play a death in a10 section 2 (carried in from section 1) before its first checkpoint reverts to the carried script, not a restart (`HaloScript: a10 loaded` once, the script clock not back at zero); New Game after the corrupt-file warning clears it (`QA_TITLE ... note=[]`). Not yet run (written while testing was on hold) |
| `test_nav_points.py` | `hud.txt` has the waypoint bitmap and its ten arrow types; on a30 section 2 `activate_team_nav_point_flag` shows a nav point (raised by its offset), a second call replaces it, deactivating removes it; `hud_show_health` and `hud_blink_shield` hide and flash HUD parts; a revert brings back the checkpoint's nav points and HUD state. Not yet run (written while testing was on hold) |
| `test_script_coverage.py` | Every command used in a mission's sleep_until/if/cond/while conditions or in a `set` value is bound and not a fixed answer where the game has the state (`game_all_quiet`, `game_safe_to_speak`, actor volume tests), and so is every ai_, unit_, object_ and vehicle_ command any script calls, and every player-moving command; a10 looking tutorial finishes; d40 countdown follows the escape deadline and holds off endgame_cinematics. On a30 section 2: `game_all_quiet` and `game_safe_to_speak` answer 1 with nothing fighting or talking; `volume_test_objects` of `(list_get (players) 0)` matches `(players)`. The `set` values, the fixed-answer list and the a30 checks are new and not yet run |
| `test_difficulty.py` | halo_difficulty picks each squad's Easy to Impossible start count (a30 lz_search matches the importer table), game_difficulty_get returns it, enemy health and damage scale, the choice persists |
| `test_flood.py` | The squad cap counts only living actors (c10 swarm encounters place behind 128 dead entries); a killed Carrier Form bursts into 4 to 8 Infection Forms; an Infection Form on a dead marine raises a Flood combat form; Infection Forms leap. Its Flood spawns now name the Flood team (2); changed and not yet run |
| `test_factions.py` | Factions and special enemies. Tables: actors.txt format 2 with unit teams (Hunter 3, Monitor and Sentinel 5, Flood 4, marines 2), the Hunter's unit melee 35-45 and collision materials (shield arm and Monitor body multiplier 0, Hunter skin flagged head), the Monitor and Engineer as kinds 10 and 11, the stealth Elites' camouflage actv flag and the sword Elites' melee-only flag; weapons.txt and equipment.txt `materials` lines (assault rifle 0.5 on Hunter armour and 0 on its shield, pistol 0.2 and 1 on armour and skin, plasma rifle 2 on Sentinels, frag 0.25 on armour), and the engine's parsed copy. In `halo_preview`: a Covenant Elite, a Flood combat form and a Sentinel pick each other as targets; a Hunter loses nothing to a rifle hit on its shield arm, 25 to a pistol hit in the back, 5 to one in the side and 5 to a rifle blast while facing; two Hunters pair off and the survivor goes berserk; a Hunter at arm's length smashes; a calm stealth Elite cloaks above 0.85 and draws below 0.35 opacity, a hit halves its cloak, which recovers; plain Elites draw opaque; the shielded Sentinel spawns shielded; the Sentinel, Monitor and Engineer rise off the floor and hold their height; the Monitor and Engineer are passive, nothing hurts the Monitor, nobody targets it, the Engineer never fires; the Monitor and Keyes draw their own models. On a30 section 2: the script's player-human allegiance holds, `ai_allegiance`/`ai_allegiance_remove` covenant-flood change the matrix, killing two marines breaks player-human (`ai_allegiance_broken` 0 then 1, the marines turn hostile) and `ai_allegiance` renews it. c20 section 1 places 343 Guilty Spark (disregarded, undamageable, flying, passive); c10 section 1's mission script allies player and humans with the Sentinels. Not yet run (written while testing was on hold) |
| `test_vehicles.py` | `packages/halo/vehicles.txt` has the six vehicles (Warthog seats and gun, turret gunner seat, models); the section maps carry the scenario vehicle placements (d40's escape Warthogs in section 6 and not the engine room, the enc7_7 turrets, a50's 14 turrets, b30's unnamed Warthog); on b30 (retail collision) E by the driver door gets in, throttle drives, E gets out onto floor, a flip throws the driver out and E flips it back, the gunner seat fires; a50's turret is manned and fires; b30's script seat commands (`object_create`, `vehicle_load_magic`, `vehicle_test_seat_list`, `unit_in_vehicle`, `unit_exit_vehicle`) act on its jeep; a Warthog drives on stock geometry (d40 section 3). Vehicle fixes: a body put part way into, or into the middle of, b30's Warthog is pushed out; a Warthog rolled into the player never has the body inside it; `object_create jeep` over the player pushes them out; an actor put inside an a50 turret is pushed out alive; at the wheel, a drop below the mesh and a Warthog sunk into the beach are recovered to a safe spot with the driver still in it; a driver thrown out upside down with no room is put back in the seat by the fall recovery (no `player left in place`); a Warthog on c20's platform1 rises with it. Not yet run (written while testing was on hold) |
| `test_enemy_vehicles.py` | Enemy and flying vehicles. Tables: vehicles.txt format 2 (Ghost and Wraith `hover`, Scorpion `tank`, Banshee `flyer`, the Wraith `player 0`), the Banshee gun's bolt and fuel rod triggers and the Scorpion cannon's two, c40 sections 1 and 4 in `FLOWN_SECTIONS` with an `.hcol` and no `.route`. On b40 section 3 (retail collision): the parked Ghost, Banshee and Wraith report classes 3, 5 and 3; E drives a Ghost (`E  Drive the Ghost`) more than 6 units and its bolts fire, E gets out, the Wraith offers no seat; E flies a Banshee (`E  Fly the Banshee`) more than 3 world units up on the climb input, where it hovers (within 1.5) with no `Halo vehicle recovery`, its bolts and fuel rod fire, E in mid-air keeps the player in, descending lands it and E gets out; a spawned Scorpion drives, its cannon fires 1 to 3 rounds in 3 s held and the secondary key fires its machine gun; an Elite put at the controls of the Ghost drives it more than 4 units and one put in the Banshee climbs 3 world units and goes through more than one strafing-run phase, both vehicles firing (the player protected). On a30 section 3: `mission_rubble_wave_2` loads its squad into `rubble_rock_cship` (rider rows while it flies, its object row on a recording, then flown) and `vehicle_unload` sets them down; `mission_extraction_cliff` lands `foehammer_cliff`, makes it enterable, `qa_cargo_approach` shows `E  Board the Pelican`, E boards it (`halodriving` true) and the script's `game_won` ends the section. c40 sections 1 and 4 play on retail collision and report flown; section 1 keeps the scenario's `fly_away_1` (no Banshee of the engine's), section 4 gets one beside the player. Not yet run (written while testing was on hold) |
| `test_presentation.py` | Presentation. Tables: `effects.txt` format 1 with the cyborg's flashlight (radius 6, cone 0.349-0.785 rad), at least 15 script effects (the large explosion an explosion of scale 1.6 with its sound and light) and a `classes` line for each of the ten caches; `weapons.txt` names zoom-in and zoom-out sounds for the pistol, sniper rifle and rocket launcher and a `loop` (and no `fire`) for the flamethrower; `vehicles.txt` an engine loop for all six vehicles and the Warthog's crash and suspension sounds; all those sounds are decoded and in `packages/audio/halo/tags/classes.txt` (the Warthog's engine class 23). On a30 section 2: `halo_flashlight` turns the flashlight on (charge falling) and off (charge recovering); `qa_fx_effect` of the large explosion 3 units ahead is known, drawn and knocks the camera (`impulse=1`), an unknown tag falls back (`unknown=1`); `player_effect_set_max_rotation` and `player_effect_start` ramp the shake to 1 and `player_effect_stop` back to 0; `cinematic_screen_effect_start`, `_set_convolution` and `_set_filter` ramp the blur to 10 and desaturation and tint to 1, `_stop` clears them; `sound_class_set_gain` fades music to 0.25 over 30 ticks and the vehicle classes to 0 at once, leaving weapon fire at 1; `qa_fx_hit` raises one damage arc that is gone 1.2 s later; two zoom steps run. With two Grunts ahead: facing the nearer (between 4 and 7.2 units) the melee lunges onto it (`MELEE_QA lunge`), and the other's hit on the player raises the arc through `halodamage`. On c10 section 1: `effect_new` through the interpreter at `enc_swamp2_turret` draws the table's effect. On d40 section 3: a spawned Warthog driven by the player counts one engine loop, none once the player is out. Not yet run (written while testing was on hold) |
| `test_package_relocatable.py` | check-no-retail passes clean and fails on staged or stray retail files; packages/ is ignored by default; package-macos.sh rejects a stub assets dir; packaged app has no Homebrew paths, is signed, carries licences and launches from a copied location |
| `test_asset_robustness.py` | Release tier. 39 cases, each a mod directory under `build/qa/` mirroring `packages/` with one file missing or damaged: `hud.txt` (missing, random bytes, a negative or huge bitmap id, truncated), `weapons.txt` (missing, format 2, truncated, a `nan` projectile speed, random bytes), `equipment.txt`, `vehicles.txt` (missing, a box inside out, truncated), `actors.txt`, `halo_a30_01.hcol` (missing, truncated, random after the header, bit flips), `halo_a30_01.mesh` (missing), `a30.hscb` (missing, truncated, version 99, random after the header, bit flips), `halo_a30_01.route` for the QA walker (random bytes, a `nan` row, missing) and the progress file (random bytes, a bad check, a `nan` checkpoint, out-of-range values, a NUL byte, over 32 MB, a state block of nonsense with valid hashes). Section cases load a30 section 2 and run `qa_robust_exercise`; each needs a clean exit after `ROBUST_QA end`, the case's fallback log line and the fallback in the ROBUST_QA row (hud=0, weapons=0, grenades=00, vehicledefs=1, actors=0, collision=0, script=0), the player alive at the end, and no sanitizer report. Flip cases only need a clean run. `--asan` for the sanitized client, `--list`, name filters. Not yet run |
| `test_soak.py` | Release tier. One QA client process loads all 55 sections in order, twice (`--passes`), each running 1.5 s once live (4 s with `--asan`), and logs memory after each (`SOAK_QA`); fails on a crash, hang or exit code, any ASan, UBSan or LSan report, a section that does not load or leaves the player dead, or footprint growth from pass 2 on (median per section over 64 MB a pass, `--growth-mb`, or the end of the last pass that much per extra pass above the end of pass 1). Writes `build/qa/soak-memory.csv`. Not yet run |
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

Control, shoot and door rows (written while testing was on hold, not yet run): a row with flag 256
(`device@position`) makes the walker stand on it and press use every 1.5 s until the machine named
stands at that position (or 20 s pass: a script may hold the control), 512 (`target@health ax ay az`)
makes it aim from its eye at the aim point and fire short bursts until the damage target's
`unit_get_health` is at most that health, clearing hostiles within 30 units every 5 s and refilling
the magazine as it runs dry (`FAIL target-not-destroyed` after 45 s), and 1024 (`door@position`)
makes it stand at a scripted door, press use every 6 s, look round, and log the door's power and
position with `halo_script_status` every 20 s until it is open that far (`FAIL door-never-opened`
after `HALOQA_RETAIL_WAIT`). Each prints `RETAIL_QA act ...` lines under the section's result. A
route longer than 3,000 rows gets 90 ms more per row on the walker's deadline and the test's timeout
(d40 section 5 is 5,085 rows).

Sections in `KNOWN_BLOCKED` (a10 sections 5 and 6, which wait at script-gated doors; b40 section
6, c10 sections 1 and 4 and c20 sections 1 to 3, which end on or include a lift ride; d40 section
5, whose fourth manifold, s3, no walkable spot can see; see `docs/macos-build.md`) are walked and
printed as `KNOWN` with the reason instead of failing; the test fails once one of them passes, so
the list stays current. `PENDING_WALK` names the sections the lift, bridge and door machinery is
expected to make pass, which have not been walked yet: a10 sections 5 and 6 (door rows), b40
section 6, c10 sections 1, 2 (new on retail collision) and 4, and c20 sections 1 to 3. They are printed as `expected to pass, pending walk`; a pending section that is
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
`tests/native/damage_qa.inc` has the damage model hooks for `test_damage_model.py`:
`qa_damage_table` (each projectile's damage at points of its damage range, each grenade's blast
across its radius and 400 rolls of each thrower's range, `halofalloff` cases), `qa_damage_vitals
HEALTH SHIELD`, `qa_damage_hit AMOUNT SIDE ACTOR PLASMA` (an exact hit on the player, from the
nearest hostile actor when ACTOR is 1), `qa_damage_actor AMOUNT SIDE LABEL` (the player's exact hit
on the nearest shielded hostile), `qa_damage_actors` (each actor's weapon and damage modifier) and
`qa_damage_emp_vehicle VEHICLE MS`; the damage target hooks for `test_damage_targets.py` are
`qa_target_state NAME LABEL` (`TARGET_QA LABEL name= found= health= damageable= centre=`),
`qa_target_trace NAME` (a trace from the eye through the target: the first target it meets),
`qa_target_damage NAME AMOUNT` (the player's damage dealt to it, `dealt=`), `qa_target_look NAME`
and `qa_target_aim X Y Z` (face the target, or a world point such as a shoot row's aim point). The game's `halo_damagetrace 1` logs every hit (`DAMAGE_TRACE`),
and `qa_vehicle_state` rows carry `stunned=` (ms of EMP stun left).
`vehicle_qa.inc` also has the enemy and flying vehicle hooks for `test_enemy_vehicles.py`:
`qa_vehicle_flight THROTTLE STRAFE CLIMB MS`, `qa_vehicle_look YAW PITCH`, `qa_vehicle_secondary 0|1`,
`qa_vehicle_pilot VEHICLE KIND TEAM SEAT` (a new actor of that kind, or with KIND -1 the nearest
living one, at the controls, with magic sight; `VEHICLE_QA pilot ... ok= armed=`), `qa_vehicle_flown`,
`qa_cargo_state LABEL` (`CARGO_QA` rows: each rider or walker of a dropship or Pelican, and each
visible script vehicle with `recording`, `flown`, `boardable`, `seats` and `model`), `qa_cargo_approach
OBJECT` (the player beside its first rider seat, with the board prompt) and `qa_cargo_board` (what E
does on foot); `qa_vehicle_state` rows add `class`, `pilot`, `phase`, `target`, `carried` and `shots`.
`tests/native/faction_qa.inc` has the faction hooks for `test_factions.py`: `qa_variant_spawn
"PATH TEXT" TEAM FORWARD SIDE` (a variant by its actv path, on an engine team or -1 for its own),
`qa_allegiance_state LABEL` (`FACTION_ALLEGIANCE` per team pair), `qa_actor_turn KIND DEGREES`
(0 faces the player, 180 its back; holds them braindead), `qa_actor_hit KIND WEAPON AMOUNT` and
`qa_actor_blast KIND WEAPON AMOUNT` (the player's exact hit with a weapon's `jpt!` as the damage
source), `qa_weapon_materials WEAPON`, `qa_actor_cloak` (`FACTION_CLOAK`), `qa_actor_model` and
`qa_actor_kill_one KIND`; `qa_actor_state` adds an `ACTOR_FACTION` row per actor (side, friendly,
flying, passive, cloak, Hunter partner and smashes, altitude over the floor), and `qa_actor_spawn`
takes the engine teams (-1 the body's own; hostiles of any team face the player).
`tests/native/frontend_qa.inc` adds `qa_progress_state` (with a `QA_PROGRESS_NOTE` line for the
progress warning, kept apart because `conoutf` cuts lines at 260 characters, and a `QA_STATE` line:
the state block's header and its record counts), `qa_progress_reload`, `qa_loadout_state`, `qa_loadout_mark`,
`qa_title_state`, `qa_nav_state`, `qa_checkpoint_save`, `qa_hs_state` (the script's tick, started
threads, visible objects, the recovery's safe floors and grace, whether at a checkpoint or
seated) and `qa_hs_global NAME [VALUE]` for the title menu, saved games and nav points;
`retail_qa.inc` adds `qa_halo_moved`.
`tests/native/soak_qa.inc` has the soak and robustness hooks: `qa_soak PASSES SETTLE` (every
section in order, `SOAK_QA pass= index= map= alive= stage= load= rss= peak= footprint=` rows,
`SOAK_QA pass-end`, `SOAK_QA FAIL timeout`, `SOAK_QA done` and a quit), `qa_soak_timeout MS`,
`qa_mem_state LABEL` (`MEM_QA`), `qa_robust_state LABEL` (`ROBUST_QA LABEL map= active= alive= hud=
weapons= equipment= grenades= vehicledefs= vehicles= actors= collision= script= enemies=`) and
`qa_robust_exercise` (`ROBUST_QA exercised` when done); memory comes from
`tests/native/qa_memory.cpp` (Mach `task_info`, its own translation unit).
`tests/native/presentation_qa.inc` has the presentation hooks for `test_presentation.py`:
`qa_fx_state LABEL` (an `FX_QA LABEL` line from `halofxdump`: effects counted, drawn and unknown,
the last effect's tag with `_` for spaces, the shake level, a knock in progress, the camera offset
in degrees, the screen effect state, blur, desaturation and tint, the video mode, the flashlight
and its charge, damage arcs showing, and the music, ambient_nature, vehicle_engine, weapon_fire and
unit_dialog class gains; and an `FX_VEHICLE LABEL engines=N` line), `qa_fx_effect "TAG" DISTANCE`
(an effect that many world units ahead), `qa_fx_script_effect "TAG" FLAG` (`effect_new` through
the interpreter at a cutscene flag), `qa_fx_hit DEGREES DISTANCE` (a damage arc for a hit from that
bearing), `qa_fx_actor_hit AMOUNT` (the nearest actor hits the player through `halodamage`) and
`qa_fx_face_actor` (turns to the nearest hostile actor, `FX_QA face NAME distance=`). The game's
`halo_fxtrace 1` logs effects and class fades and `halo_meleetrace 1` the lunge (`MELEE_QA lunge
target= distance=`). Every QA hook that teleports the player calls
`haloplayermoved` as the engine's own moves do, except `qa_retail_recover`, which stands in for a
fall or a wedge. `qa_common.read_progress()` reads version 1 to 3 progress files (a version 3 state
block comes back as `state` and `records`, its record count and FNV-1a checked) and
`write_progress()` writes version 2 by default (`version=3` and `state_block()` for a state block).
All of these compile (`-DHALO_BUILD_QA=ON`); the new ones have not been run. Configure with `-DHALO_BUILD_QA=ON`. The hooks live in
`native/` and compile into `source/src/halo.cpp` under `HALO_QA`, because they
read that file's internal campaign state. The shipping `halo-ac` never contains
them.

## Tools

Under `tests/tools/`:

- `tests/tools/compile-check-linux.sh` compiles and link-checks every target on Linux
  with public headers and stubbed macOS frameworks. Use it when no Mac is handy.
- `tests/tools/b30_collision_route.py` regenerates the b30 routes in `data/` from the
  retail cache. Set `HALO_SOURCE` to the expanded Xbox map directory.
- `tests/tools/hs_conditions.py` lists the calls inside script conditions per mission (`--all` for every call).
- `tests/tools/inspect_hs.py` prints a mission's compiled HaloScript.
- `tests/tools/capture_scenes.py` and `tests/tools/capture_autumn_flyby.py` take screenshot
  sequences for comparison with Azahar captures.
- `tests/tools/qa-key.swift` sends keystrokes to the app. `qa_common.qa_key()` builds it.

## Test plan for when testing resumes

Nothing below has been run since commit `e0806ab` except `test_route_metrics.py` (run once,
statically, while writing it). Expect first runs to need fixes in the tests as well as in the
game. Work down the list; each step names what a failure there most likely means. Logs are in
`build/qa/` (and `build/ci/<time>/` when run through `scripts/ci-macos.sh`).

0. **Assets current.** `packages/halo/*.hscb` are version 8, `weapons.txt` format 3,
   `equipment.txt`, `vehicles.txt` and `actors.txt` format 2, `effects.txt` format 1, and
   `python3 tools/import_halo_campaign.py "$maps" --metrics-only` reports every map current
   (this batch's run did). Re-run `scripts/import-macos-assets.sh "$maps"` after pulling importer
   changes.
1. **Static gates** (no game, minutes): `./scripts/ci-macos.sh --no-tests` (shell and Python
   syntax, `check-no-retail.sh`, the import check, `test_campaign_tables.py`, the route gate,
   the package build with its 55-section verification, and the no-new-warnings rebuild). Then
   the other static tests, `python3 tests/run.py tables traversal route_metrics`. The route gate
   must list exactly its eight known stubs.
2. **Boot and load** (the first in-game checks): `test_stock_smoke.py`, `test_mission_openings.py`,
   `test_section_loads.py` (all 55 sections live), `test_campaign_progression.py`,
   `test_package_relocatable.py`. A failure here blocks everything after it.
3. **Robustness and release**: `python3 tests/test_asset_robustness.py` (39 cases; start with
   `baseline`, which must pass before the others mean anything), then
   `python3 tests/test_soak.py`. Then `./scripts/build-macos.sh --asan` and both again with
   `--asan`; triage any UBSan report in a stock engine file into `tests/data/ubsan.supp` only
   if it is understood and harmless, and fix reports in `halo_*` files. Record the soak's
   pass-1 and pass-2 footprints and the slowest load in this guide.
4. **Movement and traversal on retail collision**: `test_retail_recovery.py`, then
   `python3 tests/test_retail_collision.py a10 a30 a50 b40 c10 c20 c40 d20 d40` (one walk per
   mission, about 8 minutes a section at worst). Movement feel must hold (speed, acceleration
   and jump within 10% of stock; the constants are fixed). Update `KNOWN_BLOCKED` and
   `PENDING_WALK` from the results: a10 sections 5 and 6 (door rows), b40 section 6, c10
   sections 1, 2 and 4 and c20 sections 1 to 3 (lift and bridge rows) are expected to pass;
   d40 section 5 is expected to stay blocked at s3. Walk the two stubs the route gate found,
   `--section a30:2` and `--section b40:3`, to see how far they get. `test_b30_retail_collision.py`
   for the Silent Cartographer.
5. **Combat**: `test_combat_replay.py`, `test_weapon_tags.py`, `test_weapon_fire.py`,
   `test_combat_equipment.py`, `test_damage_model.py` (check the time-to-kill table against the
   build guide), `test_damage_targets.py`, `test_actor_tags.py`, `test_actor_ai.py`,
   `test_factions.py`, `test_flood.py`, `test_difficulty.py`.
6. **Vehicles**: `test_vehicles.py`, `test_enemy_vehicles.py`, then the slow
   `test_maw_drive.py` (the Maw's escape to the finale under the countdown).
7. **Scripts, scenes and presentation**: `test_script_coverage.py`, `test_mission_exits.py`,
   `test_campaign_ending.py`, `test_nav_points.py`, `test_presentation.py`, `test_devices.py`,
   `test_library_platform.py`.
8. **Saved games and the front end**: `test_frontend_progress.py` (title menu, Continue with the
   version 3 state block, fallbacks, backup), `test_campaign_replay.py` (all 55 sections in one run).
9. **Slow and manual**: `python3 tests/run.py --slow` (`test_autumn_opening.py`,
   `test_package_input.py`, `test_maw_drive.py`), then a played pass in the packaged app: a new
   game on Normal through a10 and a30 at least, a Continue after quitting at a checkpoint, the
   flown c40 sections, the driven Maw escape, and the presentation items no test can judge (the
   flashlight's look, camera shake, screen effects, the rocket launcher's placement, the fallback
   reticle, the stealth Elites' shimmer). Note what was seen in `docs/macos-build.md` and move
   each verified item out of "Not yet verified in the game" and into the Status table.
10. **The whole chain**: `./scripts/ci-macos.sh --release --asan` once everything above passes,
    as the release check.
