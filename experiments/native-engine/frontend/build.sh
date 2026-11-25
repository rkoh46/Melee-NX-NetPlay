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
#   MU_SANITIZE=address|thread ./build.sh null|linux   test builds with AddressSanitizer or ThreadSanitizer: this program's
#                       own code and the services netplay object instrumented (not Aurora, Dawn, SDL or the game image),
#                       in their own build folders; out/mu-native-gfx-<variant>[-services]-asan or -tsan
# Each of null, linux and nro also makes the services program (out/mu-native-gfx-*-services, out/mu-native-gfx-services.nro):
# the same objects linked with the netplay object that has the app's matchmaking client, user file and reporting.
# The Linux one is pinned to the loopback stand-in (../tools/mock_matchmaking.py) and cannot reach a real service.
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
# MU_TIMEOUT_SCALE multiplies the container time limits (app-build.sh sets it where the arm64 image runs under qemu).
dock() { local limit=$(( $1 * ${MU_TIMEOUT_SCALE:-1} )) image=$2; shift 2; timeout "$limit" docker run --rm --name "${MU_DOCKER_NAME:-mnx-build}-$$-$RANDOM" --network none -v "$repo:/w" -w /w "$image" bash -ec "$*"; }
SAN="${MU_SANITIZE:-}"; SAN_SFX=""; SAN_FLAGS=""
case "$SAN" in
  "") ;;
  address) SAN_SFX=-asan ;;
  thread) SAN_SFX=-tsan ;;
  *) echo "MU_SANITIZE must be address or thread" >&2; exit 2 ;;
esac
[ -z "$SAN" ] || SAN_FLAGS="-fsanitize=$SAN -fno-omit-frame-pointer"

# The NRO's icon from melee-unlocked's melee_unlocked.ico (fetched tree): its largest picture (a PNG inside the .ico) taken out
# here, flattened on the launcher's rail navy and written as the 256x256 JPEG the NRO carries (gen/mu_ui/icon.jpg; the
# services program, which is the app, uses it). Never committed.
mu_icon() {
  local ico="$src/mu/port/app/melee_unlocked.ico" png="$gen/mu_ui/mu_icon.png" jpg="$gen/mu_ui/icon.jpg"
  [ -f "$ico" ] || { echo "missing port/app/melee_unlocked.ico in the fetched tree" >&2; exit 1; }
  [ -f "$jpg" ] && [ "$jpg" -nt "$ico" ] && return 0
  python3 -I - "$ico" "$png" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
_, kind, count = struct.unpack_from("<HHH", data, 0)
best = None
for i in range(count):
    w, h, _, _, _, _, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
    w, h = w or 256, h or 256
    if data[offset:offset + 8] == b"\x89PNG\r\n\x1a\n" and (best is None or w * h > best[0]):
        best = (w * h, offset, size)
if best is None:
    sys.exit("mu_icon: no PNG picture in the .ico")
open(sys.argv[2], "wb").write(data[best[1]:best[1] + best[2]])
PY
  dock 120 "$GFX_IMG" "convert $W/gen/mu_ui/mu_icon.png -background '#0B111E' -alpha remove -alpha off -resize 256x256 -gravity center -extent 256x256 -quality 92 $W/gen/mu_ui/icon.jpg"
  echo "icon: melee-unlocked's melee_unlocked.ico -> $jpg"
}

sync_tree() {
  [ -d "$src/mu/.git" ] || { echo "run ../fetch.sh first" >&2; exit 1; }
  [ -f "$src/out/melee_game.so" ] || { echo "run ../build.sh game first" >&2; exit 1; }
  mkdir -p "$work" "$out" "$gen" "$work/mu" "$work/runner"
  # The runner this frontend was last brought up against. Play mode needs the runner's automatic input delay
  # (--net-delay auto, netplay_bridge::Setup::delay_auto), which is newer than 1c556fc and not committed yet: until
  # it is, the files on disk are used. Pin this to the commit that contains them (MU_SYNC_REF=<commit> meanwhile).
  local ref="${MU_SYNC_REF:-eb9eabc}" stage="$work/sync-stage"
  # melee-unlocked as fetched, from the clone's own objects (the pinned commit, whatever state its working
  # files are in), exported once. Only what the host build reads and the patches touch.
  if [ ! -f "$work/mu-pristine/port/linux/host_services.cpp" ]; then
    rm -rf "$work/mu-pristine"; mkdir -p "$work/mu-pristine"
    git -C "$src/mu" archive HEAD port native | tar -x -C "$work/mu-pristine"
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
  # The other files those patches touch (game sources: patched here only so that the patches apply whole).
  sed -n 's|^+++ b/||p' "$stage"/patches/0*.patch | grep -v -e '^port/' -e '^native/' | sort -u > "$stage/extra-paths" || true
  [ -s "$stage/extra-paths" ] && git -C "$src/mu" archive HEAD $(cat "$stage/extra-paths") | tar -x -C "$stage/mu"
  for p in "$stage"/patches/0*.patch; do patch -s -p1 -d "$stage/mu" < "$p"; done
  cp "$stage/mu/port/linux/host_services.cpp" "$gen/host_services.base.cpp"     # what patches/0101 is a diff against
  patch -s -p1 -d "$stage/mu" < "$here/patches/0101-host-frontend-hooks.patch"
  patch -s -p1 -d "$stage/mu" < "$here/patches/0119-host-jukebox-hook.patch"   # Use custom music: the host jukebox (features/audio.cpp)
  patch -s -p1 -d "$stage/mu" < "$here/patches/0121-host-game-symbol.patch"   # the game's Gecko writer and reader (features/gecko.cpp)
  # melee-unlocked's player Gecko codes (user_gecko.cpp), LF, with patches/0122: the shim host header and the include paths of a copy.
  rm -rf "$gen/geckosrc"; mkdir -p "$gen/geckosrc/port/runtime/host" "$gen/gecko"
  perl -pe 's/\r$//; s/^\xEF\xBB\xBF// if $. == 1' "$stage/mu/port/runtime/host/user_gecko.cpp" > "$gen/geckosrc/port/runtime/host/user_gecko.cpp"
  patch -s -p1 -d "$gen/geckosrc" < "$here/patches/0122-user-gecko-on-the-switch.patch"
  cmp -s "$gen/geckosrc/port/runtime/host/user_gecko.cpp" "$gen/gecko/user_gecko.cpp" || cp "$gen/geckosrc/port/runtime/host/user_gecko.cpp" "$gen/gecko/user_gecko.cpp"
  rm -rf "$gen/geckosrc"
  # melee-unlocked's jukebox (Use custom music, features/audio.cpp), LF line ends, with patches/0118: the app's MusicPacks folder.
  rm -rf "$gen/jukeboxsrc"; mkdir -p "$gen/jukeboxsrc/port/runtime/hle" "$gen/jukebox"
  perl -pe 's/\r$//; s/^\xEF\xBB\xBF// if $. == 1' "$stage/mu/port/runtime/hle/jukebox.cpp" > "$gen/jukeboxsrc/port/runtime/hle/jukebox.cpp"
  patch -s -p1 -d "$gen/jukeboxsrc" < "$here/patches/0118-jukebox-on-the-switch.patch"
  cmp -s "$gen/jukeboxsrc/port/runtime/hle/jukebox.cpp" "$gen/jukebox/jukebox.cpp" || cp "$gen/jukeboxsrc/port/runtime/hle/jukebox.cpp" "$gen/jukebox/jukebox.cpp"
  rm -rf "$gen/jukeboxsrc"
  # By content and without time stamps: only files that changed get a new time, so the next build sees exactly those.
  rsync -rlpc --delete "$stage/mu/" "$work/mu/"
  rsync -rlpc --delete "$stage/experiments/native-engine/runner/" "$work/runner/"
  rm -rf "$stage"
  cmp -s "$src/out/melee_game.so" "$out/melee_game.so" || cp "$src/out/melee_game.so" "$out/melee_game.so"
  # The netplay host's pieces, made by ../build.sh: the image's snapshot exclusions and the netplay object for
  # each target (the bridge, this repository's netplay sources and ENet in one object; see ../README.md).
  for f in out/melee_game.snapexcl obj-linux/netplay.o obj-nx/netplay.o; do
    [ -f "$src/$f" ] || { echo "missing $src/$f: run ../build.sh first" >&2; exit 1; }
    mkdir -p "$work/net/$(dirname "$f")"; cmp -s "$src/$f" "$work/net/$f" || cp "$src/$f" "$work/net/$f"
  done
  # The services variant for the console (MU_NET_SERVICES=1 ../build.sh nro), when it has been made. The Linux
  # one is made here (build_net_services_linux).
  if [ -f "$src/obj-nx-services/netplay.o" ]; then
    mkdir -p "$work/net/obj-nx-services"; cmp -s "$src/obj-nx-services/netplay.o" "$work/net/obj-nx-services/netplay.o" || cp "$src/obj-nx-services/netplay.o" "$work/net/obj-nx-services/netplay.o"
  fi
  echo "runner and patches: $ref ($([ "$ref" = worktree ] && echo files on disk || git -C "$repo" rev-parse --short "$ref")); game image sha256 $(shasum -a 256 "$out/melee_game.so" | cut -c1-16)" | tee "$work/sync-state.txt"
  # This repository's GX decoder, with the address handling the native memory layout needs.
  mkdir -p "$gen/gxsrc/port/runtime/gx"
  cp "$repo/port/runtime/gx/gx_core.cpp" "$gen/gxsrc/port/runtime/gx/gx_core.cpp"
  cp "$repo/port/runtime/gx/gx_aurora_bridge.cpp" "$gen/gxsrc/port/runtime/gx/gx_aurora_bridge.cpp"
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0102-gx-core-native-memory.patch"
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0103-gx-bridge-lines-points.patch"
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0107-gx-core-array-pool-fence.patch"
  # Sub-frame presentation (patches/0106: the features' presenter) in the renderer and the sub-frame solver.
  cp "$repo/port/runtime/gx/gx_aurora.cpp" "$repo/port/runtime/gx/subframe.cpp" "$gen/gxsrc/port/runtime/gx/"
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0106-subframe-presentation.patch"
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0116-gx-looks.patch"   # the looks: owner tokens, look rules, treated textures (src/looks_live.h)
  patch -s -p1 -d "$gen/gxsrc" < "$here/patches/0120-gx-lcancel-flash-tint.patch"   # the L-cancel flash on the fighter's model (features/overlays.cpp)
  for f in gx_aurora.cpp subframe.cpp; do cmp -s "$gen/gxsrc/port/runtime/gx/$f" "$gen/$f" || cp "$gen/gxsrc/port/runtime/gx/$f" "$gen/$f"; done
  for f in gx_core.cpp gx_aurora_bridge.cpp; do cmp -s "$gen/gxsrc/port/runtime/gx/$f" "$gen/$f" || cp "$gen/gxsrc/port/runtime/gx/$f" "$gen/$f"; done
  # The app's controller input and sound output, verbatim. Copies, because a file compiled where it lives finds
  # the app's own host.h beside it before the shim's.
  mkdir -p "$gen/app"
  for f in input_sdl.cpp input_script.cpp gc_adapter_usbhs.cpp gc_adapter_none.cpp overlay_imgui.cpp; do
    cmp -s "$repo/port/runtime/host/$f" "$gen/app/$f" || cp "$repo/port/runtime/host/$f" "$gen/app/$f"
  done
  # The sound output follows the game's speed (online time sync; patches/0110).
  rm -rf "$gen/audiosrc"; mkdir -p "$gen/audiosrc/port/runtime/host"; cp "$repo/port/runtime/host/audio_sdl.cpp" "$gen/audiosrc/port/runtime/host/"
  patch -s -p1 -d "$gen/audiosrc" < "$here/patches/0110-audio-follows-game-speed.patch"
  patch -s -p1 -d "$gen/audiosrc" < "$here/patches/0117-audio-buffer-setting.patch"   # the player's buffer: Auto or Low latency (features/audio.cpp)
  cmp -s "$gen/audiosrc/port/runtime/host/audio_sdl.cpp" "$gen/app/audio_sdl.cpp" || cp "$gen/audiosrc/port/runtime/host/audio_sdl.cpp" "$gen/app/audio_sdl.cpp"
  rm -rf "$gen/audiosrc"
  # The app's in-game menu (patches/0104: pages a host adds, greyed rows, the Quit action, a HUD extra) and its sign-in
  # and notice screens (patches/0105: built off the console too, controllers through the host's input path).
  rm -rf "$gen/appsrc"; mkdir -p "$gen/appsrc/port/runtime/host"
  for f in game_menu.cpp game_menu.h switch_signin.cpp; do cp "$repo/port/runtime/host/$f" "$gen/appsrc/port/runtime/host/$f"; done
  patch -s -p1 -d "$gen/appsrc" < "$here/patches/0104-game-menu-host-pages.patch"
  patch -s -p1 -d "$gen/appsrc" < "$here/patches/0105-switch-signin-off-console.patch"
  patch -s -p1 -d "$gen/appsrc" < "$here/patches/0111-game-menu-open-page.patch"   # menu_open_extra (the Start + D-pad Up + Z chord)
  patch -s -p1 -d "$gen/appsrc" < "$here/patches/0114-game-menu-driving-pad.patch"   # the pad that drives the menu (settings_nav)
  for f in game_menu.cpp game_menu.h switch_signin.cpp; do cmp -s "$gen/appsrc/port/runtime/host/$f" "$gen/app/$f" || cp "$gen/appsrc/port/runtime/host/$f" "$gen/app/$f"; done
  rm -rf "$gen/appsrc"
  # Aurora (build/sdk/aurora, the Switch-patched source both picture variants compile) with this frontend's Aurora
  # patches (patches/01xx-aurora-*.patch), as a build-time copy; build/sdk itself is not changed.
  if [ -f "$repo/build/sdk/aurora/CMakeLists.txt" ]; then
    rm -rf "$gen/aurora-stage"; cp -R "$repo/build/sdk/aurora" "$gen/aurora-stage"
    for p in "$here"/patches/01*-aurora-*.patch; do [ -f "$p" ] && patch -s -p1 -d "$gen/aurora-stage" < "$p"; done
    rsync -rlpc --delete "$gen/aurora-stage/" "$gen/aurora/"; rm -rf "$gen/aurora-stage"
  fi
  # melee-unlocked's Training overlay names the fighters' common actions (port/runtime/gx/training_overlay.cpp): the table
  # is taken from the fetched tree at build time for the Training panel (src/overlay_draw.cpp).
  sed -n '/^const char\* const kActionNames\[\] = {/,/^};/p' "$work/mu/port/runtime/gx/training_overlay.cpp" > "$gen/action_names.inc.new" 2>/dev/null || true
  [ -s "$gen/action_names.inc.new" ] || echo 'const char* const kActionNames[] = {""};' > "$gen/action_names.inc.new"
  if cmp -s "$gen/action_names.inc.new" "$gen/action_names.inc"; then rm -f "$gen/action_names.inc.new"; else mv "$gen/action_names.inc.new" "$gen/action_names.inc"; fi
  # melee-unlocked's interface (src/mu_panel.cpp, src/mu_home.cpp): its panel and launcher code taken verbatim from the fetched
  # tree, its Source Sans 3 fonts (SIL OFL 1.1, THIRD_PARTY_NOTICES.md) and its launcher artwork, embedded below. Build-time
  # copies in gen/mu_ui only: nothing of it is committed.
  python3 -I "$here/scripts/mu_ui_extract.py" "$src/mu" "$gen/mu_ui" >/dev/null
  for f in port/runtime/gx/ui_sources/gd_melee/assets/kit/SourceSans3-Black.otf port/runtime/gx/ui_sources/gd_melee/assets/kit/SourceSans3-Bold.otf \
           port/app/bailey_cut.bmp port/app/melee_mark.bmp; do
    [ -f "$src/mu/$f" ] || { echo "missing $f in the fetched tree" >&2; exit 1; }
    cmp -s "$src/mu/$f" "$gen/mu_ui/$(basename "$f")" || cp "$src/mu/$f" "$gen/mu_ui/$(basename "$f")"
  done
  # The NRO's icon: melee-unlocked's own (port/app/launcher.ico, its largest picture), as a 256x256 JPEG (build_nro).
  mu_icon
  # Skins (src/features/cosmetics.cpp): melee-unlocked's skeleton and drawing checks and its costume families, taken from
  # its Windows-only cosmetic_mods.cpp (only these portable parts), and its pack_skin_rule; compiled from the fetched tree.
  cm="$work/mu/port/runtime/host/cosmetic_mods.cpp"
  awk '/^\/\/ The player reads this beside "Online: standard costume"/{on=0} /^namespace skeleton \{/{on=1} on' "$cm" > "$gen/cosmetics_skeleton.inc.new"
  awk '/^constexpr FighterFamily families\[\] = \{/,/^\};/' "$cm" > "$gen/cosmetics_families.inc.new"
  grep -q "costume_skeleton_matches" "$gen/cosmetics_skeleton.inc.new" && grep -q '"Fx", "Fox"' "$gen/cosmetics_families.inc.new" || { echo "sync: cosmetic_mods.cpp no longer has the parts cosmetics.cpp takes" >&2; exit 1; }
  for f in cosmetics_skeleton.inc cosmetics_families.inc; do if cmp -s "$gen/$f.new" "$gen/$f"; then rm -f "$gen/$f.new"; else mv "$gen/$f.new" "$gen/$f"; fi; done
  for f in pack_skin_rule.h pack_skin_rule.cpp; do cmp -s "$work/mu/port/runtime/host/$f" "$gen/$f" || cp "$work/mu/port/runtime/host/$f" "$gen/$f"; done
  # The image, the scripts and the reference digests, embedded as the headless runner embeds them.
  # Fight a pro and the Practice menu: melee-unlocked's phillip_ai.cpp and practice_menu.cpp, build-time copies with
  # patches/0112 (the app's own inference engine, the app's ProAI folder, downloads only when chosen) and 0113 (the
  # download question), compiled on src/proai/include's headers (CMakeLists.txt).
  rm -rf "$gen/proaisrc"; mkdir -p "$gen/proaisrc/port/runtime/hle" "$gen/proaisrc/port/runtime/gx" "$gen/proai"
  cp "$work/mu/port/runtime/hle/phillip_ai.cpp" "$work/mu/port/runtime/hle/phillip_ai.h" "$gen/proaisrc/port/runtime/hle/"
  cp "$work/mu/port/runtime/gx/practice_menu.cpp" "$work/mu/port/runtime/gx/practice_menu.h" "$gen/proaisrc/port/runtime/gx/"
  patch -s -p1 -d "$gen/proaisrc" < "$here/patches/0112-phillip-ai-own-engine.patch"
  patch -s -p1 -d "$gen/proaisrc" < "$here/patches/0113-practice-menu-download-question.patch"
  for f in hle/phillip_ai.cpp hle/phillip_ai.h gx/practice_menu.cpp gx/practice_menu.h; do
    cmp -s "$gen/proaisrc/port/runtime/$f" "$gen/proai/$(basename "$f")" || cp "$gen/proaisrc/port/runtime/$f" "$gen/proai/$(basename "$f")"
  done
  rm -rf "$gen/proaisrc"
  mkdir -p "$gen/abi"; cmp -s "$work/mu/port/runtime/abi/mu_ghost_ai.h" "$gen/abi/mu_ghost_ai.h" || cp "$work/mu/port/runtime/abi/mu_ghost_ai.h" "$gen/abi/"   # phillip_ai.h's ../abi
  # The special moves' names the Practice menu's attack rows show (upstream hle/practice_cpu.cpp special_name's table).
  sed -n '/static const char\* const names\[27\]\[4\] = {/,/^  };/p' "$work/mu/port/runtime/hle/practice_cpu.cpp" > "$gen/proai/special_names.inc.new" 2>/dev/null || true
  [ -s "$gen/proai/special_names.inc.new" ] || echo 'static const char* const names[27][4] = {};' > "$gen/proai/special_names.inc.new"
  if cmp -s "$gen/proai/special_names.inc.new" "$gen/proai/special_names.inc"; then rm -f "$gen/proai/special_names.inc.new"; else mv "$gen/proai/special_names.inc.new" "$gen/proai/special_names.inc"; fi
  rm -rf "$gen/scripts"; mkdir -p "$gen/scripts"
  cp "$work"/mu/port/scripts/parity_*.txt "$work"/mu/port/scripts/native_vs_ko.txt "$gen/scripts/"
  python3 -I "$here/../scripts/make_vs4_busy.py" "$work/mu/port/scripts/parity_vs_onett.txt" "$gen/scripts/vs4_busy.txt"
  {
    echo '.section .rodata'
    echo '.balign 16'; echo '.global mu_embedded_game'; echo "mu_embedded_game: .incbin \"$W/out/melee_game.so\""
    echo '.global mu_embedded_game_end'; echo 'mu_embedded_game_end:'
    echo '.balign 8'; echo '.global mu_embedded_snapexcl'; echo "mu_embedded_snapexcl: .incbin \"$W/net/out/melee_game.snapexcl\""
    echo '.global mu_embedded_snapexcl_end'; echo 'mu_embedded_snapexcl_end:'
    for f in "$gen"/scripts/*.txt; do n=$(basename "$f" .txt)
      echo ".balign 8"; echo "mu_script_$n: .incbin \"$W/gen/scripts/$n.txt\""; echo "mu_script_${n}_end:"
      echo ".global mu_script_$n"; echo ".global mu_script_${n}_end"
    done
    for f in "$here"/../reference/*.csv.gz; do n=$(basename "$f" .csv.gz)
      echo ".balign 8"; echo "mu_ref_$n: .incbin \"experiments/native-engine/reference/$n.csv.gz\""; echo "mu_ref_${n}_end:"
      echo ".global mu_ref_$n"; echo ".global mu_ref_${n}_end"
    done
    # Slippi's menu patches and files (this repository's port/slippi_sys), for play mode's `slippi_menus 1`.
    i=0; for f in "$repo"/port/slippi_sys/GameFiles/GALE01/*; do i=$((i+1))
      echo ".balign 8"; echo "mu_sys_$i: .incbin \"port/slippi_sys/GameFiles/GALE01/$(basename "$f")\""; echo "mu_sys_${i}_end:"
      echo ".global mu_sys_$i"; echo ".global mu_sys_${i}_end"
    done
    # melee-unlocked's fonts and launcher art (gen/mu_ui, above): mu_ui.h
    for pair in font_black:SourceSans3-Black.otf font_bold:SourceSans3-Bold.otf bailey:bailey_cut.bmp mark:melee_mark.bmp; do
      n=${pair%%:*}; f=${pair#*:}
      echo ".balign 8"; echo ".global mu_ui_$n"; echo "mu_ui_$n: .incbin \"$W/gen/mu_ui/$f\""
      echo ".global mu_ui_${n}_end"; echo "mu_ui_${n}_end:"
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
    i=0; for f in "$repo"/port/slippi_sys/GameFiles/GALE01/*; do i=$((i+1)); echo "extern \"C\" const uint8_t mu_sys_$i[], mu_sys_${i}_end[];"; done
    echo 'extern const EmbeddedFile mu_embedded_sys[] = {'
    i=0; for f in "$repo"/port/slippi_sys/GameFiles/GALE01/*; do i=$((i+1)); echo "  {\"$(basename "$f")\", mu_sys_$i, mu_sys_${i}_end},"; done
    echo '};'
    echo 'extern const size_t mu_embedded_sys_count = sizeof mu_embedded_sys / sizeof mu_embedded_sys[0];'
  } > "$gen/embedded_tables.cpp"
  python3 -I "$here/../tools/gen_api_names.py" "$work/mu/port/runtime/abi/mu_host.h" "$gen/api_names.cpp"   # the runner's host profiler
  # The replay recorder's code list (src/features/replays.cpp), from the fetched tree's own generator.
  python3 -I "$src/mu/tools/native_recording_codes.py" "$gen/native_recording_codes.h"
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

# The netplay object went through a relocatable link that dissolved its COMDAT groups, so the inline variables and
# template instances it shares with this program (standard library, the app's headers) are plain definitions in
# it: made weak in a copy, they join ours instead of clashing.
# Only its defined symbols: see netplay_link.sh (a weakened reference pulls nothing out of an archive).
L=experiments/native-engine/frontend/netplay_link.sh
WEAKEN_LINUX="for v in obj-linux obj-linux-services obj-linux-services$SAN_SFX; do [ ! -f $W/net/\$v/netplay.o ] || [ $W/net/\$v/netplay-weak.o -nt $W/net/\$v/netplay.o -a $W/net/\$v/netplay-weak.o -nt $L ] || bash $L weaken '' $W/net/\$v/netplay.o $W/net/\$v/netplay-weak.o; done"

# The netplay object with the app's real services for Linux, by the recipe of ../build.sh (net_build with
# MU_NET_SERVICES=1) from the synced runner: the bridge, this repository's netplay sources and ENet in one object,
# every other symbol local. Its service address is the loopback stand-in's, compiled in. Made here because the test
# needs one that matches the synced bridge header. The netplay sources are ../net_sources.sh's patched copy.
NET_INC="-I$W/runner -I$W/netsrc/port/runtime/hle -Iport/runtime/hle -Iport/runtime/host -Iport/runtime/gx -Iport/runtime/ppc -Iport/runtime -Iport/third_party/enet/include -Iport/third_party -Iport/third_party/nlohmann -Iport/app"
# Slippi's HTTP services (user profile, game reports, replay upload) go to the loopback stand-in too (patches/net-0008,
# ../tools/mock_slippi_http.py).
NET_DEF='-DSLIPPI_PIN_VERSION=\"3.7.0\" -DSLIPPI_PIN_TAG=\"v3.7.0\" -DSLIPPI_PIN_MATCHMAKING_URL=\"ws://127.0.0.1:8765\" -DSLIPPI_PIN_SERVICES_URL=\"http://127.0.0.1:8766\" -DMU_NET_SERVICES'
NET_CXX='slippi_online slippi_net slippi_offline slippi_net_diag slippi_match_quality slippi_text slippi_version slippi_matchmaking_ws slippi_report slippi_http_curl slippi_login'
build_net_services_linux() {
  local o="$W/net/obj-linux-services$SAN_SFX" stamp
  stamp="$(bash "$here/../net_sources.sh" "$work/netsrc")-$(cd "$repo" && cat "$W"/runner/netplay_bridge.* | shasum -a 256 | cut -c1-16)$SAN_SFX"
  [ -f "$repo/$o/netplay.o" ] && [ "$(cat "$repo/$o/stamp" 2>/dev/null)" = "$stamp" ] && return 0
  dock 900 "$LINUX_IMG" "
    rm -rf $o; mkdir -p $o/net
    for f in callbacks compress host list packet peer protocol unix; do
      gcc -fPIE -O2 -w $SAN_FLAGS -fvisibility=hidden -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_SOCKLEN_T=1 -DHAS_MSGHDR_FLAGS=1 -include arpa/inet.h -Iport/third_party/enet/include -c port/third_party/enet/\$f.c -o $o/net/enet_\$f.o
    done
    for f in $NET_CXX; do echo $W/netsrc/port/runtime/hle/\$f.cpp; done > $o/list; echo $W/runner/netplay_bridge.cpp >> $o/list
    xargs -P $jobs -I{} sh -c 'g++ -fPIE -std=gnu++20 -O2 -g -w $SAN_FLAGS -fvisibility=hidden -fvisibility-inlines-hidden $NET_INC $NET_DEF -c {} -o $o/net/\$(basename {} .cpp).o' < $o/list
    ld -r --force-group-allocation $o/net/*.o -o $o/netplay.o
    objcopy --localize-hidden $o/netplay.o"
  echo "$stamp" > "$repo/$o/stamp"
  echo "built $repo/$o/netplay.o"
}

build_image() { timeout $(( 900 * ${MU_TIMEOUT_SCALE:-1} )) docker build -t "$GFX_IMG" -f "$here/linux-gfx.Dockerfile" "$here"; }

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
  build_net_services_linux
  dock 900 "$LINUX_IMG" "
    $WEAKEN_LINUX
    cmake -S experiments/native-engine/frontend -B $W/null$SAN_SFX -G Ninja -DCMAKE_BUILD_TYPE=Release -DMU_FE_VARIANT=null \
      -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ -DMU_FE_FEATURES=${MU_FE_FEATURES:-ON} -DMU_FE_SANITIZE=$SAN >/dev/null
    cmake --build $W/null$SAN_SFX --parallel $jobs > $W/null$SAN_SFX-build.log 2>&1 || { grep -E 'error|FAILED|undefined' -A4 $W/null$SAN_SFX-build.log | head -60; exit 1; }
    bash $L check-dynamic '' $W/net/obj-linux/netplay.o $W/null$SAN_SFX/mu-native-gfx-null$SAN_SFX
    bash $L check-dynamic '' $W/net/obj-linux-services$SAN_SFX/netplay.o $W/null$SAN_SFX/mu-native-gfx-null-services$SAN_SFX
    cp $W/null$SAN_SFX/mu-native-gfx-null$SAN_SFX $W/null$SAN_SFX/mu-native-gfx-null-services$SAN_SFX $W/out/"
  echo "built $out/mu-native-gfx-null$SAN_SFX and -services$SAN_SFX"
}

build_linux() {
  [ -f "$work/dawn-linux/src/dawn/native/libwebgpu_dawn.a" ] && [ -f "$work/sdl-linux/libSDL3.a" ] || { echo "run ./build.sh deps first" >&2; exit 1; }
  fetched_deps
  build_net_services_linux
  dock 2400 "$GFX_IMG" "
    $WEAKEN_LINUX
    cmake -S experiments/native-engine/frontend -B $W/linux$SAN_SFX -G Ninja -DCMAKE_BUILD_TYPE=Release -DMU_FE_VARIANT=linux \
      -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ -DMU_FE_FEATURES=${MU_FE_FEATURES:-ON} -DMU_FE_SANITIZE=$SAN $DEPS_ARGS >$W/linux$SAN_SFX-configure.log 2>&1 || { tail -30 $W/linux$SAN_SFX-configure.log; exit 1; }
    cmake --build $W/linux$SAN_SFX --target mu_native_gfx mu_native_gfx_services --parallel $jobs > $W/linux$SAN_SFX-build.log 2>&1 || { grep -E 'error|FAILED|undefined' -A4 $W/linux$SAN_SFX-build.log | head -60; exit 1; }
    bash $L check-dynamic '' $W/net/obj-linux/netplay.o $W/linux$SAN_SFX/mu-native-gfx-linux$SAN_SFX
    bash $L check-dynamic '' $W/net/obj-linux-services$SAN_SFX/netplay.o $W/linux$SAN_SFX/mu-native-gfx-linux-services$SAN_SFX
    cp $W/linux$SAN_SFX/mu-native-gfx-linux$SAN_SFX $W/linux$SAN_SFX/mu-native-gfx-linux-services$SAN_SFX $W/out/"
  echo "built $out/mu-native-gfx-linux$SAN_SFX and -services$SAN_SFX"
}

build_nro() {
  [ -f "$repo/build/sdk/SDK_MANIFEST.json" ] || { echo "build/sdk is missing: unpack the melee-nx-netplay-sdk archive there (BUILDING.md)" >&2; exit 1; }
  fetched_deps
  dock 2400 "$NX_IMG" "
    export PATH=/opt/devkitpro/devkitA64/bin:\$PATH
    for v in obj-nx obj-nx-services; do [ ! -f $W/net/\$v/netplay.o ] || [ $W/net/\$v/netplay-weak.o -nt $W/net/\$v/netplay.o -a $W/net/\$v/netplay-weak.o -nt $L ] || bash $L weaken aarch64-none-elf- $W/net/\$v/netplay.o $W/net/\$v/netplay-weak.o; done
    cmake -S experiments/native-engine/frontend -B $W/nx -G Ninja -DCMAKE_BUILD_TYPE=Release -DMU_FE_VARIANT=switch \
      -DCMAKE_TOOLCHAIN_FILE=/w/switch/cmake/SwitchGCC.cmake -DMELEE_NX_SDK_DIR=/w/build/sdk -DMU_FE_FEATURES=${MU_FE_FEATURES:-ON} $DEPS_ARGS >$W/nx-configure.log 2>&1 || { tail -30 $W/nx-configure.log; exit 1; }
    cmake --build $W/nx --parallel $jobs > $W/nx-build.log 2>&1 || { grep -E 'error|FAILED|undefined' -A4 $W/nx-build.log | head -60; exit 1; }
    bash $L check aarch64-none-elf- $W/net/obj-nx/netplay.o $W/nx/mu-native-gfx.elf
    [ ! -f $W/nx/mu-native-gfx-services.elf ] || bash $L check aarch64-none-elf- $W/net/obj-nx-services/netplay.o $W/nx/mu-native-gfx-services.elf
    cp $W/nx/mu-native-gfx*.nro $W/out/
    # The app: the services program under the name the Homebrew Menu, the self-update and the forwarders use.
    [ ! -f $W/out/mu-native-gfx-services.nro ] || cp $W/out/mu-native-gfx-services.nro $W/out/melee-nx-netplay.nro
    ls -l $W/out/*.nro"
  echo "built $out/mu-native-gfx.nro$([ -f "$out/mu-native-gfx-services.nro" ] && echo ' and mu-native-gfx-services.nro')"
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
