"""Gekko (PowerPC 750CL) instruction decoder for static recompilation.

Produces Insn objects with a canonical mnemonic (no Rc/OE/LK suffix; those are flags)
and the fields the emitter needs. Coverage is verified against the whole Melee DOL by
recomp/stats.py; any word that does not decode is reported there.
"""


def _bits(w, start, count):
    """Extract `count` bits starting at PowerPC bit `start` (0 = MSB)."""
    return (w >> (32 - start - count)) & ((1 << count) - 1)


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def _s12(v):
    return v - 0x1000 if v & 0x800 else v


class Insn:
    __slots__ = ("addr", "raw", "op", "rc", "oe", "lk", "aa", "f")

    def __init__(self, addr, raw, op, f, rc=0, oe=0, lk=0, aa=0):
        self.addr, self.raw, self.op, self.f = addr, raw, op, f
        self.rc, self.oe, self.lk, self.aa = rc, oe, lk, aa

    def __repr__(self):
        return "%08x %s %s" % (self.addr, self.op, self.f)

    @property
    def branch_target(self):
        if self.op == "b":
            return self.f["li"] if self.aa else (self.addr + self.f["li"]) & 0xFFFFFFFF
        if self.op == "bc":
            return self.f["bd"] if self.aa else (self.addr + self.f["bd"]) & 0xFFFFFFFF
        return None


# op 19 (XL form) by xo10
OP19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
        225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}

# op 31 by xo10 (non-OE forms) and by xo9 for arithmetic forms that carry OE
OP31_ARITH = {8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe",
              138: "adde", 200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw",
              266: "add", 459: "divwu", 491: "divw"}
OP31 = {0: "cmp", 4: "tw", 19: "mfcr", 20: "lwarx", 23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and",
        32: "cmpl", 54: "dcbst", 55: "lwzux", 60: "andc", 83: "mfmsr", 86: "dcbf", 87: "lbzx",
        119: "lbzux", 124: "nor", 144: "mtcrf", 146: "mtmsr", 150: "stwcx", 151: "stwx", 183: "stwux",
        210: "mtsr", 215: "stbx", 242: "mtsrin", 246: "dcbtst", 247: "stbux", 278: "dcbt", 279: "lhzx",
        284: "eqv", 306: "tlbie", 311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 371: "mftb",
        375: "lhaux", 407: "sthx", 412: "orc", 439: "sthux", 444: "or", 467: "mtspr", 470: "dcbi",
        476: "nand", 512: "mcrxr", 533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync",
        567: "lfsux", 595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux", 659: "mfsrin",
        661: "stswx", 662: "stwbrx", 663: "stfsx", 695: "stfsux", 725: "stswi", 727: "stfdx",
        759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi", 854: "eieio", 918: "sthbrx",
        922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx", 1014: "dcbz"}

OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 24: "fres", 25: "fmuls", 28: "fmsubs", 29: "fmadds",
        30: "fnmsubs", 31: "fnmadds"}
OP63_5 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
          28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
OP63_10 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
           64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs",
           583: "mffs", 711: "mtfsf"}
OP4_5 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
         18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul",
         26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
OP4_10 = {0: "ps_cmpu0", 32: "ps_cmpo0", 64: "ps_cmpu1", 96: "ps_cmpo1", 40: "ps_neg", 72: "ps_mr",
          136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
          624: "ps_merge11", 1014: "dcbz_l"}
OP4_6 = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}

D_FORM = {3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic_rc",
          14: "addi", 15: "addis", 24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi_rc",
          29: "andis_rc", 32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu",
          38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu",
          46: "lmw", 47: "stmw", 48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs",
          53: "stfsu", 54: "stfd", 55: "stfdu"}
PSQ_D = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}

LOAD_OPS = {"lwz", "lwzu", "lbz", "lbzu", "lhz", "lhzu", "lha", "lhau", "lmw", "lfs", "lfsu", "lfd",
            "lfdu", "lwzx", "lwzux", "lbzx", "lbzux", "lhzx", "lhzux", "lhax", "lhaux", "lfsx", "lfsux",
            "lfdx", "lfdux", "lwbrx", "lhbrx", "lwarx", "lswi", "lswx", "psq_l", "psq_lu", "psq_lx",
            "psq_lux"}
STORE_OPS = {"stw", "stwu", "stb", "stbu", "sth", "sthu", "stmw", "stfs", "stfsu", "stfd", "stfdu",
             "stwx", "stwux", "stbx", "stbux", "sthx", "sthux", "stfsx", "stfsux", "stfdx", "stfdux",
             "stwbrx", "sthbrx", "stwcx", "stswi", "stswx", "stfiwx", "psq_st", "psq_stu", "psq_stx",
             "psq_stux"}


def decode(addr, w):
    op = w >> 26
    f = {"rd": _bits(w, 6, 5), "ra": _bits(w, 11, 5), "rb": _bits(w, 16, 5), "rs": _bits(w, 6, 5),
         "simm": _s16(w & 0xFFFF), "uimm": w & 0xFFFF, "rc": w & 1}
    f["fd"] = f["fs"] = f["rd"]
    f["fa"], f["fb"], f["fc"] = f["ra"], f["rb"], _bits(w, 21, 5)
    f["crfd"] = _bits(w, 6, 3)
    f["crfs"] = _bits(w, 11, 3)
    if op in D_FORM:
        name = D_FORM[op]
        f["to"] = f["rd"]
        f["l"] = _bits(w, 10, 1)
        return Insn(addr, w, name, f, rc=1 if name.endswith("_rc") else 0)
    if op in PSQ_D:
        f["w"] = _bits(w, 16, 1)
        f["i"] = _bits(w, 17, 3)
        f["simm12"] = _s12(w & 0xFFF)
        return Insn(addr, w, PSQ_D[op], f)
    if op == 16:
        f["bo"], f["bi"] = _bits(w, 6, 5), _bits(w, 11, 5)
        bd = w & 0xFFFC
        f["bd"] = bd - 0x10000 if bd & 0x8000 else bd
        return Insn(addr, w, "bc", f, aa=_bits(w, 30, 1), lk=w & 1)
    if op == 18:
        li = w & 0x03FFFFFC
        f["li"] = li - 0x04000000 if li & 0x02000000 else li
        return Insn(addr, w, "b", f, aa=_bits(w, 30, 1), lk=w & 1)
    if op == 17:
        return Insn(addr, w, "sc", f)
    xo10 = _bits(w, 21, 10)
    xo5 = _bits(w, 26, 5)
    if op == 19:
        name = OP19.get(xo10)
        if name is None:
            return None
        f["bo"], f["bi"] = _bits(w, 6, 5), _bits(w, 11, 5)
        f["crbd"], f["crba"], f["crbb"] = f["rd"], f["ra"], f["rb"]
        return Insn(addr, w, name, f, lk=w & 1)
    if op == 31:
        xo9 = xo10 & 0x1FF
        if xo9 in OP31_ARITH and (xo10 == xo9 or xo10 == xo9 + 512):
            f["sh"] = f["rb"]
            return Insn(addr, w, OP31_ARITH[xo9], f, rc=w & 1, oe=(xo10 >> 9) & 1)
        name = OP31.get(xo10)
        if name is None:
            return None
        f["sh"] = f["rb"]
        f["spr"] = (_bits(w, 16, 5) << 5) | _bits(w, 11, 5)
        f["crm"] = _bits(w, 12, 8)
        f["nb"] = f["rb"]
        f["to"] = f["rd"]
        f["l"] = _bits(w, 10, 1)
        return Insn(addr, w, name, f, rc=w & 1)
    if op in (20, 21, 23):
        f["sh"], f["mb"], f["me"] = f["rb"], _bits(w, 21, 5), _bits(w, 26, 5)
        return Insn(addr, w, {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[op], f, rc=w & 1)
    if op == 59:
        name = OP59.get(xo5)
        if name is None:
            return None
        return Insn(addr, w, name, f, rc=w & 1)
    if op == 63:
        name = OP63_5.get(xo5)
        if name is None:
            name = OP63_10.get(xo10)
            if name is None:
                return None
            f["fm"] = _bits(w, 7, 8)
            f["imm"] = _bits(w, 16, 4)
            f["crbd"] = f["rd"]
        return Insn(addr, w, name, f, rc=w & 1)
    if op == 4:
        xo6 = _bits(w, 25, 6)
        if xo6 in OP4_6:
            f["w"] = _bits(w, 21, 1)
            f["i"] = _bits(w, 22, 3)
            return Insn(addr, w, OP4_6[xo6], f)
        name = OP4_5.get(xo5)
        if name is None:
            name = OP4_10.get(xo10)
            if name is None:
                return None
        return Insn(addr, w, name, f, rc=w & 1)
    return None


def decode_range(dol, start, end):
    out = []
    for a in range(start, end, 4):
        w = dol.u32(a)
        out.append(decode(a, w))
    return out
