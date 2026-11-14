# Halo in AssaultCube

An Apple Silicon macOS adaptation of *Halo: Combat Evolved*. The campaign runs from **The Pillar of Autumn** through **The Maw** in 55 linked AssaultCube sections. The maps use original Xbox BSP geometry, textures, lighting, and selected scenario placements. AssaultCube handles movement, collision, rendering, and AI.

Open [Halo in AssaultCube.app](<build/Halo in AssaultCube.app>) in Finder or run `./scripts/play-macos.sh`. The game resumes at the most recently unlocked section. Use **Esc → Mission select** to start another mission, or pass `'-ehalo_preview'` to launch the original standalone Crash Site preview.

| Input | Action |
| --- | --- |
| W A S D, mouse | Move and aim |
| Space, Left Shift | Jump and crouch |
| Left click, R | Fire and reload |
| Q or 1 / 2 | Switch between the MA5B and M6D |
| G, F | Throw a frag grenade or melee |
| E | Use a nearby control or activate a marked objective |
| Enter | Skip the current cutscene or continue after completing a section |
| Esc, F5 | Pause or restart the current section |
| Left click after death | Revert to the most recent checkpoint |
| W / S, A / D | Drive and steer the Warthog in The Maw |

The build includes Covenant, Flood, and Sentinel models and animations, a shield and motion-tracker HUD, checkpoint recovery, campaign progress on disk, and an adapted timed Warthog escape. Most sections use two optional waves placed from the retail scenario and route markers that connect AssaultCube's walkable layer.

The build loads the original compiled HaloScript from all ten missions. It runs selected cinematics with retail cameras, BSPs, chapter titles, cast models, animation clips, and audio. It replays the Xbox scenario recordings and uses the authored device animations and groups to operate doors, controls, and lifts. Original trigger volumes can advance between adjacent playable BSP sections.

The Silent Cartographer uses the original structure BSPs for movement, weapon traces, and bot sight. The Cartographer terminal runs the original map scene, and the return route leads to the Pelican landing zone. Press **E** at the ramp to board once the Pelican arrives.

The playable route uses 55 AssaultCube sections. Full retail parity remains in progress, including continuous traversal of multi-level BSPs, vehicle and biped physics, cinematic effects and poses, allied squads, encounter branches, the full arsenal, vehicle combat, and co-op. Retail assets are generated from a local Xbox copy and ignored by Git.

The local asset library includes the original campaign, multiplayer and menu caches plus converted geometry, collision, models, bitmaps and sound variants. The build guide covers regenerating the library and explains what the app currently uses.

[Build and asset import guide](docs/macos-build.md)
