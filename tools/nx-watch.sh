#!/usr/bin/env bash
# Screenshot the console every N seconds into build/switch-pull/watch/latest.jpg (+ a short ring).
ip="${1:?ip}"; every="${2:-10}"; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
py="$here/../build/venv/bin/python"; dir="$here/../build/switch-pull/watch"; mkdir -p "$dir"
n=0
while true; do
  "$py" "$here/nx-bot.py" "$ip" shot "$dir/ring-$((n % 12)).jpg" >/dev/null 2>&1 && cp "$dir/ring-$((n % 12)).jpg" "$dir/latest.jpg"
  n=$((n + 1)); sleep "$every"
done
