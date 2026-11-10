#!/usr/bin/env python3
# Native GX: the decompilation's byte-matching GX source (doldecomp/melee extern/dolphin, every file
# marked Matching for GALE01) compiled for the console instead of translated from PowerPC.
#
# The GX library is integer bit-packing over a state block (__GXData) in guest RAM plus writes to the
# write-gather pipe. Compiled natively it is exactly the same arithmetic, so it can replace the
# translation call for call. Three things make the C run against guest memory unchanged:
#   * GCC's `#pragma scalar_storage_order big-endian` (forced in front of every header) stores every
#     struct field big-endian, so __GXData, GXTexObj and the rest read and write in guest byte order;
#   * the two function-pointer fields of __GXData become 32-bit integers (pointers are not covered by
#     the pragma and are 64-bit here) and their calls go back into the translated game (gxn_glue.h);
#   * the pipe macros write to the host FIFO (gx::fifo_store*), and every function is renamed gxn_*
#     so it cannot collide with Aurora's own GX implementation linked into the same program.
# Nothing from the decompilation is committed: like generate_fobj_host.py this runs at generation time
# against the pinned decomp checkout (ref/melee) and writes into build/game/generated/gxnative.
#   tools/generate_gx_native.py <out_dir> --decomp-root <ref/melee>
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
from pathlib import Path

FILES = ["GXAttr", "GXGeometry", "GXLight", "GXTexture", "GXBump", "GXTev", "GXPixel", "GXTransform"]

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--decomp-root", required=True)
args = ap.parse_args()
src = Path(args.decomp_root) / "extern/dolphin/src/dolphin/gx"
out = Path(args.out)
out.mkdir(parents=True, exist_ok=True)

# `long` is 32-bit for MWCC and 64-bit here: the decomp's u32/s32 (typedef'd from long) and every
# literal `unsigned long` field would double in size and shift every struct after it. A shadow
# dolphin/types.h (found before the decomp's) and the rewrite below keep them 32-bit.
def int32_longs(text):
    text = re.sub(r"\bunsigned long\b(?! long)", "unsigned int", text)
    return re.sub(r"\b(signed )?long\b(?! long)(?<!long long)", "int", text)


inc_root = Path(args.decomp_root) / "extern/dolphin/include"
for hdr in inc_root.rglob("*.h"):
    dst = out / "include" / hdr.relative_to(inc_root)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(int32_longs(hdr.read_text(errors="replace")))

# ---- the private header, with host pipe writes and integer callback fields ----
h = int32_longs((src / "__gx.h").read_text())
h = re.sub(r"#define GX_WRITE_U8\(ub\)[^\n]*", "#define GX_WRITE_U8(ub) gxn_write_u8((u8) (ub))", h)
h = re.sub(r"#define GX_WRITE_U16\(us\)[^\n]*", "#define GX_WRITE_U16(us) gxn_write_u16((u16) (us))", h)
h = re.sub(r"#define GX_WRITE_U32\(ui\)[^\n]*", "#define GX_WRITE_U32(ui) gxn_write_u32((u32) (ui))", h)
h = re.sub(r"#define GX_WRITE_F32\(f\)[^\n]*", "#define GX_WRITE_F32(f) gxn_write_f32((f32) (f));", h)
h = h.replace("GXWGFifo.f32 =", "GX_WRITE_F32_EXPR =")
h, n = re.subn(r"GXTexRegion\* \(\*texRegionCallback\)\(GXTexObj\*,\s*GXTexMapID\);", "unsigned int texRegionCallback;", h)
assert n == 1, "texRegionCallback field not found"
h, n = re.subn(r"GXTlutRegion\* \(\*tlutRegionCallback\)\(\s*unsigned int\);", "unsigned int tlutRegionCallback;", h)
assert n == 1, "tlutRegionCallback field not found"
# The layouts must be the GameCube's exactly: the build stops if a size moved.
h = h.replace("extern struct __GXData_struct* gx;", "extern struct __GXData_struct* gx;\n"
              "_Static_assert(sizeof(struct __GXData_struct) == 0x4F4, \"__GXData layout\");\n"
              "_Static_assert(sizeof(GXTexObj) == 0x20 && sizeof(GXLightObj) == 0x40 && sizeof(GXTlutObj) == 0xC, \"GX object layout\");\n"
              "_Static_assert(sizeof(GXTexRegion) == 0x10 && sizeof(GXTlutRegion) == 0x10, \"GX region layout\");", 1)
(out / "__gx.h").write_text(h)


# Cold functions that hand out pointers into big-endian fields or read guest float arrays stay
# translated: they are dropped here and not hooked.
DROP = {"GXGetVtxDescv", "GXSetTexRegionCallback", "GXSetTlutRegionCallback", "GXProject", "GXGetVtxAttrFmtv",
        "GXGetProjectionv", "GXGetViewportv", "GXGetLightPos", "GXGetLightDir", "GXGetLightColor",
        "GXGetLightAttnA", "GXGetLightAttnK", "GXGetTexObjAll", "GXGetTlutObjAll", "GXGetVtxAttrFmt",
        "GXGetVtxDesc", "GXGetScissor",
        # float arithmetic MWCC may have contracted differently, or MSL math: the translation stays exact
        "GXInitLightSpot", "GXInitLightDistAttn", "GXInitSpecularDir", "GXInitSpecularDirHA", "GXSetFog",
        "GXInitFogAdjTable", "GXSetViewport", "GXSetViewportJitter"}
# Defined natively but not hooked: integer outputs through plain pointers (little-endian here).
NO_HOOK = {"__GetImageTileCount"}


def function_span(c, name):
    """[start, end) of the definition of `name` (signature through the matching brace), or None."""
    m = re.search(r"^[^\n;{}#]*\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), c, re.M)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(c):
        if c[i] == "{":
            depth += 1
        elif c[i] == "}":
            depth -= 1
            if depth == 0:
                return m.start(), i + 1
        i += 1
    raise ValueError("unbalanced braces in " + name)


def alias_rewrite(c):
    """`p = &gx->field[i]; ... *p ...` -> `gx->field[i]` (GCC cannot point at a reversed-order scalar)."""
    out, pos = [], 0
    for m in re.finditer(r"^[^\n;{}#]*\b\w+\s*\([^;{]*\)\s*\{", c, re.M):
        if m.start() < pos:
            continue
        span = None
        depth, i = 0, m.end() - 1
        while i < len(c):
            if c[i] == "{":
                depth += 1
            elif c[i] == "}":
                depth -= 1
                if depth == 0:
                    span = (m.start(), i + 1)
                    break
            i += 1
        body = c[span[0]:span[1]]
        while True:
            a = re.search(r"(\w+)\s*=\s*&gx->(\w+)\[([^\]]+)\];", body)
            if not a:
                break
            var, field, idx = a.groups()
            rest = body[a.end():]
            nxt = re.search(r"\b%s\s*=\s*&gx->" % var, rest)
            cut = nxt.start() if nxt else len(rest)
            seg = re.sub(r"\*\s*%s\b" % var, "(gx->%s[%s])" % (field, idx), rest[:cut])
            body = body[:a.start()] + "/* %s aliased */" % var + seg + rest[cut:]
        out.append(c[pos:span[0]]); out.append(body); pos = span[1]
    out.append(c[pos:])
    return "".join(out)

defined = set()
for name in FILES:
    c = int32_longs((src / (name + ".c")).read_text())
    c = re.sub(r"GXWGFifo\.f32\s*=\s*([^;]+);", r"GX_WRITE_F32(\1);", c)
    c = re.sub(r"GXWGFifo\.u32\s*=\s*([^;]+);", r"GX_WRITE_U32(\1);", c)
    c = re.sub(r"GXWGFifo\.u16\s*=\s*([^;]+);", r"GX_WRITE_U16(\1);", c)
    c = re.sub(r"GXWGFifo\.u8\s*=\s*([^;]+);", r"GX_WRITE_U8(\1);", c)
    # Pointer fields of the internal object structs are 32-bit guest addresses (never dereferenced here).
    c = c.replace("    void *userData;", "    unsigned int userData;")
    c = c.replace("t->userData = user_data;", "t->userData = (unsigned int)(unsigned long)user_data;")
    c = c.replace("return t->userData;", "return (void*)(unsigned long)t->userData;")
    for tag, size in (("__GXTexObjInt_struct", 0x20), ("__GXTexRegionInt_struct", 0x10), ("__GXTlutObjInt_struct", 0xC),
                      ("__GXTlutRegionInt_struct", 0x10), ("__GXLightObjInt_struct", 0x40)):
        mm = re.search(r"struct %s \{.*?\n\}[^;]*;" % tag, c, re.S)
        if mm:
            c = c[:mm.end()] + "\n_Static_assert(sizeof(struct %s) == 0x%X, \"%s layout\");" % (tag, size, tag) + c[mm.end():]
    c = c.replace("gx->texRegionCallback(", "gxn_tex_region_callback(")
    c = c.replace("gx->tlutRegionCallback(", "gxn_tlut_region_callback(")
    for d in DROP:
        sp = function_span(c, d)
        if sp:
            c = c[:sp[0]] + "/* %s: translated, not native */" % d + c[sp[1]:]
    c = c.replace("*(u32*) &gx->unk == 0", "(gx->unk == 0 && gx->bpSent == 0)")
    # GXAttr's SETVAT takes pointers to the three VAT words: pass the format index instead.
    c = c.replace("static inline void SETVAT(u32 *va, u32 *vb, u32 *vc, GXAttr attr,", "static inline void SETVAT(u32 vfmt_, GXAttr attr,")
    sp = function_span(c, "SETVAT")
    if sp:
        body = c[sp[0]:sp[1]]
        body = body.replace("*va", "gx->vatA[vfmt_]").replace("*vb", "gx->vatB[vfmt_]").replace("*vc", "gx->vatC[vfmt_]")
        c = c[:sp[0]] + body + c[sp[1]:]
    c = re.sub(r"SETVAT\(va, vb, vc, ", "SETVAT(vtxfmt, ", c)
    c = alias_rewrite(c)
    # MWCC paired-single asm that copies a guest matrix into the pipe: the host does it from the guest
    # address with the existing exact routines (hle_mtx.cpp), so the pointer is passed through.
    for k in ("WriteMTXPS4x3", "WriteMTXPS3x3from3x4", "WriteMTXPS3x3", "WriteMTXPS4x2"):
        c = re.sub(r"static asm void %s\([^)]*\)\s*\{.*?\n\}\n" % k, "", c, flags=re.S)
        c = re.sub(r"%s\((\w+), &GXWGFifo\.f32\);" % k, r"gxn_%s((const void*)(\1));" % k, c)
    for m in re.finditer(r"^(?!static)[A-Za-z_][\w \*]*?\b(\w+)\s*\([^;{]*\)\s*\{", c, re.M):
        if m.group(1) not in ("if", "for", "while", "switch") and m.group(1) not in DROP:
            defined.add(m.group(1))
    (out / (name + ".c")).write_text('#include "gxn_prelude.h"\n' + c)

names = sorted(defined)
prelude = ["/* Generated by tools/generate_gx_native.py. */",
           "#pragma scalar_storage_order big-endian",
           "#define GXN_NATIVE 1",
           "#define gx gxn_gx   /* the state block pointer: set by the glue to guest RAM */",
           "#define __GXSetRange gxn___GXSetRange"]
prelude += ["#define %s gxn_%s" % (n, n) for n in names]
prelude += ['#include "gxn_glue.h"']
(out / "gxn_prelude.h").write_text("\n".join(prelude) + "\n")
(out / "gxn_functions.txt").write_text("\n".join(names) + "\n")
print("native GX: %d functions from %d files -> %s" % (len(names), len(FILES), out))

# ---- HLE wrappers: guest registers -> native call (gxn_hle.cpp, C++) ----
syms = {}
for line in (Path(args.decomp_root) / "config/GALE01/symbols.txt").read_text().splitlines():
    sm = re.match(r"(\S+) = \.\w+:0x([0-9A-Fa-f]+);", line)
    if sm and sm.group(1) not in syms:
        syms[sm.group(1)] = int(sm.group(2), 16)
allsrc = "".join((out / (f + ".c")).read_text() for f in FILES)
INT8 = {"u8", "GXBool", "s8", "char", "unsigned char"}
INT16 = {"u16", "s16"}
STRUCT_PTR = re.compile(r"\b(GXTexObj|GXLightObj|GXTexRegion|GXTlutObj|GXTlutRegion|GXFogAdjTable|GXVtxDescList|GXVtxAttrFmtList)\b")
READ_FLOATS = {("GXSetProjection", "mtx"): (4, 4), ("GXSetIndTexMtx", "offset"): (2, 3)}
hooked, protos, bodies, origs = [], [], [], []


def ctype(t):
    t = t.replace("const", "").strip()
    if t in ("f32", "float"):
        return "float"
    if t in ("f64", "double"):
        return "double"
    if t in INT8:
        return "uint8_t" if t != "s8" else "int8_t"
    if t in INT16:
        return "uint16_t" if t == "u16" else "int16_t"
    if t == "s32" or t == "int":
        return "int32_t"
    return "uint32_t"


for n in names:
    if n not in syms or n in NO_HOOK:
        continue
    m = re.search(r"^([^\n;{}#(]*?)\b%s\s*\(([^)]*)\)\s*\{" % re.escape(n), allsrc, re.M)
    if not m:
        continue
    ret = m.group(1).replace("static", "").replace("inline", "").strip()
    params = [x.strip() for x in m.group(2).split(",") if x.strip() and x.strip() != "void"]
    proto, call, pre, gi, fi, ok = [], [], [], 3, 1, True
    for k, prm in enumerate(params):
        pm = re.match(r"(.*?)\b(\w+)\s*((?:\[[^\]]*\])*)$", prm)
        typ, pname, dims = pm.group(1).strip(), pm.group(2), pm.group(3)
        if dims:                                   # a guest float matrix
            if (n, pname) in READ_FLOATS:
                r_, c_ = READ_FLOATS[(n, pname)]
                pre.append("float a%d[%d][%d]; gxn_read_floats(m, c.r[%d], &a%d[0][0], %d);" % (k, r_, c_, gi, k, r_ * c_))
                call.append("a%d" % k); proto.append("float (*)[%d]" % c_)
            else:
                call.append("(const void*)(uintptr_t)c.r[%d]" % gi); proto.append("const void*")
            gi += 1
        elif "*" in typ:
            if STRUCT_PTR.search(typ):
                call.append("gxn_host(m, c.r[%d])" % gi)
            elif re.match(r"(const\s+)?void\s*\*", typ):
                call.append("(void*)(uintptr_t)c.r[%d]" % gi)
            else:
                ok = False
            proto.append("void*"); gi += 1
        elif typ in ("GXColor", "GXColorS10"):
            sz = 4 if typ == "GXColor" else 8
            pre.append("GxnBytes%d a%d; std::memcpy(a%d.b, gxn_host(m, c.r[%d]), %d);" % (sz, k, k, gi, sz))
            call.append("a%d" % k); proto.append("GxnBytes%d" % sz); gi += 1
        else:
            ct = ctype(typ)
            if ct in ("float", "double"):
                call.append("(%s)c.f[%d].ps0" % (ct, fi)); fi += 1
            else:
                call.append("(%s)c.r[%d]" % (ct, gi)); gi += 1
            proto.append(ct)
    if not ok or gi > 11 or fi > 9:
        continue
    rct = "void" if ret == "void" else ctype(ret.replace("enum ", ""))
    protos.append('%s gxn_%s(%s);' % (rct, n, ", ".join(proto) or "void"))
    invoke = "gxn_%s(%s)" % (n, ", ".join(call))
    store = ("c.r[3] = (uint32_t)%s;" % invoke) if rct not in ("void", "float") else ("c.f[1].ps0 = c.f[1].ps1 = %s;" % invoke if rct == "float" else invoke + ";")
    orig = "guest::f_%08X_orig" % syms[n]
    origs.append("void f_%08X_orig(ppc::Context&, uint8_t*);" % syms[n])
    bodies.append('HLE(%s) { gxn_enter(c, m); GXN_VERIFIED(%s, %s, [&] { %s %s }); }' % (n, n, orig, " ".join(pre), store))
    hooked.append(n)

cpp = ["// Generated by tools/generate_gx_native.py: guest-register wrappers for the native GX functions.",
       '#include "hle.h"', '#include "gxn_glue_host.h"', "#include <cstdint>", "#include <cstring>",
       'extern "C" {'] + protos + ["}"]
cpp += ["namespace guest {"] + origs + ["}"]
cpp += bodies
(out / "gxn_hle.cpp").write_text("\n".join(cpp) + "\n")
(out / "gxn_hooked.txt").write_text("\n".join(hooked) + "\n")
print("native GX: %d functions hooked" % len(hooked))
