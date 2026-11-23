#!/usr/bin/env bash
# Builds mu-addrprobe.nro in the project's toolchain image. Output: build/experiments/addrprobe/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
out="$repo/build/experiments/addrprobe"; mkdir -p "$out"
docker run --rm -v "$repo:/w" -w /w "${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}" bash -ec '
  export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
  o=build/experiments/addrprobe
  aarch64-none-elf-gcc -O2 -g -Wall -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections \
    -D__SWITCH__ -I/opt/devkitpro/libnx/include experiments/native-engine/addrprobe/main.c \
    -specs=/opt/devkitpro/libnx/switch.specs -L/opt/devkitpro/libnx/lib -lnx -o $o/mu-addrprobe.elf
  elf2nro $o/mu-addrprobe.elf $o/mu-addrprobe.nro'
echo "built $out/mu-addrprobe.nro"
