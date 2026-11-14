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
python3 tools/import_halo_campaign.py "$maps"
python3 tools/import_halo_models.py "$maps/a30.map" "$root"
python3 tools/import_halo_campaign_actors.py "$maps" "$root"
python3 tools/import_halo_audio.py "$maps/a30.map" "$root"
python3 tools/import_halo_scripts.py "$maps"
python3 tools/import_halo_library.py "$maps"
