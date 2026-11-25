#!/usr/bin/env bash
# CISO and GCZ discs (frontend/src/disc_ciso_gcz.cpp): a 48 MiB made-up image (a GALE01 header, random, repeated and empty
# blocks) written as CISO and as GCZ by tools/ciso_gcz_make.py (the formats as Dolphin writes them: some GCZ blocks stored, an
# empty CISO block left out), then converted back by the app's code: both must equal the image byte for byte; a GCZ with one
# byte changed must be refused by its block check. In the aarch64 Linux container, no network.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"
timeout 900 docker run --rm --network none --name "${MU_DOCKER_NAME:-mnx}-ciso-gcz-check-$$" -v "$repo:/r" -w /r "$IMG" bash -c '
  set -e
  t=/tmp/cg; mkdir -p $t
  python3 -I experiments/native-engine/tools/ciso_gcz_make.py $t
  cat > $t/main.cpp <<CPP
#include "disc_ciso_gcz.h"
#include <cstdio>
int main(int argc, char** argv) {
  std::string error;
  const bool ok = fe::disc_ciso_gcz::convert(argv[1], argv[2], nullptr, &error);
  std::printf("%s -> %s: %s%s\n", argv[1], argv[2], ok ? "converted" : "refused: ", error.c_str());
  return ok ? 0 : 1;
}
CPP
  g++ -std=c++17 -O2 -Iexperiments/native-engine/frontend/src $t/main.cpp experiments/native-engine/frontend/src/disc_ciso_gcz.cpp -lz -o $t/conv
  for f in ciso gcz; do $t/conv $t/disc.$f $t/out-$f.iso; cmp $t/disc.iso $t/out-$f.iso && echo "$f: identical to the image ($(stat -c %s $t/out-$f.iso) bytes)"; done
  if $t/conv $t/bad.gcz $t/out-bad.iso; then echo "bad.gcz: ACCEPTED (wrong)"; else echo "bad.gcz: refused, as it must be"; fi'
