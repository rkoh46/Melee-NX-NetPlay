#!/usr/bin/env bash
# Screenshot the Switch every few seconds into a folder, with the screen class in the name.
#   tools/nx-shots.sh <ip> <count> [interval-seconds] [dir]
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ip="${1:?ip}"; count="${2:-10}"; every="${3:-5}"; dir="${4:-$here/../build/switch-pull/shots}"
mkdir -p "$dir"
py="$here/../build/venv/bin/python"
for ((i = 0; i < count; i++)); do
  f="$dir/$(date +%H%M%S).jpg"
  state=$("$py" "$here/nx-bot.py" "$ip" state "$f" 2>/dev/null || echo "noshot")
  echo "$f $state"
  sleep "$every"
done
