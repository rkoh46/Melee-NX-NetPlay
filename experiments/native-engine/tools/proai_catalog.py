#!/usr/bin/env python3
"""Writes the app's Fight a pro catalog (frontend/src/proai/catalog.json) from Phillip's published models.

Phillip (slippi-ai by Vlad Firoiu, MIT) publishes its trained policies on Hugging Face (vladfi/phillip-models, MIT) with
an index (index-v1.json): each model's pinned download URL, SHA-256, size, characters, opponents and delay. This reads
a saved copy of that index and the downloaded .onnx files (their own "slippi_ai" metadata: the players each policy can be
conditioned on, who its self-play trained as, its controller and inputs) and writes the catalog melee-unlocked's
phillip_ai.cpp reads (format version 1, the fields upstream's tools/ghost_ai/phillip_catalog.py writes), with the same
choices: the newest version of each policy, one character among the pros' characters, delay 18 for the lighter
policies and the 21-frame "custom_v1" ones (about 90 MB) as the heavy ones; a policy is listed only when it knows a pro
by name. Upstream ships the imitation policies in its zip and fetches the others from the same pinned URLs; this app
ships none and downloads every one only when the player asks (features/proai.cpp).

    python3 -I tools/proai_catalog.py INDEX.json MODELS_DIR OUT.json

The models are read as data (onnx's protobuf reader); nothing in them is executed.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib
import json
import re
import sys
from pathlib import Path

import onnx

# Fight a pro's players (their index is the game's: 0 Mang0, 1 Zain, 2 none, 3 Cody Schwab, 4 aMSa), the character each
# plays and the names Phillip's policies may know them by (upstream's list).
PROS = {0: ("FALCO", ["MANG#0", "Mang0", "mang0"]), 1: ("MARTH", ["ZAIN#0", "Zain"]),
        3: ("FOX", ["IBDW#0", "Cody"]), 4: ("YOSHI", ["AMSA#0", "Amsa"])}
KINDS = {"MARIO": 0, "FOX": 1, "CPTFALCON": 2, "DK": 3, "KIRBY": 4, "BOWSER": 5, "LINK": 6, "SHEIK": 7, "NESS": 8, "PEACH": 9,
         "POPO": 10, "NANA": 11, "PIKACHU": 12, "SAMUS": 13, "YOSHI": 14, "JIGGLYPUFF": 15, "MEWTWO": 16, "LUIGI": 17,
         "MARTH": 18, "ZELDA": 19, "YLINK": 20, "DOC": 21, "FALCO": 22, "PICHU": 23, "GAMEANDWATCH": 24, "GANONDORF": 25, "ROY": 26}
HF = "https://huggingface.co/vladfi/phillip-models/resolve/"


def version(name):
    found = re.search(r"_v(\d+(?:\.\d+)*)$", name)
    return tuple(int(x) for x in found.group(1).split(".")) if found else (0,)


def supported(m):
    """'' when the game supplies every input (upstream phillip_ai.cpp supported())."""
    if m.get("format_version") != 1 or m.get("batch_size") != 1:
        return "format"
    if m["controller"]["type"] not in ("default", "custom_v1"):
        return "controller"
    cfg = m["config"]
    if cfg["embed"].get("items", {}).get("type", "skip") not in ("skip", "flat"):
        return "items"
    p = cfg["embed"]["player"]
    if p.get("with_speeds") or p.get("with_controller"):
        return "player fields"
    if cfg.get("observation", {}).get("frame_skip", {}).get("skip", 0) or cfg["policy"].get("frame_skip", 1) != 1:
        return "frame skip"
    return ""


def main():
    index_path, models_dir, out_path = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    index = json.loads(index_path.read_text())
    wanted = {char for char, _ in PROS.values()}
    light = [e for e in index["models"] if e["size"] <= 50e6 and e["delay"] == 18]
    heavy = [e for e in index["models"] if 50e6 < e["size"] <= 100e6 and e["delay"] == 21]
    chosen = []
    for group in (light, heavy):
        best = {}
        for e in group:
            key = (tuple(e["characters"]), tuple(sorted(e["opponents"])), e["delay"], e["agent_type"])
            if key not in best or version(e["name"]) > version(best[key]["name"]):
                best[key] = e
        chosen += [e for e in best.values() if len(e["characters"]) == 1 and e["characters"][0] in wanted]
    files = {p.stem: p for p in models_dir.rglob("*.onnx")}
    models, skipped = [], []
    for e in sorted(chosen, key=lambda e: e["name"]):
        if not e["url"].startswith(HF):
            skipped.append((e["name"], "not a Hugging Face URL"))
            continue
        path = files.get(e["name"])
        if path is None:
            skipped.append((e["name"], "not downloaded"))
            continue
        if hashlib.sha256(path.read_bytes()).hexdigest() != e["sha256"]:
            sys.exit(f"{e['name']}: SHA-256 differs from the index")
        m = json.loads({p.key: p.value for p in onnx.load(str(path), load_external_data=False).metadata_props}["slippi_ai"])
        why = supported(m)
        if why:
            skipped.append((e["name"], why))
            continue
        character = KINDS[e["characters"][0]]
        names = {}
        for pro, (char, ids) in PROS.items():
            if KINDS[char] != character:
                continue
            code = next((m["name_map"][i] for i in ids if i in m["name_map"]), None)
            if code is not None:
                names[str(pro)] = int(code)
        if not names:
            skipped.append((e["name"], "knows no Fight a pro player by name"))
            continue
        kind = "imitation" if e["agent_type"] == "IMITATION" else "rl"
        trained_as = (m.get("agent_config") or {}).get("name") or []
        if isinstance(trained_as, str):
            trained_as = [trained_as]
        codes = {m["name_map"].get(n) for n in trained_as}
        opponents = [] if kind == "imitation" else sorted(KINDS[o] for o in e["opponents"])
        newest = m["controller"]["type"] == "custom_v1"
        priority = 1 if kind == "imitation" else (4 if len(opponents) == 1 else 2) if newest else 3
        models.append({"file": path.name, "sha256": e["sha256"], "kind": kind, "character": character, "opponents": opponents,
                       "delay": int(m["config"]["policy"]["delay"]), "names": names, "priority": priority,
                       "heavy": e["size"] > 50 * 1024 * 1024, "source": e["url"], "size": e["size"], "trained_as": trained_as,
                       "pro_trained": {p: kind == "imitation" or c in codes for p, c in names.items()},
                       "likeness_ok": False, "temperature": 1.0})
    catalog = {"version": 1, "credit": "Phillip (slippi-ai) by Vlad Firoiu, MIT license; models from huggingface.co/vladfi/phillip-models (MIT)",
               "index_updated": index.get("updated", ""), "models": models}
    out_path.write_text(json.dumps(catalog, indent=1) + "\n")
    for pro, (char, _) in PROS.items():
        mine = [x for x in models if str(pro) in x["names"]]
        print(f"pro {pro} ({char}): {len(mine)} policies, {sum(x['size'] for x in mine) / 1e6:.0f} MB "
              f"(base {sum(x['size'] for x in mine if x['kind'] == 'imitation') / 1e6:.0f} MB)")
    for name, why in skipped:
        print(f"skipped {name}: {why}")
    print(f"wrote {out_path}: {len(models)} policies")


if __name__ == "__main__":
    main()
