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
apply() {   # $1 the git tree, $2 the patch
  if git -C "$1" apply --reverse --check "$2" 2>/dev/null; then echo "have  $(basename "$2")"
  elif git -C "$1" apply --check "$2" 2>/dev/null; then git -C "$1" apply "$2"; echo "apply $(basename "$2")"
  else echo "fetch: $(basename "$2") does not apply to $1" >&2; exit 1; fi
}
for p in "$here"/patches/0*.patch; do apply "$tree" "$p"; done
for p in "$here"/patches/decomp-*.patch; do apply "$tree/sourceport/extern/melee" "$p"; done
echo "tree: $tree"
