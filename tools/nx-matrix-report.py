#!/usr/bin/env python3
# Summarises tools/nx-matrix.sh runs from their logs (build/matrix/*.log) as a Markdown table:
# simulation ms per frame (match average and worst), frames over 16.7 ms, and frames the renderer
# dropped during the match, split at frame 1800 (before: Aurora's boot-time pipeline warm-up, ~30 s,
# still running on the worker cores; after: steady play).
#   tools/nx-matrix-report.py [build/matrix]
# SPDX-License-Identifier: GPL-3.0-or-later
import glob, os, re, sys
STAGES = {2: "Fountain of Dreams", 3: "Pokémon Stadium", 4: "Princess Peach's Castle", 5: "Kongo Jungle", 6: "Brinstar",
          7: "Corneria", 8: "Yoshi's Story", 9: "Onett", 10: "Mute City", 11: "Rainbow Cruise", 12: "Jungle Japes",
          13: "Great Bay", 14: "Hyrule Temple", 15: "Brinstar Depths", 16: "Yoshi's Island", 17: "Green Greens",
          18: "Fourside", 19: "Mushroom Kingdom", 20: "Mushroom Kingdom II", 22: "Venom", 23: "Poké Floats",
          24: "Big Blue", 25: "Icicle Mountain", 27: "Flat Zone", 28: "Dream Land N64", 29: "Yoshi's Island N64",
          30: "Kongo Jungle N64", 31: "Battlefield", 32: "Final Destination"}
d = sys.argv[1] if len(sys.argv) > 1 else "build/matrix"
rows = []
for log in glob.glob(os.path.join(d, "stage*.log")):
    m = re.match(r"stage(\d+)-(\d)p(-teams)?\.log", os.path.basename(log))
    if not m: continue
    stage, players, teams = int(m.group(1)), int(m.group(2)), bool(m.group(3))
    lines = open(log, errors="replace").read().splitlines()
    start = None; ok = False
    for l in lines:
        mm = re.match(r"scene: major 02 minor 02 \(frame (\d+)\)", l)
        if mm and start is None: start = int(mm.group(1))
        if re.search(r"switch result: exit=0", l): ok = True
    if start is None: continue
    ms = []; worst = 0.0; late_at = {}; drops = []; frame = 0
    for l in lines:
        mm = re.match(r"\[frame (\d+)\].*sim: ([\d.]+) ms/frame \(worst ([\d.]+), late (\d+), (\d+) so far", l)
        if mm:
            frame = int(mm.group(1)); late_at[frame] = int(mm.group(5))
            if start + 120 < frame and frame % 60 == 0 and frame < 99999 and frame <= start + 120 + 2160 + 60:
                ms.append(float(mm.group(2))); worst = max(worst, float(mm.group(3)))
        mm = re.match(r"renderer: .* (\d+) stale frames dropped so far", l)
        if mm: drops.append((frame, int(mm.group(1))))
    def at(series, f):
        v = 0
        for ff, n in series:
            if ff <= f: v = n
        return v
    keys = sorted(late_at)
    late0 = max([late_at[k] for k in keys if k <= start + 120] or [0]); late1 = late_at[keys[-1]] if keys else 0
    d_start, d_warm, d_end = at(drops, start), at(drops, 1800), (drops[-1][1] if drops else 0)
    rows.append((stage, players, teams, ok, sum(ms) / len(ms) if ms else 0, worst, late1 - late0, d_warm - d_start, d_end - d_warm, len(ms) * 60))
rows.sort(key=lambda r: (r[1], r[2], r[0]))
print("| Stage | Players | Avg ms | Worst ms | Late frames | Drops (warm-up) | Drops (after) | Match frames |")
print("|---|---|---:|---:|---:|---:|---:|---:|")
for r in rows:
    name = STAGES.get(r[0], str(r[0])) + ("" if r[3] else " (no clean exit)")
    print("| %s | %d%s | %.2f | %.1f | %d | %d | %d | %d |" % (name, r[1], " teams" if r[2] else "", r[4], r[5], r[6], r[7], r[8], r[9]))
