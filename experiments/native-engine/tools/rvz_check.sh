#!/usr/bin/env bash
# RVZ and WIA discs (frontend/src/app.cpp app_disc_convert): melee-unlocked's own disc_rvz_test.cpp and fixtures (made by its
# tests/rvz_fixtures.py from Dolphin's writer: every compression, RVZ packing, WIA purge, damaged and Wii images), built from
# the fetched tree with the decoders the app links (the fetched tree's bzip2 and LZMA SDK, the system's Zstandard) and run in
# the aarch64 Linux container with no network. `./rvz_check.sh IN OUT` converts one image instead (a whole disc).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"
args=""; mounts=()
if [ $# -eq 2 ]; then
  mkdir -p "$(dirname "$2")"
  mounts=(-v "$(cd "$(dirname "$1")" && pwd -P):/in:ro" -v "$(cd "$(dirname "$2")" && pwd -P):/outdir")
  args="/in/$(basename "$1") /outdir/$(basename "$2")"
fi
timeout 1500 docker run --rm --network none --name "mnx-rvz-check-$$" -v "$repo:/r" ${mounts[@]+"${mounts[@]}"} -w /r -e ARGS="$args" "$IMG" bash -c '
  set -e
  mu=build/native-engine/mu/port; tp=$mu/third_party
  mkdir -p /tmp/zshim/zstd; echo "#include <zstd.h>" > /tmp/zshim/zstd/zstd.h
  for c in $tp/bzip2/blocksort.c $tp/bzip2/huffman.c $tp/bzip2/crctable.c $tp/bzip2/randtable.c $tp/bzip2/compress.c $tp/bzip2/decompress.c \
           $tp/bzip2/bzlib.c $tp/lzma/LzmaDec.c $tp/lzma/Lzma2Dec.c; do gcc -O2 -w -DBZ_EXPORT -I$tp -c $c -o /tmp/$(basename $c).o; done
  g++ -std=c++17 -O2 -DBZ_EXPORT -DMU_RVZ_DATA="\"$PWD/$mu/tests/data/rvz\"" -I/tmp/zshim -I$tp -I$mu/app \
    $mu/tests/disc_rvz_test.cpp $mu/app/disc_rvz.cpp /tmp/*.o -lzstd -o /tmp/disc_rvz_test
  /tmp/disc_rvz_test $ARGS'
