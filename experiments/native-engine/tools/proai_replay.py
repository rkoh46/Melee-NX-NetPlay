#!/usr/bin/env python3
"""Runs the steps the app recorded (MELEE_PRO_AI_RECORD=<file>: the packed inputs the game's observations became and the
app engine's outputs, src/proai/ort_lite.cpp) through onnxruntime and compares the outputs step by step. The other half
of tools/proai_check: that one feeds onnxruntime's inputs to the app's engine, this one the app's inputs to onnxruntime.

    python3 -I tools/proai_replay.py MODEL.onnx RECORD.rec

SPDX-License-Identifier: GPL-3.0-or-later
"""
import struct
import sys

import numpy as np
import onnxruntime as ort

TYPES = {1: np.float32, 6: np.int32, 9: np.bool_, 2: np.uint8, 7: np.int64}


def packs(f):
    (n,) = struct.unpack("<I", f.read(4))
    out = []
    for _ in range(n):
        (length,) = struct.unpack("<H", f.read(2))
        name = f.read(length).decode()
        t, w = struct.unpack("<iI", f.read(8))
        out.append((name, TYPES[t], w))
    return out


def main():
    so = ort.SessionOptions()
    so.log_severity_level = 3
    sess = ort.InferenceSession(sys.argv[1], so, providers=["CPUExecutionProvider"])
    with open(sys.argv[2], "rb") as f:
        assert f.read(8) == b"PAIREC1\n"
        ins, outs = packs(f), packs(f)
        (steps,) = struct.unpack("<I", f.read(4))
        worst, sampled = 0.0, 0
        for _ in range(steps):
            feed = {n: np.frombuffer(f.read(w * np.dtype(t).itemsize), t).reshape(1, w) for n, t, w in ins}
            got = {n: np.frombuffer(f.read(w * np.dtype(t).itemsize), t).reshape(1, w) for n, t, w in outs}
            ref = dict(zip([o[0] for o in outs], sess.run([o[0] for o in outs], feed)))
            differ = False
            for n, t, w in outs:
                if t == np.float32:
                    worst = max(worst, float(np.max(np.abs(ref[n] - got[n]))))
                elif not np.array_equal(ref[n], got[n]):
                    differ = True
            sampled += differ
    print(f"{steps} recorded steps: largest float difference {worst:.3g}; sampled controller differs in {sampled} steps")


if __name__ == "__main__":
    main()
