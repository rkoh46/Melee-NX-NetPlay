#!/usr/bin/env bash
# PhobGCC settings on a stock controller (frontend/src/features/phob.cpp): melee-unlocked's own phob_test.cpp (calibration
# reaches the firmware's notch targets, the filters, trigger modes, remaps, lockouts, PhobScope's captures) on phob.cpp and
# phobscope.cpp from the fetched tree, then tools/phob_check.cpp (this program's report path: adapter reports in, the
# firmware's 1 kHz loop, what the game gets), in the aarch64 Linux container with no network.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"
timeout 900 docker run --rm --network none --name "mnx-phob-check-$$" -v "$repo:/r" -w /r "$IMG" bash -c '
  set -e
  h=build/native-engine/mu/port/runtime/host
  g++ -std=c++17 -O2 -I$h build/native-engine/mu/port/tests/phob_test.cpp $h/phob.cpp $h/phobscope.cpp -o /tmp/phob_test
  echo "== upstream phob_test"; /tmp/phob_test | tail -5
  g++ -std=c++17 -O2 -I$h -Iexperiments/native-engine/frontend/src -Iexperiments/native-engine/frontend/src/features -DMNX_PHOB_CHECK \
    experiments/native-engine/tools/phob_check.cpp experiments/native-engine/frontend/src/phob_adapter.cpp $h/phob.cpp -o /tmp/phob_check
  echo "== phob_check"; /tmp/phob_check'
