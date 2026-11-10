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

# Natively compiled decomp C (docs/NATIVE_C.md): the files in port/recomp/gml_files.txt are compiled
# for the GameCube, lowered onto guest memory and built for the console, in the toolchain image.
# Their hook list must exist before the translation (bootstrap_port.py) runs.
gml_out="$repo/build/game/generated/gml"
mkdir -p "$gml_out"
gml_files=$(grep -v '^#' "$repo/port/recomp/gml_files.txt" 2>/dev/null | tr '\n' ' ' || true)
if [[ -n "${gml_files// /}" ]]; then
  phys="$(cd "$repo" && pwd -P)"
  rm -f "$gml_out/gml_native.o"   # a failed lowering must not leave the previous object behind
  docker run --rm -v "$phys:/project" "${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}" \
    bash /project/tools/gml/build.sh /project/build/game/generated/gml $gml_files </dev/null
  python3 "$repo/tools/gml/gen_wrappers.py" "$gml_out/gml_native.ll" "$gml_out/symbols.map" "$gml_out"
else
  : > "$gml_out/gml_hooked.txt"; echo '// no native decomp C' > "$gml_out/gml_hle.cpp"
fi

python3 "$repo/tools/bootstrap_port.py" --decomp-root "$decomp" --dol "$dol" \
  --build-dir "$repo/build/game" --gct-base 0x8065CC80 --frontend switch --stage generate
# Native GX from the decompilation's matching source (compiled with GCC; see the script).
python3 "$repo/tools/generate_gx_native.py" "$repo/build/game/generated/gxnative" --decomp-root "$decomp"
cp "$repo/port/runtime/gx/gxnative/gxn_glue.h" "$repo/build/game/generated/gxnative/"
echo "Generated game: $repo/build/game/generated/guest"
