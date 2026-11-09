#!/usr/bin/env python3
# Turns a console profile into the recompiler's hot-function list (port/recomp/hot_functions.txt):
# the guest functions that hold `--coverage` of the guest thread's samples in translated code, hottest
# first. The recompiler places them in .text.hot.* sections, which the linker script lays out together,
# so the code the game runs every frame shares pages and cache lines instead of being spread over
# 60 MB of translation.
#   tools/nx-hotlist.py <profile.txt> <nm -S output of the ELF that produced it> [--coverage 0.97]
# The nm file comes from `aarch64-none-elf-nm -S -C <elf>` (tools/nx-hotlist.sh runs both steps).
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import bisect
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument("profile")
ap.add_argument("nm")
ap.add_argument("--coverage", type=float, default=0.97)
ap.add_argument("--out", default="port/recomp/hot_functions.txt")
args = ap.parse_args()

head = open(args.profile).readline()
anchor_rt = int(re.search(r"anchor_runtime=([0-9a-f]+)", head).group(1), 16)
syms = []
anchor_elf = None
for line in open(args.nm):
    parts = line.split(None, 3)
    if len(parts) < 4 or parts[2] not in "tTwW":
        continue
    addr, size, name = int(parts[0], 16), int(parts[1], 16), parts[3].strip()
    if name.startswith("switch_profile_start("):
        anchor_elf = addr
    m = re.match(r"guest::f_([0-9A-F]{8})(_orig)?\(", name)
    if m and size:
        syms.append((addr, addr + size, m.group(1)))
if anchor_elf is None:
    sys.exit("switch_profile_start not found in the nm output")
syms.sort()
starts = [s[0] for s in syms]
slide = anchor_rt - anchor_elf

counts = {}
for line in open(args.profile):
    p = line.split()
    if len(p) != 3 or p[0] != "pc":
        continue
    a = int(p[1], 16) - slide
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and syms[i][0] <= a < syms[i][1]:
        counts[syms[i][2]] = counts.get(syms[i][2], 0) + int(p[2])

total = sum(counts.values())
ranked = sorted(counts.items(), key=lambda kv: -kv[1])
out, acc = [], 0
for addr, n in ranked:
    out.append(addr)
    acc += n
    if acc >= total * args.coverage:
        break
with open(args.out, "w") as f:
    f.write("# Hot guest functions (tools/nx-hotlist.py from a CPU-match profile), hottest first.\n")
    f.write("# %d of %d sampled functions, %.1f%% of %d translated-code samples.\n" % (len(out), len(ranked), 100.0 * acc / max(1, total), total))
    for a in out:
        f.write(a + "\n")
print("%d hot functions (%.1f%% of %d samples in translated code) -> %s" % (len(out), 100.0 * acc / max(1, total), total, args.out))
