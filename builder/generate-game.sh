#!/usr/bin/env bash
# Translate the player's own Melee disc into the generated game sources, on the
# host (needs python3 >= 3.11 and git). Nothing from the disc is committed; the
# output lives in build/ (ignored). Usage: bash builder/generate-game.sh /path/to/GALE01.iso
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
iso="${1:-}"
[[ -n "$iso" && -f "$iso" ]] || { echo "usage: $0 /path/to/melee-ntsc-1.02.iso" >&2; exit 64; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 69; }

decomp="$repo/ref/melee"
[[ -f "$decomp/config/GALE01/symbols.txt" ]] || { echo "Pinned decomp missing; run bash builder/fetch-deps.sh melee" >&2; exit 66; }

mkdir -p "$repo/build/disc"
dol="$repo/build/disc/main.dol"
[[ -f "$dol" ]] || python3 "$repo/tools/extract_dol.py" "$iso" "$dol"

python3 "$repo/tools/bootstrap_port.py" --decomp-root "$decomp" --dol "$dol" \
  --build-dir "$repo/build/game" --gct-base 0x8065CC80 --frontend switch --stage generate
echo "Generated game: $repo/build/game/generated/guest"
