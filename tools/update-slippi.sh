#!/usr/bin/env bash
# Follow a new Slippi release: adopt it when this build can speak it, then rebuild from your disc.
#
#   bash tools/update-slippi.sh /path/to/GALE01.iso     # check, adopt if possible, rebuild both apps
#   bash tools/update-slippi.sh --check                 # only say what the latest release would need
#
# Slippi's matchmaking refuses anything older than its latest release. Some releases only change the
# game side (Slippi's code list and game files): those are adopted here, on your computer, by fetching
# the new files and rebuilding. Releases that change how Slippi's programs talk to each other need a
# Melee-NX-NetPlay update first; this script then changes nothing and says which parts changed.
# It never builds an app that reports a Slippi version it does not implement.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/.." && pwd -P)"
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 69; }
if [[ "${1:-}" == "--check" ]]; then
  shift
  exec python3 "$here/update-slippi.py" --check "$@"
fi
iso="${1:-}"
[[ -n "$iso" && -f "$iso" ]] || { echo "usage: $0 /path/to/GALE01.iso   (or: $0 --check)" >&2; exit 64; }
iso="$(cd "$(dirname "$iso")" && pwd)/$(basename "$iso")"
shift
before="$(shasum -a 256 "$repo/tools/slippi_pin.json" | cut -d' ' -f1)"
status=0
python3 "$here/update-slippi.py" "$@" || status=$?
if [[ $status -eq 3 ]]; then
  echo
  echo "Nothing was rebuilt. First get the newest Melee-NX-NetPlay (git pull, or run install.sh again): it may"
  echo "already implement this release. If it does not yet, online play has to wait for that update."
  exit 3
fi
[[ $status -eq 0 ]] || exit "$status"
after="$(shasum -a 256 "$repo/tools/slippi_pin.json" | cut -d' ' -f1)"
if [[ "$before" == "$after" && -f "$repo/dist/sd/switch/melee-nx-netplay/melee-nx-netplay.nro" && "${MELEE_NX_FORCE_REBUILD:-0}" != 1 ]]; then
  echo "Nothing to rebuild (MELEE_NX_FORCE_REBUILD=1 rebuilds anyway)."
  exit 0
fi
# The same build the installer does, on this checkout as it is (no git pull over the adopted files).
MELEE_NX_HOME="$repo" MELEE_NX_NO_PULL=1 exec bash "$repo/install.sh" "$iso"
