# Build Halo in AssaultCube on macOS

The engine source comes from AssaultCube Lockdown Edition v1.3.0.2, commit `1ece5af7533983bb8f827616381d282470793d90`. Engine and ENet notices are in `source/`. Stock data is read from a local AssaultCube RC1 release. The ten Halo missions, viewmodels, characters, compiled mission scripts, cinematic geometry and audio are generated from expanded Halo CE Xbox map caches. [AssaultCube v1.3.0.2](https://github.com/assaultcube/AC/tree/v1.3.0.2)

## Dependencies

Install Apple's command line tools, Python 3.9 or later, CMake, and the Homebrew formulae `sdl2-compat`, `sdl2_image`, `libvorbis` and `libogg`. The cinematic audio importer uses FFmpeg. The client uses the system OpenGL and OpenAL frameworks. `CMAKE_PREFIX_PATH` can point CMake at libraries outside the default Homebrew prefix.

```bash
brew install cmake sdl2-compat sdl2_image libvorbis libogg ffmpeg
```

Set `AC_ASSET_ARCHIVE` to the original `AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2` file. The scripts also look for it in `~/Downloads`. The staging script checks its SHA-256, extracts game data and retains the bundled license in `build/runtime/docs/license.html`.

## Import, build and play

Run from the repository root with the directory that contains the expanded North American Xbox Halo CE 1.0 maps. Halo3DS's prepared `halo-source` directory has this layout. The campaign import needs all ten mission caches; the asset library also imports any multiplayer and menu caches in the same directory.

```bash
maps="/path/to/halo-source"
./scripts/import-macos-assets.sh "$maps"
./scripts/package-macos.sh
```

Open `build/Halo in AssaultCube.app` in Finder. The app includes the staged data and converted files from `packages/`. It links against the build Mac's Homebrew libraries. Run `./scripts/build-macos.sh` and `./scripts/play-macos.sh` for source development. The campaign importer supports `--mission` and `--part` for focused map edits. The script importer supports `--mission` and can regenerate the cinematic BSP meshes and models with `--rebuild`. Run `python3 tools/import_halo_scripts.py "$maps" --mission a10` before repackaging to upgrade an existing Autumn flyby mesh.

`build/retail-assets/catalog.json` indexes the local development library. It records every cache and tag, all BSPs, scenario placement tables, animation tracks, and looping sound definitions. The library copies the original caches into `sources/` and exports first-person and static models, full BSP meshes and collision, bitmap PNGs, and sound variants. Bitmap exports include cubemap faces and volume slices; P8 bump images preserve palette indices. WAV assets preserve the original Xbox sample encoding. The catalog records conversion gaps, and the copied caches retain the original data for future work. The campaign app uses its own converted media; multiplayer flow, the original menu, and additional weapon behavior still need engine integration. Rerun only the library with `python3 tools/import_halo_library.py "$maps"`; add `--rebuild` to regenerate converted assets.

The Silent Cartographer uses retail collision across all three sections. To update an existing campaign install, regenerate the maps and script projections, then package the app.

```bash
python3 tools/import_halo_campaign.py "$maps" --mission b30
python3 tools/import_halo_scripts.py "$maps" --mission b30
./scripts/package-macos.sh
```

The original structure collision surfaces handle movement, weapon traces, and bot sight. Follow the path from the beach to the facility, use the Cartographer terminal, and climb back to the surface. Press **E** near the Pelican's rear ramp once it reaches the landing zone. The adapted entry leaves the security door open so the route starts at the descent.
To launch that section directly, run `./scripts/play-macos.sh '-ehalo_section 4 2'`.

The launchers use `~/Library/Application Support/Halo in AssaultCube` for settings, screenshots and `clientlog.txt`. Set `HALO_AC_PROFILE` to use another directory. Pass engine options directly to `play-macos.sh`. Use `--stock` first to smoke-test the AssaultCube baseline with a separate profile. For example, this launches the stock map in a window and requests a screenshot and a clean exit after startup.

```bash
./scripts/play-macos.sh --stock -t0 -w960 -h600 --loadmap=ac_complex \
  '-esleep 1000 [screenshot] 1; sleep 1800 [quit] 1'
```

## Campaign asset library

Run `python3 tools/import_halo_asset_library.py "$maps"` to extract the Xbox campaign source assets into `build/halo-assets`. Each mission's JSON index maps original tag IDs and virtual addresses to its tag region, BSPs, texture images and mip chains, sound permutations, model parts, animation tracks and recordings. The material and lens-flare entries identify their bitmap dependencies and preserve the authored stages and reflections. Shared payloads use their SHA-256 as the filename in `blobs/`. Use `--mission` for a single cache or `--output` for a different library path. The playable app uses the converted assets in `packages/`. The source library is available for subsequent engine integration.

## Retail weapons

`tools/halo_weapons.py` reads every `weap` tag in the ten campaign caches, follows each trigger to its projectile and damage tags, and writes `packages/halo/weapons.txt`. The game loads that table at startup. Rounds, reload time, rate of fire and its ramp, spread, heat, battery, charge time, zoom levels, aim assist, projectile speed and homing, and damage all come from the tags. The importer also records the first-person and pickup models from the asset library, so run it after `import_halo_library.py` and before `import_halo_campaign.py`. The campaign importer stores each enemy's carried weapon (from its actor variant) and the scenario's weapon placements in the section maps.

```bash
python3 tools/halo_weapons.py "$maps"
python3 tools/import_halo_campaign.py "$maps"
```

The player carries two of the nine campaign weapons. Walking over a weapon you carry takes its rounds, and **E** swaps the current weapon for one on the ground. **Mouse 2** or **Z** steps through the weapon's zoom levels. Holding fire charges the plasma pistol. Covenant weapons show battery and heat instead of rounds, and overheat. Enemies fire their actor's weapon with a reduced share of its damage and drop it when killed. Reticle artwork, weapon sounds beyond the MA5B, M6D and plasma rifle, and the rocket launcher's first-person placement still need work.

## Actor AI

`tools/halo_actors.py` writes `packages/halo/actors.txt` from the campaign `actr` and `actv` tags: vision and hearing, firing range, rate and error, burst timing, combat range, target leading, grenades, vitality, shield-based hiding, panic chances, berserk thresholds and position hold times. Run it after `halo_weapons.py`. `import_halo_scripts.py` then writes version 6 `.hscb` files. These add each encounter's combat positions, squad states and position-group masks, platoon rules, per-start variants and command lists, and the scenario's command lists.

Campaign bots think with `source/src/halo_ai.cpp` instead of AssaultCube's bot AI. AssaultCube still handles movement, pathfinding and physics. Actors:
- notice targets inside their vision cone, or hear gunfire, after a reaction delay
- alert nearby squadmates
- take combat positions from their squad's groups for attacking, defending or pursuing
- fire in retail bursts and throw grenades
- hide when their shields are low, panic when friends or leaders die, and go berserk when badly hurt
- follow command lists.

Marine and crew encounters now spawn on the player's side and fight the Covenant and Flood. Platoon rules switch squads between attacking and defending as casualties mount. The scripts' AI commands work, including `ai_actors` lists, `ai_status`, `ai_living_fraction`, `ai_strength`, magic sight, follow and fight orders, `ai_migrate` to new positions, command lists and their status, and `objects_can_see_*`. Vehicle boarding commands are stand-ins until vehicles exist. Balance has only been checked in short QA runs: enemies are accurate at close range.

## Retail mission scripting

The importer reads the original scenario syntax datum array, strings, script and global tables, oriented trigger volumes, camera points, flags, chapter strings, AI squads and conversations. It keeps the compiled nodes and their validated handles. The script import includes 1,673 device placements, 119 BSP switch records, and 349 recorded input streams across the ten missions. Some Xbox voice tags carry no subtitle text.

The client executes script expressions at 30 ticks per second. It handles script calls, startup and dormant threads, conditionals, timing, globals, original volume tests, supported hostile placements, conversations and checkpoint requests. It keeps thread and global state during the ordinary section-to-section handoff. Cinematic entry and exit cues connect the retail script functions to the current route. Those scenes use the original camera data, scripted audio starts and waits, and retail BSP and object assets. Camera transitions use the observer's quintic interpolation for position, field of view and axis-angle rotation, carrying velocity and acceleration across waypoints. Compressed skeletal clips are sampled at the retail cadence. The in-game console command `halo_scripttrace 1` logs camera cuts and audio starts. `halo_script_status` lists the unbound commands reached in the current mission.

| System | Implemented | Remaining parity work |
| --- | --- | --- |
| Mission scripting | Retail programs, door and lift position tracks, shared device groups, player controls, adjacent BSP transitions, checkpoints and supported squad operations | Multi-level traversal, alternate BSP routes, complete command bindings, mission branches and co-op |
| Cutscenes | Retail cue scripts, cameras, fades, titles, scene geometry, skeletal clips, recorded movement, seat and attachment markers | Original movement simulation, per-frame marker transforms, lighting, additional effects and full cue coverage |
| Dialogue and sound | Original impulse and conversation assets, tag-defined script timing and selected music and Foley tracks | Speaker and permutation selection, positional mixing and class gain transitions |

## Reference and verification

The local Halo3DS fork supplies the Xbox cache layout, BSP and texture formats, model animation transforms, Xbox IMA ADPCM and compressed animation reference decoders, and HaloScript execution semantics. The importers read the prepared Xbox CE 1.0 A30 cache directly. On this Mac the expanded file is under `My 3DS/build/local-assets/NEW2DS/halo-source/` in the neighboring project. Its SHA-256 is `287959f34a7379a1dbec5fec15b4d94d17fe97ccd861f5c0c8fc80148b7ed293`. [Halo3DS upstream](https://github.com/Generalkidd/Halo3DS)

The signed Apple Silicon app played The Pillar of Autumn opening at normal speed through the return to player control. The flyby uses the original filtered hull lightmaps and six nozzle flares. The camera position and viewing direction matched the Azahar capture within 0.003 Halo units and 0.024 degrees across eleven sampled ticks. Sky compositing and the hull's bump and emissive shading still differ. The native device checks exercised door collision before, during and after opening, the first Autumn BSP switch, and the second Assault on the Control Room lift carrying the player. A separate replay completed all 55 sections with injected positions and combat outcomes, including direct placement in the Library's elevated BSP switch volumes. A continuous manual playthrough and the Library lift approaches remain unverified. No iOS simulators were used.

| Mission | Playable sections | Adapted landmarks and flow |
| --- | ---: | --- |
| The Pillar of Autumn | 7 | Cryo area, ship corridors, lifeboat route |
| Halo | 3 | Crash Site, structure and cavern approach, evacuation area |
| The Truth and Reconciliation | 5 | Canyon, muster bay, hangar, prison and return |
| The Silent Cartographer | 3 | Beach approach, Cartographer interaction, return to the beach |
| Assault on the Control Room | 7 | Forerunner corridors, snowy exterior and control room |
| 343 Guilty Spark | 7 | Swamp, containment interiors, escape into the swamp |
| The Library | 4 | Four Library BSPs and Index interaction |
| Two Betrayals | 5 | Reverse Control Room route and final pulse generator interaction |
| Keyes | 6 | Cruiser exterior and interiors, implant recovery and hangar return |
| The Maw | 8 | Autumn interiors, reactor interaction and timed Warthog escape |

## Verification

Run `./scripts/test-macos.sh` to build the game and the `halo-ac-qa` client, package the app and run the quick test tier. Add `--slow` for the full-speed opening and keystroke tests. Results, logs and profiles go to `build/qa/`. [Test guide](../tests/README.md)
