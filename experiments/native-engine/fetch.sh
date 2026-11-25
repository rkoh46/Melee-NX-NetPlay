#!/usr/bin/env bash
# Fetches melee-unlocked at the pinned commit into build/native-engine/mu (gitignored) and applies our patches.
# Nothing fetched here is ever committed to this repository.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
work="$repo/build/native-engine"; tree="$work/mu"
PIN=67868aa                      # melee-unlocked v1.0.5
SRC="${MU_SOURCE:-https://github.com/Hero88go/melee-unlocked.git}"   # MU_SOURCE=file:///path/to/clone works offline
IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"   # aarch64 Linux image with GCC 15.2 (see linux-gcc15.Dockerfile)
mkdir -p "$work"
if [ ! -d "$tree/.git" ]; then
  git clone --depth 1 --branch v1.0.5 "$SRC" "$tree"
  [ "$(git -C "$tree" rev-parse --short=7 HEAD)" = "$PIN" ] || { echo "fetch: not the pinned commit $PIN" >&2; exit 1; }
  git -C "$tree" submodule update --init --depth 1 sourceport/extern/melee
  # The project's own source preparation (applies its melee-native.patch to the decomp). Third-party script: container only.
  timeout 300 docker run --rm --network none -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e 'GIT_CONFIG_VALUE_0=*' \
    -v "$tree:/mu" -w /mu "$IMG" python3 tools/prepare_native_sources.py
  git -C "$tree/sourceport/extern/melee" add -A      # index = prepared baseline, so our decomp patch shows as a diff
else
  echo "fetch: $tree exists (delete it to start over); applying the patches it does not have yet"
fi
# Every patch once: a patch added after the clone (a new decomp-*.patch) is applied to an existing tree too, and one
# already in it is left alone. A patch that neither applies nor is already applied stops the fetch.
# Each tree keeps the names of the patches applied to it (.mnx-applied): a later patch that changes the same lines as an
# earlier one (0004 after 0002) makes the earlier one's reverse check fail, so the record is asked first.
apply() {   # $1 the git tree, $2 the patch
  local name rec="$1/.mnx-applied"; name="$(basename "$2")"
  if [ -f "$rec" ] && grep -qxF "$name" "$rec"; then echo "have  $name"
  elif git -C "$1" apply --reverse --check "$2" 2>/dev/null; then echo "$name" >> "$rec"; echo "have  $name"
  elif git -C "$1" apply --check "$2" 2>/dev/null; then git -C "$1" apply "$2"; echo "$name" >> "$rec"; echo "apply $name"
  else echo "fetch: $name does not apply to $1 (if it is already in, add its name to $rec)" >&2; exit 1; fi
}
# A tree from before the record: patches go in in list order, so one that does not apply forward is already in when it
# is itself reverse-applicable or a later patch of the list is (0002 under 0004). Written once into .mnx-applied.
seed() {   # $1 the git tree, then the patches in order
  local tree=$1 rec="$1/.mnx-applied" i j; shift; [ -f "$rec" ] && return 0
  local list=("$@"); : > "$rec"
  for ((i = 0; i < ${#list[@]}; i++)); do
    git -C "$tree" apply --check "${list[i]}" 2>/dev/null && continue
    for ((j = i; j < ${#list[@]}; j++)); do
      if git -C "$tree" apply --reverse --check "${list[j]}" 2>/dev/null; then basename "${list[i]}" >> "$rec"; break; fi
    done
  done
}
seed "$tree" "$here"/patches/0*.patch
seed "$tree/sourceport/extern/melee" "$here"/patches/decomp-*.patch
for p in "$here"/patches/0*.patch; do apply "$tree" "$p"; done
for p in "$here"/patches/decomp-*.patch; do apply "$tree/sourceport/extern/melee" "$p"; done
# Small permissive audio decoders for Use custom music (frontend/src/media_decode.cpp), pinned by commit and SHA-256: minimp3
# (CC0), stb_vorbis (public domain or MIT), dr_wav and dr_flac (public domain or MIT No Attribution). Never committed.
dec="$work/audio-decoders"; mkdir -p "$dec"
pinned() {   # $1 URL, $2 SHA-256
  local name; name="$(basename "$1")"
  if [ -f "$dec/$name" ] && [ "$(shasum -a 256 "$dec/$name" | cut -d' ' -f1)" = "$2" ]; then echo "have  $name"; return; fi
  curl -fsSL --max-time 120 -o "$dec/$name.part" "$1"
  [ "$(shasum -a 256 "$dec/$name.part" | cut -d' ' -f1)" = "$2" ] || { rm -f "$dec/$name.part"; echo "fetch: $name is not the pinned file" >&2; exit 1; }
  mv "$dec/$name.part" "$dec/$name"; echo "fetch $name"
}
pinned https://raw.githubusercontent.com/lieff/minimp3/48e3a085f0e1a99446b34614e1e7f17f09dc2e82/minimp3.h eb4b44812cfe77a358aa1820462cc877473a8d97206009ebecd18b60a123875a
pinned https://raw.githubusercontent.com/lieff/minimp3/48e3a085f0e1a99446b34614e1e7f17f09dc2e82/minimp3_ex.h ec1240ad4f29961ed83a9da160fcb190ce2253b618aaae36dfbd7bab4d5eedcf
pinned https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_vorbis.c 97a8febc738e6a29fafc8d8bc557c7f17ad3db7ba6544485f59395cc6008b764
pinned https://raw.githubusercontent.com/mackron/dr_libs/e381981afa52800b9a3864df548353ff67252fbc/dr_wav.h b36c32661cfbbfb1cc0beb19f7a9d2567065e5a297f64172dcebfff59885f37b
pinned https://raw.githubusercontent.com/mackron/dr_libs/e381981afa52800b9a3864df548353ff67252fbc/dr_flac.h bd569c910a7c465e481fd50af84e2871e81e2154e30ac510d98f953cc0d5a546
# FFmpeg for the frontend's Linux test build (menu video backgrounds, frontend/build.sh ffmpeg; the console build uses
# devkitPro's FFmpeg portlib): its release n7.1 from FFmpeg's own repository, pinned by commit. LGPL-2.1-or-later as built
# there (no GPL parts enabled). Never committed.
ff="$work/ffmpeg-src"; ff_commit=b08d7969c550a804a59511c7b83f2dd8cc0499b8
if [ "$(git -C "$ff" rev-parse HEAD 2>/dev/null)" != "$ff_commit" ]; then
  rm -rf "$ff"; git clone -q --depth 1 --branch n7.1 https://git.ffmpeg.org/ffmpeg.git "$ff"
  [ "$(git -C "$ff" rev-parse HEAD)" = "$ff_commit" ] || { echo "fetch: FFmpeg n7.1 is not the pinned commit" >&2; exit 1; }
  echo "fetch FFmpeg n7.1"
else
  echo "have  FFmpeg n7.1"
fi
echo "tree: $tree"
