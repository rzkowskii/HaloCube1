# Halo in AssaultCube

An Apple Silicon macOS adaptation of *Halo: Combat Evolved*. The campaign runs from **The Pillar of Autumn** through **The Maw** in 55 linked AssaultCube sections. The maps use original Xbox BSP geometry, textures, lighting and scenario placements, and the original compiled HaloScript drives each mission. AssaultCube handles movement, rendering and the engine; most sections collide with the original structure BSP.

Open [Halo in AssaultCube.app](<build/Halo in AssaultCube.app>) in Finder or run `./scripts/play-macos.sh`. The game opens on the title menu: **Continue** resumes the saved game at its last checkpoint, **New Game** asks for a difficulty and starts The Pillar of Autumn, **Mission select** lists the missions reached so far. In a game, **Esc** pauses, with Restart section, Mission select, Difficulty, Settings, Main menu and Quit. Pass `'-ehalo_preview'` to launch the original standalone Crash Site preview.

| Input | Action |
| --- | --- |
| W A S D, mouse | Move and aim; in a vehicle, throttle and steer (the Banshee strafes with A and D) |
| Space, Left Shift | Jump and crouch; in a Banshee, climb and descend |
| Left click, R | Fire and reload; in a vehicle's gun seat or at a Ghost's, Scorpion's or Banshee's controls, fire its gun |
| Q or 1 / 2 | Switch between your two carried weapons |
| Mouse 2 or Z | Zoom (pistol, sniper rifle, rocket launcher); in a vehicle, its second trigger (fuel rod, machine gun) |
| G, H | Throw a grenade, switch between frag and plasma grenades |
| F | Melee (it lunges at an enemy just out of reach) |
| X | Flashlight |
| E | Use a control, swap for a weapon on the ground, get in or out of a vehicle, flip one back, board a landed Pelican |
| Enter | Skip the current cutscene or continue after completing a section |
| Esc, F5 | Pause or restart the current section |
| Left click after death | Revert to the most recent checkpoint |

What is in the build: the ten missions with their retail scripts, cutscenes (retail cameras, scene geometry, cast models, animation clips and audio), devices, lifts and trigger volumes; Covenant, Flood, Sentinel, Monitor and Keyes models; actor AI from the retail `actr` and `actv` tags with squads, firing positions, command lists, factions and allegiances, Hunters, stealth Elites and flyers; the nine campaign weapons from their tags, frag and plasma grenades, equipment and powerups, per-weapon audio; the Warthog, turrets, Ghost, Scorpion and Banshee for the player, and those and the Wraith for actors; the retail HUD with nav points; checkpoints, four difficulties, a saved game that resumes the mission script, and an ending with credits. Retail collision covers all 55 sections (50 on foot, 5 driven or flown), with fall and wedge recovery and the scenery's collision; none is left on AssaultCube's stock geometry (most of the routes over it are not yet walked in the game).

**Status.** Much of the recent work has been written and compiled while testing was on hold and has not been run in the game; [docs/macos-build.md](docs/macos-build.md) has the per-section Status table (traversal mode and what has been verified) and the list under "Not yet verified in the game", and [tests/README.md](tests/README.md) the test plan for when testing resumes. Known gaps: eight sections have stub routes in the route gate (among them a30 section 2 and b40 section 3, which have not been walked), c10 sections 3, 5 and 6 and d40 section 3 stay on stock geometry because the engine's fixed jump cannot make the retail links, d40 section 5's fourth manifold cannot be shot from a walkable spot, and the movement, vehicle physics and AI are AssaultCube's and the engine's, not retail parity.

The campaign is single-player; co-op, multiplayer and splitscreen are out of scope. Retail assets are generated from a local Xbox copy of Halo CE (the expanded maps directory named in the build guide) and ignored by Git. This is an unofficial fan project, not affiliated with Microsoft, 343 Industries or Bungie; see [NOTICE.md](NOTICE.md).

```bash
./scripts/build-macos.sh && ./scripts/play-macos.sh   # development
./scripts/package-macos.sh                             # build/Halo in AssaultCube.app (relocatable, signed)
./scripts/test-macos.sh [--slow] [--release]           # build, package, run the tests
./scripts/ci-macos.sh [--release] [--asan]             # local CI: static checks, import check, build, tests
```

[Build and asset import guide](docs/macos-build.md) · [Tests](tests/README.md) · [Notice](NOTICE.md)
