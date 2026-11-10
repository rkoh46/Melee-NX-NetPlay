#!/usr/bin/env python3
"""Lists the guest functions Slippi's Gecko codes patch (a hook or an instruction write inside them).

Native decomp C cannot see those patches, so tools/gml/build.sh drops their bodies before
optimisation: every call to them, native or translated, reaches the patched translation.
Usage: gecko_patched.py <main.dol> <symbols.txt> <gct base> [sys dir] > keep_translated.txt
"""
import contextlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "port/recomp"))
from dol import Dol            # noqa: E402
from symbols import SymbolMap  # noqa: E402
import recomp                  # noqa: E402

dol, symbols = Dol(sys.argv[1]), SymbolMap(sys.argv[2])
gs = recomp.GeckoSet(int(sys.argv[3], 0), sys.argv[4] if len(sys.argv) > 4 else None)
with contextlib.redirect_stdout(sys.stderr):
    gs.apply(dol)
patched = {h.hook for h in gs.hooks} | set(gs.optional_text)
for p in [gs.boot, gs.main]:
    for addr, blob in p.writes:
        if dol.in_text(addr):
            patched.update(range(addr & ~3, addr + len(blob), 4))
names = {s.name for s in (symbols.containing(a) for a in patched) if s is not None}
print("\n".join(sorted(names)))
