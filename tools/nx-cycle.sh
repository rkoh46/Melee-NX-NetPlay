#!/usr/bin/env bash
# One hands-free test cycle on the Switch (needs sys-botbase and sys-ftpd on the console):
#   tools/nx-cycle.sh <ip> [aurora|headless]
# From the HOME menu: hold R, launch the first game tile (Atmosphère turns it into the Homebrew
# Menu with the application's memory), press Y for the netloader, send the NRO and stream its
# output until it exits (frames= in launcher.ini bounds a measurement run). Afterwards the
# profile, if one was written, is fetched and named.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ip="${1:?usage: tools/nx-cycle.sh <switch-ip> [aurora|headless]}"; frontend="${2:-aurora}"
py="$here/../build/venv/bin/python"; [[ -x "$py" ]] || py=python3   # Pillow lives in the venv (screen classification)
bot() { "$py" "$here/nx-bot.py" "$ip" "$@"; }
echo "== launching the Homebrew Menu with the application's memory"
bot hbmenu-netloader "$here/../build/switch-pull/launch" || { echo "could not reach the Homebrew Menu"; exit 1; }
sleep 1
echo "== sending the NRO"
NX_WAIT=120 "$here/nx-run.sh" "$ip" "$frontend"
echo "== run over; fetching the profile if there is one"
"$here/nx-profile.sh" "$ip" "$frontend" 30 2>/dev/null || echo "(no profile this run)"
