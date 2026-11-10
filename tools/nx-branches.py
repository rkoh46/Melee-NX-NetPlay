#!/usr/bin/env python3
# Merges the counting build's Logs/branch_counts.txt files (one per console run, "addr taken total"
# per line) into port/recomp/branch_profile.txt, which recomp.py turns into branch probabilities
# (__builtin_expect_with_probability) so the compiler lays out each function's executed path
# contiguously. Counts are summed per guest branch address.
#   MELEE_BRANCH_PROFILE=gen bash builder/generate-game.sh <iso>   # counting build, then run matches
#   tools/nx-branches.py build/branches/*.txt                      # merge
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import collections

ap = argparse.ArgumentParser()
ap.add_argument("files", nargs="+")
ap.add_argument("--out", default="port/recomp/branch_profile.txt")
args = ap.parse_args()
taken = collections.Counter()
total = collections.Counter()
for path in args.files:
    for line in open(path):
        parts = line.split()
        if len(parts) != 3:
            continue
        a = int(parts[0], 16)
        taken[a] += int(parts[1])
        total[a] += int(parts[2])
with open(args.out, "w") as f:
    f.write("# Guest branch counts from console runs (tools/nx-branches.py): address taken executed\n")
    for a in sorted(total):
        f.write("%08X %d %d\n" % (a, taken[a], total[a]))
print("%d branches from %d files -> %s" % (len(total), len(args.files), args.out))
