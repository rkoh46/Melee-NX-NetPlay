#!/usr/bin/env bash
# Configure and build the NRO inside the toolchain container. Needs the
# generated game (builder/generate-game.sh on the host) and, for the aurora
# frontend, the Dawn/SDL builds (build-graphics.sh) and a Mesa/NVK tree.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
stage="${1:-all}"
jobs="${MELEE_BUILD_JOBS:-4}"
frontend="${MELEE_NX_FRONTEND:-headless}"
case "$stage" in configure|build|all) ;;
  *) echo "usage: $0 [configure|build|all]" >&2; exit 64;; esac
case "$frontend" in headless|aurora) ;;
  *) echo "MELEE_NX_FRONTEND must be headless or aurora" >&2; exit 64;; esac

game="$repo/build/game"
build_dir="$repo/build/switch-$frontend"
require_file() { [[ -f "$1" ]] || { echo "Required file missing: $1 ($2)" >&2; exit 66; }; }

do_configure() {
  require_file "$game/generated/guest/guest_sources.cmake" "run builder/generate-game.sh first"
  require_file "$game/port-inputs.json" "run builder/generate-game.sh first"
  require_file "$repo/ref/melee/src/sysdolphin/baselib/fobj.c" "run builder/fetch-deps.sh"
  local extra=()
  if [[ "$frontend" == aurora ]]; then
    sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
    case "$sdl_variant" in
      dusklight) sdl_source="$repo/ref/SDL-dusklight"; sdl_build="$repo/build/sdl-switch-dusklight" ;;
      legacy)    sdl_source="$repo/ref/SDL";           sdl_build="$repo/build/sdl-switch" ;;
      *) echo "MELEE_SDL_VARIANT must be dusklight or legacy" >&2; exit 64 ;;
    esac
    require_file "$repo/build/dawn-switch/src/dawn/native/libwebgpu_dawn.a" "run builder/docker.sh graphics all"
    require_file "$sdl_build/libSDL3.a" "run builder/docker.sh graphics all"
    extra=(-DMELEE_DAWN_SOURCE="$repo/ref/dawn" -DMELEE_DAWN_BUILD="$repo/build/dawn-switch"
           -DMELEE_SDL_SOURCE="$sdl_source" -DMELEE_SDL_BUILD="$sdl_build")
    if [[ -f "${MESA_NVK_ROOT:-/mesa-nvk}/src/nouveau/vulkan/rust_switch_stubs.c" ]]; then
      extra+=(-DMESA_NVK_ROOT="${MESA_NVK_ROOT:-/mesa-nvk}")
    else
      echo "No Mesa/NVK tree: compiling without it (MELEE_NX_ALLOW_NO_MESA); the link step needs the archives." >&2
      extra+=(-DMESA_NVK_ROOT= -DMELEE_NX_ALLOW_NO_MESA=ON)
    fi
  fi
  cmake -S "$repo" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchGCC.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMELEE_NX_FRONTEND="$frontend" \
    -DMELEE_DECOMP_ROOT="$repo/ref/melee" \
    -DMELEE_PORT_GENERATED_DIR="$game/generated/guest" \
    -DMELEE_PORT_INPUT_MANIFEST="$game/port-inputs.json" \
    "${extra[@]}"
}

do_build() { cmake --build "$build_dir" --parallel "$jobs"; }

case "$stage" in
  configure) do_configure ;;
  build)     do_build ;;
  all)       do_configure; do_build ;;
esac
echo "build-game.sh [$stage] done: $build_dir/port/melee-nx-netplay.nro (frontend: $frontend)"
