#!/bin/bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
if [[ $# != 1 || ! -d "$1" ]]; then
    printf 'Usage: %s /path/to/halo-source\n' "$0" >&2
    exit 2
fi
maps="$(cd -- "$1" && pwd -P)"
cd -- "$root"

python3 tools/import_halo_a30.py "$maps/a30.map"
# The library exports the weapon view and pickup models that weapons.txt names,
# and the campaign sections record enemy weapons and placements by its index.
python3 tools/import_halo_library.py "$maps"
python3 tools/halo_weapons.py "$maps"
# Actor behaviour tables; the scripts importer stores their indices per squad start.
python3 tools/halo_actors.py "$maps"
# Vehicle definitions and models; the campaign sections record vehicle placements by their index.
python3 tools/halo_vehicles.py "$maps"
python3 tools/import_halo_campaign.py "$maps"
python3 tools/import_halo_models.py "$maps/a30.map" "$root"
python3 tools/import_halo_campaign_actors.py "$maps" "$root"
# The script effects, the flashlight and the sound classes; the audio import decodes the effects'
# sounds and the vehicles' engine, crash and suspension sounds the tables name, and every
# permutation of the units' dialogue sounds dialogue.txt names (pain, death, screams, combat calls).
python3 tools/halo_effects.py "$maps"
python3 tools/halo_dialogue.py "$maps"
python3 tools/import_halo_audio.py "$maps/a30.map" "$root"
python3 tools/import_halo_scripts.py "$maps"
