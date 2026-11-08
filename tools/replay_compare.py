"""Plays a .slp replay back through the playback build and compares the recording it writes with
the original, frame by frame: positions, action states, percent, stocks, facing for every
player. This is the frame-exactness oracle against Slippi Dolphin (which recorded the original).

    python tools/replay_compare.py <replay.slp> [--exe build-review/port/Release/melee_port_playback.exe]
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from melee_iso import require_iso

ROOT = Path(__file__).resolve().parents[1]


def parse_slp(path):
    """Returns (settings bytes, {frame: {player: post dict}}, {frame: {player: pre dict}})."""
    d = open(path, "rb").read()
    i = d.find(b"raw")
    if i < 0:
        raise SystemExit(f"{path}: no raw block")
    i += 3
    assert d[i:i+2] == b"[$"
    n = struct.unpack(">I", d[i+5:i+9])[0]
    raw = d[i+9:i+9+n]
    assert raw[0] == 0x35
    count = (raw[1] - 1) // 3
    sizes = {raw[2+3*k]: struct.unpack(">H", raw[3+3*k:5+3*k])[0] for k in range(count)}
    pos = 1 + raw[1]
    post, pre, start = {}, {}, None
    while pos < len(raw):
        c = raw[pos]
        s = sizes.get(c)
        if s is None:
            break
        body = raw[pos:pos+1+s]
        if c == 0x36:
            start = bytes(body)
        elif c == 0x37:
            f = struct.unpack(">i", body[1:5])[0]; pl = body[5]; follower = body[6]
            jx, jy, cx, cy, trig = struct.unpack(">fffff", body[0x19:0x2D])
            buttons = struct.unpack(">I", body[0x2D:0x31])[0]
            pre.setdefault(f, {})[(pl, follower)] = dict(jx=jx, jy=jy, cx=cx, cy=cy, trig=trig, buttons=buttons)
        elif c == 0x38:
            f = struct.unpack(">i", body[1:5])[0]; pl = body[5]; follower = body[6]
            char = body[7]
            state = struct.unpack(">H", body[8:10])[0]
            x, y, facing = struct.unpack(">fff", body[0xA:0x16])
            percent = struct.unpack(">f", body[0x16:0x1A])[0]
            shield = struct.unpack(">f", body[0x1A:0x1E])[0]
            stocks = body[0x21]
            post.setdefault(f, {})[(pl, follower)] = dict(char=char, state=state, x=x, y=y, facing=facing, percent=percent, shield=shield, stocks=stocks)
        pos += 1 + s
    return start, post, pre


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("replay", type=Path)
    ap.add_argument("--exe", type=Path, default=ROOT / "build-review/port/Release/melee_port_playback.exe")
    ap.add_argument("--iso", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "reports/native-validation/playback")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--visible", action="store_true", help="show the window instead of running hidden and fast")
    args = ap.parse_args()
    args.iso = require_iso(args.iso)
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    for old in glob.glob(str(out / "*.slp")):
        os.remove(old)
    cmd = [str(args.exe), "--iso", str(args.iso), "--sys-dir", str(ROOT / "port/slippi_sys_playback"), "--replay", str(args.replay.resolve()),
           "--volume", "0", "--replay-dir", str(out), "--card-dir", str(out / "card"), "--log-file", str(out / "port.log")]
    if not args.visible:
        cmd += ["--hidden", "--fast"]
    print("running:", " ".join(cmd[1:]))
    try:
        run = subprocess.run(cmd, cwd=ROOT, timeout=args.timeout, capture_output=True)
        print("exit", run.returncode)
    except subprocess.TimeoutExpired:
        print("timed out")
        return 2
    recorded = sorted(glob.glob(str(out / "*.slp")))
    if not recorded:
        print("no recording written; see", out / "port.log")
        return 2
    orig_start, orig_post, orig_pre = parse_slp(args.replay)
    new_start, new_post, new_pre = parse_slp(recorded[-1])
    print(f"original frames {min(orig_post)}..{max(orig_post)} ({len(orig_post)}), recorded {min(new_post)}..{max(new_post)} ({len(new_post)})")
    frames = sorted(set(orig_post) & set(new_post))
    mismatches = 0
    first = None
    for f in frames:
        for key, a in orig_post[f].items():
            b = new_post[f].get(key)
            if b is None:
                mismatches += 1; first = first or (f, key, "missing in recording"); continue
            for field in ("state", "x", "y", "facing", "percent", "stocks", "char"):
                if a[field] != b[field]:
                    mismatches += 1
                    if first is None:
                        first = (f, key, f"{field}: original {a[field]} vs port {b[field]}")
                    break
    compared = sum(len(orig_post[f]) for f in frames)
    print(f"{compared} player-frames compared over {len(frames)} frames; {mismatches} mismatches")
    if first:
        print("first divergence: frame", first[0], "player/follower", first[1], first[2])
    return 0 if mismatches == 0 and frames else 1


if __name__ == "__main__":
    sys.exit(main())
