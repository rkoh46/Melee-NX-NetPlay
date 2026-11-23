#!/usr/bin/env python3
"""Statistics of a --dirty-dump file written by mu-native: which 4 KiB pages each VS match frame changed.
usage: dirty_stats.py dump.bin [...]"""
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys, collections

def load(path):
    d = open(path, 'rb').read(); at = 0; spans = []; frames = []
    while at < len(d):
        frame, n = struct.unpack_from('<II', d, at); at += 8
        vals = struct.unpack_from('<%dI' % n, d, at); at += 4 * n
        if frame == 0xFFFFFFFF: spans = list(zip(vals[0::2], vals[1::2]))
        else: frames.append((frame, vals))
    return spans, frames

def pct(sorted_values, p): return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * p))]

def region(page):
    if page < 0x82800000: return 'MEM1 %02d-%02d MiB' % ((page - 0x80000000) >> 22 << 2, ((page - 0x80000000) >> 22 << 2) + 4)
    return 'image .data' if page < 0x83020000 else 'image .bss'

for path in sys.argv[1:]:
    spans, frames = load(path)
    total = sum(s for _, s in spans)
    pages_total = len({p for a, s in spans for p in range(a & ~4095, a + s, 4096)})
    counts = sorted(len(v) for _, v in frames)
    union = set(); freq = collections.Counter()
    for _, v in frames: union.update(v); freq.update(v)
    print('== %s' % path)
    print('snapshot: %d spans, %.1f MiB, %d pages; %d VS match frames' % (len(spans), total / 2**20, pages_total, len(frames)))
    print('pages dirtied per frame: median %d, p90 %d, p99 %d, max %d (%.0f / %.0f / %.0f KiB)' % (
        pct(counts, .5), pct(counts, .9), pct(counts, .99), counts[-1], pct(counts, .5) * 4, pct(counts, .99) * 4, counts[-1] * 4))
    print('union over the match: %d pages = %.1f MiB (%.1f%% of the snapshot)' % (len(union), len(union) / 256, 100 * len(union) / pages_total))
    # Stability: pages dirtied in frame N that were clean in every one of the previous W frames.
    for window in (1, 7, 60, 600):
        recent = collections.deque(); seen = collections.Counter(); new = []
        for _, v in frames:
            if len(recent) >= window: new.append(sum(1 for p in v if p not in seen))
            recent.append(v); seen.update(v)
            if len(recent) > window:
                for p in recent.popleft():
                    seen[p] -= 1
                    if not seen[p]: del seen[p]
        new.sort()
        print('  not dirtied in the previous %3d frames: median %d, p99 %d, max %d, frames with none %.0f%%' % (
            window, pct(new, .5), pct(new, .99), new[-1], 100 * sum(1 for x in new if x == 0) / len(new)))
    # Pages never seen before in the whole match so far (after the first 60 frames).
    seen = set(); first = []
    for i, (_, v) in enumerate(frames):
        fresh = sum(1 for p in v if p not in seen); seen.update(v)
        if i >= 60: first.append(fresh)
    first.sort()
    print('  never dirtied before in this match (after frame 60): median %d, p99 %d, max %d, total %d' % (pct(first, .5), pct(first, .99), first[-1], sum(first)))
    # How concentrated: pages dirtied in more than X% of frames.
    n = len(frames)
    for share in (.9, .5, .1, .01):
        hot = [p for p, c in freq.items() if c >= share * n]
        covered = sum(freq[p] for p in hot) / max(1, sum(freq.values()))
        print('  pages dirtied in >= %2.0f%% of frames: %5d (%.1f MiB), covering %.1f%% of all page writes' % (share * 100, len(hot), len(hot) / 256, 100 * covered))
    by = collections.Counter(); by_union = collections.Counter()
    for p, c in freq.items(): by[region(p)] += c; by_union[region(p)] += 1
    print('  by range (average pages per frame / pages in the union):')
    for name in sorted(by): print('    %-16s %7.1f / %5d' % (name, by[name] / n, by_union[name]))
    # Rollback cost shape: union of dirty pages over a 7-frame window.
    win = []
    for i in range(7, len(frames)):
        u = set()
        for _, v in frames[i - 7:i]: u.update(v)
        win.append(len(u))
    win.sort()
    print('  union over 7 consecutive frames: median %d, p99 %d, max %d pages' % (pct(win, .5), pct(win, .99), win[-1]))
