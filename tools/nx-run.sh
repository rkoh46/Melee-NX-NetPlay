#!/usr/bin/env bash
# Send an NRO to a Switch on the local network and stream its output back.
#
#   tools/nx-run.sh <switch-ip> [aurora|headless] [-- extra nxlink args]
#
# On the Switch: Atmosphère, Homebrew Menu opened by holding R while launching a game
# (full memory), then press Y so the netloader is waiting. The program's stdout (every
# host log line: frame timing, clocks, Slippi connection, the fatal reason on a crash)
# is shown here and saved to build/nxlink-logs/<time>.log. Settings and the disc are
# still read from sdmc:/switch/melee-nx-netplay/ on the SD card.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/.." && pwd -P)"
ip="${1:?usage: tools/nx-run.sh <switch-ip> [aurora|headless]}"; shift
frontend="${1:-aurora}"; [[ $# -gt 0 ]] && shift
[[ "${1:-}" == "--" ]] && shift
nro="$repo/build/switch-$frontend/port/melee-nx-netplay.nro"
[[ -f "$nro" ]] || { echo "no $nro (build it: MELEE_NX_FRONTEND=$frontend bash builder/docker.sh game all)" >&2; exit 66; }
[[ -x "$here/bin/nxlink" ]] || bash "$here/build-nxlink.sh"
mkdir -p "$repo/build/nxlink-logs"
log="$repo/build/nxlink-logs/$(date +%Y%m%d-%H%M%S)-$frontend.log"
echo "sending $(du -h "$nro" | cut -f1) $frontend NRO to $ip; output also in $log"
# -r 5: the netloader answers a ping before accepting the upload; give the Switch a few tries.
exec "$here/bin/nxlink" -a "$ip" -r 5 -s "$@" "$nro" 2>&1 | tee "$log"
