#!/usr/bin/env bash
# Builds mu-faultprobe.nro in the project's toolchain image. Output: build/experiments/faultprobe/.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
out="$repo/build/experiments/faultprobe"; mkdir -p "$out"
timeout 300 docker run --rm --network none -v "$repo:/w" -w /w "${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}" bash -ec '
  export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
  o=build/experiments/faultprobe; e=experiments/native-engine
  aarch64-none-elf-gcc -O2 -g -Wall -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections \
    -D__SWITCH__ -I/opt/devkitpro/libnx/include $e/faultprobe/main.c $e/faultprobe/canary.S \
    $e/runner/fixedmap_horizon.c $e/runner/dirtytrack_horizon.c $e/runner/dirtytrack_horizon.S \
    -specs=/opt/devkitpro/libnx/switch.specs -L/opt/devkitpro/libnx/lib -lnx -o $o/mu-faultprobe.elf
  aarch64-none-elf-nm $o/mu-faultprobe.elf | grep -E " T (__libnx_exception_entry|mu_fault_handler|mu_canary_store)$"
  elf2nro $o/mu-faultprobe.elf $o/mu-faultprobe.nro'
echo "built $out/mu-faultprobe.nro"
