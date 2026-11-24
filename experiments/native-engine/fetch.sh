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
echo "tree: $tree"
