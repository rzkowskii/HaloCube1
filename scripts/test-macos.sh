#!/bin/bash
# Build the game and the halo-ac-qa client, package the app, then run tests/.
# Arguments go to tests/run.py, for example --slow or a name filter.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
"$root/scripts/check-no-retail.sh"
"$root/scripts/package-macos.sh" -DHALO_BUILD_QA=ON
exec python3 "$root/tests/run.py" "$@"
