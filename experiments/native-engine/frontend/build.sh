#!/usr/bin/env bash
# Builds mu-native-gfx, the native-engine runner with a picture. Everything lands in build/native-engine-gfx/
# (gitignored); build/native-engine/ is only read.
#   ./build.sh sync     own copy of melee-unlocked (exported from the fetched clone, with ../patches and patches/0101), of
#                       the runner as committed (MU_SYNC_REF=worktree: as on disk) and of the game image; writes the
#                       patched decoder (patches/0102) and the embedded files
#   ./build.sh image    the Linux test image (mu-arm-gfx): Xvfb, Mesa's software Vulkan driver, X11 headers
#   ./build.sh deps     Dawn and SDL3 for aarch64 Linux (test build only), from ref/dawn and ref/SDL-dusklight
#   ./build.sh null     Linux, no graphics device: decoder and frame check   -> out/mu-native-gfx-null
#   ./build.sh linux    Linux with Aurora on Vulkan (the picture test)       -> out/mu-native-gfx-linux
#   ./build.sh nro      Horizon                                              -> out/mu-native-gfx.nro
#   ./build.sh          sync, null, nro
# Needs: ../fetch.sh and ../build.sh game (the tree and melee_game.so), build/sdk (the Switch graphics SDK the
# aurora frontend of the app uses) and the sources Aurora fetched in an earlier app build (build/switch-aurora/_deps).
# Every container has a time limit and no network.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
src="$repo/build/native-engine"; work="$repo/build/native-engine-gfx"; out="$work/out"; gen="$work/gen"
LINUX_IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"
GFX_IMG="${MU_GFX_IMAGE:-mu-arm-gfx}"
NX_IMG="${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"
jobs="${MELEE_BUILD_JOBS:-8}"
W=build/native-engine-gfx
dock() { local limit=$1 image=$2; shift 2; timeout "$limit" docker run --rm --network none -v "$repo:/w" -w /w "$image" bash -ec "$*"; }

sync_tree() {
  [ -d "$src/mu/.git" ] || { echo "run ../fetch.sh first" >&2; exit 1; }
  [ -f "$src/out/melee_game.so" ] || { echo "run ../build.sh game first" >&2; exit 1; }
  mkdir -p "$work" "$out" "$gen" "$work/mu" "$work/runner"
  local ref="${MU_SYNC_REF:-HEAD}" stage="$work/sync-stage"
  # melee-unlocked as fetched, from the clone's own objects (the pinned commit, whatever state its working
  # files are in), exported once. Only what the host build reads and the patches touch.
  if [ ! -f "$work/mu-pristine/port/linux/host_services.cpp" ]; then
    rm -rf "$work/mu-pristine"; mkdir -p "$work/mu-pristine"
    git -C "$src/mu" archive HEAD port native sourceport/game/CMakeLists.txt sourceport/game/shim/mu_console_boot.c sourceport/tools | tar -x -C "$work/mu-pristine"
  fi
  # The runner and its patches as committed (MU_SYNC_REF=worktree takes the files on disk instead, which
  # may be in the middle of someone's change).
  rm -rf "$stage"; mkdir -p "$stage/patches"
  if [ "$ref" = worktree ]; then
    mkdir -p "$stage/experiments/native-engine"; cp -R "$here/../runner" "$stage/experiments/native-engine/runner"
    cp "$here"/../patches/0*.patch "$stage/patches/"
  else
    git -C "$repo" archive "$ref" experiments/native-engine/runner experiments/native-engine/patches | tar -x -C "$stage"
    cp "$stage"/experiments/native-engine/patches/0*.patch "$stage/patches/"
  fi
  cp -R "$work/mu-pristine" "$stage/mu"
  for p in "$stage"/patches/0*.patch "$here/patches/0101-host-frontend-attach.patch"; do patch -s -p1 -d "$stage/mu" < "$p"; done
  # By content and without time stamps: only files that changed get a new time, so the next build sees exactly those.
  rsync -rlpc --delete "$stage/mu/" "$work/mu/"
  rsync -rlpc --delete "$stage/experiments/native-engine/runner/" "$work/runner/"
  rm -rf "$stage"
  cmp -s "$src/out/melee_game.so" "$out/melee_game.so" || cp "$src/out/melee_game.so" "$out/melee_game.so"
  echo "runner and patches: $ref ($([ "$ref" = worktree ] && echo files on disk || git -C "$repo" rev-parse --short "$ref")); game image sha256 $(shasum -a 256 "$out/melee_game.so" | cut -c1-16)" | tee "$work/sync-state.txt"
  # This repository's GX decoder, with the address handling the native memory layout needs.
  mkdir -p "$gen/gxsrc/port/runtime/gx"
  cp "$repo/port/runtime/gx/gx_core.cpp" "$gen/gxsrc/port/runtime/gx/gx_core.cpp"
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0102-gx-core-native-memory.patch"
  cp "$gen/gxsrc/port/runtime/gx/gx_core.cpp" "$gen/gx_core.cpp"
  # The image, the scripts and the reference digests, embedded as the headless runner embeds them.
  rm -rf "$gen/scripts"; mkdir -p "$gen/scripts"
  cp "$work"/mu/port/scripts/parity_*.txt "$work"/mu/port/scripts/native_vs_ko.txt "$gen/scripts/"
  python3 -I "$here/../scripts/make_vs4_busy.py" "$work/mu/port/scripts/parity_vs_onett.txt" "$gen/scripts/vs4_busy.txt"
  {
    echo '.section .rodata'
    echo '.balign 16'; echo '.global mu_embedded_game'; echo "mu_embedded_game: .incbin \"$W/out/melee_game.so\""
    echo '.global mu_embedded_game_end'; echo 'mu_embedded_game_end:'
    for f in "$gen"/scripts/*.txt; do n=$(basename "$f" .txt)
      echo ".balign 8"; echo "mu_script_$n: .incbin \"$W/gen/scripts/$n.txt\""; echo "mu_script_${n}_end:"
      echo ".global mu_script_$n"; echo ".global mu_script_${n}_end"
    done
    for f in "$here"/../reference/*.csv.gz; do n=$(basename "$f" .csv.gz)
      echo ".balign 8"; echo "mu_ref_$n: .incbin \"experiments/native-engine/reference/$n.csv.gz\""; echo "mu_ref_${n}_end:"
      echo ".global mu_ref_$n"; echo ".global mu_ref_${n}_end"
    done
    echo '.section .note.GNU-stack,"",%progbits'
  } > "$gen/embedded.S"
  {
    echo '// generated by frontend/build.sh'
    echo '#include <cstddef>'; echo '#include <cstdint>'
    echo 'struct EmbeddedFile { const char* name; const uint8_t* begin; const uint8_t* end; };'
    for f in "$gen"/scripts/*.txt; do n=$(basename "$f" .txt); echo "extern \"C\" const uint8_t mu_script_$n[], mu_script_${n}_end[];"; done
    echo 'extern const EmbeddedFile mu_embedded_scripts[] = {'
    for f in "$gen"/scripts/*.txt; do n=$(basename "$f" .txt); echo "  {\"$n\", mu_script_$n, mu_script_${n}_end},"; done
    echo '};'
    echo 'extern const size_t mu_embedded_script_count = sizeof mu_embedded_scripts / sizeof mu_embedded_scripts[0];'
    for f in "$here"/../reference/*.csv.gz; do n=$(basename "$f" .csv.gz); echo "extern \"C\" const uint8_t mu_ref_$n[], mu_ref_${n}_end[];"; done
    echo 'extern const EmbeddedFile mu_embedded_references[] = {'
    for f in "$here"/../reference/*.csv.gz; do n=$(basename "$f" .csv.gz); echo "  {\"$n\", mu_ref_$n, mu_ref_${n}_end},"; done
    echo '};'
    echo 'extern const size_t mu_embedded_reference_count = sizeof mu_embedded_references / sizeof mu_embedded_references[0];'
  } > "$gen/embedded_tables.cpp"
  echo "synced $work"
}

# The sources Aurora downloads when it is configured (fmt, Dear ImGui, SQLite, Tracy, xxHash), taken from an
# earlier build of the app so that configuring here needs no network.
fetched_deps() {
  local from="$repo/build/switch-aurora/_deps" name
  [ -d "$from/imgui-src" ] || { echo "build the app's aurora frontend once (it fetches Aurora's dependencies): $from is missing" >&2; exit 1; }
  mkdir -p "$work/deps"
  for name in fmt imgui sqlite3 tracy xxhash; do rsync -a "$from/$name-src/" "$work/deps/$name-src/"; done
}
DEPS_ARGS="-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_FMT=/w/$W/deps/fmt-src -DFETCHCONTENT_SOURCE_DIR_IMGUI=/w/$W/deps/imgui-src
  -DFETCHCONTENT_SOURCE_DIR_SQLITE3=/w/$W/deps/sqlite3-src -DFETCHCONTENT_SOURCE_DIR_TRACY=/w/$W/deps/tracy-src -DFETCHCONTENT_SOURCE_DIR_XXHASH=/w/$W/deps/xxhash-src"
DEPS_ARGS=$(echo $DEPS_ARGS)

build_image() { timeout 900 docker build -t "$GFX_IMG" -f "$here/linux-gfx.Dockerfile" "$here"; }

build_deps() {
  dock 3000 "$GFX_IMG" "
    cmake -S ref/dawn -B $W/dawn-linux -G Ninja -C experiments/native-engine/frontend/DawnLinux.cmake -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ >/dev/null
    cmake --build $W/dawn-linux --target webgpu_dawn --parallel $jobs | tail -1
    cmake -S ref/SDL-dusklight -B $W/sdl-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER_GPU=OFF \
      -DSDL_X11=ON -DSDL_X11_SHARED=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_ALSA=OFF -DSDL_JACK=OFF \
      -DSDL_SNDIO=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF >/dev/null
    cmake --build $W/sdl-linux --target SDL3-static --parallel $jobs | tail -1"
}

build_null() {
  dock 900 "$LINUX_IMG" "
    cmake -S experiments/native-engine/frontend -B $W/null -G Ninja -DCMAKE_BUILD_TYPE=Release -DMU_FE_VARIANT=null \
      -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ >/dev/null
    cmake --build $W/null --parallel $jobs > $W/null-build.log 2>&1 || { grep -E 'error|FAILED|undefined' -A4 $W/null-build.log | head -60; exit 1; }
    cp $W/null/mu-native-gfx-null $W/out/"
  echo "built $out/mu-native-gfx-null"
}

build_linux() {
  [ -f "$work/dawn-linux/src/dawn/native/libwebgpu_dawn.a" ] && [ -f "$work/sdl-linux/libSDL3.a" ] || { echo "run ./build.sh deps first" >&2; exit 1; }
  fetched_deps
  dock 2400 "$GFX_IMG" "
    cmake -S experiments/native-engine/frontend -B $W/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DMU_FE_VARIANT=linux \
      -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ $DEPS_ARGS >$W/linux-configure.log 2>&1 || { tail -30 $W/linux-configure.log; exit 1; }
    cmake --build $W/linux --target mu_native_gfx --parallel $jobs > $W/linux-build.log 2>&1 || { grep -E 'error|FAILED|undefined' -A4 $W/linux-build.log | head -60; exit 1; }
    cp $W/linux/mu-native-gfx-linux $W/out/"
  echo "built $out/mu-native-gfx-linux"
}

build_nro() {
  [ -f "$repo/build/sdk/SDK_MANIFEST.json" ] || { echo "build/sdk is missing: unpack the melee-nx-netplay-sdk archive there (BUILDING.md)" >&2; exit 1; }
  fetched_deps
  dock 2400 "$NX_IMG" "
    cmake -S experiments/native-engine/frontend -B $W/nx -G Ninja -DCMAKE_BUILD_TYPE=Release -DMU_FE_VARIANT=switch \
      -DCMAKE_TOOLCHAIN_FILE=/w/switch/cmake/SwitchGCC.cmake -DMELEE_NX_SDK_DIR=/w/build/sdk $DEPS_ARGS >$W/nx-configure.log 2>&1 || { tail -30 $W/nx-configure.log; exit 1; }
    cmake --build $W/nx --parallel $jobs > $W/nx-build.log 2>&1 || { grep -E 'error|FAILED|undefined' -A4 $W/nx-build.log | head -60; exit 1; }
    cp $W/nx/mu-native-gfx.nro $W/out/
    ls -l $W/out/mu-native-gfx.nro"
  echo "built $out/mu-native-gfx.nro"
}

case "${1:-all}" in
  sync) sync_tree ;;
  image) build_image ;;
  deps) build_deps ;;
  null) build_null ;;
  linux) build_linux ;;
  nro) build_nro ;;
  all) sync_tree; build_null; build_nro ;;
  *) echo "usage: build.sh [sync|image|deps|null|linux|nro]" >&2; exit 2 ;;
esac
