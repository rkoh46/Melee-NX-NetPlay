#!/usr/bin/env bash
# Builds mu-layoutprobe.nro in the project's toolchain image. Output: build/experiments/layoutprobe/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
out="$repo/build/experiments/layoutprobe"; mkdir -p "$out"
docker run --rm --network none -v "$repo:/w" -w /w "${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}" bash -ec '
  export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
  o=build/experiments/layoutprobe
  aarch64-none-elf-gcc -O2 -g -Wall -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections \
    -D__SWITCH__ -I/opt/devkitpro/libnx/include experiments/native-engine/layoutprobe/main.c experiments/native-engine/runner/fixedmap_horizon.c \
    -specs=/opt/devkitpro/libnx/switch.specs -L/opt/devkitpro/libnx/lib -lnx -o $o/mu-layoutprobe.elf
  elf2nro $o/mu-layoutprobe.elf $o/mu-layoutprobe.nro'
echo "built $out/mu-layoutprobe.nro"
