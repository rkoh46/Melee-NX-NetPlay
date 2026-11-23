#!/usr/bin/env python3
"""Compares the state digests of two netplay instances by match frame (the retrace column is ignored).
A frame's row is what that instance showed after first simulating the frame; when it had predicted the
other player's input wrongly, the row differs until the rollback, which is not observed again. So rows
may differ for a few frames around a misprediction and must agree again afterwards.
usage: compare_net_digests.py a.csv b.csv"""
# SPDX-License-Identifier: GPL-3.0-or-later
import sys
def load(path):
    rows = {}
    for line in open(path):
        c = line.rstrip('\n').split(',')
        if int(c[3]): rows[int(c[3])] = c[1:]     # last row seen for each match frame
    return rows
a, b = load(sys.argv[1]), load(sys.argv[2])
frames = sorted(set(a) & set(b))
if not frames: sys.exit('no match frames in common')
different = [f for f in frames if a[f] != b[f]]
runs = []
for f in different:
    if runs and f == runs[-1][1] + 1: runs[-1][1] = f
    else: runs.append([f, f])
print('match frames in both: %d (%d..%d)' % (len(frames), frames[0], frames[-1]))
print('frames whose shown state differs: %d, in %d runs, longest %d frames' % (len(different), len(runs), max([r[1] - r[0] + 1 for r in runs], default=0)))
tail = [f for f in frames[-60:] if a[f] != b[f]]
print('last 60 frames in common: %s' % ('identical' if not tail else '%d differ' % len(tail)))
print('final frame %d: %s' % (frames[-1], 'IDENTICAL state on both sides' if a[frames[-1]] == b[frames[-1]] else 'DIFFERENT'))
if different and len(runs) <= 12: print('runs:', ' '.join('%d-%d' % (r[0], r[1]) for r in runs))
