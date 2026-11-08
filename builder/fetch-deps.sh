#!/usr/bin/env bash
# Fetch pinned upstream sources into ref/. Existing trees are left unchanged.
# Usage: bash builder/fetch-deps.sh [all|melee|melee-pc|dawn|sdl|sdl-legacy|sdl-dusklight|mesa]
# `melee` is the doldecomp/melee checkout the recompiler reads symbols from (no
# game code is compiled from it). `melee-pc` is fetched only for its vendored
# Aurora (the Switch renderer patches target that tree). `mesa` clones the
# Switch Mesa/NVK fork; build it with its own build-switch.sh (see BUILDING.md).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
cd "$repo"

# Patch bases; update these only alongside the corresponding patches.
DECOMP_URL="https://github.com/doldecomp/melee.git"
DECOMP_REV="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["decomp"]["commit"])' "$repo/tools/port_source_pins.json")"

MESA_URL="https://github.com/danfromtico/mesa-switch"
MESA_REV="0ff52079ca9e9e837ed27eb95a8d239f439a1864"

MELEE_PC_URL="https://github.com/999sian/melee-pc"
MELEE_PC_REV="7c9a468f4f8206780c4cd762be1da7772daaeabf"

DAWN_URL="https://github.com/encounter/dawn"
DAWN_REV="80ee0043018a51532ea0fa2e77496cc66634157e"

SDL_URL="https://github.com/libsdl-org/SDL"
SDL_LEGACY_TAG="release-3.4.4"
SDL_DUSKLIGHT_TAG="release-3.4.10"

have() { command -v "$1" >/dev/null 2>&1; }
have git || { echo "git not found on PATH." >&2; exit 69; }

skip_if_present() {
  local dir="$1"
  if [[ -d "$dir" ]]; then
    printf 'Already present, leaving alone: %s\n' "$dir"
    return 0
  fi
  return 1
}

fetch_melee() {
  skip_if_present ref/melee && return 0
  echo "==> ref/melee (doldecomp/melee $DECOMP_REV, shallow)"
  mkdir -p ref/melee
  git -C ref/melee init --quiet
  git -C ref/melee remote add origin "$DECOMP_URL"
  git -C ref/melee fetch --depth 1 --quiet origin "$DECOMP_REV"
  git -C ref/melee checkout --quiet --detach "$DECOMP_REV"
}

fetch_mesa() {
  skip_if_present ref/mesa-switch && return 0
  echo "==> ref/mesa-switch ($MESA_REV, shallow)"
  git clone --depth 1 "$MESA_URL" ref/mesa-switch
  git -C ref/mesa-switch fetch --depth 1 --quiet origin "$MESA_REV"
  git -C ref/mesa-switch checkout --quiet --detach "$MESA_REV"
}

fetch_melee_pc() {
  skip_if_present ref/melee-pc && return 0
  echo "==> ref/melee-pc ($MELEE_PC_REV)"
  git clone "$MELEE_PC_URL" ref/melee-pc
  git -C ref/melee-pc checkout --detach "$MELEE_PC_REV"
  # Aurora is vendored inside melee-pc; no separate clone.
  git -C ref/melee-pc submodule update --init --recursive
}

fetch_dawn() {
  skip_if_present ref/dawn && return 0
  echo "==> ref/dawn ($DAWN_REV)"
  git clone "$DAWN_URL" ref/dawn
  # The pinned commit is not on the default branch; fetch it by hash.
  git -C ref/dawn fetch origin "$DAWN_REV"
  git -C ref/dawn checkout --detach "$DAWN_REV"
  # The standalone CMake build needs these; the full submodule set is enormous and unused here.
  git -C ref/dawn submodule update --init --recursive third_party/abseil-cpp third_party/jinja2 \
    third_party/markupsafe third_party/spirv-headers/src third_party/spirv-tools/src \
    third_party/vulkan-headers/src third_party/vulkan-utility-libraries/src
}

fetch_sdl_legacy() {
  skip_if_present ref/SDL && return 0
  echo "==> ref/SDL ($SDL_LEGACY_TAG, comparison/fallback variant)"
  git clone --depth 1 --branch "$SDL_LEGACY_TAG" "$SDL_URL" ref/SDL
}

fetch_sdl_dusklight() {
  skip_if_present ref/SDL-dusklight && return 0
  echo "==> ref/SDL-dusklight ($SDL_DUSKLIGHT_TAG, default variant)"
  git clone --depth 1 --branch "$SDL_DUSKLIGHT_TAG" "$SDL_URL" ref/SDL-dusklight
}

mkdir -p ref

case "${1:-all}" in
  melee)         fetch_melee ;;
  mesa)          fetch_mesa ;;
  melee-pc)      fetch_melee_pc ;;
  dawn)          fetch_dawn ;;
  sdl)           fetch_sdl_legacy; fetch_sdl_dusklight ;;
  sdl-legacy)    fetch_sdl_legacy ;;
  sdl-dusklight) fetch_sdl_dusklight ;;
  all)
    fetch_melee
    fetch_melee_pc
    fetch_dawn
    fetch_sdl_dusklight
    ;;
  *)
    echo "usage: $0 [all|melee|melee-pc|dawn|sdl|sdl-legacy|sdl-dusklight|mesa]" >&2
    exit 64
    ;;
esac

cat <<'MSG'

Reference trees ready under ref/.

Still required before building: a built Mesa/NVK Switch tree, exported as
MESA_NVK_ROOT. Switch ships no vendor Vulkan driver for homebrew and devkitPro's
switch-mesa package is EGL/GLES-only, so this is not optional and not something
this script can shortcut. See BUILDING.md, section "Mesa/NVK".

Next:
  bash builder/docker.sh image
  bash builder/docker.sh graphics all
  bash builder/docker.sh melee configure
  bash builder/docker.sh melee build
MSG
