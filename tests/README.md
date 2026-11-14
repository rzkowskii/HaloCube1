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
| `test_campaign_replay.py` | All 55 sections complete in one run |
| `test_weapon_tags.py` | Retail weapon table matches known tag values and the engine's parsed copy |
| `test_weapon_fire.py` | Rate of fire, zoom, overheat, needler ramp, charged shot, swap, revert, enemy weapons and drops |
| `test_actor_tags.py` | Retail actor table matches known tag values and the engine's parsed copy |
| `test_actor_ai.py` | Hearing and squad alerts, burst fire, marines against the Covenant, squad positions, script AI commands, command lists; the unit and object-state commands (berserk, grenades, damage immunity, vitality, damage, follow, migrate, attach, renew, suspend, player vitality, hover, open, collideable, a50 cell doors) |
| `test_campaign_ending.py` | game_won on The Maw and the finale cue raise the ending and mark progress beaten; credits end at the menu; early game_won is ignored; game_lost reverts |
| `test_script_coverage.py` | Every command used in a mission's sleep_until/if conditions is bound, and so is every ai_, unit_, object_ and vehicle_ command any script calls; a10 looking tutorial finishes; d40 countdown follows the escape deadline and holds off endgame_cinematics |
| `test_difficulty.py` | halo_difficulty picks each squad's Easy to Impossible start count (a30 lz_search matches the importer table), game_difficulty_get returns it, enemy health and damage scale, the choice persists |
| `test_flood.py` | The squad cap counts only living actors (c10 swarm encounters place behind 128 dead entries); a killed Carrier Form bursts into 4 to 8 Infection Forms; an Infection Form on a dead marine raises a Flood combat form; Infection Forms leap |
| `test_package_relocatable.py` | check-no-retail passes clean and fails on staged or stray retail files; packages/ is ignored by default; package-macos.sh rejects a stub assets dir; packaged app has no Homebrew paths, is signed, carries licences and launches from a copied location |
| `test_autumn_opening.py` | Slow: four-minute Autumn opening at normal speed |
| `test_package_input.py` | Slow: real keystrokes into the packaged app |
| `test_maw_drive.py` | Slow: Warthog escape driven by keystrokes |

## QA client

`halo-ac-qa` is the game plus console commands for injected positions, damage
and scripted outcomes. Configure with `-DHALO_BUILD_QA=ON`. The hooks live in
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
