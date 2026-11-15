#!/bin/bash
# Local CI for the macOS build: static checks, retail-leak check, import check, build, package and
# the quick test tier, each step logged under build/ci/<time>/ with a summary at the end.
#
#   ./scripts/ci-macos.sh                 static checks, import check, build + package, quick tier
#   ./scripts/ci-macos.sh --no-tests      everything up to the package, no tests
#   ./scripts/ci-macos.sh --release       also the release tier (asset robustness, 55-section soak)
#   ./scripts/ci-macos.sh --asan          also build the ASan/UBSan client (build/macos-asan) and run
#                                         the robustness and soak tests with it
#   ./scripts/ci-macos.sh --metrics       refresh the route metrics first (import_halo_campaign.py
#                                         --metrics-only; needs HALO_SOURCE, the expanded Xbox maps)
#
# Exits non-zero at the first failed step (the summary says which). Written while testing was on
# hold: not yet run end to end (docs/macos-build.md, "Not yet verified in the game").
set -uo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
tests=1 release=0 asan=0 metrics=0
for arg in "$@"; do
    case "$arg" in
        --no-tests) tests=0 ;;
        --release) release=1 ;;
        --asan) asan=1 ;;
        --metrics) metrics=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) printf 'unknown option %s\n' "$arg" >&2; exit 2 ;;
    esac
done

logs="$root/build/ci/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$logs"
summary=()
started=$SECONDS

# step <name> <command...>: runs it with its output in $logs/<name>.log; stops CI on failure.
step() {
    local name="$1"; shift
    local begin=$SECONDS
    printf '==> %s\n' "$name"
    if "$@" >"$logs/$name.log" 2>&1; then
        summary+=("PASS  $(printf '%5ds' $((SECONDS - begin)))  $name")
    else
        summary+=("FAIL  $(printf '%5ds' $((SECONDS - begin)))  $name  ($logs/$name.log)")
        finish 1
    fi
}

finish() {
    printf '\n=== CI summary (%ds, logs in %s)\n' $((SECONDS - started)) "$logs"
    printf '%s\n' "${summary[@]}"
    exit "$1"
}

# 1. Static checks: shell syntax, Python syntax, no retail data in git.
step shell-syntax bash -c 'for f in "$0"/scripts/*.sh "$0"/tests/tools/*.sh; do bash -n "$f" || exit 1; done' "$root"
step python-syntax bash -c 'cd "$0" && python3 -m py_compile tools/*.py tests/*.py tests/tools/*.py scripts/*.py' "$root"
step check-no-retail "$root/scripts/check-no-retail.sh" "$root"

# 2. Import check: the 55-section import is complete (the package script's verification, without
# building), the C++ tables agree with the importer, and no section's route became a stub.
if (( metrics )); then
    if [[ -z "${HALO_SOURCE:-}" ]]; then
        printf 'HALO_SOURCE (the expanded Xbox map directory) is needed for --metrics\n' >&2
        exit 2
    fi
    step route-metrics-refresh python3 "$root/tools/import_halo_campaign.py" "$HALO_SOURCE" --metrics-only
fi
step import-complete bash -c 'cd "$0" && PYTHONPATH=tools python3 - <<"PYTHON"
from pathlib import Path
from import_halo_campaign import CRASH_SITE, MISSIONS, STOCK_SECTIONS, VALIDATED_COLLISION
packages = Path("packages")
missing = [f"halo/{t}" for t in ("weapons.txt", "equipment.txt", "actors.txt", "vehicles.txt", "effects.txt", "hud.txt")
           if not (packages / "halo" / t).is_file()]
for name in ("maps/halo_a30.cgz", "maps/halo_a30.cfg", "maps/halo_a30.mesh") + (("maps/halo_a30.hcol",) if CRASH_SITE else ()):
    if not (packages / name).is_file():
        missing.append(name)
for m in MISSIONS:
    if not (packages / f"halo/{m.cache}.hscb").is_file():
        missing.append(f"halo/{m.cache}.hscb")
    for part in range(len(m.sections)):
        retail = m.cache in VALIDATED_COLLISION and (m.cache, part) not in STOCK_SECTIONS
        for suffix in ("cgz", "cfg", "mesh") + (("hcol",) if retail else ()):
            name = f"maps/halo_{m.cache}_{part + 1:02}.{suffix}"
            if not (packages / name).is_file():
                missing.append(name)
print(f"{len(missing)} missing" + (": " + ", ".join(missing[:20]) if missing else ""))
raise SystemExit(1 if missing else 0)
PYTHON' "$root"
step campaign-tables python3 "$root/tests/test_campaign_tables.py"
step route-metrics python3 "$root/tests/test_route_metrics.py" --table

# 3. Build the game and the QA client, package the app (package-macos.sh verifies all 55 sections,
# bundles the dylibs, signs, and runs check-no-retail again). No new compiler warnings: the build
# log's warnings must all be the stock engine's deprecation warnings.
step package "$root/scripts/package-macos.sh" -DHALO_BUILD_QA=ON
# A failed rebuild fails the step too (its output has no warning lines to catch).
step build-warnings bash -c 'cd "$0" && touch source/src/halo*.cpp tests/native/*.cpp || exit 1
    out="$(cmake --build build/macos --parallel 4 2>&1)"; status=$?
    printf "%s\n" "$out"
    (( status == 0 )) || exit 1
    if printf "%s\n" "$out" | grep "warning:" | grep -qv "Wdeprecated-declarations"; then exit 1; fi
    exit 0' "$root"
if (( asan )); then
    step build-asan "$root/scripts/build-macos.sh" --asan
fi

# 4. Tests.
if (( tests )); then
    step quick-tier python3 "$root/tests/run.py"
    if (( release )); then
        step asset-robustness python3 "$root/tests/test_asset_robustness.py"
        step soak python3 "$root/tests/test_soak.py"
    fi
    if (( asan )); then
        step asset-robustness-asan python3 "$root/tests/test_asset_robustness.py" --asan
        step soak-asan python3 "$root/tests/test_soak.py" --asan
    fi
fi
finish 0
