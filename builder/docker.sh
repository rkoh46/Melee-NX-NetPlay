#!/usr/bin/env bash
# Container entry point. See BUILDING.md and switch/docker/README.md.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Physical path: the checkout may be reached through a symlink, and Docker mounts need the real one.
repo="$(cd "$here/.." && pwd -P)"

image="${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"
sdk_image="${MELEE_DOCKER_SDK_IMAGE:-melee-nx-netplay-sdk:latest}"
jobs="${MELEE_BUILD_JOBS:-4}"
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
frontend="${MELEE_NX_FRONTEND:-headless}"
# Explicit tags also work with Docker daemons that reject tagless image inspect.
[[ "${image##*/}" == *:* || "$image" == *@* ]] || image+=:latest
[[ "${sdk_image##*/}" == *:* || "$sdk_image" == *@* ]] || sdk_image+=:latest

usage() {
  cat <<'HELP'
Usage: bash builder/docker.sh COMMAND [STAGE]

  image                         Build the reusable GCC/Clang toolchain image
  sdk-image                     Package the toolchain and an existing Mesa build
  graphics [prepare|dawn|sdl|aurora|all]
  game [configure|build|all]      Configure and build the NRO (MELEE_NX_FRONTEND=headless|aurora)
  shell                         Open a container shell

Environment:
  MELEE_DOCKER_IMAGE             Toolchain or SDK image (default: melee-nx-netplay-build)
  MELEE_DOCKER_SDK_IMAGE         SDK output tag (default: melee-nx-netplay-sdk)
  MELEE_NX_FRONTEND              headless (default; no Mesa needed) or aurora
  MESA_NVK_ROOT                 Host Mesa build; optional with an SDK image
  MELEE_BUILD_JOBS              Parallel compile jobs (default: 4)
  MELEE_SDL_VARIANT              dusklight (default) or legacy

See BUILDING.md for setup and switch/docker/README.md for SDK reuse.
HELP
}

stage="${1:-help}"
case "$stage" in
  -h|--help|help) usage; exit 0 ;;
  image|sdk-image|graphics|game|shell) ;;
  *) echo "Unknown command: $stage" >&2; usage >&2; exit 64 ;;
esac

command -v docker >/dev/null 2>&1 || {
  echo "docker not found on PATH. Install Docker and start its daemon." >&2
  exit 69
}

build_image() {
  docker build -t "$image" "$repo/switch/docker"
  echo "Built image: $image"
}

ensure_image() {
  if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "Image '$image' not found; building it first."
    build_image
  fi
}

resolve_mesa() {
  local root="${MESA_NVK_ROOT:-}"
  if [[ -z "$root" ]]; then
    echo "Set MESA_NVK_ROOT to a built Switch Mesa tree. See BUILDING.md." >&2
    return 66
  fi
  [[ -d "$root" ]] || { echo "MESA_NVK_ROOT does not exist: $root" >&2; return 66; }
  if [[ ! -f "$root/src/nouveau/vulkan/rust_switch_stubs.c" ]]; then
    echo "Missing Switch/NVK compatibility source under MESA_NVK_ROOT: $root" >&2
    return 66
  fi
  if ! compgen -G "$root/builddir-switch/*" >/dev/null 2>&1; then
    echo "No builddir-switch/ under $root. See BUILDING.md." >&2
    return 66
  fi
  printf '%s' "$root"
}

build_sdk_image() {
  local mesa toolchain_id
  mesa="$(resolve_mesa)"
  ensure_image
  toolchain_id="$(docker image inspect "$image" --format '{{.Id}}')"
  if [[ "$sdk_image" == "$image" ]]; then
    echo "MELEE_DOCKER_SDK_IMAGE must differ from MELEE_DOCKER_IMAGE." >&2
    return 64
  fi
  # Stream only the linked archives, ABI bridge, and manifests as build context.
  MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$repo:/project:ro" -v "$mesa:/mesa-source:ro" \
    "$image" python3 /project/builder/package-mesa.py /mesa-source \
    --toolchain-id "$toolchain_id" |
    docker build -t "$sdk_image" --build-arg "TOOLCHAIN_IMAGE=$image" -
  echo "Built SDK: $sdk_image"
}

run_in_container() {
  local needs_mesa="$1"
  shift
  local mounts=() tty=()
  set +u
  if [[ -n "${MESA_NVK_ROOT:-}" ]]; then
    local mesa
    mesa="$(resolve_mesa)"
    mounts=(-v "$mesa:/mesa-nvk:ro")
  fi
  # Reference trees may be symlinks to checkouts elsewhere on the host (a shared
  # Dashdance or Melee-NX clone); mount each target at its own path, read-only.
  local link target
  for link in "$repo"/ref/*; do
    [[ -L "$link" ]] || continue
    target="$(cd "$link" 2>/dev/null && pwd -P)" || continue
    mounts+=(-v "$target:$target:ro")
  done
  ensure_image
  [[ -t 1 ]] && tty=(-t)
  # Git Bash must leave the container-side paths in -v and -w intact.
  MSYS_NO_PATHCONV=1 docker run --rm -i "${tty[@]}" \
    -v "$repo:/project" "${mounts[@]}" -w /project \
    -e MELEE_BUILD_JOBS="$jobs" \
    -e MELEE_SDL_VARIANT="$sdl_variant" \
    -e MELEE_NX_FRONTEND="$frontend" \
    -e MELEE_NX_BUILD_SUFFIX="${MELEE_NX_BUILD_SUFFIX:-}" \
    -e MELEE_NX_PLAYBACK="${MELEE_NX_PLAYBACK:-0}" \
    -e MESA_NVK_ROOT=/mesa-nvk \
    "$image" bash -c '
      if [[ "$1" == required && ! -f /mesa-nvk/src/nouveau/vulkan/rust_switch_stubs.c ]]; then
        echo "Mesa/NVK is missing. Set MESA_NVK_ROOT or select an SDK image; see BUILDING.md." >&2
        exit 66
      fi
      shift
      exec "$@"
    ' -- "$needs_mesa" "$@"
}

case "$stage" in
  image) build_image ;;
  sdk-image) build_sdk_image ;;
  graphics) run_in_container optional bash builder/build-graphics.sh "${2:-all}" ;;
  game)
    needs=optional   # build-game.sh compiles the aurora frontend without Mesa and says so; the link needs it
    run_in_container "$needs" bash builder/build-game.sh "${2:-all}" ;;
  shell) run_in_container optional bash ;;
esac
