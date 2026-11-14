#!/bin/bash
# Fail if any retail-derived file is tracked, staged, or untracked-but-not-ignored
# (i.e. would be committed by `git add -A`). Generated Halo assets live under
# packages/ and must stay git-ignored. Usage: check-no-retail.sh [repo-root]
set -euo pipefail

root="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)}"
# Files under packages/ that are ours rather than retail-derived.
allowed=(packages/maps/halo_a30.cfg)
bad=()
while IFS= read -r -d '' file; do
    [[ -e "$root/$file" || -L "$root/$file" ]] || continue
    flag=0
    case "$file" in
        packages/*)
            flag=1
            for ok in "${allowed[@]}"; do [[ "$file" == "$ok" ]] && flag=0; done ;;
        build/*) flag=1 ;;
        *.hscb|*.hcol|*.cgz|*.xbe|*.map|*.blob|*.cache) flag=1 ;;
    esac
    [[ $flag -eq 1 ]] && bad+=("$file")
done < <(git -C "$root" ls-files -z --cached --others --exclude-standard)

if (( ${#bad[@]} )); then
    printf 'Retail-derived files would be committed:\n' >&2
    printf '  %s\n' "${bad[@]:0:20}" >&2
    (( ${#bad[@]} > 20 )) && printf '  ... and %d more\n' $(( ${#bad[@]} - 20 )) >&2
    exit 1
fi
echo "check-no-retail: clean"
