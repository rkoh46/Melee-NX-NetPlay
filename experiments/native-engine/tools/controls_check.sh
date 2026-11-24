#!/usr/bin/env bash
# Builds and runs tools/controls_check.cpp (the controller setup's processing, profiles and Dolphin import) in the
# aarch64 Linux container, with no network. Needs ../fetch.sh first (box_melee.h and settings_chord.h come from the
# fetched melee-unlocked tree).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
IMG="${MU_LINUX_IMAGE:-mu-arm-gcc15}"
timeout 600 docker run --rm --network none --name "mnx-controls-check-$$" -v "$repo:/r" -w /r "$IMG" bash -c '
  set -e
  g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/controls_check experiments/native-engine/tools/controls_check.cpp \
    experiments/native-engine/frontend/src/controls_files.cpp -Iexperiments/native-engine/frontend/src \
    -Ibuild/native-engine/mu/port/runtime/host
  /tmp/controls_check /tmp/controls-check'
