#!/usr/bin/env bash
# Package the prebuilt, game-free Switch SDK that install.sh downloads: the
# Mesa/NVK archives, the Dawn and SDL3 Switch builds with their headers, and the
# Switch-patched Aurora source. Nothing from Melee is in it. Run after
# `docker.sh graphics all` with MESA_NVK_ROOT pointing at a built mesa-switch.
# Usage: bash builder/package-sdk.sh [version]   -> build/melee-nx-netplay-sdk-<version>.tar.gz
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd -P)"
version="${1:-$(date +%Y%m%d)}"
mesa="${MESA_NVK_ROOT:?set MESA_NVK_ROOT to the built mesa-switch tree}"
image="${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
case "$sdl_variant" in
  dusklight) sdl_src="$repo/ref/SDL-dusklight"; sdl_build="$repo/build/sdl-switch-dusklight" ;;
  legacy)    sdl_src="$repo/ref/SDL";           sdl_build="$repo/build/sdl-switch" ;;
esac
dawn_src="$repo/ref/dawn"; dawn_build="$repo/build/dawn-switch"
aurora="$repo/ref/melee-pc/extern/aurora"
need() { [[ -e "$1" ]] || { echo "missing: $1 ($2)" >&2; exit 66; }; }
need "$mesa/src/nouveau/vulkan/rust_switch_stubs.c" "mesa-switch checkout with the Switch overlay"
need "$mesa/builddir-switch/src/nouveau/vulkan/libnvk.a" "run mesa-switch's build-switch.sh"
need "$dawn_build/src/dawn/native/libwebgpu_dawn.a" "run docker.sh graphics all"
need "$sdl_build/libSDL3.a" "run docker.sh graphics all"
need "$aurora/CMakeLists.txt" "run fetch-deps.sh"

stage="$repo/build/sdk-stage"
rm -rf "$stage"; mkdir -p "$stage"
echo "==> Mesa/NVK archives"
mkdir -p "$stage/mesa/src/nouveau/vulkan"
cp "$mesa/src/nouveau/vulkan/rust_switch_stubs.c" "$stage/mesa/src/nouveau/vulkan/"
(cd "$mesa" && find builddir-switch -name '*.a' ! -name 'libsanity_check_for_rust.a' -print0 |
  while IFS= read -r -d '' a; do mkdir -p "$stage/mesa/$(dirname "$a")"; cp "$a" "$stage/mesa/$a"; done)
# Thin archives point at object files by path; repack them as regular archives
# inside the toolchain container so the SDK is self-contained.
MSYS_NO_PATHCONV=1 docker run --rm -v "$stage:/stage" -v "$mesa:/mesa:ro" "$image" bash -c '
  set -e
  cd /stage/mesa
  for a in $(find builddir-switch -name "*.a"); do
    if [ "$(head -c 8 "$a")" = "!<thin>" ]; then
      AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar
      members=$($AR t "/mesa/$a")   # member paths are relative to the archive directory
      rm "$a"
      (cd "/mesa/$(dirname "$a")" && $AR rcs "/stage/mesa/$a" $members)
      echo "repacked thin archive: $a"
    fi
  done'
echo "==> Dawn"
mkdir -p "$stage/dawn/src/third_party" "$stage/dawn/build/src/dawn/native"
cp -R "$dawn_src/include" "$stage/dawn/src/include"
rsync -a --include='*/' --include='*.h' --include='*.inc' --exclude='*' "$dawn_src/third_party/abseil-cpp/" "$stage/dawn/src/third_party/abseil-cpp/"
cp -R "$dawn_build/gen" "$stage/dawn/build/gen"
cp "$dawn_build/src/dawn/native/libwebgpu_dawn.a" "$stage/dawn/build/src/dawn/native/"
echo "==> SDL3 ($sdl_variant)"
mkdir -p "$stage/sdl/src" "$stage/sdl/build"
cp -R "$sdl_src/include" "$stage/sdl/src/include"
cp "$sdl_build/libSDL3.a" "$stage/sdl/build/"
echo "==> Aurora (Switch-patched source)"
rsync -a --exclude='.git' "$aurora/" "$stage/aurora/"
cat > "$stage/SDK_MANIFEST.json" <<EOF
{
  "name": "melee-nx-netplay-sdk",
  "version": "$version",
  "created_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "contents": "Mesa/NVK Switch archives (danfromtico/mesa-switch), Dawn and SDL3 Switch builds with headers, Switch-patched Aurora source (melee-pc fork + Melee-NX patches). No game content.",
  "mesa_commit": "$(git -C "$mesa" rev-parse HEAD 2>/dev/null || echo unknown)",
  "dawn_commit": "$(git -C "$dawn_src" rev-parse HEAD 2>/dev/null || echo unknown)",
  "sdl_commit": "$(git -C "$sdl_src" rev-parse HEAD 2>/dev/null || echo unknown)",
  "repo_commit": "$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)",
  "sdl_variant": "$sdl_variant"
}
EOF
out="$repo/build/melee-nx-netplay-sdk-$version.tar.gz"
echo "==> $out"
tar -C "$stage" -czf "$out" .
(cd "$repo/build" && shasum -a 256 "$(basename "$out")" > "$out.sha256")
du -h "$out" | cut -f1
