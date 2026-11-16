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

Nothing has been run since commit `e0806ab`. Every change since then is listed, with the tests and
QA hooks that check it, in `docs/macos-build.md` ("Not yet verified in the game": the earlier work
by area) and `docs/unverified-changes.md` (batch 5 onwards, batch by batch). The tests marked "Not
yet run" below are new, and the existing tests have not been re-run against that work. Expect first
runs to need fixes in the tests as well as the game. Changed for that work and equally unrun:
`test_campaign_progression.py`, `test_campaign_ending.py` (also the title menu after the credits),
`test_maw_drive.py`, `test_retail_collision.py`, `test_weapon_tags.py`, `test_flood.py` (its Flood
spawns are on the Flood team now), `test_combat_equipment.py` (a30's frag counts), `qa_common.py`,
the script fidelity changes to `test_campaign_tables.py` (Call and variant cue rows),
`test_frontend_progress.py` (items 12 and 13), `test_script_coverage.py` (`set` values, fixed
answers, item 3) and `tests/tools/hs_conditions.py` (`set` values), and the new `test_mission_exits.py`.
Every test that reads `weapons.txt` or `equipment.txt` needs them regenerated
(`tools/halo_weapons.py`: weapons.txt format 3, equipment.txt format 2); the game refuses older
tables and falls back to the stock AssaultCube weapons.

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
| `test_b30_retail_collision.py` | Silent Cartographer route on retail BSP collision: its own waypoints (`tests/data/b30-collision-qa-route.txt`) down shaft A in section 2, traces, an authored checkpoint and revert, the Cartographer's control and scene. Since the route batch sections 1 and 2 have planned routes (no longer `RASTER_ROUTES`) on the same layout, so this is the regression check of that layout; `test_retail_collision.py b30` walks the planned routes |
| `test_retail_collision.py` | Generic retail-collision walk (default `a10`), one playthrough per mission: for each section with a `packages/maps/<section>.hcol` the log says `loaded N retail collision surfaces`, the player spawns standing on retail floor, every spawn, marker, weapon, equipment placement and item has floor under it (vehicle placements, team 102, settle themselves and are skipped), the importer's `.route` is walked at full speed to the exit, and movement feels like stock geometry (speed, acceleration, jump within 10%). Driven and flown sections (d40 6 to 8, c40 1 and 4) are walked by the vehicle walker on their planned vehicle routes (`VEHICLE_ROUTES`, batch 7, not yet run; movement feel is not required of them, c40 1 and d40 8 are `KNOWN_BLOCKED`, see Vehicle rows below). a30's walk starts at the Crash Site (`halo_a30`, walked as `a30:0`) since it is imported from the retail BSP (`CRASH_SITE`, batch 6, not yet run). See below |
| `test_retail_recovery.py` | On `halo_a10_01`: a drop 20 units below the retail mesh, a fall in the open past its edge and feet sunk into the floor each put the player back, alive and standing, within 4 units of where they stood (`Halo recovery:` in the log), with no revert. Then the teleport rule: `qa_halo_moved` (what every non-walking move calls, `haloplayermoved`) drops the safe floors (`QA_HS_STATE moved safe=0 grace=1`, `Halo moved: QA teleport`), a wedge right after it is still in the floor 1.1 s later (the one-second grace, then 0.75 s wedged) and nudged out by 3.3 s, and safe floors are recorded again. Not yet run (written while testing was on hold) Batch 9: the drops start 4 s into live play, once Heroic's `fast_setup` has let the player out of the cryotube (`CARGO_QA seat ... boarded=-`). |
| `test_route_metrics.py` | Stub-route gate, static (no game, no Mac): every imported section has `packages/maps/<section>.metrics.json` (written by every import, or by `tools/import_halo_campaign.py "$maps" --metrics-only`) in the current format, its mode (walked, raster, driven, flown, stock) matches the importer's tables, the maps and routes on disk are what the importer writes (and, since the jump governor batch, the generated map `.cfg`: a metrics file whose `cfg_current` is false fails; since the ownerless-kill batch one without the field counts as stale too, and the metrics format is 2, so metrics from before the jump governors fail the format check), and the sections whose route is a stub (under 100 steps, reach under 5% unless a walked route ends in its exit, exit offset over 25 source units, a walked route ending outside its exit volume) are exactly `KNOWN_STUBS` (empty since the c10 section 5 batch, which took the last one, c10 section 5, out: it is walked on retail collision now, round `bridge_c` up the west bay's debris pile over one walking assist; the route batch took c40 section 1 out, its planned route now reaching `bsp 2,10` through `door_c1`, and the scenery batch c10 section 6; batch 10 took d40 section 8 out, its planned drive now reaching `grand_finale`; batch 5 fixed a30 section 2, b30 section 3 and b40 section 3, and b30 section 3's mode is now walked: `RASTER_ROUTES` kept b30 sections 1 and 2 on the raster until the route batch, and is empty now; batch 6 took c10 section 3 out, now walked over two walking assists). With `CRASH_SITE` the Crash Site's `halo_a30.metrics.json` is checked too, as a walked section (batch 6, not yet run). A new stub fails (the table must not grow), and so does a listed one that is fixed. `--table` prints every section. A driven or flown section with a planned vehicle route (`vehicle_route` in its metrics, batch 7) is judged as a walked one; c40 sections 1 (since the route batch) and 4 and d40 sections 6 to 8 are not stubs. Run once while writing it, to fill the table; the batch 7 changes have not been run |
| `test_traversal_routes.py` | Route files only, no game: rows parse, lift deck rows (flag 16) and light-bridge rows (flag 128) name their device, each `LIFT_RIDES` section boards its lift on one deck row (the last, or the one before the first flag-4 row), `STOCK_SECTIONS` (empty since the d40 section 3 batch) have no `.hcol` or `.route`, d40 section 3 has an `.hcol` and a walked route of plain rows on the upper level, from its anchor into `bsp 2,3` (the d40 section 3 batch), and `DRIVEN_SECTIONS` (d40's Warthog run, c40's flown sections 1 and 4) have an `.hcol` and a `.route` exactly when `VEHICLE_ROUTES` plans one, whose vehicle rows (4096, 8192, 16384) name a vehicle type, a driven route all Warthog rows with steps of one cell but at a jump (flag 2), a flown one boarding a Banshee before its first flight row with flight rows at most 8 units apart and every get-out row after a flight row, and whose metrics record it (`vehicle_route`, c40 section 1's extra BSP 12) (batch 7, not yet run; since the route batch its route boards and gets out twice, walks between its first landing and second boarding and ends in its exit), b30 sections 1 and 2 are walked routes ending in their exits (section 2's door row at `shafta_lock_door` before its use row) and d40 section 5 has three to five throw rows whose metrics' `throws` all go off beyond the frag's outer radius from the thrower (the route batch, not yet run), and the Maw's planned drive of sections 6 to 8 (`vehicle_route.drive.seconds`, about 88 s) takes at most two thirds of the 180 s escape countdown (batch 10, not yet run); no route row stands inside a vehicle parked at the section start (the map's team-102 placements without the script-created flag, boxed as `halo_vehicle.cpp` sizes them from `vehicles.txt`, widened by the body's radius); action rows (256 use, 512 shoot with an aim point, 1024 door, 2048 throw with an aim point) name their device and repeat the step row before them, d40 section 5 uses its four manifold controls, shoots the s1, n1 and n3 detectors and throws at s3's from one spot after the s3 control (consecutive throw rows), a10 sections 5 and 6 wait at `tunnel_door_2` and `cryo_door_2` on door rows, a10 section 5's metrics show the route passing `tunnel_trigger_close` before `tunnel_door_2`'s row (`door_order`), and b30's return is a walked route with no action rows that ends at the Pelican's ledge (its metrics). Batch 6 (not yet run): a section's `.hcol` has an AST1 trailer exactly when `ASSISTS` lists it, naming its last surfaces, each two-sided and walkable; the metrics count them and the route has a row on each; with `CRASH_SITE`, `halo_a30` has an `.hcol` and a route of plain, crouch, jump and ledge-drop rows. Not yet run |
| `test_retail_assists.py` | Walking assists (`ASSISTS` in `tools/import_halo_campaign.py`, the `.hcol`'s AST1 trailer; c10 sections 3, 5 and 6 today). For each listed section started as campaign play: the load line counts its walking assists (`(M static machines, K walking assists)`); `qa_retail_assists` reports for each that a ray down through its centre does not stop on it (shots, sight lines and probes pass), a body's capsule swept onto it stands on it, and a grenade-sized sweep passes through; `qa_retail_assist_stand` drops the player onto each, and `qa_retail_where` finds them alive, on the floor, within half a unit of its height. Batch 6, not yet run |
| `test_retail_scenery.py` | Scenery collision (`SCENERY_COLLISION` in `tools/import_halo_campaign.py`, the `.hcol`'s SCN1 trailer; every section on retail collision since the batch after the c10 section 5 batch, 54 with the Crash Site). Without the game: every `.hcol`'s trailers are in order (DEV1, SCN1, AST1), a section outside the table has no SCN1, a listed section's SCN1 matches its metrics' `scenery` count with ascending, non-overlapping surface ranges inside the file, and the Crash Site's mapmodel angles (`mapmodel_angles`) rebuilt through a re-implementation of `md3.h`'s quaternion path are within 0.6 degrees of Halo's rotation (`euler_rows`, y reflected) over 5,000 random angles, with attr6 in its signed byte. In the game, for each listed section with placements (b40 sections 4 and 6, c40 section 5 and d40 section 6 have none and are checked statically only) started as campaign play: the load line counts its scenery (`, S scenery objects (T surfaces)` after the machines-and-assists part), and `qa_retail_scenery` reports for each placement that a ray, a grenade-sized sweep and a body's capsule coming down onto the top of its bounds stop on a scenery surface; at most a tenth may miss (thin placements: a pine's trunk under its crown's bounds), each listed; 50 sections, so it takes a while. Scenery batch and the batch after the c10 section 5 batch, not yet run |
| `test_scenery_uses.py` | The use-aware scenery rule (`object_uses` in `tools/import_halo_scripts.py`, `scenery_scripted` in `tools/halo_collision.py`). No game; needs the expanded caches (`HALO_SOURCE`; skipped without them). The uses each decision rests on (a30's `pod_of_death` only as `objects_attach`'s parent, a50's `field_generator_a` and b40's `pelican_crashed` only in `objects_predict`, b30's `shaft_box_1` to `_3` only in `cutscene_extraction_exit`, c10's `crashed_dropship` and `x50_rock` only in `ini_x50_preclean`, a10's `field_x30` only in `mission_final`), those placements counted as baked with the importer's `SCENERY_SCRIPTS_AT_END` and `SCENERY_KEPT`, and listed in the SCN1 trailers of the Crash Site, a10 section 7, a50 section 1, b30 sections 1 and 3 and c10 section 1; a10's `bridge_barricade_1`, d40's `fighter_hangar` and b30's `rock_kick` still scripted; an unlisted function, a `set`, a global's value and a child of `objects_attach` count as changes; the Crash Site has no mapmodel for `intro_tree`. The use-aware rule's batch, not yet run |
| `test_scene_meshes.py` | Every campaign `halo_scene_*.mesh` and its textures match a fresh export, and (the MD3 range fix) every cinematic cast export the scripts use is on disk and in its `.hscb`, with no orphan directory under `packages/models/halo/scene/<map>/`: runs `tools/import_halo_scripts.py "$HALO_SOURCE" --check-scenes` (`stale_scene_meshes` and `check_scenes`, a minute or two, writes nothing under `packages/`). No game; needs the caches (skipped without them). A failure lists the stale files, missing exports, orphans or `.hscb` paths: re-run the import for that mission (`--rebuild` for stale meshes). The use-aware rule's batch, not yet run (the tool itself was run: all ten current, and after the MD3 range fix all ten with their cast) |
| `test_md3_range.py` | Cinematic MD3 exports past the 16-bit vertex range (the MD3 range fix). No game, no caches: a synthetic model whose frames fit packs byte for byte as `pack_vertex` does at mdlscale 100; moved 600 units in a frame it keeps mdlscale 100 with each frame named `halo_origin` and its box centre as `local_origin`, and decodes as `md3.h` loads it (origin added, then `scaleverts`) to 4 x the posed position within a vertex step; one frame 300 units across gets the smallest larger mdlscale that fits; an export left with no surface removes the directory it created (not one it found); `prune_scene_models` removes only unowned export-named directories. With the generated assets: the 27 restored clips are in their `.hscb`, every clip model the `.hscb` names has `tris.md3` and an integer mdlscale, no scene model directory lacks `tris.md3`. Not yet run |
| `test_content_currency.py` | Map and scene-texture currency compare content, not compressed bytes (the content-currency fix). No game, no caches: `cgz_disk_map` (`tools/import_halo_campaign.py`, what `cgz_current` compares) gives back the same map from a `.cgz` gzipped at levels 9, 1 and 6 with another header mtime, and None (stale) for an empty, truncated, stream-damaged, bad-CRC, non-gzip or missing file; `png_content` (`tools/halo_map_art.py`, what `stale_scene_meshes` compares textures by) finds the same image recompressed at another level with its IDAT split in two equal, and gives None for a truncated PNG, a bad chunk CRC, a cut stream, no IEND and a non-PNG; `write_png` leaves a file with the same content untouched (mtime and bytes, also one another zlib wrote) and rewrites one whose pixels or size changed, or that is damaged. With metrics on disk, every `packages/maps/*.metrics.json` has `cgz_raw_sha1` beside `cgz_sha1`, and where `cgz_current` is true the gunzipped `.cgz` hashes to it. Not yet run |
| `test_scenery_raster.py` | The cube-raster stamp (`scenery_raster` in `tools/halo_collision.py`: the baked scenery in each section's `.cgz` cube raster, which AssaultCube's flood waypoints, its sight checks on stock geometry and the stock fallback read). Without the game, for all 55 sections: the metrics' `raster` record is there and its `scenery_solid` is the number of solid cubes marked with `RASTER_SCENERY_VDELTA` in the `.cgz` (none, and nothing raised or kept, where the `.hcol` has no SCN1); no route row stands in a solid cube but rows with no native floor at their cube (lift decks, flights, exit falls, vehicle legs off the raster), never in a scenery cube; no actor, weapon, equipment, start, marker, light, item or mapmodel entity is in a solid cube (a vehicle may be, where its retail point's cube has no floor, and in a driven or flown section a start, marker or light on a vehicle route cell with none), the counts matching the metrics; and, independently of the importer's code, the SCN1 polygons that are not walkable tops, sampled at their centroids and at vertices pulled a fifth of the way in, fall in solid cubes or above or below the body's column (2 to 5.2 units over the cube's floor), but for cubes a route row or entity stands in, with at most 0.5% of a section's samples missing (listed). In the game, for seven sections with a lot of scenery (the Crash Site, a30 section 3, a50 section 3, b30 section 1, c10 section 3, d20 section 2, d40 section 3) on retail collision, once the flood is done, `qa_raster_waypoints`: the engine holds the marked cubes the metrics count and lets light through them (`lightpass=1`), no flood waypoint is in a solid cube, and at most 1% (or 2) have a scenery top or underside in the body's column at their cube's centre. The cube-raster stamp, not yet run |
| `test_campaign_replay.py` | All 55 sections complete in one run |
| `test_weapon_tags.py` | Retail weapon table matches known tag values and the engine's parsed copy. Damage columns (`jpt!` lower bound, upper bound min and max): rocket 80/300/330 high explosive, shotgun pellet 8/18/25, plasma rifle bolt 10/12/14, AR and sniper bullets flat (10, 101), human weapon melee 40/50/60, lower ≤ upper min ≤ upper max on every projectile. Format 3 columns: rocket blast inner radius 0.5 of 2, air damage range shotgun 1.5-3, plasma pistol and rifle 20-50, AR 5-15, sniper 1000-1000, the charged plasma bolt 5-20 with side effect 3 (EMP). The column checks are new and not yet run |
| `test_weapon_fire.py` | Rate of fire, zoom, overheat, needler ramp, charged shot, swap, revert, enemy weapons and drops |
| `test_combat_equipment.py` | `equipment.txt` has both grenade types (the plasma grenade sticks, its fuse starts at rest, its blast is EMP; both blasts 80 lower, 120-120 upper in the format 2 columns), the overshield, camouflage, health pack and grenade pickups and the AR ammo tied to its weapon; eight player weapons name a fire sound; section maps carry team-103 equipment placements and no stand-in boxes beside them. On a30 section 2: every table sound is decoded, the grenade switch needs grenades of the other type, a plasma grenade is thrown and explodes, the last one switches back to frag, dropped plasma grenade, health pack, overshield (charges to about three times the shield, takes the next hit, drains), camouflage and AR ammo are picked up, a revert restores the start's frags (none with a30's starting profile, batch 8; the test gives two before the grenade switches; two with an older `.hscb`) and no powerups. On a30 section 3: the Covenant carry and throw plasma grenades, a plasma grenade sticks to an actor, killed actors drop grenades, actors lose track of the camouflaged player. Not yet run (written while testing was on hold) |
| `test_damage_targets.py` | Damage targets. Every `.hscb` is version 8 or 9; with `HALO_SOURCE`, d40's eight manifold detectors are the only damage targets (vitality 125, a 1.1 x 1.6 x 0.24 box); `halo_d40_05.route` uses the four manifold controls (256 rows) and shoots the s1, n1 and n3 detectors (512 rows). On d40 section 5: a detector starts whole (`unit_get_health` 1), `object_cannot_take_damage` stops damage and `object_can_take_damage` lets it in, 50 points leave 1 - 50 / (125 x Heroic's enemy_vitality from actors.txt) (0.667; 0.6 before the difficulty-table work), a trace meets the target, a b detector shot to 0 (200 points, past Heroic's 150) destroys its manifold once `enc5_1_manager` runs (`manifold_s1_destroyed` 1) and the s1 detector is immune with its piston closed; then the player stands on the route's n1 use row, uses the control, stands on its shoot row, aims at the aim point and fires, and the detector loses health. Not yet run (written while testing was on hold) |
| `test_frag_throw.py` | d40 section 5's planned frag throws at s3 (`ThrowTarget`, route flag 2048), in the game without the walk. `halo_d40_05.route` has throw rows, consecutive at one spot after the `enc5_1_pistons3` use row, naming s3 or s3b with an aim point. On d40 section 5 with `enc5_1_manager` running: the player uses the s3 control from its use row, then from the throw spot throws two frags at each planned aim (`qa_grenade_throw` after `qa_target_aim`), logging where each frag lay before it went off (`GRENADE throwN_M`) and both detectors' health; passes when one detector is down to 0.1. A failure prints the frag positions to compare with the importer's simulation (`GrenadeArcs`). Not yet run (written while testing was on hold) |
| `test_damage_model.py` | Damage fidelity. Tables: weapons.txt format 3, grenade blasts 80 lower, and every hostile actor variant's time to kill the player on Normal (shots from full shield and health) within 16% of the old integer slot damage at close range (`docs/macos-build.md`, Damage fidelity, has the table). On a30 section 2: `qa_damage_table` shows the shotgun pellet at 18/18/13/8/8 at 0, 1.5, 2.25, 3 and 7 world units, the plasma rifle bolt 12/11/10 at 20, 35 and 101, the pistol flat; both grenades 120 at the centre, 100 halfway, 80 at the outer radius; actors' grenade rolls inside [80, 120] and spread, the player's 120; `halofalloff` on fixed cases; exact hits carry their fraction (three 0.5 hits take one point and leave 0.5); a hit is taken by the shield first; EMP drains the shield, then the overshield and shield. On d40 section 3 (stock geometry, `qa_stock_geometry 1`: the section is on retail collision since the d40 section 3 batch): an EMP holds a driven Warthog for its stun (under 1.5 units moved under full throttle), then it drives over 5 units. On a30 section 3: with `halo_damagetrace 1` and the player protected, every actor shot on the player is exact and inside its weapon's [lower, upper max] times the variant's modifier (at least 1), some fractional; actor grenade blasts on the player deal 80 to 240; the player's EMP hit drains a shielded actor's shield. The modifier-0 item (not yet run): on a30 section 2 a spawned `elite commander energy sword` and `sentinel` have `DAMAGE_ACTOR weapon ... scale=1.000`, an `elite minor plasma rifle` 0.5; the table check takes a modifier of 0 as 1. The cyborg item (not yet run): `packages/halo/player.txt` holds the cyborg's coll; on a30 section 2 `qa_damage_player_unit` reports it loaded (shield type 22, indirect material 1, 6000 ms stun, 4000 ms recharge) and `qa_damage_shot` hits of 10 leave: plasma rifle bolt shield 55 (from 75) and health 70 (from 75 at no shield), its excess over a 5 shield health 68 with 0.5 carried; plasma pistol bolt shield 69; pistol bullet health 60; needle shield 65; assault rifle melee shield 61; with `halo_playermaterials 0` an actor's plasma pistol bolt shield 55 (the built-in 2x). Not yet run (written while testing was on hold) Batch 9 (not yet run): on b40 section 3, the parked Ghost and a spawned Scorpion driven by Elites (`qa_vehicle_pilot`) and stunned for 4 s (`qa_damage_emp_vehicle`) move less than 2 units between 1 s and 3.6 s into the stun, their pilots still at the controls (the aiDrive fix). Since the actor-melee work (not yet run) a30 section 3's actor hits on the player leave out melee blows (gun 0), which must be 30 to 35 or 151 |
| `test_melee_materials.py` | Batch 9 damage fidelity. Tables: every player weapon's `melee` line has side effect 2, the needle's `supercombine 0.75 0.5 1 40 60 60 3 3` line and `sound supercombine` (needler_super_expl), the rocket's `core 0.6` and both grenade blasts' `core 0.4`, and `halo/engineer/md3.cfg` has `md3anim` clips. On a30 section 2 (`halo_damagetrace 1`): `qa_damage_table` parses the core, `super=1` with side 3 and `melee_side=2`; `qa_damage_melee` leaves a calm Elite standing from the front, an Elite in combat with the player from behind and a calm one from behind with `halo_meleeunsuspecting 0`, and kills a calm one from behind (one `DAMAGE_UNSUSPECTING`); `qa_damage_region` on a Hunter with the pistol's `jpt!` lands on skin (13) from behind and at head height from the side, armour (12) at the side belly, the shield arm (32) in front, armour at the side head with `halo_hunterregions 0`; by default (`halo_actormaterials 1` since the headshot work) an Elite hit logs material 14 at scale 1, its shield taking twice the sniper hit it takes with `halo_actormaterials 0` (which logs no material), and a Flood combat form 0.05 (material 19); `qa_damage_needles` 6 set off no supercombine, 7 one (`SUPERCOMBINE needles=7 damage=60`), 7 with `halo_supercombine 0` none. Items 6 and 7 (batch 5's critic): `qa_damage_melee_friend` from behind on a calm marine logs `DAMAGE_UNSUSPECTING_SPARED` (team 1, attached 0) and no `DAMAGE_UNSUSPECTING`, its `DAMAGE_TRACE` the melee's own amount; a frontal belly-height hit just after a smash (`qa_damage_hunter_act 0`) lands on the shield arm (32), just after a cannon shot (`qa_damage_hunter_act 1`) on the skin (13). Items 8 to 10 (the shield-leak work, gaps 1 and 4 of the batch 10 critic): a `jackal minor plasma pistol` (`qa_variant_spawn`) takes a frontal plasma rifle 10 on material 3 (`DAMAGE_SHIELD` type 16, leak 0: shield 190, health 50), a frontal pistol 10 on it for nothing, a hit from behind (material 0), the plasma rifle at the head (1) and a rocket blast (`qa_damage_blast`, 0) on its body (health 40, shield 200), a frontal 250 empties the shield and leaves health 50, and with the shield down (`qa_damage_keep 1`) a frontal pistol hit reaches the body; `qa_shield_timing` gives the Jackal 15000/33 ms, the Elite minor 12000/10000, the marine 8000/1000, the crewman and stealth Elite `shield_max=0`, and a Jackal hit for 50 stays at 150 a second later (`qa_damage_state`) and is back at 200 15.5 s after; a hostile `marine assault rifle` hit from behind shares 0.8 (8 to the shield, 2 passed), then 0.8 x (0.8 x shield fraction + 0.2). Item 11 (the headshot work, gaps 1 and 3 of the batch 11 critic): `jptflags` bit 0x2 on the pistol and sniper bullets only, and `DAMAGE_FLAGS headshots=1` for exactly those; an `elite minor plasma rifle` survives a pistol 10 at the head with its shield up (shield 92), a plasma rifle 50 empties its shield with no excess (health 100), then (`qa_damage_keep 1`) a body pistol 10 leaves 90, a head pistol 10 with `halo_headshots 0` leaves 80, and with 1 kills it; a Hunter survives the sniper's 5 on its armour and dies to it in the back; a Flood combat form survives the pistol at head height; exactly two `DAMAGE_HEADSHOT` lines (kind 1 material 14, kind 3 material 13); a `sentinel_shielded` loses 40 of its shield to the needler's 40 and no health, and a second 40 leaves its health at 60. Not yet run |
| `test_script_seats.py` | Batch 9 scripted seats. a10 section 1 on Normal: `unit_enter_vehicle (player0) cryotube_1 ct-driver` holds the player (`CARGO_QA ... boarded=cryotube_1 scripted=1`), `qa_cargo_board` (E) does not get them out, the eye stays put, `effect_new_on_object_marker` at cryotube_1's `driver` marker logs the marker, and the tutorial's `unit_exit_vehicle` lets them out within 185 s (not the guard); on Heroic `fast_setup` seats and releases within 6 s; `halo_scriptseatmax 4` releases after about 4 s (`held too long`); `halo_scriptseat 0` never seats; a QA teleport (`qa_campaign_advance`) out of the seat is kept (`moved elsewhere`). Items 6 and 7 (gaps 3 and 10 of batch 5's critic): b40 section 1 with its opening scene played through (plain sleeps, not `qa_after_gameplay`): `cutscene_insertion_a leaves the player seated in insertion_pelican (p-riderlf)`, `CARGO_QA boarded=insertion_pelican scripted=1` samples moving with the Pelican, then `cutscene_insertion_b`'s `unit_exit_vehicle` lets them out; with `halo_scriptseatmax 3` the guard lets them out of the flying Pelican onto a floor (`put on the floor under the seat`, or `no floor under the seat` and back where they were seated) and the next two samples a second apart stand still. c20 section 1: `ai_kill bsp0_monitor` leaves the placed Monitor alive (`ai_kill spares`), `ai_erase` removes him (living count 0, `ai_erase removes`). Not yet run |
| `test_actor_tags.py` | Retail actor table matches known tag values and the engine's parsed copy. The shield-leak work (not yet run): every `material` line has its fifth field, the leak, and every variant with a coll its `resistance` and `shieldtime` lines (Jackal leaks 1,1,1,0, `resistance 0 60 100 16 4 0 0`, `shieldtime 0 15 0 1`; marine leak 0.2, failure 0 1 0.2, 8 s and 1 s; Elite leak 0, 12 s and 10 s); with the caches (`HALO_SOURCE`) every variant against the coll bytes; the engine's `ACTOR_SHIELD` lines (`qa_actor_table`) match. The melee-delay and grenade-timeout export (not yet run): a fourth `berserk` field (actr +888; Hunter 0.1, Elite minor 5, major 4, commander 2, marine 0) on every variant with an actr and a seventh `grenade` field (actv +424; Grunt minor 5, marine 20) on all, against the tag bytes with the caches, and the engine's `ACTOR_TIMER` lines match |
| `test_actor_ai.py` | Hearing and squad alerts, burst fire, marines against the Covenant, squad positions, script AI commands, command lists; the unit and object-state commands (berserk, grenades, damage immunity, vitality, damage, follow, migrate, attach, renew, suspend, player vitality, hover, open, collideable, a50 cell doors). Item 6, combat by difficulty (the combat-scaling work, not yet run): on a30 section 2 at each level `qa_actor_combat` gives the Covenant (and the Flood, Sentinels, no team) the level's rate_of_fire, projectile_error, burst_error, new_target_delay, burst_separation, target_tracking, target_leading, overcharge, special fire, grenade_chance and grenade_timer values from actors.txt's `difficulty` lines, the marines and the player Normal's, every team the level's melee delays and guidance_vs_player; per living actor the trigger delay is `max(66, 1000 / (rate * rate_of_fire))`, the error, burst separation and grenade chance the variant's times the team value, the lead gate `min(1, lead + target_leading) > 0`, the smash interval 1.6 or 2.199 s plus scale times the `berserk` line's actr melee delay (0 berserk) plus base (scale times the whole interval plus base for a swarm actor or without the field), `ACTOR_TIMERS` the `grenade` line's encounter timeout; one hostile actor that fired has the scaled delay in its slot; after `qa_actor_grenade_note` every member of the noted encounter has `grenade_left` equal to `grenade_wait`, the timeout times its team's grenade_timer_scale in whole ticks (the melee-delay and grenade-timeout export, not yet run). Item 4's vitality now in Halo's points (10 of the stored maximum, 0.25 keeping 1 health). Item 7, script vitality (the script-vitality change, not yet run): on a30 section 2 at Normal, `unit_set_maximum_vitality` 1 0 leaves `ACTOR_VITALITY` `body_points=1 start=1 health=1 shield_max=0 scripted=1`, 40 0 then 10 0 `start=40 health=10`, points above the maximum cap at full, `ai_renew` refills to 1 and 40, 0 0 kills an `object_cannot_take_damage` actor (`ai_living_count` one lower), a checkpoint, death and revert bring both back with their maxima and the progress file's two `s squad` records end `max 1 0` and `max 40 0`. The playfight change (not yet run): in the unit-commands item the `unit_impervious` actor is hurt by `damage_object` (only `object_cannot_take_damage` protects), and the player is protected under `object_cannot_take_damage` and hurt under `unit_impervious` (`QA_VITALS protected=0`); item 8 on a30 section 2 at Normal: `ai_playfight lz_search true` before `qa_actor_place lz_search` flags (8) every lz_search member and no first_marine one, the flagged ones with `ACTOR_COMBAT` `scaled_error` 2 x error x projectile_error + 0.01745, `separation` 1.7 x, `ACTOR_TIMERS` `playfight=1`, `burst` 0.6 x and `grenade_wait` twice the scaled timeout (the others unchanged); a member `ai_migrate_by_unit` moves to first_marine loses the flag; `ai_playfight first_marine/<its first squad> true` flags every first_marine member, the migrated one too; after `qa_checkpoint_save`, both off, death and revert every member of both is flagged again and the progress file has `s playfight 2 <the two indices>`; once they fight (`halo_damagetrace 1`) every `DAMAGE_PLAYFIGHT` line has `factor=0.300` and `scaled` 0.3 x `amount`. Item 9, the scripts' effect damage on actors (the effect-damage default change, not yet run): statically `containment_1_flag_4` lies in `containment_1_slam` and `tools/halo_effect_reach.py` finds its plasma blast (2 wu) after `containment_1_trigger_4`; on a10 section 1 at Heroic `halo_effectdamage = 1`, the placed, braindead `containment_1/doom_crewman` at 1 point (`ACTOR_VITALITY doom ... health=1`) stood at the flag (`qa_hs_cmd`'s new `flag:NAME` token) keeps its `ai_living_count` through the blast with `halo_effectdamage 0` and loses one to it with 1, a `DEATH_TRACE` with `killer=- ownerless=1`. Item 10, Halo's melee (the actor-melee work, not yet run): on the preview at Normal, protected, an Elite minor with a plasma rifle 1.2 units off (`ACTOR_MELEE_QA model=1 ranged=1`) charges within its range and its `strike_unit` blows are 30 to 35; the stealth sword Elite 2 units off (`ranged=0`) charges within 4 and its `strike_weapon` blows are 151. Item 11, special fire (the special-fire change, not yet run): statically actors.txt's `special` lines sit on the two Jackal plasma pistol variants (`1 1 0.3 5 0.4 0`, `1 1 0.2 5 0.4 0`) and the Banshee pilot (`2 3 1 10 0 0`) only, against actv +340 to +348 and +248/+252 when `HALO_SOURCE` has the caches; on the preview at Normal, protected, with `halo_specialfire 2` (every roll succeeds) a Jackal major 6 units off logs `ACTOR_SPECIAL_FIRE roll` lines at least 5 s apart (extra 5 to 6.5), a `burst` line with extra 0.4, `release` with value 1 exactly when the hold is 0.6 s or more, and every side-3 `DAMAGE_TRACE` from it is 14.000 (the charged bolt's 70 x 0.5 x 0.4); `qa_actor_combat` gives it an `ACTOR_SPECIAL` row `mode=1 situation=1 chance=0.300 delay=5.000 modifier=0.400` and a Grunt major none (`special_left=-1`); with `halo_specialfire 0` no `ACTOR_SPECIAL_FIRE` line |
| `test_campaign_ending.py` | game_won on The Maw and the finale cue raise the ending and mark progress beaten (no game left to continue); credits end at the title menu (`QA_ENDING ... title=1`, new and not yet run); early game_won is ignored; game_lost reverts |
| `test_mission_exits.py` | Static (needs `HALO_SOURCE`, no Mac): every retail script that takes the camera (`cinematic_start` or `camera_control`) is a cue row of `halo_script.cpp`, is called from one, or is in its `NOT_CUED` list with the reason it plays as gameplay. QA client: each mission's last section plays its exit scene on `qa_halo_finale` (`Halo scene: <exit> start`), Enter skips it and Enter again starts the next mission's first section (`Halo: <mission>, section 1`); The Maw's finale raises the ending and the credits end at the title menu. a30's extraction follows the player's site: standing by `river_hide_flag`, `cliff_hide_flag` or `rubble_hide_flag` (`qa_halo_flag`) the exit plays that variant (`... chosen for the exit`). Scenes the scripts call mid-section (`qa_hs_cue`, as the script's call does): d20's `cinematic_captain` leaves the player at `player0_capt_done`, d40's `cinematic_bridge` at `player0_playon_base`, a50's `cutscene_rescue` then `ini_post_rescue` at `prison_player0_teleport` (`Halo script: player placed at <flag>`), and the section's exit does not replay them; a Call scene skipped at once still runs the rest of its script (b30's `cutscene_shaftb_goal` leaves `shafta_lock_door` open). Gap 3 of batch 5's critic: statically, `SCENE_WAKES` lists the 69 gameplay scripts the 19 cued scenes wake (through static calls and the woken scripts' own wakes, read from the maps) and `SCENE_SEATS` the one that seats the player (b40's `pelican_up`); the test fails on any change. Item 6: b30 section 1's insertion played 12 s then skipped (`qa_scene_skip`, `QA_SKIP 1`): `music_b30, woken by cutscene_insertion, goes on in play`, and `qa_hs_script music_b30 1` / `2` answer 1 / 0. Item 5 (batch 8): a gameplay loop started before d40's bridge scene plays again after it (`AUDIO_QA ... restart=` while the scene runs, `loops=` after), and a conversation spoken when a50's rescue starts ends at status 5 (`QA_CONVERSATION`). Not yet run (written while testing was on hold) |
| `test_frontend_progress.py` | The title menu (`halo_title`) on a fresh profile; New Game on Heroic saves the game at a10's start (version 3, no state block); a checkpoint saves place, loadout, difficulty and the mission script's state (a state block whose count and hash hold), and a second launch's Continue restores them and the script where it was (`cryo_door_1`'s position, the global `delay_lost` set to 1234, a script clock no earlier than the checkpoint's), keeping the previous file as `.bak`; finishing a10 section 1 carries the loadout into section 2 and the save at its start keeps the carried script, which Continue resumes; a corrupt progress file is replaced by its backup (or reset without one) with a warning in the log and the title menu; version 1 files still load and unlock the missions reached; a state block with another `.hscb` hash (the Continue entry says the scripts changed) or a damaged record falls back to restarting the script at the checkpoint with a warning; a version 2 file with a checkpoint still continues there; a save at the wheel of b30's beach Warthog continues at the wheel (skipped without b30 or `vehicles.txt`); in live play a death in a10 section 2 (carried in from section 1) before its first checkpoint reverts to the carried script, not a restart (`HaloScript: a10 loaded` once, the script clock not back at zero); New Game after the corrupt-file warning clears it (`QA_TITLE ... note=[]`). Item 14 (batch 8): a checkpoint with a30's `foehammer_cliff` made enterable and the music class at 0.25 writes `s board` and `s gain`, and Continue keeps both (`qa_hs_object boardable`, `FX_QA ... music=`); one with a50's `captain_keyes` attached writes `s attached` and Continue brings the actor back (`ATTACH_QA`). Item 15 (gap 10 of batch 5's critic): a checkpoint while a10's tutorial holds the player in `cryotube_1` (Normal) writes `s boarded` ending in the scripted flag 1, and Continue puts the player back in the tube (`player aboard cryotube_1 again`, `CARGO_QA ... boarded=cryotube_1 scripted=1`). Item 16 (the stood-members change, not run): on a30 section 2 with `lz_search` placed, every living member hurt to 0.5/0.25 (`qa_squad_hurt`) and a checkpoint taken, the `s squad` records end with the major flag, the fractions and the stood point; a death and revert, and a Continue, bring the members back within 1.5 units of where they stood with those fractions (`qa_squad_stood`: `SQUAD_STOOD`, `SQUAD_STOOD_STATE ... stood= fallback=`; only the counted fallbacks may come back at their starts), and the same save cut back to the major flag (block hash rebuilt) still continues with the script, the members at their starts at full health. Not yet covered by a test (review fix): a checkpoint on a50 section 4 while the jailed marines are attached but still posed writes `s attaching`, and after Continue `unit_stop_custom_animation` still makes them actors (`ATTACH_QA ... pending=`, then `count=` up). Item 3 and 4's `qa_loadout_mark` give an unarmed a10 start the assault rifle first. Not yet run (written while testing was on hold) |
| `test_nav_points.py` | `hud.txt` has the waypoint bitmap and its ten arrow types; on a30 section 2 `activate_team_nav_point_flag` shows a nav point (raised by its offset), a second call replaces it, deactivating removes it; `hud_show_health` and `hud_blink_shield` hide and flash HUD parts; a revert brings back the checkpoint's nav points and HUD state. Not yet run (written while testing was on hold) |
| `test_script_coverage.py` | Every command used in a mission's sleep_until/if/cond/while conditions or in a `set` value is bound and not a fixed answer where the game has the state (`game_all_quiet`, `game_safe_to_speak`, actor volume tests), and so is every ai_, unit_, object_ and vehicle_ command any script calls, and every player-moving command; a10 looking tutorial finishes; d40 countdown follows the escape deadline and holds off endgame_cinematics. On a30 section 2: `game_all_quiet` and `game_safe_to_speak` answer 1 with nothing fighting or talking; `volume_test_objects` of `(list_get (players) 0)` matches `(players)`. The `set` values, the fixed-answer list and the a30 checks are new and not yet run |
| `test_falling_damage.py` | Falling damage (not yet run). Static: player.txt's `falling` line is every cache's matg falling_damage block (3, 6 and 16.5 units; runtime velocities sqrt(2 g d) with physics.c:279's gravity; globals\falling 10/155/155, globals\distance 105, category falling), and `tools/halo_fall_scan.py` finds every section's jump apex under 3 Halo units and no planned on-foot drop the distance model hurts (while the engine's own landing speed would hurt some). On a30 section 2 (`qa_fall`, `qa_fall_actor`, `qa_fall_state`, `qa_fall_def`; actors not shooting): 2 units nothing; 4 units 10 + 145 x scale on the shield, the same on Legendary (no difficulty); off with `halo_fallingdamage 0`; 7 units kills (scale 1, 155); 20 units two `kind=distance` hits of 105 mid-air kill; an lz_search actor dropped 2 units takes nothing, 7 units 155 |
| `test_script_damage.py` | damage_object and damage_new by their jpt! (not yet run). Static: effects.txt's `jpt` records, after every effect, are the six jpt!s the scripts name (pulsegenerator 499/500, flags 0x40, 0.5 on the cyborg shield; oven heat 25/30, flame; shock explosion 1.5-2.5, 10/30-50; the a10 zapper 0; doom 500; plummet 1000, falling), equal to every cache's type-26 constants. On a30 section 2 (`qa_hs_damage`): the pulse generator empties the shield and leaves 10 health; oven heat takes 15 of the shield; the shock explosion 30-50; `halo_scriptjpt 0` the old 40; on an lz_search actor the pulse takes no health; a protected player survives the doom jpt!, the plummet kills. On a10 section 1 the zapper by damage_new at `tutorial_zapper_flag` deals nothing |
| `test_difficulty.py` | halo_difficulty picks each squad's Easy to Impossible start count by Halo's rule (Easy and Normal `min_count`, Hard `(min+max)/2`, Impossible `max_count` even below `min_count`; ../Halo3DS encounters.c:2191; a30 lz_search selects 17, 17, 16 and 18 starts, matching the importer table; since the squad-count batch, not yet run), game_difficulty_get returns it, enemy health and damage scale, the choice persists. Since the difficulty-table work (not yet run): the ten caches share one matg `difficulty_information` table and actors.txt's `difficulty` lines equal it; per level `qa_difficulty` gives enemy_damage and the Covenant's enemy_vitality, enemy_shield and enemy_recharge, the marines' and the player's friend values, no team's enemy_vitality and `table=1`, and `DIFFICULTY_DAMAGE` gives enemy_damage on hostile-owned damage (on the player, the marines, the Flood, no team), factor 1 with divisor enemy_damage on the owner's own side, and 1 for the player's, the marines' and ownerless damage; the spawned Elite's health is Normal's times enemy_vitality |
| `test_major_upgrades.py` | Major upgrades at squad placement (the squad-count and major-upgrade batch; written while testing was on hold, not yet run). Static: actors.txt format 3 carries the globals' 35-value difficulty table (equal to every cache's matg; rows 28-30 equal halo_ai.cpp's fallback) and a `major` line per variant with a major reference (each resolves; equal to the actv's +36 reference); every .hscb is version 10 with each squad's `major_upgrade` (2,851 normal, 9 few, 25 many, 89 none, 14 all; squad+128) and Halo's squad sizes (601 squads changed on Easy, 444 on Hard, 132 on Impossible against the old rule); a Python port of ai_consider_major_upgrade stays within one actor per squad. Game: `qa_major_upgrade_sim` matches the table and the port on every difficulty; a30 section 2's lz_search on Easy and Impossible places only start variants or their majors (`qa_major_upgrades`), none squads never and all squads always upgraded, more majors on Impossible; a checkpoint, death and revert, and a Continue, bring every member back as the same variant (state block: a major flag of `1` on a major's `s squad`, which since the stood-members change is followed by the health, shield and stood point, 11 or 15 fields; one `s upgrade` record) |
| `test_flood.py` | The squad cap counts only living actors (c10 swarm encounters place behind 128 dead entries); a killed Carrier Form bursts into Infection Forms; an Infection Form on a dead marine raises a Flood combat form; Infection Forms leap. Its Flood spawns now name the Flood team (2); changed and not yet run. Since the Flood tag batch (not yet run): item 0 (static, needs `HALO_SOURCE`) checks actors.txt's `unit` (unit +380, bipd +756), `spawn` (bipd +588/+604/+606/+608, the variant by index) and `pop` (coll +188's effe jpt! with the pop flag) lines against the tag bytes in c10, c20, c40, d20 and d40, the carrier's `spawn` naming `flood_infection` 3 6 3, `pop` lines on the two infection variants only, and the random speed increase bit on the nopop form alone; item 2 (`halo_damagetrace 1`) expects the `spawn` line's 3 to 6 forms (4 to 8 without it), a `FLOOD_SPAWN` naming the variant and one `FLOOD_THROW` per form at velocity x 0.25 to 0.5 world units a second; item 5 spawns three infection forms 0.4 units apart and kills the first: the first `FLOOD_POP` has at least one candidate, on Normal `one=0` and `hit` equal to the candidates, on Legendary `one=1` and `hit` at most 1; item 6 on Legendary spawns a nopop and an ordinary form, and after 2.5 s `qa_flood_stagger` shows the nopop one in combat at `value * (cn % 137) * 0.00729927 + 1` and the ordinary one at 1. Since the carrier-squad change (not yet run): item 7 (static; the tag part needs `HALO_SOURCE`) checks actors.txt's `destroyed` lines against `destroyed_effect` in b30, c10, c20, d20 and d40 (the carrier's `characters\floodcarrier\effects\body destroyed`, none on the infection forms) and effects.txt's carrier blast `damage 1 2 40 70 85`; items 8 to 11 run on c20 section 1 (Normal, `halo_unitblast 0` but item 11) with `qa_actor_place enc2_1`: item 8 kills one carrier (`qa_squad_kill_one enc2_1/carriers`) and expects `FLOOD_SPAWN ... squad=` not -1, one to `count` `FLOOD_ADOPT ... squad=enc2_1/carriers` lines in one swarm actor, `SQUAD_COUNT` living up by forms less one, nonswarm down by one (and `ai_nonswarm_count` equal), `actors` the carrier entries plus 1 and `ai_living_fraction` living over actors; `ai_migrate` to `enc2_1/combats` takes the forms along, and after `ai_erase enc2_1/combats` there is no `FLOOD_POP` or `FLOOD_SPAWN`, the trace says `ai_erase deletes` and the squad is empty; item 9's `ai_kill enc2_1/carriers` pops the forms and the killed carriers' forms join the squad again; item 10 saves a checkpoint after a burst (`qa_checkpoint_save`), dies and reverts: as many `SQUAD_STOOD reverted` rows with `spawned=` set as were saved, one `s spawned ... =enc2_1 ... =carriers` record each in the progress file, and a Continue brings them back; item 11 (`halo_unitblast 1`) expects `UNIT_BLAST variant=characters\floodcarrier\floodcarrier ... dealt=1` before the first `FLOOD_ADOPT`. Since the actor-melee work (not yet run): items 4 and 6 run with `halo_actormelee 0`; item 12 (static, needs `HALO_SOURCE`) checks every `melee` line against actv +352/+356/+368/+372, actr +892/+896/+932, the weapon's flags bit 15 and `melee_animation`, a few retail rows, no marine melee and player.txt's `unit` bit 22; on the preview at Normal item 13's unarmed human form (`ACTOR_MELEE_QA model=1 knife=1 ranged=0`, reach 1.180, `key_ms=495`) charges within 6 and its `strike_unit` blows and `DAMAGE_TRACE` amounts on the player are 20 to 30; item 14's infection form on the full shield deals one 3 (`contact_fried`) and is gone with no `FLOOD_POP`; item 15's, after `qa_halo_damage 75`, deals `contact_attached` 2.933 and stays; item 16's carrier `suicide`s within 3.7 and `burst`s about 1.33 s later (its melee animation), `UNIT_BLAST` and a 3 to 6 `FLOOD_SPAWN` follow and it never strikes |
| `test_starting_profiles.py` | Player starting profiles (batch 8). Static: every `.hscb` is version 9 and reads to its end (`qa_common.read_hscb`); a10's profile 0 has no weapon and no grenades and its `bridge0_profile` the pistol 6/48, a30's the rifle 60/120 and pistol 12/64 with no grenades, a50's the sniper rifle 4/64 and rifle 60/360 with four frags, c20's `easy_start` the shotgun 12/48 and pistol 12/60 with four frags; with `HALO_SOURCE` the importer's `profile_rows` agree. QA client: a10 section 1 starts unarmed (`QA_LOADOUT unarmed=1 gun=0`, no frags), `player_add_equipment player bridge0_profile false` gives the pistol, a revert before the first checkpoint goes back to unarmed; a10 section 3 on its own starts with the pistol; a30 section 2 with its profile; c20 section 1 with `easy_start` on Easy and the profile 0 rifle on Normal; a10 section 1 carried into section 2 stays unarmed; a checkpoint saved unarmed writes `empty 0` and `empty 1` and Continue is unarmed. Item 7 (gap 6 of batch 5's critic; the fresh-start batch for sections 2 to 4 and the tutorial resume, its final integration review for section 7): a10 sections 2 to 7 started on their own on Normal log `a10 section N started afresh: the script brought forward (G globals set, 2 threads passed over, W woken)` (section 2: 1 and 0; 3, 4 and 7: 5 and 0; 5 and 6: 5 and 2) and `tutorial_setup resumed after its statement S (wake T)` (section 2: 63, `tutorial_first_contact`; the others 65, `tutorial_weapon`); 12 s in, `mark_fast_setup` is set, the player was never seated in `cryotube_1`, and the section's own mission threads have started (section 2 `mission_crossfire` and `mission_bridge`, with `mission_shoot` and `mark_bridge_cutscene` still waiting for its bridge scene; section 3 the airlock, flank and loop threads; section 4 `mission_knot` and `mission_stairs`; 5 and 6 `mission_tunnel`, `cryo_search`, `mission_boom`; 7 `mission_final`); `tutorial_setup` still waits in section 2 and has finished in 3 to 7, where `tutorial_motiontracker` has started and `tutorial_weapon` has not; section 3 on Heroic is brought forward with no resume; section 1 is untouched (no `started afresh`, the tutorial seats the player in the cryotube). Item 8 (the fresh-start batch): on Normal in section 6 `melee_door_2` is put in (visible, 0.8325, unpowered) and `motiontracker_door_2` powered off; `qa_device_melee melee_door_2 3 1` logs `Halo: a melee opened melee_door_2` once and leaves it at 1.0; on Heroic the door never appears and `motiontracker_door_2` keeps its power. Not yet run |
| `test_ai_attach.py` | `ai_attach` and story figures (batch 8). Static: the `.hscb` story-figure flags (a50's Keyes, freed and jailed marines and `keyes_prison`, d40's `cortana`, d20's `keyes_flood`, a10's `keyesa10`; no corpse placed dead, no c20 `monitor` object) and Keyes's squads defaulting to `captain_ingame`. QA client on a50 section 4: `captain_keyes` created and attached becomes an actor two ticks later (`ATTACH_QA alive=1 drawn=0`, `unit_get_health` 1, `ai_living_count prison_captain` 1), comes back after a checkpoint, death and revert, dies to a lethal `damage_object` (`unit_get_health` -1) and stays dead after another revert (`gone=1`); `enc_spawn_marines`'s jailed marines stay posed figures (`pending ... posed=1 drawn=1`) until `unit_stop_custom_animation`, then become actors; `object_destroy` erases one at once. Items 6 to 9 (gap 4 of batch 5's critic): `unit_set_current_vitality` 10 given with the attach is queued and the actor spawns at 0.40 (10 of the captain's 25 points; Keyes at 0.5 below is 12.5 points, 13 of 25 health, within 0.03); during `qa_hs_cue cutscene_rescue` Keyes's object is visible at his actor's feet (`qa_hs_object visible` 1, `attachgap` 0); Keyes at 0.5 and `free_marine_1` attached in section 4 come along into section 5 after `qa_halo_finale` (`comes along into section 5, health 0.50`, `came along with the player into section 5`, `ATTACH_QA carried ... alive=1 health=0.50`, `unit_get_health` 0.5); the progress file's `s attached` record for him ends in the health and the carried flag, and Continue brings him back alive at 0.5 (behind the player again when the flag is 1). Items 10 and 11 (gap 11 of batch 5's critic and the melee review): Keyes moved off his spawn (`qa_attach_move`) comes back after a checkpoint, death and revert where he stood, and `ai_teleport_to_starting_location prison_captain` takes him to his spawn, not to the checkpoint spot; the checkpoint's `s attached` record has 23 fields, its last four (the start) apart from its point; the melee from behind on the attached Keyes logs `DAMAGE_UNSUSPECTING_SPARED attached=1` and no `DAMAGE_UNSUSPECTING`. Item 9 now reads the health and carried flag at fields 16 and 18. Item 12 (the fresh-start batch, gap 1 of batch 6's critic): Keyes (0.5) and `free_marine_1` carried into section 5 with `halo_carriedhold 1` wait unspawned (`ATTACH_QA held carried=captain_keyes squad=prison_captain/...`); `ai_living_count prison_captain` is 1 and `ai_strength` 0.5; `ai_migrate prison_captain hangar_captain_halls/squad_switch` and `ai_migrate_by_unit` of the marine retarget both (`carried in and not spawned yet, migrates to`), the counts follow (0 and 1), an `ai_erase` of the old squads spares them, and with the hold off both come along in the new squads, Keyes at 0.5. Not yet run |
| `test_factions.py` | Factions and special enemies. Tables: actors.txt format 2 with unit teams (Hunter 3, Monitor and Sentinel 5, Flood 4, marines 2), the Hunter's unit melee 35-45 and collision materials (shield arm and Monitor body multiplier 0, Hunter skin flagged head), the Monitor and Engineer as kinds 10 and 11, the stealth Elites' camouflage actv flag and the sword Elites' melee-only flag; weapons.txt and equipment.txt `materials` lines (assault rifle 0.5 on Hunter armour and 0 on its shield, pistol 0.2 and 1 on armour and skin, plasma rifle 2 on Sentinels, frag 0.25 on armour), and the engine's parsed copy. In `halo_preview`: a Covenant Elite, a Flood combat form and a Sentinel pick each other as targets; a Hunter loses nothing to a rifle hit on its shield arm, 25 to a pistol hit in the back, 5 to one in the side and 5 to a rifle blast while facing; two Hunters pair off and the survivor goes berserk; a Hunter at arm's length smashes; a calm stealth Elite cloaks above 0.85 and draws below 0.35 opacity, a hit halves its cloak, which recovers; plain Elites draw opaque; the shielded Sentinel spawns shielded; the Sentinel, Monitor and Engineer rise off the floor and hold their height; the Monitor and Engineer are passive, nothing hurts the Monitor, nobody targets it, the Engineer never fires; the Monitor and Keyes draw their own models. On a30 section 2: the script's player-human allegiance holds, `ai_allegiance`/`ai_allegiance_remove` covenant-flood change the matrix, killing two marines breaks player-human (`ai_allegiance_broken` 0 then 1, the marines turn hostile) and `ai_allegiance` renews it. c20 section 1 places 343 Guilty Spark (disregarded, undamageable, flying, passive); c10 section 1's mission script allies player and humans with the Sentinels. Not yet run (written while testing was on hold) |
| `test_vehicles.py` | `packages/halo/vehicles.txt` has the six vehicles (Warthog seats and gun, turret gunner seat, models); the section maps carry the scenario vehicle placements (d40's escape Warthogs in section 6 and not the engine room, the enc7_7 turrets, a50's 14 turrets, b30's unnamed Warthog); on b30 (retail collision) E by the driver door gets in, throttle drives, E gets out onto floor, a flip throws the driver out and E flips it back, the gunner seat fires; a50's turret is manned and fires; b30's script seat commands (`object_create`, `vehicle_load_magic`, `vehicle_test_seat_list`, `unit_in_vehicle`, `unit_exit_vehicle`) act on its jeep; a Warthog drives on stock geometry (d40 section 3 with `qa_stock_geometry 1`, which keeps its `.hcol` unloaded since the d40 section 3 batch put it on retail collision). Vehicle fixes: a body put part way into, or into the middle of, b30's Warthog is pushed out; a Warthog rolled into the player never has the body inside it; `object_create jeep` over the player pushes them out; an actor put inside an a50 turret is pushed out alive; at the wheel, a drop below the mesh and a Warthog sunk into the beach are recovered to a safe spot with the driver still in it; a driver thrown out upside down with no room is put back in the seat by the fall recovery (no `player left in place`); a Warthog on c20's platform1 rises with it. Not yet run (written while testing was on hold) |
| `test_enemy_vehicles.py` | Enemy and flying vehicles. Tables: vehicles.txt format 2 (Ghost and Wraith `hover`, Scorpion `tank`, Banshee `flyer`, the Wraith `player 0`), the Banshee gun's bolt and fuel rod triggers and the Scorpion cannon's two, c40 sections 1 and 4 in `FLOWN_SECTIONS` with an `.hcol` and their planned vehicle route as `.route` (batch 7). On b40 section 3 (retail collision): the parked Ghost, Banshee and Wraith report classes 3, 5 and 3; E drives a Ghost (`E  Drive the Ghost`) more than 6 units and its bolts fire, E gets out, the Wraith offers no seat; E flies a Banshee (`E  Fly the Banshee`) more than 3 world units up on the climb input, where it hovers (within 1.5) with no `Halo vehicle recovery`, its bolts and fuel rod fire, E in mid-air keeps the player in, descending lands it and E gets out; a spawned Scorpion drives, its cannon fires 1 to 3 rounds in 3 s held and the secondary key fires its machine gun; an Elite put at the controls of the Ghost drives it more than 4 units and one put in the Banshee climbs 3 world units and goes through more than one strafing-run phase, both vehicles firing (the player protected). On a30 section 3: `mission_rubble_wave_2` loads its squad into `rubble_rock_cship` (rider rows while it flies, its object row on a recording, then flown) and `vehicle_unload` sets them down; `mission_extraction_cliff` lands `foehammer_cliff`, makes it enterable, `qa_cargo_approach` shows `E  Board the Pelican`, E boards it (`halodriving` true) and the script's `game_won` ends the section. c40 sections 1 and 4 play on retail collision and report flown; section 1 keeps the scenario's `fly_away_1` (no Banshee of the engine's), section 4 gets one beside the player. Not yet run (written while testing was on hold) |
| `test_vehicle_damage.py` | Vehicle damage and the vehicle actor variants (batch 7). Tables: `vehicles.txt` `vitality` lines (Wraith 450, Ghost 160 and Banshee 100 "only damaged while occupied", the Banshee taking its pilot's damage, Warthog, Scorpion and turret 0), `destroyed` effects (in `effects.txt` too), the Wraith driver's invisible seat and a `variant` per vehicle naming an actors.txt variant. On b40 section 3: `VEHICLE_HEALTH` rows start whole with a variant; an empty Ghost takes nothing; the Wraith takes 100 points (health 0.6 to 0.9) and `unit_get_health` follows; with an Elite at its controls a 2000-point blast destroys it (`the Wraith is destroyed`, exists 0, the death explosion's effect, the Elite dead, `unit_get_health` -1); a Grunt at a spawned turret's gun hits the protected player for at most the turret bolt's upper bound times the turret gunner variant's 0.333; the player's rifle on the Wraith hits its body (`takes ... from the player`) and `qa_vehicle_protect 1` stops vehicle damage. Items 5 to 8 (gaps 8 and 11 of batch 5's critic): a frag beside the piloted Wraith (`qa_vehicle_blast`) leaves its Elite driver as he was (`covered=1`) and lowers the Wraith's health, beside an Elite-ridden Ghost hurts both (`covered=0`); `effects.txt` gives the death explosion `damage 1.5 2.5 55 55 65` and metal 0, and a Grunt 1.3 half-widths out takes 55 to 65 from the player when the Wraith is destroyed (`death explosion deals its damage`); level probes above the Wraith's box miss level and hit it pitched -30 or rolled 30 (`qa_vehicle_tilt`, `qa_vehicle_probe`); with `halo_vehiclewreck 1` the destroyed Wraith leaves a solid wreck (`qa_vehicle_wrecks`, `wreck=1 blocked=1`) that a revert to the checkpoint before it removes; with it off (item 2) `wreck=0`. Item 9 (the fresh-start batch, the critic's worry 3 of batch 6): a human-team marine beside the Wraith takes its death explosion and no betrayal is counted (`FACTION_BETRAYALS after human=0`, no `turn on the player`): the blast is nobody's; the ownerless-kill batch extends it: if the marine dies its `DEATH_TRACE` reads `killer=- ownerless=1 alerted=0`, and a Covenant run (Grunt B braindead 2.5 half-lengths along the Wraith, outside the blast; Grunt A beside it as in item 6; an Elite at the controls) shows the Elite's death the destroyer's (`killer=player`, `alerted>=1`), Grunt A's death by the explosion nobody's (`killer=- ownerless=1 alerted=0`), Grunt B alive and no `turn on the player`. Item 10 (the modifier-0 work): the Warthog's `variant` has modifier 0, and on b40 section 3 the `VEHICLE_HEALTH` rows' `gun_modifier` is 1 for a spawned Warthog, the gunner variant's for a spawned turret and the pilot variant's for the parked Ghosts. Not yet run |
| `test_vehicle_jump.py` | The Warthog's jumps on d40 section 8's planned drive (batch 10; the jump governor batch). Static: `halo_d40_08.route` ends in its exit with at least two jump rows, its metrics record the drive (`vehicle_route.drive`: gravity, time, speed, top, governors), `vehicles.txt` has the Warthog's `physics` line; with the drive planned below 0.9 of top speed, the metrics list one jump governor per planned jump (its take-off row, radius at least 12, the drive's speed) and `halo_d40_08.cfg` holds the same `halo_vehiclegovern` lines. QA client on d40 section 8: `qa_vehicle_governors` must list the cfg's governors, on. Then two passes over every planned jump: the governors off and the QA throttle capped at the planned speed (`qa_vehicle_cruise`), the Warthog put about 30 units before the take-off row at that speed; and (a drive planned slower only) the governors on, no cap, the Warthog put down at its top speed, full throttle (a player's run). Each time full throttle straight ahead (`qa_vehicle_jump`); the engine's gravity must match the metrics'; the take-off is the first stretch in the air that starts within 4 units of the take-off row or past it (an earlier crest hop on the run-up is printed separately, not judged); the jump must land upright within 3 units of the landing row's height and at least 60% of the planned length on, never falling 8 units below it. Prints the motion every 100 ms on a failure. Not yet run (written while testing was on hold) |
| `test_presentation.py` | Presentation. Tables: `effects.txt` format 1 with the cyborg's flashlight (radius 6, cone 0.349-0.785 rad), at least 15 script effects (the large explosion an explosion of scale 1.6 with its sound and light) and a `classes` line for each of the ten caches; `weapons.txt` names zoom-in and zoom-out sounds for the pistol, sniper rifle and rocket launcher and a `loop` (and no `fire`) for the flamethrower; `vehicles.txt` an engine loop for all six vehicles and the Warthog's crash and suspension sounds; all those sounds are decoded and in `packages/audio/halo/tags/classes.txt` (the Warthog's engine class 23). On a30 section 2: `halo_flashlight` turns the flashlight on (charge falling) and off (charge recovering); `qa_fx_effect` of the large explosion 3 units ahead is known, drawn and knocks the camera (`impulse=1`), an unknown tag falls back (`unknown=1`); `player_effect_set_max_rotation` and `player_effect_start` ramp the shake to 1 and `player_effect_stop` back to 0; `cinematic_screen_effect_start`, `_set_convolution` and `_set_filter` ramp the blur to 10 and desaturation and tint to 1, `_stop` clears them; `sound_class_set_gain` fades music to 0.25 over 30 ticks and the vehicle classes to 0 at once, leaving weapon fire at 1; `qa_fx_hit` raises one damage arc that is gone 1.2 s later; two zoom steps run. With two Grunts ahead: facing the nearer (between 4 and 7.2 units) the melee lunges onto it (`MELEE_QA lunge`), and the other's hit on the player raises the arc through `halodamage`. On c10 section 1: `effect_new` through the interpreter at `enc_swamp2_turret` draws the table's effect. On d40 section 3 (stock geometry, `qa_stock_geometry 1`): a spawned Warthog driven by the player counts one engine loop, none once the player is out. Batch 9 (not yet run): on d40 section 3 a spawned turret manned by the player and turned 5 degrees every 100 ms counts one engine loop, still one 150 ms after the last turn, none 700 ms after, none while turned 0.5 degrees every 100 ms; on a30 section 2 after `qa_checkpoint_save`, a shake, a blur and a damage arc are gone after the player is killed and `qa_halo_revert` (the music fade of 0.25 stays); and the blur's copy (`FX_QA copy=800x600`, `copytex` the frame size with `npot=1`, else 1024x1024) and the flashlight spot are made again after `resetgl` (`screen_tex=1`) and the copy follows `screenres 640 480`. Item 8 (gap 11 of batch 5's critic): `effects.txt` gives the large explosion `damage 1.5 2.5 5 15 25` (category 4) with 33 `materials` and the cinematic medium explosion none; on c10 section 1 `halo_effectdamage = 1` by default (the effect-damage default change, not yet run), `effect_new` of the large explosion deals nothing with `halo_effectdamage 0` and logs `deals its jpt! damage` with 1, the cinematic variant `has no jpt! damage`. Not yet run (written while testing was on hold) |
| `test_package_relocatable.py` | check-no-retail passes clean and fails on staged or stray retail files; packages/ is ignored by default; package-macos.sh rejects a stub assets dir; packaged app has no Homebrew paths, is signed, carries licences and launches from a copied location |
| `test_asset_robustness.py` | Release tier. 39 cases, each a mod directory under `build/qa/` mirroring `packages/` with one file missing or damaged: `hud.txt` (missing, random bytes, a negative or huge bitmap id, truncated), `weapons.txt` (missing, format 2, truncated, a `nan` projectile speed, random bytes), `equipment.txt`, `vehicles.txt` (missing, a box inside out, truncated), `actors.txt`, `halo_a30_01.hcol` (missing, truncated, random after the header, bit flips), `halo_a30_01.mesh` (missing), `a30.hscb` (missing, truncated, version 99, random after the header, bit flips), `halo_a30_01.route` for the QA walker (random bytes, a `nan` row, missing) and the progress file (random bytes, a bad check, a `nan` checkpoint, out-of-range values, a NUL byte, over 32 MB, a state block of nonsense with valid hashes). Section cases load a30 section 2 and run `qa_robust_exercise`; each needs a clean exit after `ROBUST_QA end`, the case's fallback log line and the fallback in the ROBUST_QA row (hud=0, weapons=0, grenades=00, vehicledefs=1, actors=0, collision=0, script=0), the player alive at the end, and no sanitizer report. Flip cases only need a clean run. `--asan` for the sanitized client, `--list`, name filters. Not yet run |
| `test_soak.py` | Release tier. One QA client process loads all 55 sections in order, twice (`--passes`), each running 1.5 s once live (4 s with `--asan`), and logs memory after each (`SOAK_QA`); fails on a crash, hang or exit code, any ASan, UBSan or LSan report, a section that does not load or leaves the player dead, or footprint growth from pass 2 on (median per section over 64 MB a pass, `--growth-mb`, or the end of the last pass that much per extra pass above the end of pass 1). Writes `build/qa/soak-memory.csv`. Not yet run |
| `test_autumn_opening.py` | Slow: four-minute Autumn opening at normal speed |
| `test_package_input.py` | Slow: real keystrokes into the packaged app |
| `test_maw_drive.py` | Slow: The Maw's escape in a driven Warthog. Section 6 starts the countdown, at the wheel on retail collision or on foot by a Warthog on stock geometry; E gets in, out and in again; the QA autopilot (`qa_vehicle_autopilot`) drives section 6 into section 7 at the wheel with the countdown carried over, and section 8 to the finale before the deadline (the jump governor batch: driven by the QA vehicle walker, `qa_retail_start` on `halo_d40_08.route`, at full throttle with no cruise cap: the section's jump governors, `halo_vehiclegovern` in its `.cfg`, must load, `qa_vehicle_governors`, and hold the car, the `governor at` trace line; the autopilot steers at the objective marker and need not take the planned drop); killed at the wheel, the revert puts the player in a Warthog again; on retail collision section 6 seats the player in one of the scenario's parked Warthogs (object bound, no second Warthog on it) and the scripts' seat tests for `trench_jeep1` to `4` and `asspain_1` to `3` see the player at the wheel in sections 6, 7 and 8; then E and W keystrokes into the packaged app exit cleanly. Not yet run (written while testing was on hold) |

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
then walks on from the next row (flag 4). Rows with flag 128 cross a light bridge (`bridge_e@1.00`; no route crosses `bridge_c`, `UNCROSSED_BRIDGES`):
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
(d40 section 5 is about 5,000 rows).

Throw rows (flag 2048, `target@health ax ay az`; written while testing was on hold, not yet run): the
importer's `ThrowTarget` (d40 section 5's s3 manifold) simulates frag throws on the retail collision
(`GrenadeArcs` in `tools/halo_collision.py`) and writes one row per aim at the best spot, best first.
The walker walks onto the row's spot in short taps until it is within 0.15 units and still, aims from
the eye above the row at the aim point (so the yaw and pitch are the planner's), throws a frag
(giving itself one if it has none), logs where the frag lies 1.4 s later (`GRENADE throw ...`) and
the target's health 2.8 s after the throw (`RETAIL_QA act throw ... health`), and throws again, four
times an aim (the second and third half a degree either side), then moves to the next aim row. The
damage adds up over the throws. It moves on once `unit_get_health` is at most the row's health and
fails with `FAIL target-not-destroyed` after the target's last aim row (`FAIL frag-never-thrown` if the
engine refuses every throw at a row for 45 s).

Vehicle rows (batch 7, written while testing was on hold, not yet run): the driven and flown sections'
planned routes (`VEHICLE_ROUTES` in `tools/import_halo_campaign.py`) have rows with flag 4096 (drive
or fly to the point: the vehicle's floor point, or a flyer's origin; with 2 too, a jump's landing),
8192 (board the vehicle type in the fifth column from where the row stands) and 16384 (get out at the
row). The walker (`qaRetailVehicle`) walks to the nearest vehicle of that type's driver seat and
presses use on a board row; seated, it holds full throttle steering at a row a little ahead, a flyer's
nose pitched at it and its climb trimming the height, backs off a second when stalled, flips the
vehicle back if thrown out, and fails with `FAIL vehicle-stuck` after 25 s without reaching a row
(`FAIL vehicle-not-found`, `vehicle-never-boarded`, `vehicle-not-seated`, `vehicle-could-not-get-out`
for the other steps); on a get-out row it hovers down over the row and presses use. It keeps the
vehicles from harm (`halo_vehicledamage 0`), prints `RETAIL_QA vehicle ...` lines (every 5 s, and
boardings, flips and jumps under the section's result) and skips the movement-feel trial on vehicle
rows; a flown section's route markers in the air are left out of the floor audit.

b30's return (`halo_b30_03`) ends with the Pelican: at the end of its route the walker stands on the
end marker and presses use every 1.5 s (`RETAIL_QA interact "Board the Pelican" ...`) until the
section completes, `FAIL interaction-never-completed` after two minutes. Other sections end at their
route's end as before.

Sections in `KNOWN_BLOCKED` (a10 sections 5 and 6, which wait at script-gated doors; b40 section
6, c10 sections 1 and 4 and c20 sections 1 to 3, which end on or include a lift ride; d40 section
5, whose fourth manifold, s3, no walkable spot can see, and which now throws frags at it; see
`docs/macos-build.md`) are walked and printed as `KNOWN` with the reason instead of failing; the
test fails once one of them passes, so the list stays current. `PENDING_WALK` names the sections the
lift, bridge, door and throw machinery and this batch's route fixes are expected to make pass, which
have not been walked yet: a10 sections 5 (walked on from section 1) and 6 (door rows), the Crash Site
(`halo_a30`, batch 6: retail collision, walked as `a30:0`), a30 section 2 (`halo_a30_01`, re-anchored),
b30 sections 1 and 2 (the route batch: planned walks, no longer the raster route; section 2 with a
door row at `shafta_lock_door` and a use row at the Cartographer's control), b30 section 3 (walked
return, Pelican boarding), c40 section 1 (the route batch: the flown route now walks into
`pulse_1_trigger` and on through `door_c1`), b40 section 3 (ends in the b3 doorway) and 6, c10
sections 1, 2 (new on retail collision), 3 (batch 6: over two walking assists, x50, the upper
doorway) and 4, c20 sections 1 to 3, and d40 section 5 (throw rows). They are printed as `expected to
pass, pending walk`; a pending section that is not in `KNOWN_BLOCKED` (the Crash Site, c10 sections 2
and 3, a30 section 2, b30 sections 1 to 3, b40 section 3, c40 section 1) fails the test like any other
when its walk fails.

Walking assists and slides (batch 6, not yet run): a section in `ASSISTS` has authored invisible ramps
or planks in its `.hcol` (AST1), where Halo's higher jump makes a link the engine's fixed jump cannot;
the route walks them like floor (no route flag: the walker just walks). A section in `SLIDES` (the
Crash Site) may drop down a slope too steep to stand on (flag 8, as a ledge drop): the walker walks
off the edge and the body slides down it.

The importer writes the route next to each `.hcol` (`tools/import_halo_campaign.py --collision
validated|all|none`; the default `validated` writes both for the missions in
`VALIDATED_COLLISION`). Route rows are `x y z [flags [device@position]]`, with flag 1 for a
crouched step, 2 for a jump, 4 for the first step after a lift ride, 8 for a walk off a ledge onto
the floor below (`LEDGE_DROPS`; the walker walks it like any other step, and d20 section 1's last
row, over the shaft its exit volume hangs in, `EXIT_FALLS`, has the flag too), 16 for a lift deck
(32: call the lift from the landing, 64: send it from the control on its deck) and 128 for a light
bridge (32 on its first row: use the bridge's control from the row before); 256, 512, 1024 and 2048
are the action rows above.

## QA client

`halo-ac-qa` is the game plus console commands for injected positions, damage
and scripted outcomes. `tests/native/retail_qa.inc` has the route walker and adds
`qa_retail_where` and `qa_retail_recover fall|void|wedge` for the recovery test, and (batch 6)
`qa_retail_assists` (each walking assist's ray, body and grenade checks) and
`qa_retail_assist_stand i` (drop the player onto assist i, a QA move) for `test_retail_assists.py`, and (the scenery batch)
`qa_retail_scenery` (each baked scenery placement's ray, grenade and body checks from above) for `test_retail_scenery.py`, and (the cube-raster stamp)
`qa_raster_waypoints` (the flood waypoints in solid cubes or with scenery in the body's column, the map's scenery cubes and whether light passes them) for `test_scenery_raster.py`, and (the d40 section 3 batch)
`qa_stock_geometry 1`, which keeps the next sections' `.hcol` unloaded (`retail collision off for <map>` in the log) so that
`test_vehicles.py`, `test_presentation.py` and `test_damage_model.py` still drive on stock geometry now that no campaign section is stock.
`tests/native/vehicle_qa.inc` has the `qa_vehicle_*` hooks (batch 7 adds `qa_vehicle_damage`, `qa_vehicle_aim`
and `qa_vehicle_protect`, and `qa_vehicle_state`'s `VEHICLE_HEALTH` lines, for `test_vehicle_damage.py`; the modifier-0 work adds their `gun_modifier=`) (the vehicle fixes add
`qa_vehicle_place_player`, `qa_vehicle_place_actor`, `qa_vehicle_inside`, `qa_vehicle_push`,
`qa_vehicle_watch`, `qa_vehicle_stuck` and `qa_vehicle_throw`; batch 10 adds `qa_vehicle_jump` and
`qa_vehicle_cruise` for `test_vehicle_jump.py`, `test_maw_drive.py` and the walk's capped drives; the jump
governor batch adds `qa_vehicle_governors` (the section's `halo_vehiclegovern` governors) for both; gaps 8 and 11 of batch 5's
critic add `qa_vehicle_blast`, `qa_vehicle_tilt`, `qa_vehicle_probe` and `qa_vehicle_wrecks` for `test_vehicle_damage.py`
items 5 to 8, and damage_qa.inc's `qa_damage_melee_friend` and `qa_damage_hunter_act` for `test_melee_materials.py`
items 6 and 7; the shield-leak work adds damage_qa.inc's `qa_damage_blast KIND WEAPON AMOUNT LABEL`, `qa_damage_state KIND LABEL`, `qa_damage_keep 0|1` and `qa_shield_timing` (`SHIELD_TIMING` per living actor) for items 8 to 10, and `ACTOR_SHIELD` lines to `qa_actor_table`) and `tests/native/combat_qa.inc`
the grenade, item and sound hooks (both listed in `docs/macos-build.md`).
`tests/native/damage_qa.inc` has the damage model hooks for `test_damage_model.py`:
`qa_damage_table` (each projectile's damage at points of its damage range, each grenade's blast
across its radius and 400 rolls of each thrower's range, `halofalloff` cases), `qa_damage_vitals
HEALTH SHIELD`, `qa_damage_hit AMOUNT SIDE ACTOR PLASMA` (an exact hit on the player, from the
nearest hostile actor when ACTOR is 1), `qa_damage_actor AMOUNT SIDE LABEL` (the player's exact hit
on the nearest shielded hostile), `qa_damage_actors` (each actor's weapon and damage modifier), `qa_damage_table`'s
`DAMAGE_FLAGS weapon= trigger= flags= headshots= melee_flags=` lines (the `jptflags`),
`qa_damage_player_unit` (`DAMAGE_PLAYER_UNIT loaded= enabled= path= materials= indirect= shield_type=
stun_ms= recharge_ms= scaled_ms=`: player.txt and `playerShieldTiming`), `qa_damage_shot WEAPON
TRIGGER AMOUNT LABEL` (an exact mid-body hit on the player carrying weapons.txt weapon WEAPON's
trigger TRIGGER `jpt!`, -1 its melee, then `DAMAGE_QA LABEL hp= shield=`) and
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
`qa_actor_combat LABEL` (`tests/native/campaign_qa.inc`, the combat-scaling work, not yet run) prints
`COMBAT_DIFFICULTY` and `COMBAT_DIFFICULTY_MORE` per team (`halocombatscaling`) and `ACTOR_COMBAT` per
living actor (the brain's `combatValues`: `delay`, `slot`, `scaled_error`, `leads`, `separation`,
`grenade_scaled`, `melee`, `smash`).
`tests/native/frontend_qa.inc` adds `qa_progress_state` (with a `QA_PROGRESS_NOTE` line for the
progress warning, kept apart because `conoutf` cuts lines at 260 characters, and a `QA_STATE` line:
the state block's header and its record counts), `qa_progress_reload`, `qa_loadout_state`, `qa_loadout_mark`,
`qa_title_state`, `qa_nav_state`, `qa_checkpoint_save`, `qa_hs_state` (the script's tick, started
threads, visible objects, the recovery's safe floors and grace, whether at a checkpoint or
seated) and `qa_hs_global NAME [VALUE]` for the title menu, saved games and nav points;
`retail_qa.inc` adds `qa_halo_moved`. Batch 8 (not yet run): `qa_loadout_state` adds `unarmed=` and `gun=` (the
AssaultCube slot; 0 is the knife an empty slot stands for), `qa_loadout_mark` gives an unarmed selected
slot the assault rifle first, and `qa_profile_state` prints the loaded script's starting profiles
(`QA_PROFILE i name= health= shield= w0= w1= frags= plasmas=`, then `QA_PROFILE count=`);
`faction_qa.inc`'s `qa_actor_vitality LABEL` prints per living actor `ACTOR_VITALITY LABEL cn= name= kind= team= health= start= shield= shield_max= body_points= shield_points= scripted=` (the stored maxima in Halo's points, `test_actor_ai.py` item 7; the script-vitality change); its `qa_attach_state LABEL` prints each actor `ai_attach` made (`ATTACH_QA LABEL object=
squad= alive= gone= health= pos= distance= variant= drawn=`), each attached object still without one
(`pending= visible= posed= recording= riding= drawn=`) and `count= figures=`, and `qa_attach_move OBJECT
FORWARD SIDE` puts an attached actor that far from where it stands (`ATTACH_QA move object= ok=`,
`test_ai_attach.py` item 10); the fresh-start batch adds, to `qa_attach_state`, a line per attached actor carried in
and not spawned yet (`carried= squad= health= waiting=`), and to `qa_allegiance_state` a
`FACTION_BETRAYALS LABEL <team>=<kills>` line per team (`test_vehicle_damage.py` item 9);
`campaign_qa.inc`'s `qa_device_melee NAME DISTANCE SIDE LABEL` stands the player before a machine,
square on, and swings the melee (`DEVICE_QA melee LABEL object= placed= position=`,
`test_starting_profiles.py` item 8); the AI probes (`qa_hs_cmd`, `qa_hs_ai`) take `encounter/squad`
names, and `qa_hs_cmd` a cutscene flag as `flag:NAME` (its index; `test_actor_ai.py` item 9); the cvar `halo_carriedhold 1` keeps carried actors from spawning (`test_ai_attach.py` item 12);
`presentation_qa.inc`'s `qa_audio_state LABEL` (`AUDIO_QA LABEL loaded= scene= loops=[...] talking=[...]
playing= finished= restart=[...]`) and `qa_conversation_state NAME` (`QA_CONVERSATION NAME status`);
`qa_hs_object boardable obj:NAME`. Batch 9 (not yet run): `damage_qa.inc` adds `qa_damage_melee BEHIND
AWARE LABEL` (the nearest hostile actor renewed, the player 0.6 world units behind or in front of it,
the actor put in combat with the player or unaware, then the player's melee), `qa_damage_region
KIND BEARING HEIGHT WEAPON AMOUNT LABEL` (the player's exact hit with a weapon's projectile `jpt!`
from that bearing, struck at that height of the body) and `qa_damage_needles COUNT LABEL` (needles
striking at once: `DAMAGE_NEEDLES LABEL needles= detonations=`), each with a `DAMAGE_REGION LABEL name=
kind= shield= shield_max= health= alive= status=` line; `DAMAGE_TABLE` adds `core= super= super_timer=
super_inner= super_outer= super_umin= super_side= melee_side=` and `DAMAGE_GRENADE` `core=`; the game
logs `DAMAGE_UNSUSPECTING` and `SUPERCOMBINE` with `halo_damagetrace 1`. `presentation_qa.inc` adds
`qa_fx_yaw DEGREES`, and `FX_QA` lines add `copy= copytex= npot= screen_tex= spot_tex= screen_rebuilds=
spot_rebuilds=`. `CARGO_QA` header lines add `scripted=` and `held=` (ms in a scripted seat).
`retail_qa.inc`'s walker holds while the script holds the player in a seat (`RETAIL_QA held in a
scripted seat`, `let out of the scripted seat after N s`), turning for the look prompts, and the
time held does not count against the section's deadline. `qa_common.read_hscb(path)` reads a `.hscb` (versions 5 to 9) as the
engine does, to its last byte, and returns its names, profiles, squad defaults and object flags.
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

Nothing below has been run since commit `e0806ab` except `test_route_metrics.py` (run statically
while writing it, before batch 5) and `test_campaign_tables.py` (run once, statically, by an
earlier batch). No test has run any of the work in `docs/unverified-changes.md` (batch 5 to the
special-fire change); the steps below name each batch's checks by the batch's name there.
Expect first runs to need fixes in the tests as well as in the game. Work down the list; each step
names what a failure there most likely means. Logs are in `build/qa/` (and `build/ci/<time>/` when
run through `scripts/ci-macos.sh`).

0. **Assets current.** `packages/halo/*.hscb` are version 10 (the squad-count and major-upgrade batch's
   `squad_upgrade_rows` trailer after batch 8's version 9; `qa_common.read_hscb` reads each
   to its end, and the import prints each mission's starting profiles count and story figures), `weapons.txt` format 3,
   `equipment.txt` and `vehicles.txt` format 2, `actors.txt` format 3 (35 `difficulty` lines before the
   variants, `major`, `unit`, `spawn`, `pop`, `resistance`, `shieldtime` and (the actor-melee work) twelve-field `melee` lines, (the special-fire change) three six-field `special` lines, five-field `material`, seven-field `grenade` and (with an actr) four-field `berserk` lines in them: re-run `tools/halo_actors.py`, then
   `tools/import_halo_scripts.py`, if any is missing), `effects.txt` format 1 (with `damage` and
   `materials` lines since gap 11 of batch 5's critic, and the units' body destroyed effects, 24 effects, since the carrier-squad change, with actors.txt's 7 `destroyed` lines: re-run `tools/halo_effects.py` if they are missing), and
   `python3 tools/import_halo_campaign.py "$maps" --metrics-only` reports every map current
   (the last full run, in the final integration review of the shield-leak work and the batches with
   it, found all 55 current, format 2, 50 walked, 3 driven, 2 flown, no stub, no warning, 4,123
   scenery placements, `cgz_raw_sha1` in every file, and the cyborg work's regenerated actors.txt
   left them current; the content-currency fix's run under the system's Python 3.9.6 into a scratch
   copy differed only in the informational `cgz_sha1`), and `python3 tools/import_halo_scripts.py "$maps" --check-scenes` lists no stale
   scene mesh (the use-aware rule's batch regenerated 61 of the 68). Re-run `scripts/import-macos-assets.sh "$maps"` after pulling importer
   changes.
1. **Static gates** (no game, minutes): `./scripts/ci-macos.sh --no-tests` (shell and Python
   syntax, `check-no-retail.sh`, the import check, `test_campaign_tables.py`, the route gate,
   the package build with its 55-section verification, and the no-new-warnings rebuild; since the
   special-fire change both the import check and the package need `packages/halo/player.txt`). Then
   the other static tests, `python3 tests/run.py tables traversal route_metrics`. The route gate
   must find no stub at all (`KNOWN_STUBS` is empty since the c10 section 5 batch; the sections
   that left it, batch by batch, are listed in `docs/unverified-changes.md`, What every section is
   waiting on, all from the importer's own metrics, not from this gate); it
   also reads the Crash Site's metrics now (`halo_a30`, walked, no stub). `test_traversal_routes.py` checks
   d40 section 5's throw rows (consecutive, at one spot, after the s3 control), a10 section 5's
   `door_order` (tunnel_trigger_close before tunnel_door_2's row), b30's walked return, the walking
   assists' AST1 trailers (c10 section 3's two, each under a route row) and the Crash Site's route,
   and (batch 10) that the Maw's planned drive of sections 6 to 8 (about 88 s) fits two thirds of the
   180 s countdown. The route batch: c40 section 1 boards and gets out twice and ends in its exit,
   b30 sections 1 and 2 are walked routes ending in their exits (section 2's door row at
   `shafta_lock_door` before its use row on `holohalo_1`), and d40 section 5 has three to five throw
   rows whose metrics put every blast beyond 2.5 units from the thrower. The final integration
   review resampled c40 sections 1 and 4's flights (1,576 and 167 rows), so no two consecutive
   flight rows are more than 8.5 apart; before it both failed that check. The c10 section 5 batch: no
   route row stands on a bridge `UNCROSSED_BRIDGES` lists, and c10 section 5's route has only crouch,
   jump and ledge-drop rows, exactly two drops, a jump (the 0.28 hop from the crate onto the rubble) and
   ends in its exit; its assist's AST1 surface is under a route row (the generic assist check). The batch after
   the c10 section 5 batch re-planned every section with its scenery (29 walked routes and c40 section 1's first
   walk moved, d40 section 5 by 68 steps; the throw rows and their metrics are the same aims): the traversal
   checks run on the new routes, and `test_retail_scenery.py`'s static half (every SCN1 against its metrics,
   then the mapmodel-angle sweep) belongs here too. The d40 section 3 batch: the route gate expects
   d40 section 3 in mode `walked` (no section is `stock` now), `test_traversal_routes.py` checks its
   route (plain rows only, none more than 8 cube units off the start's level, entry offset under 1, ending
   in `bsp 2,3`), and `test_retail_scenery.py`'s static half now holds d40 section 3's SCN1 (13 placements)
   and c10 section 1's (156, with `x50_rock`; 157 since the use-aware rule's batch, with `crashed_dropship`) against their metrics. The use-aware rule's batch: `python3 tests/test_scenery_uses.py` (with `HALO_SOURCE` set: the uses, decisions and SCN1 trailers of the seven named placements it bakes, ten section placements with b30's crates in two sections; a failure on a use list means the script reading changed, on an SCN1 that the section was not re-imported) and `python3 tests/test_scene_meshes.py` (every campaign scene mesh current; the tool it runs found all ten missions current after the regeneration). A failure here means a
   section was not re-imported after the importer changed (`--metrics-only` says which).
   The content-currency fix: `python3 tests/test_content_currency.py` (no caches; a failure on a synthetic case
   means `cgz_disk_map`, `png_content` or `write_png`'s skip is wrong, one on `cgz_raw_sha1` that the metrics
   predate the fix: re-run `--metrics-only`), then `test_route_metrics.py` on the regenerated metrics (all 55
   current, as before) and `test_scene_meshes.py` (`--check-scenes` with the PNG content comparison: all ten
   current; a texture listed stale there now differs in pixels or size, not only in its deflate stream). If a
   second Python with another zlib is installed (say Python 3.13 from Homebrew), run `--metrics-only` and
   `--check-scenes` under it too: every map and texture must stay current, where before the fix every `.cgz`
   showed stale (the fix's run under the system's 3.9.6 showed the maps; it links the same zlib 1.2.12, so it did
   not exercise the textures). After any later import, a
   texture's mtime moves only when its pixels changed (the failing clip exports whose skins this note used to
   name are gone since the MD3 range fix: they export now, and the import prunes orphan directories).
   The MD3 range fix: `python3 tests/test_md3_range.py` (no caches; a synthetic failure means `write_export`'s
   origin or scale choice or `prune_scene_models` is wrong, an asset failure that the `.hscb` or scene models
   predate the fix: re-run `tools/import_halo_scripts.py "$maps"`), then `test_scene_meshes.py` must report
   every mission's cast on disk and in its `.hscb`, no orphan; the import's summary line must say
   `0 failed cast exports` for all ten (only c10's `keyes x50_4110captain` is listed as not in its graph).
2. **Boot and load** (the first in-game checks): `test_stock_smoke.py`, `test_mission_openings.py`,
   `test_section_loads.py` (all 55 sections live), `test_campaign_progression.py`,
   `test_package_relocatable.py`. A failure here blocks everything after it. a30 section 2
   (`halo_a30_01`), the test bed of most QA-client tests (weapon fire, damage, presentation,
   equipment, actors, factions, difficulty, nav points, script coverage, robustness), now spawns on
   the valley floor in front of the first structure instead of on the rock top (batch 5), and the
   Crash Site before it plays on retail collision at a new scale (batch 6): a test that spawns or
   expects things relative to the old start may need its offsets fixed on its first run, not the game.
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
   `PENDING_WALK` from the results: a10 sections 5 and 6 (door rows; in the walk carried in from
   section 1, and since gap 6's fresh-start fast-forward on their own too, step 8: look for the
   `tunnel_door_2` power going to 1 about 25 s after the route enters tunnel_trigger_close),
   b40 section 6, c10 sections 1, 2 and 4 and c20 sections 1 to 3 (lift and bridge rows) are
   expected to pass. d40 section 5 is expected to pass if the simulated throws land in the game:
   since the route batch the walker throws from floor 2's walkway, lobbing up at pitch 48 into the
   s3 channel's open stretch (five aims, yaw 186 to 194); read its `RETAIL_QA act throw` and
   `GRENADE throw` lines (where each frag lay) against the aims the importer printed. If they hit the
   top floor's walkway edge or fall back out of the channel, the simulation (`GrenadeArcs`: its edge
   sweep is now the engine's; the throw velocity and bounce response are the next suspects) is off;
   `ThrowTarget`'s `reach`, `far`, `below`, `pitches` and `aims` are the knobs. The player is
   protected in the walk; to see that the thrower is out of the blast (7.4 units planned), throw the
   first aim by hand from the spot in the packaged app and check that the shields do not drop. Walk
   the re-planned sections, `--section a30:2`, `--section b40:3`, `--section b30:3` (the last
   ends by boarding the Pelican with E; `RETAIL_QA interact` lines), and since the route batch
   `python3 tests/test_retail_collision.py b30` (sections 1 and 2 on their planned routes: section 1
   to `bsp 0,1`; section 2's `RETAIL_QA act door shafta_lock_door ... opened` should come at once, the
   use row's `act use holohalo_1 ... reached` after the Cartographer scene, and the section end only
   after the walk back up to `bsp 1,0`; a `FAIL door-never-opened` there means
   `Scenario::startCartographer` did not open the door, a stall at the bottom that the scene's
   handoff to `player0_map_done` was not followed), then `test_b30_retail_collision.py` (its own
   waypoints on section 2's unchanged layout: if it fails where it passed before, the shared layout,
   spawn or entities changed with the planned route).
   Batch 9: on Normal the a10 walk now waits in the cryotube until the tutorial lets the player out
   (`RETAIL_QA held in a scripted seat`, then `let out of the scripted seat after N s`; expect about
   one to two minutes); a walk that ends `FAIL deadline` there means the hold did not end, and
   `test_script_seats.py` says why.
   Batch 6: run `test_retail_assists.py` first (if an assist does not carry a body, c10 section 3's
   walk cannot pass), then `--section c10:3` (hall C's plank, hall D's ramp onto the bench, x50 at
   control_lab_trigger, out through the upper doorway once post_helmet_door_4 is powered: the c10
   walk from section 1 now goes on to section 4 and stops at section 5) and `--section a30:0` (the
   Crash Site on retail collision: from the lifepod, two slides, `route flag 8`, down into the basin
   to first_marine_extraction; then the a30 walk from section 0 into `halo_a30_01`). Look at the Crash
   Site in the packaged app too: the opening now starts at the lifepod on the retail BSP's mesh at
   scale 4.1, with the trees, boulders and lifepod as mapmodels at that scale
   (`packages/maps/halo_a30.cfg`'s mapmodel lines; drawing only since the scenery batch, which
   collides with their real shapes from the `.hcol`'s SCN1 trailer); compare it with the authored map (screenshots in
   `build/qa/`), and check the a30 insertion cinematic's cameras land in the same places (the Crash
   Site's script projection changed with it). For d40 section 3's floors (stock until the d40 section 3
   batch, which kept its lower floor out of the route), the static findings
   to check in Halo are in `docs/macos-build.md` (d40 section 3's beam; c10 section 6 left stock
   geometry in the scenery batch, c10 section 5 in the c10 section 5 batch).
   Scenery batch: `test_retail_scenery.py` first (its static half needs no game: if a trailer does
   not match the metrics, re-import the section; in the game, a placement that stops nothing means
   its polygons are not where its model is: the rotation, `euler_rows`, is the first suspect: since the
   final integration review it is Halo's own composition, read from `../Halo3DS`'s source). Then `test_retail_assists.py` (c10 section 6's two ramps up the debris),
   `test_traversal_routes.py` (the SCN1 parse, both sections' assists used by their routes) and the
   walks: `--section a30:0` (the Crash Site round and over the real trees and boulders; since the use-aware rule's batch its lifepod, `pod_of_death`, collides too and the walk starts just outside its rear end, not inside it; `intro_tree` is no longer drawn: a
   walk that snags on a boulder's edge or a trunk, or stands inside one, says the placement or the
   planner's clearance is off), `--section c10:3` (re-planned with the channel crates and hall D's
   debris colliding) and `--section c10:6` (new on retail collision: from the lower BSP 4/3 doorway,
   past `last_door_a`, up the debris over the two assists, across `bridge_e` from `brdige_e_cont_a`,
   then calling and riding `lift_d` up).
   c10 section 5 batch: `test_retail_scenery.py` and `test_retail_assists.py` now include c10 section
   5 (38 placements; one ramp onto the lying crate at the debris pile's foot), then `--section c10:5`
   (new on retail collision: from the bottom of `lift_b`, the 1.9 drop to the lower storey, west to
   the bay, up the ramp onto the crate, the 0.28 hop onto the rubble (route flag 2), over the metal
   sheet and up the invisible `door_blast_collision` ramp onto the balcony, east along the upper
   storey without stepping on `bridge_c`, the 1.9 drop by the lower doorway, into `bsp 3,4` #0). A walk
   that stalls at the crate means the ramp's top does not meet the crate (the scenery pose); one that
   stalls at the hop means the engine's jump does not clear 0.28 there (a second assist would stand
   in for it: still a Halo standing jump); one that slides back off the pile means the sheet or the
   invisible ramp is not where the planner put it. Then the whole c10 walk from section 1, which no
   longer stops at section 5.
   Batch after the c10 section 5 batch (scenery everywhere): every walk is re-planned round its scenery,
   so run `test_retail_scenery.py` over all 50 sections with placements first (a section whose placements
   stop nothing points at the `.hcol`, a load line without the scenery part at a refused file: the
   importer now refuses to write one past the loader's limits), then the full
   `python3 tests/test_retail_collision.py` (every mission; the a10 sections walked in earlier batches
   are re-planned too, so they are no longer known-good: a10 section 1's cryo bay, section 7's hangar
   crates). A walk that snags or stands still beside a crate, barricade, rock or invisible plane means
   the planner's clearance and the engine's body disagree about that placement: compare the
   `RETAIL_QA stuck` position with the importer's scenery for the section. Watch the actors too: they
   follow AssaultCube's flood waypoints, which since the cube-raster stamp are kept out of the scenery (its
   cubes are solid in the native raster), so an actor pushing into a crate or rock (stuck, or jittering
   against it) means a waypoint link still crosses it: run `test_scenery_raster.py` for that section. Then the vehicle sections
   with scenery: `--section d40:8` (55 placements round the pit room; the planned drive came out the
   same, so a crash there that the walk without scenery did not have is the chassis meeting scenery the
   planner passed), d40 sections 6 and 7, and c40 sections 1 and 4 (120 and 122 placements). And in the
   packaged app: the human props now drawn by their environment shader's base map (a10's barricades, gun
   racks and computer banks, b40's and c40's props: textures upside down, missing or stretched mean the
   `senv` base map's UVs need the shader's map scale), the frame rate in a30 section 3 (about 160,000
   triangles, 128,000 of them its pines and rocks), and the Crash Site's mapmodels now turned by
   `mapmodel_angles`: a tilted boulder or the lifepod should sit flush on the ground and match where it
   collides (shoot its edges); before, every turned one was mirrored.
   In the packaged app look at the scenery drawn in the meshes (every section now, the Crash
   Site's small boulders and the models no mapmodel stands for): faces inside out mean the mesh's
   triangle order for models is reversed (`write_world_mesh`'s `scenery`), a model off its collision
   means the rotation; the seven mesh-drawn Crash Site pines' boughs should be cut out by their alpha
   (cutout batches), not opaque cards; check that the Crash Site's mapmodels sit where they collide (mapmodels stand
   on whole cells, up to half a cell off), and shoot a crate, a boulder and a pine (shots stop on
   them now). In Halo, if it can be checked: whether c10 section 5's debris pile is the way round
   `bridge_c` (the c10 section 5 batch routes over it, reading the 28-degree invisible
   `door_blast_collision` plane as a ramp onto the balcony and the steep one as a guard), and whether d40 section 3's player climbs the ribbed walls'
   chamfers onto the grate deck.
   The d40 section 3 batch: `python3 tests/test_retail_scenery.py` (d40 section 3 loads with `, 13 scenery
   objects`; c10 section 1 with 156, `x50_rock`'s boulder in the swamp stopping rays, grenades and bodies;
   c40 section 1 with 120, unchanged), then `--section d40:3` (new on retail collision: the player starts
   on the upper level where the stock route started and walks 169 plain rows to `bsp 2,3`; a stall means
   the upper level's retail floor and the planner disagree; a fall to the lower floor means the start is
   off the walkway's edge), then the whole d40 walk from section 1, which no longer stops at section 3:
   section 2's exit hands over to section 3's start on the upper level, as on stock geometry (if the
   engine instead places the player at `bsp 2,1` on the lower floor, the hand-over needs the section
   start, not the volume). Then `--section c10:1` (its route is unchanged, but the boulder now stands
   beside it: a snag there means its pose is off). Check in Halo, when possible, that a player arriving
   from section 2 cannot reach the upper level from the lower floor either (the judgement call).
   The cube-raster stamp: `python3 tests/test_scenery_raster.py`, static half first (no game: a metrics
   record without `raster`, or a count that does not match the `.cgz`, means the maps predate the stamp:
   re-import; a route row or an actor, weapon or equipment entity in a solid cube means `native_cubes`'
   `opened` set or the placements' filter is off; many wall samples in open cubes mean `scenery_raster`
   misreads the polygons, a flipped y or a wrong cube square). Then its game half: `marked` short of the
   metrics means the loader dropped the vdelta, `lightpass=0` that `load_world` did not recognise the
   section map; waypoints with scenery in their column mean the flood found an open cube the stamp missed
   (compare the listed point with the section's `cube raster:` import line and the scenery there). Then
   watch the actors where the scenery is thickest (the Crash Site's boulders, b30 section 1's beach rocks,
   a50's hangar crates, c10 section 3's channel crates, d20 section 2's rock shards): an actor that walks
   round a crate instead of into it is the stamp working; one that stands still where it used to cross a
   crate-strewn floor means the flood lost a link there (a passage one cube wide between scenery cubes),
   and `wpvisible 1` with `wpinfo 1` (AssaultCube's waypoint drawing) shows the gap. The model tint should not change near
   scenery: a weapon or actor darkening as it passes a crate means the light stopped at a scenery cube
   (`stopslight` in `worldlight.cpp`). Nor should models pop out of sight: an actor, item or Crash Site
   tree behind a waist-high crate or rock should stay drawn, and so should everything round the player
   standing on a tall crate or rock; one that vanishes means the occlusion map stopped at a scenery cube
   (`occludes` in `worldocull.cpp`). Then `test_vehicles.py` item 6, `test_presentation.py` items 4 and 5
   and `test_damage_model.py`'s Warthog EMP, which drive on d40 section 3's raster (`qa_stock_geometry 1`):
   its 13 placements are now solid cubes in the room south of the start (a scratch view of the raster put
   them 9 to 16 cubes south of the straight drive west from the start), so a Warthog that stops short there
   has hit a scenery cube.
   The use-aware rule's batch: `python3 tests/test_retail_scenery.py` for the Crash Site, a10 section 7, a50
   section 1, b30 sections 1 and 3 and c10 section 1 (264, 33, 265, 373, 358 and 157 scenery objects on their
   load lines): the lifepod (`pod_of_death`), the two field generators, b30's three shaft crates and c10's
   crashed dropship must stop the ray, the grenade and the body. Then `python3 tests/test_retail_collision.py
   --section a30:0`: the player must spawn on the ground just outside the pod's open rear end (not inside it,
   not on its roof) and walk the 1,123 rows to `first_marine_extraction`; a spawn that
   drops or slides means the anchor `(31.35, -100.45, 58.7)` is off the ground, a stall in the first rows
   that the pod's polygons and the planner disagree. By hand: the pod is drawn where it collides (the
   mapmodel and the `.hcol` share `euler_rows`/`mapmodel_angles`), no pine stands where `intro_tree` was,
   the dead marines and pilot still sit in the pod (`setup_dead`'s `objects_attach` on a baked parent),
   and `intro_1b` plays right after `intro_1` (the player starts outside `lz_lifeboat`). Watch a few
   cinematics in their regenerated scene worlds (a10's opening, a30's insertion, d40's ending) for missing
   or wrongly textured geometry. The MD3 range fix's 27 clips, never seen before (by hand, through
   `scripts/play-macos.sh`): a10's x30 lifepods (`lifeboat_x30_docked` taking off, `lifeboat_x30_double`'s
   two flights, `lifeboat_burn` falling), a30's `cutscene_intro` pod shedding its airbrakes, a50's energy
   lift (six marines and the Chief), c10's x50 (the Chief, Mendoza, Bisenti, marine 2, the infection form)
   and d20's jump down and lift. Each must move along its path at the right size, not sit at the scene's
   origin (an engine without `md3.h`'s `halo_origin` load draws every frame at the origin), with no
   shimmer or vertex snapping close up; a30's pod is the coarsest (mdlscale 1433, a step of 0.014 units).
   The log must show no `could not load model` for `halo/scene/`. The 75 device and object models
   the fix's `--rebuild` refreshed (written by older exporters; a10 24, d40 12, c10 11, b40 7, b30 6, c40 6,
   c20 4, a50 3, a30 2) must still draw and animate as before.
5. **Combat**: `test_combat_replay.py`, `test_weapon_tags.py`, `test_weapon_fire.py`,
   `test_combat_equipment.py`, `test_damage_model.py` (check the time-to-kill table against the
   build guide), `test_damage_targets.py`, `test_frag_throw.py` (the planned throws at d40's s3
   in the game; run it before walking d40 section 5: if it fails, the walk will too),
   `test_actor_tags.py`, `test_actor_ai.py`,
   `test_factions.py`, `test_flood.py`, `test_difficulty.py` (since the squad-count batch Easy
   selects Normal's starts and lz_search selects 17, 17, 16, 18: a failure on the expected totals
   means a stale `.hscb`, re-run `tools/import_halo_scripts.py`), then `test_major_upgrades.py`
   (static part first: a table mismatch means a stale actors.txt or `.hscb`; in the game, if the
   `qa_major_upgrade_sim` counts differ from the Python port by more than one, look at float
   contraction in `haloconsidermajorupgrade`; if a member is neither its start's variant nor its
   major, `startVariant` and the `major` resolution in halo_ai.cpp's parse; if a revert re-rolls a
   variant, `SquadBot::major` is not reaching `restoreSquads`; by hand on Legendary, a30's first
   fights should show gold-armoured Elite majors and red Grunt majors often, Easy rarely). Then the
   difficulty-table work, all in `test_difficulty.py` and `test_damage_targets.py`: if the static
   table check fails, re-run `tools/halo_actors.py` (a stale actors.txt) or read `difficulty_rows`
   against globals+284; if `table=0`, actors.txt has no `difficulty` lines and every value is the
   fallback; a wrong friend value for the player or the marines means `halodifficultyteamvalue`'s
   hostility test (`haloteamhostile(HALO_TEAM_PLAYER, team)`) or the allegiance defaults; a
   `DIFFICULTY_DAMAGE` pair off by enemy_damage means `halodifficultydamagefactor`'s owner or target
   test; the detector's `half` health off means `Scenario::damageTarget`'s scale. Then by hand, with
   `halo_damagetrace 1`: on Legendary a grunt's plasma on the player logs `DAMAGE_DIFFICULTY
   owner=covenant target=player level=3 factor=1.800`, and the player's health and shield fall by
   1.29 times Normal's share per hit (the HUD), recharging from empty in about 2 s after the 6 s
   delay (8 s on Easy); a marine under Covenant fire logs `target=human factor=1.800`; an Elite hit
   by its own grunt's plasma logs `factor=1.000 divisor=1.800`; `halo_vehicletrace 1` on b40
   section 3's Wraith shows `(team none)` empty and `(team covenant)` with its Elite. Re-run
   `test_frag_throw.py` and the d40 section 5 walk on Heroic: the detectors now need 150 points, so
   if the planned throws no longer bring s3 to 0.1, re-plan `ThrowTarget` (more aims or `least`).
   `test_vehicle_damage.py` and `test_damage_model.py` run on Normal and should be unchanged. Then the
   combat-scaling work: `python3 tests/run.py actor_ai` (item 6, four runs of a30 section 2, one per
   level). If `table=0` or a team value is off, the same checks as `qa_difficulty`'s (a stale
   actors.txt, or `halodifficultyteamvalue`'s hostility test: the marines and the player must read
   Normal); a `tracking`, `leading` or `melee_base` of 1 with `table=0` means the additive fallback in
   `difficultyAt`. A `delay` off by more than 1 ms means the float rounding in `combatValues` against
   the Python; no actor with `slot` equal to `delay` on Easy, Heroic or Legendary means `fire()` never
   reaches the slot write (every actor held its fire: check `ai_magically_see_players` and `shots`).
   Then by hand on Legendary in a30's first fights: Grunts' plasma pistols and needlers fire visibly
   faster with shorter pauses and tighter spread than on Normal, more grenades come, and needles home
   in fully (on Normal they turn at half their rate towards the player: compare a strafe against a
   needler Grunt with `halo_difficulty 1` and `3`); on Easy grenades are rare. Watch a Hunter pair on
   b30 or c10: smashes every 1.65 to 2.25 s on Legendary, 3.3 to 3.9 s on Easy (the melee-delay and grenade-timeout export's figures, below). b40's Wraith mortar
   (a semi-automatic vehicle gun) should fire a little faster on Legendary. If the marines' fire
   changes with the level, their team reads as hostile (`haloteamhostile(HALO_TEAM_PLAYER, human)`).
   Then the Flood tag batch, `python3 tests/run.py test_flood.py` with `HALO_SOURCE` set: item 0
   (static) first, where a mismatch means a stale actors.txt (re-run `tools/halo_actors.py`) or the
   `unit_record` offsets; item 2's burst must be 3 to 6 forms with one `FLOOD_THROW` each (4 to 8 and
   no throws means actors.txt has no `spawn` line); item 5's `FLOOD_POP` must show `one=0` on Normal and
   `one=1`, `hit` at most 1 on Legendary (a pop that hits nothing on Normal: the visibility test or the
   0.75 unit cutoff at the section's scale); item 6's `qa_flood_stagger` scale on Legendary for the
   nopop form only. By hand in c20 or d20: a carrier's forms hop outwards, shooting one form of a tight
   swarm pops its neighbours (a chain on Heroic and Legendary), and a pop never hurts the player.
   Then the carrier-squad change, `test_flood.py` items 7 to 11: if item 8 finds no `FLOOD_ADOPT`,
   check `FLOOD_SPAWN`'s `squad=` (-1/-1: the carrier was not a squad member, `haloscenariosquadof`)
   and that `spawnRing` placed the forms (`Halo: flood Infection Form rises`); if `actors` is off by
   the forms, `assignSwarm` gave them separate swarm ids (they need the same variant and squad); if
   the revert loses them, look for `s spawned` in the progress file and at `restoreSquads`' start
   pointer (`spawnedStarts`). If c20 section 1's own scripts place or erase enc2_1 meanwhile, pick a
   quieter encounter (enc2_4). By hand on c20 and d20: a carrier's blast hurts the player within about
   two units (on Legendary harder, by enemy_damage), never its own forms; `ai_living_count` of a
   carriers squad stays up until the forms are dead, so the next spawner wave comes later than before;
   Sentinels in c10, c20 and d40 now explode when they die (a Sentinel's death explosion, 0.3 to 1 unit).
   Then the actor-melee work, `test_flood.py` items 12 to 16 and `test_actor_ai.py` item 10 (Normal,
   `halo_damagetrace 1`). Item 12 (static, `HALO_SOURCE`): a `melee` mismatch means a stale actors.txt
   or the offsets in `tools/halo_actors.py` (`ACTV_MELEE_RANGE` and friends, `melee_animation`'s seat,
   class and slot 39). Item 13: no `ACTOR_MELEE charge` means `meleeAttack` never ran (`ACTOR_MELEE_QA`
   `model=0`: the variant has no `melee` line; `knife=0`: `spawnEnemy` still gave it a gun); charges but
   no `strike_unit` means the strike test (the reach, the 30-degree facing, or touching bodies at the
   preview's 2.75 scale), `miss` lines that the blow's 0.5 margin or 60 degrees are too tight. Item 14:
   `contact_attached` on a full shield means `halounitshielded` or the cyborg's flags (player.txt's
   `unit` line). Item 15: a fried form means `qa_halo_damage 75` left shield. Item 16: a carrier that
   swings (`strike_unit`) lacks the unit's bit 8; one that never bursts, its range (2.5, stalking) or
   the strike at 0.8; a `suicide` with no `burst` 1.33 s later, `meleeBurst` in `think`. `test_actor_ai.py` item 10: an Elite that never charges at 1.2 units is always
   mid-burst (`burstUntil`) or the delay is counted from a stale `meleeLast`; a sword blow of 75 means
   `meleeSource` took the slot's modified damage. Then by hand in c10 (Normal and Legendary): unarmed
   combat forms rush and hit, armed ones club at arm's length between bursts, infection forms burst on
   the shield for a sliver each and chew a bare player, carriers waddle up and burst; check that the
   walker (protected) still finishes the Flood sections and that Hunters still smash as before
   (`test_factions.py` item 5). `halo_actormelee 0` is the comparison.
   Then the special-fire change, `test_actor_ai.py` item 11 (Normal, `halo_damagetrace 1`). A static
   mismatch means a stale actors.txt or `ACTV_SPECIAL_FIRE`/`ACTV_SPECIAL_DAMAGE` in `tools/halo_actors.py`.
   No `roll` at all: `specialFire` never ran (no `special` line, `halo_specialfire`, the weapon's second
   trigger or charging time) or `specialSituation` never held (`lastSeen` not this think); rolls but no
   `release`: no burst reached a pull facing the target; releases of value 1 with no side-3 hit: the
   trigger `haloactorshottrigger` gave was not 1 (`specialRelease` cleared early) or the guided bolt
   missed; a hit of 35: `haloactorburstmodifier` returned 1 (the burst ended before the shot). In play
   on a30 and a50 (Normal and Legendary): a Jackal now and then pauses, then opens with a charged
   bolt that tracks, strips the shield and overheats its pistol; `halo_specialfire 0` is the comparison.
   Then batch 8's
   `test_starting_profiles.py` (its static part first: if a profile's weapons or rounds differ, read
   `profile_rows` in `tools/import_halo_scripts.py` against the scenario's +840 block; in the game,
   check that an unarmed a10 shows no viewmodel or reticle, fire does nothing and melee still
   strikes, and that the bridge pistol arrives at `shoot_trigger_3`) and `test_ai_attach.py` (if no
   actor appears, look for `HaloScript: <object> attached to <encounter> has no actor variant` or a
   spawn with no floor: `attachActors` retries every second; if Keyes appears twice after a revert,
   the `gone` entries are the first suspect). Re-run `test_combat_equipment.py`, whose a30 start now has
   no frags (a30's starting profile). Batch 9: `test_weapon_tags.py` (the melee side effect, `core`
   and `supercombine` lines) and `test_melee_materials.py`. If the melee from behind does not kill,
   read the `DAMAGE_TRACE` for `side=2` (an old weapons.txt without the fourth melee field gives 0:
   re-run `tools/halo_weapons.py`) and the `DAMAGE_UNSUSPECTING` line's `facing` (the actor's yaw
   convention) and `status`; if the Hunter's head or belly lands on armour, the hit height (the
   hook's `at` point) against `hitHeight`'s body (eye height plus aboveeye) is the first suspect;
   if the supercombine never fires, `SUPERCOMBINE` needs seven needles inside 0.75 s on one body.
   Batch 5's critic: `test_melee_materials.py` items 6 and 7 (a marine struck from behind must log
   `DAMAGE_UNSUSPECTING_SPARED` and live unless the melee's own 50 to 60 kills it; if the belly is
   open after the smash, `firedAt` is being set by something other than `halofiretrigger`; if it stays
   shut after `qa_damage_hunter_act 1`, `materialScale`'s window), then `test_ai_attach.py` items 10
   and 11 (if `ai_teleport_to_starting_location` sends Keyes to his checkpoint spot, the checkpoint
   still rewrites `objectStarts`; if the revert does not bring him back where he stood, `restoreSquads`'
   use of `stood`). In play: hit a marine from behind (no instant kill; enough blows still betray),
   and on a50 walk Keyes somewhere, reach a checkpoint, die, and watch him come back there.
   The shield-leak work: `test_actor_tags.py` (statically first: the leaks, `resistance` and
   `shieldtime` against the coll bytes), then `test_melee_materials.py` items 8 to 10. If a Jackal's
   body hit drains its shield, `DAMAGE_SHIELD` is missing (an old actors.txt: re-run
   `tools/halo_actors.py`) or names material 3 (`hitMaterial`'s facing, the Hunter's convention);
   if a frontal plasma hit reaches the body, `bodyScale` for the hand shield; if the Jackal's shield
   creeps back instead of snapping back at 15 s, `enemyShieldTiming`. In play on a30 and a50: Jackals
   should fall to a few body shots round their shield, bullets should bounce off the shield, plasma
   should pop it; marines should die noticeably faster under fire; stealth Elites have no shield.
   The headshot work: `test_melee_materials.py` items 4, 8 and 11, `test_damage_model.py`'s table
   check and modifier item, `test_vehicle_damage.py` item 10. If no headshot kills, read
   `DAMAGE_FLAGS` (headshots=0: an old weapons.txt without `jptflags`, re-run `tools/halo_weapons.py`)
   and the `DAMAGE_SHIELD` line (a head hit must land on the head-flagged material: the hit height
   against `hitHeight`); if an Elite's body hit logs a 0.5 or 0.8 scale, `hitMaterial`'s indirect
   material; if the Sentinel's needles hurt its body, `materialScale`'s special shield branch; if a
   modifier-0 variant shows scale 0.5, actors.txt failed to load (`haloactordef` null in `spawnEnemy`).
   The cyborg work: `test_damage_model.py`'s cyborg item. If `loaded=0`, player.txt is missing or
   refused (re-run `tools/halo_actors.py`; the console says why); if every hit takes the built-in
   amounts, no `DAMAGE_PLAYER` line means `pendingSource` lost its `materials` (an old weapons.txt);
   a wrong excess (`rifle-excess`) is the shield step's `struckAt` branch; a wrong stun or recharge,
   `playerShieldTiming`. In play on a30 and a50 on Normal: Grunt and Jackal plasma pistols should take
   about twice as long to bring the player down as before, Elite plasma rifles strip the shield fast
   but hurt health slowly, the Covenant turrets hurt more; on Legendary check the player still
   survives the scripted openings (the route walker's survival under fire changes).
   The script-vitality change: `test_actor_ai.py` items 4 and 7, then `test_ai_attach.py` items 6, 8,
   9 and 12 (if `start` stays at the variant's after a maximum, `haloactorsetstarthealth`; if a revert
   loses the maximum, `noteStood`/`restoreVitality` or the `s squad` record's `max` field). In play:
   a10 on Normal, the containment_1 doom_crewman (1 point of 25) should die to the scripted plasma
   blast at `containment_1_flag_4` once the door shuts on him and the player sees him (by default since
   the effect-damage default change; `test_actor_ai.py` item 9 first); cryo_tech and
   the crossfire crew should still survive (protected), airlock_1's marine and the knot crew go down
   to a hit; c10's marine_suicidal should die to one hit, its bay_a marines to one or two.
   Then `test_damage_model.py`'s AI-pilot EMP item: if a stunned AI Ghost still moves, `aiDrive`'s
   stun check is the suspect.
   The melee-delay and grenade-timeout export: `test_actor_ai.py` item 6 (if `melee_timer` or
   `grenade_timeout` is -1, actors.txt lacks the `berserk` or `grenade` field: re-run
   `tools/halo_actors.py`; `GRENADE_NOTE note none` means no lz_search actor has an encounter, so
   `haloscenariosquadof`; a `grenade_left` below `grenade_wait` right after the note, a clock or
   `encounterThrows` key problem). In play on a30 and b30: a Grunt encounter should not throw two
   grenades within 5 s on Normal (2.5 s on Legendary); a Hunter should smash about every 2 s on
   every level but Easy (3.3 to 3.9 s); the Elite commander's sword about every 3.2 s on Normal.
   The playfight change: `test_actor_ai.py` item 8, then the unit-commands item (its impervious and
   player-protection checks changed). If the joiners are not flagged, `configureActor`'s playfight
   line; if the migrated member keeps or misses the flag, `migrateBot`; if a squad reference flags one
   squad only, the `ai_playfight` handler's `& 0xffff`; if the revert loses the flags,
   `savedPlayfighting` in `haloscenariocheckpoint`/`haloscenariorevert`; no `s playfight` record,
   `writeState`; no `DAMAGE_PLAYFIGHT` line, the two encounters never fought (raise the wait) or
   `haloactordamagefactor`. In play on Normal: a10's flank fight and a30's first LZ should lose a
   marine or two before the player joins; a10's containment_1 forward Elite may kill forward security
   once its protection lifts (the doom crewman still waits for the blast); d40's trench Flood hit the player at about a third.
   Also check c10's swamp Flood and incident marines still take no damage (they are protected
   besides impervious) and that a revert keeps `ai_braindead` (a10's first_contact crew).
   The falling damage: `python3 tools/halo_fall_scan.py` first (exit 0; a listed drop with `halo dmg`
   above 0 means a re-planned route now drops 3 Halo units or more: re-plan it or turn
   `halo_fallingdamage` off), then `python3 tests/run.py falling_damage`. Its static half failing on
   the cache comparison means a stale player.txt (re-run `tools/halo_actors.py`). In the game: `FALL_QA
   ... blocked` means a30 section 2's start has no headroom (move the lifts to an open spot); no
   `FALL_DAMAGE` at 4 units means `updateFall` never saw the landing (`onfloor` stayed false, or the
   500 ms gap reset it: a very slow frame); an amount under 64 (10 + 145 x 0.37, level ground) means the
   landing floor's normal (`halofallcontact`) is tilted, under 24 that it is steeper than the ground; a Legendary shield off by 1.4 means halodamage's
   falling exception (`HALO_DAMAGE_CATEGORY_FALLING`) did not apply; no `kind=distance` at 20 units
   means the trace below found nothing (a hole in a30's mesh) or the Halo-equivalent velocity is
   slower than a free fall (a stray slide contact). Then walk every section once with the walker
   unprotected (`haloplayerprotect` off in `retail_qa.inc`, by hand: the protected walker never shows
   it) and look for `FALL_DAMAGE target=player`: d20 section 2's 2.92-unit ledge drop is the closest to
   the 3-unit bound. By hand: a running jump off a10's cryo bay walkways or b30's cliffs hurts (shield
   flash, beyond about 3 Halo units) and kills beyond about 6; sliding down b30's and a30's steep
   rocks hurts less than a free fall of the same height; Grunts thrown by a grenade over a ledge
   take falling damage; infection forms, Sentinels and Engineers never do.
6. **Vehicles**: `test_vehicles.py` (item 6 since the d40 section 3 batch drives on d40 section 3 with
   `qa_stock_geometry 1`: the log must say `retail collision off for halo_d40_03`; if it says `loaded N
   retail collision surfaces`, the switch was set too late), `test_enemy_vehicles.py`, `test_vehicle_damage.py` (batch 7:
   vehicle body vitality, the Wraith destroyed with its pilot and `unit_get_health` -1, the turret
   gunner variant's modifier; if the Wraith is never hit by the rifle, check `halovehicletrace`'s box
   against the drawn model; in play, check that b40's Covenant fire passes through their own Wraith
   (an actor's shots stop only on a vehicle a hostile rides) and that a marine at the Warthog's gun
   still misses (the all-zero Warthog gunner variant gives no error, `fightingVariant`) while its rounds
   now hit at full damage (`gun_modifier=1`, item 10)), then the slow `test_maw_drive.py` (the Maw's escape to the finale under
   the countdown). Then the planned vehicle routes (batch 7): `python3 tests/test_traversal_routes.py`
   (the vehicle rows), `python3 tests/test_retail_collision.py d40` (sections 6 and 7 driven on their
   planned routes after the walked 1 to 5: read the `RETAIL_QA vehicle` lines; a stall on section 6's
   ramp up from the elevator means the car's climb (12 units a second squared against half the
   vehicle gravity, 35.8 there since batch 10, times sin(pitch)) needs more run-up than the landing
   gives; section 8 is now expected to reach `grand_finale`, `PENDING_WALK`, driven at its planned
   27 units a second, `qa_vehicle_cruise`) and `--section c40:4` (board, fly to the pad, walk into `bsp 0,5`; if the
   flight hits the canyon walls, `FLIGHT_MARGIN` and the walker's reach are the knobs) and `--section
   c40:1` (since the route batch expected to pass, `PENDING_WALK`: the walk from the platform through
   the doorway corridor into `pulse_1_trigger`, `door_c1` then powered, back to the Banshee, the flight
   down to the c bridge ending up to 6 units from the pad, and the walk through `door_c1` into
   `bsp 2,10`; a stall in the corridor at retail y about -634.5 or -621.75 means a BSP seal is still in
   the `.hcol`, `held_bsps`; one at `door_c1` that `pulse_1_trigger` did not power it, read the
   script status). In play: walk through the doorway into the generator room by hand (the body
   should pass with room either side) and look at the room drawn from BSP 12.
   Batch 10, the Warthog's jumps: first re-run `python3 tools/import_halo_campaign.py "$maps" --mission
   d40 --part 8 --metrics-only` and check it prints `arrives only at 27.0 units a second` and ends in
   `grand_finale` (673 rows, three jumps: the pit room's two and a short hop at about x 688). Then `python3 tests/run.py vehicle_jump`: read each jump's
   line (left the ground within about 4 units of the take-off row, tangent, landed where, how far on;
   `hop on the run-up` lines are only informational). The jump governor batch: the static part checks
   one governor per jump matching `halo_d40_08.cfg`, the log must show `VEHICLE_QA governors count=3
   enabled=1`, the `capped-*` passes run with governors off at the cruise cap and the `governed-*` passes
   from top speed with no cap (look for `Halo vehicle: governor at 459,154,7.8 holds 27.0`; a `governed-0`
   that stops near x 494 and falls means the braking came too late or overshot). If the drop lands short in the pit or
   hits the hanging bridge segment (the motion stops near x 494 and falls), compare the take-off
   height and speed with the plan; the planner finds the segment hit at 0.8 of top speed (28.8) and
   clear at 0.77 (27.7), so the margin at 27 is about a unit a second; the planner starts the fall
   once the rear wheels pass the edge, as the engine does: check the log's take-off point against that. If the bowl's kick hits the doorway's lintel (it stops near x 556 and falls back), the
   launch `tangent` is too steep: the log's `tangent` before the take-off should be about 0.25 of the
   speed (14 degrees). Then `test_retail_collision.py --section d40:8` and the slow
   `test_maw_drive.py` (item 3: section 8 to the finale). Check sections 6 and 7 still drive: the
   lower vehicle gravity (35.8 and 22.0, from 40) makes crests launch further (section 7's floatier
   at 22). In play: drive the run by hand at full throttle: by the end of the pit room's entry road the
   jump governor (halo_vehiclegovernor, on by default) should brake the Warthog to 27 units a second
   (the HUD shows about 130 KM/H, 27 units a second at the section's scale of about 2.24, and the line "Easing off for the jump  -  130 KM/H") and the
   drop should clear the hanging segment; the same at the bowl's lip and the hop at about x 688.
   Judge whether the braking reads as an accommodation a player accepts or as the car being taken
   over (the radius is GOVERNOR_SECONDS of travel, 27 units; the brakes come on only above the cap).
   Then `halo_vehiclegovernor 0` and once at full speed, to see the Warthog meet the hanging segment;
   `halo_vehiclegovernorhint 0` hides the HUD line; drive a Warthog over b30's and a30's crests and a Ghost off a ledge to judge the new
   gravity's feel elsewhere.
   Gaps 8 and 11 of batch 5's critic (vehicle damage polish): `python3 tests/run.py vehicle_damage`
   items 5 to 8. Item 5: if the Wraith's driver is still hurt, `covered` in the `VEHICLE_QA blast`
   line says whether `halovehicleridercovered` saw the seat as covered (0: the seat flags or the
   coll's flag 2 were not read, re-run `tools/halo_vehicles.py`); if the Wraith's health does not drop,
   the grenade went off out of reach (`halovehiclesnear`'s `boxDistance`) or behind the floor (its
   line to the box's middle). Item 6: no `death explosion deals its damage` means `effects.txt` has no
   `damage` line for the death explosion (re-run `tools/halo_effects.py`); no hit on the Grunt with
   the line present means it stood out of reach or its middle out of the centre's sight. Item 7: a
   level probe that already hits means the box's height (`vehicleTop`) is larger than the test's
   geometry assumes; a tilted one that misses, the sign of the pitch or roll in `hitBox` against the
   drawn model (`halovehiclesrender`): shoot a Warthog nosing up a ramp and a banking Banshee by hand
   and check where the shots stop. Item 8: a wreck the revert does not remove means the checkpoint's
   vehicle copy kept it (`halovehiclescheckpoint` runs before the destruction there). In play, with
   `halo_vehiclewreck 1`: blow up b40's Wraith and a Ghost and walk into their wrecks (solid, drawn,
   smoking); with it off, nothing is left (as before). Rocket a Wraith from close by with an Elite in
   it: the Elite should die with it, not before.
7. **Scripts, scenes and presentation**: `test_script_coverage.py`, `test_mission_exits.py` (item 5,
   batch 8: if the loop it starts is one d40's bridge scene stops itself, the restart is right not to
   happen; pick another loop; item 3 now runs the static `ini_post_rescue` with `qa_hs_script
   ini_post_rescue 3`, the final review's QA action for static scripts, since waking (action 0) only
   starts startup, dormant and continuous scripts: if the player is not placed at
   `prison_player0_teleport`, that action is the first suspect),
   `test_campaign_ending.py`, `test_nav_points.py`, `test_presentation.py` (items 4 and 5 on d40 section 3
   with `qa_stock_geometry 1` since the d40 section 3 batch, as `test_damage_model.py`'s Warthog EMP),
   `test_devices.py`,
   `test_library_platform.py`. Batch 9: `test_script_seats.py` (run it before the a10 walk: if the
   cryotube never lets the player out on Normal, read which tutorial script is still waiting with
   `halo_script_status`; `halo_scriptseatmax` 180 s is the guard), and `test_presentation.py` items 5
   to 7 (if `resetgl` crashes or the copy stays `screen_tex=0`, `halofxcleanupgl` in `cleanupgl` is
   the first suspect; if `screenres` keeps 800x600 the window may not resize under the test's flags,
   which the test reports without failing). Gap 11 of batch 5's critic: `test_presentation.py` item 8
   (the `damage` lines first: missing means `tools/halo_effects.py` was not re-run; then the
   interpreter: a `deals its jpt! damage` line with `halo_effectdamage 0` means the cvar is not read;
   no `halo_effectdamage = 1`, the default was not rebuilt). The effect-damage default change (on by
   default): `test_actor_ai.py` item 9 (a static part, then a10 section 1 on Heroic: if the crewman
   lives with the cvar on, `halo_damagetrace 1`'s `DAMAGE_TRACE` lines for him, then `haloareablast`'s
   line of sight; `ownerless=0` means the null-owner scope was lost). `python3 tools/halo_effect_reach.py`
   lists every damaging scripted blast against the planned routes (no game run). In play, on Normal:
   a10's containment_1 forward_security and crossfire crew, c10's swamp rocketeers, c40's glass Flood
   and d40's Flood near the escape blasts should fall to them as in Halo; d40's Warthog escape (the
   36 u/s floor, about twice Halo's speed on section 8) can meet `enc6_9_blast3` about 2.2 wu off (a
   few points of shield); the walker is protected (`haloplayerprotect`), so its runs are unchanged.
   The script-jpt change: `test_script_damage.py` (its static half first: no `jpt` records means
   `tools/halo_effects.py` was not re-run; then a30: a pulse generator that takes health means
   halodamage's only-hurts-shields step, `DAMAGE_JPTFLAGS` missing means the record's flags were not
   parsed, `halo_scriptjpt 0` not giving 40 means the fallback was lost; a10: a zapper hit means the
   0-damage early return in `haloareablast`). In play: the a10 shield tutorial (cryo bay) and c40's
   three pulse generators (the c40 walk into `pulse_1_trigger`, `test_retail_collision.py`) still go on
   (no script waits on the damage); unprotected on Normal the pulse leaves the shield empty and the
   health as it was. Guaranteed kills (b30 shafta_killer, c20 fall_killerz, c40 kill_box_1, d40
   prelift_slayer) now spare a protected player, as Halo's object_cannot_take_damage does: a walker
   that used to die in one of those volumes while protected now lives.
8. **Saved games and the front end**: `test_frontend_progress.py` (title menu, Continue with the
   version 3 state block, fallbacks, backup; item 14, batch 8: the `board`, `gain` and `attached`
   records; item 15, gap 10: the scripted cryotube seat in `s boarded`: if Continue leaves the player on
   foot, look for `bad boarded vehicle` in the log, then the revert's re-seat conditions in
   `haloscenariorevert`; item 16, the stood-members change: after `qa_squad_hurt 0.5 0.25`, a
   checkpoint, a death and `qa_halo_revert`, every lz_search member must come back with health about
   0.5 and within 1.5 units of where it stood, `stood` above `fallback` in `SQUAD_STOOD_STATE`; a
   member back at its start means `stoodSpot`'s floor or wall check failed there, health 1 that the
   `s squad` record lost its trailing fields; an auto-revert or a script save between the death and
   the probe can move members, so re-read the log's revert lines before blaming the restore),
   `test_campaign_replay.py` (all 55 sections in one run).
   Gaps 3, 4, 6 and 10 of batch 5's critic, in this order: `test_mission_exits.py`'s static part
   (`SCENE_WAKES`: if it differs, the decompile walk or the cue table changed; update the table only
   after reading what the new script does) and item 6 (if `music_b30` does not go on, check that the
   insertion's first tick ran before the skip, `QA_SKIP 1` at 12 s, then `Scenario::endCue`'s transfer);
   `test_script_seats.py` items 6 and 7 (if no seat is taken, look for `left the player a seat in
   insertion_pelican, which is gone`: the Pelican was hidden at the scene's end; if the player rides
   but is never let out, `cutscene_insertion_b`'s `recording_time` wait is the suspect; in the guard
   run a fall after the release means `haloplayerfloorbelow` found a floor the body does not fit);
   `test_ai_attach.py` items 6 to 9 (if Keyes does not come along, look for `comes along into section
   5`: missing means the snapshot was empty, `snapshotAttached` runs only on live ticks; present but no
   `came along with the player` means every spot behind the player had no floor or was out of the
   player's sight (the final review's wall check), `spawnCarried` retries each second; since the fresh-start batch
   the log says which with `waits to come along into section 5:`); `test_starting_profiles.py` item 7, then `python3 tests/test_retail_collision.py
   --section a10:5` and `--section a10:6` started on their own (both expected to pass now: a stall at
   `tunnel_door_2` with `mission_tunnel` started means its `tunnel_trigger_close` wait, read the
   walker's door log; `cryo_door_2` should be open on arrival) and the chained a10 walk from section 1.
   In play: b40's opening (the ride in the Pelican's bay after the flyover, out at the landing), b30
   with its music, a50's hangar with Keyes and the marines arriving behind you from the cell block,
   and a Continue from the cryotube.
   The fresh-start batch (two gaps and two worries of batch 6's critic), in this order: `test_starting_profiles.py`
   item 7 (a missing `started afresh` line for sections 2 to 4 means `freshScripts`' rows; a missing
   `tutorial_setup resumed` line on Normal means `resumeAfterWake` found no top-level `wake` of the
   named script, which it logs as `cannot resume`; a thread of the section not started 12 s in means
   `mission_a10` is still in a `sleep 120` or waiting for a flag the row does not set; read
   `HS_SCRIPT mission_a10 2` and the globals) and item 8 (if the swing does not open `melee_door_2`,
   look for `MELEE_QA lunge` with `halo_meleetrace 1`: a hostile in the cone takes the blow first; then
   `placed=0` means the object is not a machine of section 6; a placed swing that misses means the
   door's panels at frame 99 are not where `haloscenarioqadevicefront` aims, 0.2 Halo units off the
   middle); `test_ai_attach.py` item 12 (if `held` shows Keyes spawned, the hold cvar was not read; if
   the migration line is missing, `qa_hs_cmd`'s `hangar_captain_halls/squad_switch` did not resolve:
   `haloscenarioencounter` returns -1 for a squad name it does not know); `test_vehicle_damage.py`
   item 9 (a betrayal count of 1 means a blast with an owner; check `destroyVehicle`'s
   `haloeffectblast` call). Since the final integration review a10 section 7 has a fresh-start row too:
   walk `--section a10:7` on its own again (it walked before without `mission_final`, whose encounters and
   `final_door_1` now run; a stall at `final_door_1` means `end_trigger` lies past it and the door waits on
   `lifepod_2_anti`). In play: a10 on Normal from section select at sections 2, 3, 4 and 7 (no
   cryotube, the HUD whole, the threads of the section running: the cafeteria and airlock fights),
   then on Normal through section 6 by `motiontracker_door_1` and a melee on `melee_door_2`; a50's
   hangar arrival with Keyes as before; a Ghost destroyed beside marines (they may die, but do not
   turn on you).
   The ownerless-kill batch: `python3 tests/run.py vehicle_damage` item 9's Covenant run (and
   `route_metrics`, after a `--metrics-only` run, for the format 2 and the missing `cfg_current`
   check). If Grunt A's `DEATH_TRACE` reads `killer=player`, the kill happened outside the ownerless
   scope (`haloareablast`'s `ScriptScope`, `ownerlessDamage` in `halo_ai.cpp`); no `DEATH_TRACE` for
   Grunt A means it survived (a material scale, or the blast's line of sight to its middle). An
   Elite driver's line with `alerted=0` means Grunt B was out of the 15-unit neighbour radius or
   in combat (not braindead: `qa_actor_turn`), and the run then proves nothing about alerts; a
   missing Grunt B in `ACTOR_QA after` means it stood in the blast (move it further along). Edit
   mode, by hand: load `halo_d40_08`, `xmap_store a` then `xmap_restore a` (edit mode), then load a
   map with no governors: `qa_vehicle_governors` should report `count=0` (before the batch it kept
   d40 section 8's three); reload `halo_d40_08`: `count=3`, not 6. In play: a Ghost destroyed
   beside a Covenant squad does not bring its survivors' attention to you by that kill alone.
   Since the final integration review the scripts' kills are ownerless too: with `halo_damagetrace 1`,
   `qa_hs_ai ai_kill <squad> 0` (or `qa_hs_cmd damage_object ...`, `ai_erase_all`) on an actor with
   hostile neighbours braindead within 15 units should log `DEATH_TRACE ... killer=- ownerless=1
   alerted=0`; `killer=player` means the scope in `Scenario::eraseSquads`, `ai_erase_all` or
   `damage_object` (`halo_script_ai.inc`) is not ownerless. `test_script_seats.py`'s Monitor item and
   `test_ai_attach.py` item 3 (`damage_object` on Keyes) should be unchanged.
9. **Slow and manual**: `python3 tests/run.py --slow` (`test_autumn_opening.py`,
   `test_package_input.py`, `test_maw_drive.py`), then a played pass in the packaged app: a new
   game on Normal through a10 and a30 at least, a Continue after quitting at a checkpoint, the
   flown c40 sections, the driven Maw escape, and the presentation items no test can judge (the
   flashlight's look, camera shake, screen effects, the rocket launcher's placement, the fallback
   reticle, the stealth Elites' shimmer). Batch 8 in play: a10 from the cryo bay unarmed to the
   bridge pistol and the first rifle picked up (it fills the empty slot); a50's rescue with Keyes
   and the freed marines following as actors and the game lost if Keyes dies; d40's bridge scene with
   the music before it coming back after; a Continue at a30's extraction once the Pelican is enterable;
   c20 on Easy with the shotgun. Batch 9 in play: a10's opening in the cryotube (the view from the
   tube's camera marker, E doing nothing, the tutorial letting you out), a melee from behind on a
   Grunt or Elite that has not seen you (one blow), Hunters hit at the head from the side, seven
   needles on an Elite (the pink super detonation, its sound), the Engineer's animation (b30's tags
   only: spawn one with `qa_actor_spawn 11`), the blur after changing the resolution in the options
   menu, and a balance pass with `halo_actormaterials 1` (the default since the headshot work: compare the
   build guide's "Time to kill actors" table, and check that pistol and sniper headshots kill Grunts,
   Jackals, unshielded Elites and Hunters from behind, that the Flood are slow to fall to the assault
   rifle and the sniper rifle, that needles drain shielded Sentinels' shields only, that the
   energy-sword commander's lunge kills a full-health player on Normal, and that marines with pistols
   or sniper rifles do not land headshots often). Batch after the c10 section 5 batch: the cutscenes'
   tilted and rolled camera points (the static comparison found `poseOffset` and the AssaultCube view equal to
   Halo's `matrix4x3_rotation_from_angles`, so a horizon that leans the wrong way or a shot looking off its
   subject would point at the roll's sign in `renderAngles` or `transplayer`, not the order), and the Crash
   Site's turned mapmodels against the retail game if it can be checked. Note what was seen in the
   Status table of `docs/macos-build.md` and take each verified item out of its "Not yet verified in
   the game" list or out of `docs/unverified-changes.md`.
10. **The whole chain**: `./scripts/ci-macos.sh --release --asan` once everything above passes,
    as the release check.
