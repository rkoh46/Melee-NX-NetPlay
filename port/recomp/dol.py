"""GameCube DOL loader: builds a flat 24 MB guest RAM image (big-endian bytes)."""
import struct

RAM_BASE = 0x80000000
RAM_SIZE = 0x01800000


class Section:
    def __init__(self, kind, index, addr, size, offset):
        self.kind, self.index, self.addr, self.size, self.offset = kind, index, addr, size, offset

    @property
    def end(self):
        return self.addr + self.size

    def __repr__(self):
        return "%s%d %08x-%08x" % (self.kind, self.index, self.addr, self.end)


class Dol:
    def __init__(self, path):
        with open(path, "rb") as stream:
            data = stream.read()
        if len(data) < 0x100:
            raise ValueError("truncated DOL header")
        text_off = struct.unpack(">7I", data[0:28])
        data_off = struct.unpack(">11I", data[28:72])
        text_addr = struct.unpack(">7I", data[72:100])
        data_addr = struct.unpack(">11I", data[100:144])
        text_size = struct.unpack(">7I", data[144:172])
        data_size = struct.unpack(">11I", data[172:216])
        self.bss_addr, self.bss_size, self.entry = struct.unpack(">3I", data[216:228])
        self.sections = []
        for i in range(7):
            if text_size[i]:
                self.sections.append(Section("text", i, text_addr[i], text_size[i], text_off[i]))
        for i in range(11):
            if data_size[i]:
                self.sections.append(Section("data", i, data_addr[i], data_size[i], data_off[i]))
        self.ram = bytearray(RAM_SIZE)
        for s in self.sections:
            if s.offset < 0x100 or s.offset > len(data) or s.size > len(data) - s.offset:
                raise ValueError("truncated or invalid DOL section: %r" % s)
            if s.addr < RAM_BASE or s.end > RAM_BASE + RAM_SIZE:
                raise ValueError("section outside RAM: %r" % s)
            self.ram[s.addr - RAM_BASE:s.end - RAM_BASE] = data[s.offset:s.offset + s.size]
        self.text_ranges = [(s.addr, s.end) for s in self.sections if s.kind == "text"]

    def in_text(self, addr):
        return any(a <= addr < e for a, e in self.text_ranges)

    def in_ram(self, addr):
        return RAM_BASE <= addr < RAM_BASE + RAM_SIZE

    def u32(self, addr):
        o = addr - RAM_BASE
        return struct.unpack_from(">I", self.ram, o)[0]

    def write_u32(self, addr, value):
        struct.pack_into(">I", self.ram, addr - RAM_BASE, value & 0xFFFFFFFF)

    def write_bytes(self, addr, blob):
        o = addr - RAM_BASE
        self.ram[o:o + len(blob)] = blob
