"""Dump decoded instructions around an address: python dump.py 80019d8c [before] [after]"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from dol import Dol
from symbols import SymbolMap
from gekko import decode

ROOT = Path(__file__).resolve().parents[2]


def fmt(ins):
    if ins is None:
        return "??"
    f = ins.f
    keys = ["rd", "ra", "rb", "simm", "uimm", "spr", "sh", "mb", "me", "bo", "bi"]
    parts = ["%s=%s" % (k, (hex(f[k]) if isinstance(f[k], int) and abs(f[k]) > 9 else f[k])) for k in keys if k in f]
    t = ins.branch_target
    if t is not None:
        parts.append("-> %08x" % t)
    return "%-10s %s" % (ins.op, " ".join(parts))


def main():
    dol = Dol(ROOT / "melee/orig/GALE01/sys/main.dol")
    symbols = SymbolMap(ROOT / "melee/config/GALE01/symbols.txt")
    before, after = 24, 2
    for arg in sys.argv[1:]:
        if arg.startswith("-b"):
            before = int(arg[2:])
            continue
        if arg.startswith("-a"):
            after = int(arg[2:])
            continue
        addr = int(arg, 16)
        func = symbols.containing(addr)
        print("== %08x in %s" % (addr, func))
        for a in range(max(func.addr, addr - before * 4), min(func.end, addr + after * 4 + 4), 4):
            print("  %08x  %s" % (a, fmt(decode(a, dol.u32(a)))))


if __name__ == "__main__":
    main()
