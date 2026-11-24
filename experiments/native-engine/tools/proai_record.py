#!/usr/bin/env python3
"""Records steps of one of Phillip's ONNX policies through onnxruntime, for tools/proai_check.cpp.

Each step feeds the policy a game state in the packing of slippi-ai's onnx_policies.py (format version 1, the
model's own "slippi_ai" metadata: which leaf sits where in which packed input) and its own previous outputs as
history (state.* -> prev_state.*, actions.* -> prev_actions.*), exactly as the game's host does. The game state is
synthetic but in range: two fighters moving about the stage, actions held for a few frames, percent rising, items
now and then, a reset half way. The packed inputs and onnxruntime's outputs of every step are written to one file;
proai_check runs the same inputs through the frontend's onnx_lite and compares the outputs.

    python3 -I tools/proai_record.py MODEL.onnx OUT.rec [--steps 600] [--seed 1]

Needs onnx, onnxruntime and numpy (a venv under build/, see proai_check.sh). The model is read as data only.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import struct
import sys

import numpy as np
import onnx
import onnxruntime as ort

DTYPES = {"float32": (np.float32, 1), "int32": (np.int32, 6), "bool": (np.bool_, 9), "uint8": (np.uint8, 2), "int64": (np.int64, 7)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("out")
    ap.add_argument("--steps", type=int, default=600)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    meta = json.loads({p.key: p.value for p in onnx.load(args.model, load_external_data=False).metadata_props}["slippi_ai"])
    so = ort.SessionOptions()
    so.log_severity_level = 3
    so.intra_op_num_threads = 1
    sess = ort.InferenceSession(args.model, so, providers=["CPUExecutionProvider"])
    fed = {i.name for i in sess.get_inputs()}
    out_names = [o.name for o in sess.get_outputs()]

    packs_in = {}   # pack -> (numpy type, onnx type, width, {leaf: (offset, size)})
    for pack, leaves in meta["inputs"].items():
        t = pack.split(".", 1)[1]
        width = max(l["offset"] + l["size"] for l in leaves)
        packs_in[pack] = (DTYPES[t][0], DTYPES[t][1], width, {l["name"]: (l["offset"], l["size"]) for l in leaves})
    packs_out = {}
    for pack, leaves in meta["outputs"].items():
        t = pack.split(".", 1)[1]
        width = max(l["offset"] + l["size"] for l in leaves)
        packs_out[pack] = (DTYPES[t][0], DTYPES[t][1], width, {l["name"]: (l["offset"], l["size"]) for l in leaves})
    in_order = [p for p in packs_in if p in fed]
    rng = np.random.default_rng(args.seed)
    bufs = {p: np.zeros((1, packs_in[p][2]), packs_in[p][0]) for p in packs_in}
    leaf_at = {}
    for p, (_, _, _, leaves) in packs_in.items():
        for name, (off, size) in leaves.items():
            leaf_at[name] = (p, off, size)

    def put(name, value, index=0):
        if name not in leaf_at:
            return
        p, off, size = leaf_at[name]
        if index < size:
            bufs[p][0, off + index] = value

    for name, v in meta["initial_state"].items():
        flat = np.asarray(v["value"], dtype=np.float64).reshape(-1)
        for k, x in enumerate(flat):
            put(name, x, k)

    names = sorted(set(int(v) for v in meta.get("name_map", {}).values())) or [0]
    rating = float((meta.get("agent_config") or {}).get("rating", 0.0) or 0.0)
    # The synthetic match.
    char = {0: int(rng.integers(0, 26)), 1: int(rng.integers(0, 26))}
    pos = {0: np.array([-30.0, 0.0]), 1: np.array([30.0, 0.0])}
    vel = {0: np.zeros(2), 1: np.zeros(2)}
    action = {0: 14, 1: 14}
    hold = {0: 0, 1: 0}
    percent = {0: 0, 1: 0}
    stage = int(rng.choice([0x08, 0x12, 0x06, 0x1A, 0x18, 0x19]))
    items = {}
    name_code = int(rng.choice(names))

    with open(args.out, "wb") as f:
        f.write(b"PAIREC1\n")
        f.write(struct.pack("<I", len(in_order)))
        for p in in_order:
            n = p.encode()
            f.write(struct.pack("<H", len(n)) + n + struct.pack("<iI", packs_in[p][1], packs_in[p][2]))
        f.write(struct.pack("<I", len(out_names)))
        for p in out_names:
            n = p.encode()
            f.write(struct.pack("<H", len(n)) + n + struct.pack("<iI", packs_out[p][1], packs_out[p][2]))
        f.write(struct.pack("<I", args.steps))
        for step in range(args.steps):
            reset = step == 0 or step == args.steps // 2
            if step == args.steps // 2:
                stage = int(rng.choice([0x08, 0x12, 0x06, 0x1A, 0x18, 0x19]))
            for k in (0, 1):
                vel[k] = np.clip(vel[k] * 0.9 + rng.normal(0, 0.6, 2), -3, 3)
                pos[k] = np.clip(pos[k] + vel[k], [-90, -20], [90, 80])
                if hold[k] <= 0:
                    action[k] = int(rng.integers(0, 386))
                    hold[k] = int(rng.integers(1, 30))
                hold[k] -= 1
                if rng.random() < 0.02:
                    percent[k] = min(percent[k] + int(rng.integers(1, 20)), 999)
                pre = f"game.p{k}."
                put(pre + "x", pos[k][0])
                put(pre + "y", pos[k][1])
                put(pre + "facing", rng.random() < 0.5)
                put(pre + "percent", percent[k])
                put(pre + "shield_strength", float(rng.uniform(0, 60)))
                put(pre + "on_ground", pos[k][1] <= 0.5)
                put(pre + "jumps_left", int(rng.integers(0, 3)))
                put(pre + "action", action[k])
                put(pre + "character", char[k])
                put(pre + "invulnerable", rng.random() < 0.05)
                for f_ in ("x", "y", "shield_strength", "percent", "action", "character", "jumps_left"):
                    put(pre + "nana." + f_, 0)
                for f_ in ("exists", "facing", "invulnerable", "on_ground"):
                    put(pre + "nana." + f_, False)
            put("game.stage", stage)
            put("game.randall.x", float(np.sin(step / 50.0) * 50) if stage == 0x06 else 0.0)
            put("game.randall.y", -33.25 if stage == 0x06 else 0.0)
            put("game.fod_platforms.left", float(20 + 5 * np.sin(step / 40.0)) if stage == 0x08 else 0.0)
            put("game.fod_platforms.right", float(28 + 5 * np.cos(step / 40.0)) if stage == 0x08 else 0.0)
            if rng.random() < 0.03 and len(items) < 15:
                free = [s for s in range(15) if s not in items]
                items[int(rng.choice(free))] = (int(rng.integers(0, 236)), int(rng.integers(0, 12)), float(rng.uniform(-80, 80)), float(rng.uniform(0, 60)))
            if items and rng.random() < 0.03:
                del items[int(rng.choice(list(items)))]
            for s in range(15):
                pre = f"game.items.item_{s}."
                it = items.get(s)
                put(pre + "exists", it is not None)
                put(pre + "type", it[0] if it else 0)
                put(pre + "state", it[1] if it else 0)
                put(pre + "x", it[2] if it else 0.0)
                put(pre + "y", it[3] if it else 0.0)
            put("name", name_code)
            put("needs_reset", reset)
            put("temperature", 1.0)
            put("rating", rating)
            for name, (p, off, size) in leaf_at.items():
                if name.startswith("noise."):
                    bufs[p][0, off:off + size] = rng.uniform(0.0, 1.0, size).astype(bufs[p].dtype)
            feed = {p: bufs[p] for p in in_order}
            outs = sess.run(out_names, feed)
            for p in in_order:
                f.write(bufs[p].tobytes())
            for p, o in zip(out_names, outs):
                f.write(np.ascontiguousarray(o, dtype=packs_out[p][0]).tobytes())
            # Feedback: its own sampled controller and recurrent state are the next step's history.
            got = dict(zip(out_names, outs))
            for p, (_, _, _, leaves) in packs_out.items():
                for name, (off, size) in leaves.items():
                    target = "prev_" + name
                    if (name.startswith("state.") or name.startswith("actions.")) and target in leaf_at and leaf_at[target][2] == size:
                        for k in range(size):
                            put(target, got[p][0, off + k], k)
    print(f"{args.model}: {args.steps} steps written to {args.out}")


if __name__ == "__main__":
    sys.exit(main())
