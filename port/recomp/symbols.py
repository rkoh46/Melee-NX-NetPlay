"""Reads doldecomp/melee config/GALE01/symbols.txt (decomp-toolkit format)."""
import re

LINE = re.compile(r"^(\S+)\s*=\s*(\S+?):0x([0-9A-Fa-f]+);\s*//\s*(.*)$")


class Function:
    __slots__ = ("name", "addr", "size", "section", "scope")

    def __init__(self, name, addr, size, section, scope):
        self.name, self.addr, self.size, self.section, self.scope = name, addr, size, section, scope

    @property
    def end(self):
        return self.addr + self.size

    def __repr__(self):
        return "%s@%08x+%x" % (self.name, self.addr, self.size)


class SymbolMap:
    def __init__(self, path):
        self.functions = []
        self.objects = {}  # addr -> (name, size)
        self.names = {}    # addr -> name (any symbol)
        seen = set()
        for line in open(path, encoding="utf-8", errors="replace"):
            m = LINE.match(line.strip())
            if not m:
                continue
            name, section, addr, attrs = m.groups()
            addr = int(addr, 16)
            attr = dict(a.split(":", 1) for a in attrs.split() if ":" in a)
            self.names.setdefault(addr, name)
            if attr.get("type") == "function":
                size = int(attr.get("size", "0"), 16)
                if size == 0 or addr in seen:
                    continue
                seen.add(addr)
                self.functions.append(Function(name, addr, size, section, attr.get("scope", "")))
            elif attr.get("type") == "object":
                self.objects[addr] = (name, int(attr.get("size", "0"), 16))
        self.functions.sort(key=lambda f: f.addr)
        self.by_addr = {f.addr: f for f in self.functions}
        self.by_name = {}
        for f in self.functions:
            self.by_name.setdefault(f.name, f)
        self._starts = [f.addr for f in self.functions]

    def add_function(self, func):
        """Registers a synthetic function (Gecko caves / hooks outside any symbol)."""
        import bisect
        if func.addr in self.by_addr:
            return
        self.functions.append(func)
        self.functions.sort(key=lambda f: f.addr)
        self.by_addr[func.addr] = func
        self.by_name.setdefault(func.name, func)
        self.names.setdefault(func.addr, func.name)
        self._starts = [f.addr for f in self.functions]

    def containing(self, addr):
        import bisect
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0:
            f = self.functions[i]
            if f.addr <= addr < f.end:
                return f
        return None

    def name_of(self, addr):
        f = self.by_addr.get(addr)
        return f.name if f else self.names.get(addr)
