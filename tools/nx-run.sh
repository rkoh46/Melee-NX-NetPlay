#!/usr/bin/env bash
# Send an NRO to a Switch on the local network and stream its output back.
#
#   tools/nx-run.sh <switch-ip> [aurora|headless] [-- extra nxlink args]
#
# On the Switch: Atmosphère, Homebrew Menu opened by holding R while launching a game
# (full memory), then press Y so the netloader is waiting. This script keeps pinging (UDP,
# which the netloader ignores until it is open) for up to NX_WAIT seconds (default 600), so
# it can be started before Y is pressed. Never probe port 28280 with a TCP connection: the
# netloader treats any connection as the upload and quits with "Error getting name length".
# The program's stdout (every host log line: frame timing, clocks, Slippi connection, the
# fatal reason on a crash) is shown here and saved to build/nxlink-logs/<time>.log. Settings
# and the disc are read from sdmc:/switch/melee-nx-netplay/ on the SD card; the uploaded NRO
# is saved over the copy in that folder.
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
stamp="$(date +%Y%m%d-%H%M%S)"
log="$repo/build/nxlink-logs/$stamp-$frontend.log"
cp "$repo/build/switch-$frontend/port/melee-nx-netplay.elf" "$repo/build/nxlink-logs/$stamp-$frontend.elf"   # the profile of this run resolves against this ELF
ln -sf "$stamp-$frontend.elf" "$repo/build/nxlink-logs/current.elf"
echo "sending $(du -h "$nro" | cut -f1) $frontend NRO to $ip when its netloader answers; output also in $log"
deadline=$(( $(date +%s) + ${NX_WAIT:-600} ))
while :; do
  # -r 20 is about one second of pings; -s streams stdout after the upload until the program exits.
  set +e
  "$here/bin/nxlink" -a "$ip" -r 20 -s -p melee-nx-netplay/melee-nx-netplay.nro "$@" "$nro" 2>&1 </dev/null | tee -a "$log"
  set -e
  if ! tail -c 200 "$log" | grep -q "Connection to .* failed"; then break; fi
  if (( $(date +%s) >= deadline )); then echo "gave up: the netloader never answered (press Y in the Homebrew Menu)"; exit 1; fi
  : > "$log"
  sleep 1
done
