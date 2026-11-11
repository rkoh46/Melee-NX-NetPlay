#!/usr/bin/env bash
# Names the addresses in a log's "hitch:" lines (switch_hitch.cpp: where the game thread was when a
# frame ran past 40/150/600 ms).
#   tools/nx-hitch.sh <log> [elf]      elf defaults to build/switch-aurora/port/melee-nx-netplay.elf;
#                                     use the ELF of the build that wrote the log (build/releases/<version>.elf).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"; repo="$(cd "$here/.." && pwd -P)"
log="${1:?usage: tools/nx-hitch.sh <log> [elf]}"
elf="${2:-$repo/build/switch-aurora/port/melee-nx-netplay.elf}"
relelf="$(cd "$(dirname "$elf")" && pwd -P)/$(basename "$elf")"; relelf="${relelf#$repo/}"
image="${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"
tc=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
anchor_elf=$(docker run --rm -v "$repo:/project" "$image" $tc-nm "/project/$relelf" </dev/null | awk '$3 ~ /switch_hitch_start/ && !f {print $1; f=1}')
[[ -n "$anchor_elf" ]] || { echo "switch_hitch_start not in $elf" >&2; exit 65; }
grep "hitch: frame" "$log" | while IFS= read -r line; do
  anchor_rt=$(sed -E 's/.*switch_hitch_start=([0-9a-f]+).*/\1/' <<< "$line")
  addrs=$(sed -E 's/.*pc=([0-9a-f]+) lr=([0-9a-f]+) up=([0-9a-f,]*).*/\1 \2 \3/' <<< "$line" | tr ',' ' ')
  elfaddrs=""
  for a in $addrs; do elfaddrs+=" $(printf '0x%x' $(( 0x$a - 0x$anchor_rt + 0x$anchor_elf )))"; done
  echo "== ${line%% pc=*}"
  docker run --rm -v "$repo:/project" "$image" $tc-addr2line -f -C -e "/project/$relelf" $elfaddrs </dev/null | paste - - | awk -F'\t' '{printf "   %s  (%s)\n", $1, $2}' | sed 's|/project/||'
done
