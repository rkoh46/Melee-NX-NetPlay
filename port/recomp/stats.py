"""Decode coverage and control-flow statistics for the whole DOL."""
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from dol import Dol
from symbols import SymbolMap
from analyze import analyze_all

ROOT = Path(__file__).resolve().parents[2]


def main():
    dol = Dol(ROOT / "melee/orig/GALE01/sys/main.dol")
    symbols = SymbolMap(ROOT / "melee/config/GALE01/symbols.txt")
    infos, extra = analyze_all(dol, symbols)
    total = sum(len(i.insns) for i in infos.values())
    bad = [(i.func, a, w) for i in infos.values() for a, w in i.bad]
    ops = collections.Counter(ins.op for i in infos.values() for ins in i.insns if ins)
    unresolved = [(i.func, a) for i in infos.values() for a in i.unresolved_bctr]
    jt = sum(len(i.jumptables) for i in infos.values())
    jt_entries = sum(t.count for i in infos.values() for t in i.jumptables.values())
    print("functions:", len(infos), "instructions:", total)
    print("undecodable words:", len(bad))
    for func, a, w in bad[:20]:
        print("  %08x %08x in %s" % (a, w, func.name))
    print("distinct ops:", len(ops))
    print("jump tables:", jt, "entries:", jt_entries)
    print("unresolved bctr:", len(unresolved))
    for func, a in unresolved[:30]:
        print("  %08x in %s" % (a, func.name))
    print("functions with bctrl:", sum(1 for i in infos.values() if i.has_bctrl),
          "blrl:", sum(1 for i in infos.values() if i.has_blrl))
    mid = {k: v for k, v in extra.items()}
    print("mid-function entry targets:", sum(len(v) for v in mid.values()))
    for owner, targets in list(mid.items())[:20]:
        print("  owner %s -> %s" % (symbols.name_of(owner) if owner else None, ["%08x" % t for t in sorted(targets)][:6]))
    # Coverage of text by functions
    covered = sum(f.size for f in symbols.functions if dol.in_text(f.addr))
    text = sum(e - s for s, e in dol.text_ranges)
    print("text bytes covered by functions: %d / %d (%.2f%%)" % (covered, text, 100.0 * covered / text))
    print("ops:", sorted(ops.items(), key=lambda kv: -kv[1]))


if __name__ == "__main__":
    main()
