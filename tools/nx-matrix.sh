#!/usr/bin/env bash
# Runs CPU-only VS matches on the console, one per configuration, and tabulates frame timing:
#   tools/nx-matrix.sh <ip> <frames> <config>...     config = stage:players[:teams]   e.g. 31:2  32:4  31:4:teams
# stage is a Slippi stage id (2 Fountain of Dreams .. 31 Battlefield, 32 Final Destination), players 2
# (Fox, Marth) or 4 (Fox, Marth, Falco, Peach), all level-9 CPUs (force_css / force_stage / force_teams in
# launcher.ini; offline VS mode only). Each run keeps a mid-match screenshot and its log; the table goes
# to build/matrix/results.tsv (match average and worst simulation ms, frames over 16.7 ms and frames
# the renderer dropped, both counted from the start of the match).
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"; repo="$(cd "$here/.." && pwd -P)"
ip="${1:?ip}"; frames="${2:?frames}"; shift 2
out="$repo/build/matrix"; mkdir -p "$out"
ini_src="$repo/dist/sd/switch/melee-nx-netplay/launcher.ini"
results="$out/results.tsv"
[[ -f "$results" ]] || printf "config\texit\tmatch_frames\tavg_ms\tworst_ms\tlate\tdropped\tlog\n" > "$results"
for cfg in "$@"; do
  IFS=: read -r stage players teams <<< "$cfg"
  tmp="$(mktemp)"
  grep -vE '^(frames|profile|profile_frames|force_css|force_stage|force_teams|hle_verify)=' "$ini_src" > "$tmp"
  { echo "frames=$frames"; echo "profile=0"; echo "force_css=$([[ "$players" == 4 ]] && echo 4 || echo 1)"; echo "force_stage=$stage"
    [[ "${teams:-}" == "teams" ]] && echo "force_teams=1"; } >> "$tmp"
  curl -sS --connect-timeout 10 -T "$tmp" "ftp://$ip:${NX_FTP_PORT:-5000}/switch/melee-nx-netplay/launcher.ini" || { echo "$cfg: cannot push launcher.ini"; rm -f "$tmp"; continue; }
  rm -f "$tmp"
  tag="stage${stage}-${players}p${teams:+-$teams}"
  ( sleep 70; "$repo/build/venv/bin/python" "$here/nx-bot.py" "$ip" shot "$out/$tag.jpg" >/dev/null 2>&1 ) &
  "$here/nx-cycle.sh" "$ip" aurora > "$out/$tag.cycle.txt" 2>&1
  wait
  log="$(ls -t "$repo"/build/nxlink-logs/2026*-aurora.log | head -1)"
  cp "$log" "$out/$tag.log"
  python3 - "$out/$tag.log" "$cfg" "$results" <<'EOF'
import re, sys
log, cfg, results = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(log, errors="replace").read().splitlines()
start = None; exit_code = "?"
for l in lines:
    m = re.match(r"scene: major 02 minor 02 \(frame (\d+)\)", l)
    if m and start is None: start = int(m.group(1))
    m = re.search(r"switch result: exit=(\d+)", l)
    if m: exit_code = m.group(1)
avg_n = avg_s = 0; worst = 0.0; late0 = late1 = None; drop0 = drop1 = None
for l in lines:
    m = re.match(r"\[frame (\d+)\].*sim: ([\d.]+) ms/frame \(worst ([\d.]+), late (\d+), (\d+) so far", l)
    if m and start is not None:
        f, ms, w, total = int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(5))
        if f <= start + 120: late0 = total            # skip the load and the first two seconds of the match
        elif f % 60 == 0 and f < 99999:
            avg_s += ms; avg_n += 1; worst = max(worst, w); late1 = total
    m = re.match(r"renderer: .* (\d+) stale frames dropped so far", l)
    if m and start is not None:
        d = int(m.group(1))
        if late1 is None: drop0 = d
        drop1 = d
mf = (avg_n * 60)
row = [cfg, exit_code, str(mf), "%.2f" % (avg_s / avg_n if avg_n else 0), "%.1f" % worst,
       str((late1 or 0) - (late0 or 0)), str((drop1 or 0) - (drop0 or 0)), log.split("/")[-1]]
open(results, "a").write("\t".join(row) + "\n")
print("\t".join(row))
EOF
done
