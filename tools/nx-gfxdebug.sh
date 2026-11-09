#!/usr/bin/env bash
# Set the renderer's live debug bits (Logs/gfx_debug.txt, read once a second by the Aurora backend)
# and screenshot the result.
#   tools/nx-gfxdebug.sh <ip> <bits> [out.jpg]
# Bits: 1 skip draws sampling EFB copies, 2 sample guest RAM instead of the GPU copy, 4 checkerboard
# instead of the copy, 8 native copy format (I4...), 16 clear after partial copies, 32 draw dump of the
# next frame (Logs/draws-now.txt), 64 perform no texture copies, 128 half-size copies (blit shader path).
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ip="${1:?ip}"; bits="${2:?bits}"; out="${3:-$here/../build/switch-pull/gfx-$bits.jpg}"
tmp="$(mktemp)"; echo "$bits" > "$tmp"
curl -sS --connect-timeout 10 -T "$tmp" "ftp://$ip:${NX_FTP_PORT:-5000}/switch/melee-nx-netplay/Logs/gfx_debug.txt" || exit 1
rm -f "$tmp"
sleep 4.5
"$here/../build/venv/bin/python" "$here/nx-bot.py" "$ip" shot "$out"
