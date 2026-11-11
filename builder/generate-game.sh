#!/usr/bin/env bash
# Translate the player's own Melee disc into the generated game sources, on the
# host (needs python3 >= 3.11 and git). Nothing from the disc is committed; the
# output lives in build/ (ignored). Usage: bash builder/generate-game.sh /path/to/GALE01.iso
# MELEE_NX_PLAYBACK=1: the Melee-NX Replays app's translation (Slippi's playback code set) in build/game-playback.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
iso="${1:-}"
[[ -n "$iso" && -f "$iso" ]] || { echo "usage: $0 /path/to/melee-ntsc-1.02.iso" >&2; exit 64; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 69; }

decomp="$repo/ref/melee"
[[ -f "$decomp/config/GALE01/symbols.txt" ]] || { echo "Pinned decomp missing; run bash builder/fetch-deps.sh melee" >&2; exit 66; }

game="$repo/build/game"; sys_dir="$repo/port/slippi_sys"; playback_args=()
if [[ "${MELEE_NX_PLAYBACK:-0}" == 1 ]]; then
  game="$repo/build/game-playback"; sys_dir="$repo/port/slippi_sys_playback"; playback_args=(--playback)
fi
mkdir -p "$repo/build/disc"
dol="$repo/build/disc/main.dol"
[[ -f "$dol" ]] || python3 "$repo/tools/extract_dol.py" "$iso" "$dol"

# Natively compiled decomp C (docs/NATIVE_C.md): the files in port/recomp/gml_files.txt are compiled
# for the GameCube, lowered onto guest memory and built for the console, in the toolchain image.
# Their hook list must exist before the translation (bootstrap_port.py) runs.
gml_out="$game/generated/gml"
mkdir -p "$gml_out"
gml_files=$(grep -v '^#' "$repo/port/recomp/gml_files.txt" 2>/dev/null | tr '\n' ' ' || true)
if [[ -n "${gml_files// /}" ]]; then
  phys="$(cd "$repo" && pwd -P)"
  rm -f "$gml_out/gml_native.o"   # a failed lowering must not leave the previous object behind
  # Functions Slippi patches stay translated (native C would not see the patch).
  python3 "$repo/tools/gml/gecko_patched.py" "$dol" "$decomp/config/GALE01/symbols.txt" 0x8065CC80 "$sys_dir" > "$gml_out/keep_translated.txt"
  docker run --rm -v "$phys:/project" "${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}" \
    bash /project/tools/gml/build.sh "/project/${gml_out#$repo/}" $gml_files </dev/null
  python3 "$repo/tools/gml/gen_wrappers.py" "$gml_out/gml_native.ll" "$gml_out/symbols.map" "$gml_out"
else
  : > "$gml_out/gml_hooked.txt"; echo '// no native decomp C' > "$gml_out/gml_hle.cpp"
fi

python3 "$repo/tools/bootstrap_port.py" --decomp-root "$decomp" --dol "$dol" \
  --build-dir "$game" ${playback_args[@]+"${playback_args[@]}"} --gct-base 0x8065CC80 --frontend switch --stage generate
# Native GX from the decompilation's matching source (compiled with GCC; see the script).
python3 "$repo/tools/generate_gx_native.py" "$game/generated/gxnative" --decomp-root "$decomp"
cp "$repo/port/runtime/gx/gxnative/gxn_glue.h" "$game/generated/gxnative/"
# The Homebrew Menu icons (the emblem is drawn here, never committed): needs Pillow.
py="$repo/build/venv/bin/python"; [[ -x "$py" ]] || py=python3
if "$py" -c "import PIL" 2>/dev/null; then
  "$py" "$repo/tools/make_switch_icon.py" "$repo/build/icon.jpg"
  "$py" "$repo/tools/make_switch_icon.py" "$repo/build/icon-replays.jpg" replays
fi
echo "Generated game: $game/generated/guest"
