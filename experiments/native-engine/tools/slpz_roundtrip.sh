#!/usr/bin/env bash
# .slpz round trip in a container with no network: builds slpz_roundtrip.cpp against the fetched tree's
# port/app/replay_slpz.cpp and the system's zstd, then runs it on every .slp in DIR.
#   slpz_roundtrip.sh DIR [--keep OUT_DIR]
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
dir="$(cd "${1:?folder with .slp files}" && pwd -P)"; shift
keep_args=(); keep_mount=()
if [ "${1:-}" = --keep ]; then mkdir -p "$2"; keep_mount=(-v "$(cd "$2" && pwd -P)":/keep); keep_args=(--keep /keep); fi
timeout 300 docker run --rm --network none -v "$repo:/w:ro" -v "$dir":/slp:ro "${keep_mount[@]}" "${MU_LINUX_IMAGE:-mu-arm-gcc15}" bash -ec '
  mkdir -p /tmp/shim/zstd; echo "#include <zstd.h>" > /tmp/shim/zstd/zstd.h
  g++ -std=gnu++17 -O2 -w -I/tmp/shim -I/w/build/native-engine/mu/port/app /w/experiments/native-engine/tools/slpz_roundtrip.cpp -lzstd -o /tmp/slpz_roundtrip
  find /slp -maxdepth 1 -name "*.slp" -print0 | sort -z | xargs -0 /tmp/slpz_roundtrip "$@"' rt "${keep_args[@]}"
