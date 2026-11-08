// HLSL generation for GX pipelines. Ported from Dolphin VideoCommon (GPL-2.0-or-later):
// VertexShaderGen.cpp, LightingShaderGen.h, PixelShaderGen.cpp (D3D11 integer-math path).
#include "gx_shader.h"
#include "gx_texture.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gx {

namespace {

struct Code {
  std::string s;
  void w(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
  }
};

uint64_t fnv(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

// Lighting uid (Dolphin LightingUidData) built from XF channel controls.
struct LightUid { uint32_t matsource = 0, enablelighting = 0, ambsource = 0, diffusefunc = 0, attnfunc = 0, light_mask = 0; };

LightUid lighting_uid(const uint32_t* xf_regs, uint32_t numColorChans) {
  LightUid u;
  for (uint32_t j = 0; j < numColorChans; ++j) {
    uint32_t color = xf_regs[0x0E + j], alpha = xf_regs[0x10 + j];
    u.matsource |= lit_matsource(color) << j;
    u.matsource |= lit_matsource(alpha) << (j + 2);
    u.enablelighting |= lit_enable(color) << j;
    u.enablelighting |= lit_enable(alpha) << (j + 2);
    if (u.enablelighting & (1 << j)) {
      u.ambsource |= lit_ambsource(color) << j;
      u.attnfunc |= lit_attnfunc(color) << (2 * j);
      u.diffusefunc |= lit_diffusefunc(color) << (2 * j);
      u.light_mask |= lit_lightmask(color) << (8 * j);
    }
    if (u.enablelighting & (1 << (j + 2))) {
      u.ambsource |= lit_ambsource(alpha) << (j + 2);
      u.attnfunc |= lit_attnfunc(alpha) << (2 * (j + 2));
      u.diffusefunc |= lit_diffusefunc(alpha) << (2 * (j + 2));
      u.light_mask |= lit_lightmask(alpha) << (8 * (j + 2));
    }
  }
  return u;
}

void gen_light(Code& o, const LightUid& u, int index, int litchan, bool alpha) {
  const char* swizzle = alpha ? "a" : "rgb";
  o.w("ldir = lights[5*%d+3].xyz - pos.xyz;\n", index);
  int attnfunc = (u.attnfunc >> (2 * litchan)) & 3;
  int diffusefunc = (u.diffusefunc >> (2 * litchan)) & 3;
  switch (attnfunc) {
    case 0: case 2:
      o.w("ldir = normalize(ldir);\nattn = 1.0;\nif (length(ldir) == 0.0)\n\t ldir = _norm0;\n");
      break;
    case 1:
      o.w("ldir = normalize(ldir);\n");
      o.w("attn = (dot(_norm0, ldir) >= 0.0) ? max(0.0, dot(_norm0, lights[5*%d+4].xyz)) : 0.0;\n", index);
      o.w("attn = max(0.0, dot(lights[5*%d+1].xyz, float3(1.0, attn, attn*attn))) / dot(%s(lights[5*%d+2].xyz), float3(1.0, attn, attn*attn));\n",
          index, diffusefunc == 0 ? "" : "normalize", index);
      break;
    case 3:
      o.w("dist2 = dot(ldir, ldir);\ndist = sqrt(dist2);\nldir = ldir / dist;\nattn = max(0.0, dot(ldir, lights[5*%d+4].xyz));\n", index);
      o.w("attn = max(0.0, dot(lights[5*%d+1].xyz, float3(1.0, attn, attn*attn))) / dot(lights[5*%d+2].xyz, float3(1.0,dist,dist2));\n", index, index);
      break;
  }
  switch (diffusefunc) {
    case 0:
      o.w("lacc.%s += round(attn * lights[5*%d].%s);\n", swizzle, index, swizzle);
      break;
    default:
      o.w("lacc.%s += round(attn * %sdot(ldir, _norm0)) * lights[5*%d].%s);\n", swizzle,
          diffusefunc != 1 ? "max(0.0," : "(", index, swizzle);
      break;
  }
  o.w("\n");
}

void gen_lighting(Code& o, uint32_t numColorChans, const LightUid& u, uint32_t components) {
  for (uint32_t j = 0; j < numColorChans; ++j) {
    bool colormatsource = (u.matsource >> j) & 1;
    o.w("{\n");
    if (colormatsource) {
      if (components & (VB_HAS_COL0 << j)) o.w("mat = round(color%d * 255.0);\n", j);
      else if (components & VB_HAS_COL0) o.w("mat = round(color0 * 255.0);\n");
      else o.w("mat = float4(255.0,255.0,255.0,255.0);\n");
    } else {
      o.w("mat = materials[%d];\n", j + 2);
    }
    if (u.enablelighting & (1 << j)) {
      if (u.ambsource & (1 << j)) {
        if (components & (VB_HAS_COL0 << j)) o.w("lacc = round(color%d * 255.0);\n", j);
        else if (components & VB_HAS_COL0) o.w("lacc = round(color0 * 255.0);\n");
        else o.w("lacc = float4(255.0,255.0,255.0,255.0);\n");
      } else {
        o.w("lacc = materials[%d];\n", j);
      }
    } else {
      o.w("lacc = float4(255.0,255.0,255.0,255.0);\n");
    }
    bool alphamatsource = (u.matsource >> (j + 2)) & 1;
    if (alphamatsource != colormatsource) {
      if (alphamatsource) {
        if (components & (VB_HAS_COL0 << j)) o.w("mat.w = round(color%d.w * 255.0);\n", j);
        else if (components & VB_HAS_COL0) o.w("mat.w = round(color0.w * 255.0);\n");
        else o.w("mat.w = 255.0;\n");
      } else {
        o.w("mat.w = materials[%d].w;\n", j + 2);
      }
    }
    if (u.enablelighting & (1 << (j + 2))) {
      if (u.ambsource & (1 << (j + 2))) {
        if (components & (VB_HAS_COL0 << j)) o.w("lacc.w = round(color%d.w * 255.0);\n", j);
        else if (components & VB_HAS_COL0) o.w("lacc.w = round(color0.w * 255.0);\n");
        else o.w("lacc.w = 255.0;\n");
      } else {
        o.w("lacc.w = materials[%d].w;\n", j);
      }
    } else {
      o.w("lacc.w = 255.0;\n");
    }
    if (u.enablelighting & (1 << j))
      for (int i = 0; i < 8; ++i) if (u.light_mask & (1 << (i + 8 * j))) gen_light(o, u, i, j, false);
    if (u.enablelighting & (1 << (j + 2)))
      for (int i = 0; i < 8; ++i) if (u.light_mask & (1 << (i + 8 * (j + 2)))) gen_light(o, u, i, j + 2, true);
    o.w("ilacc = int4(lacc);\nilacc = clamp(ilacc, 0, 255);\nilacc += ilacc >> 7;\n");
    o.w("o.colors_%d = float4((int4(mat) * ilacc) >> 8) / 255.0;\n", j);
    o.w("}\n");
  }
}

}  // namespace

// ---------------------------------------------------------------- uids
uint64_t VSUid::hash() const { return hash_bytes(this, sizeof *this); }
bool VSUid::operator==(const VSUid& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
uint64_t PSUid::hash() const { return hash_bytes(this, sizeof *this); }
bool PSUid::operator==(const PSUid& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }

VSUid make_vs_uid(const DrawCall& dc) {
  VSUid u{};
  u.components = dc.components & (VB_HAS_COL0 | VB_HAS_COL1 | VB_HAS_NRM0 | VB_HAS_NRM1 | VB_HAS_NRM2 | 0x7FFFFF);
  u.numTexGens = dc.xf_regs[0x3F] & 15;
  u.numColorChans = dc.xf_regs[0x09] & 3;
  // Only the registers that influence codegen.
  for (int i = 0x0E; i <= 0x11; ++i) u.xf_regs[i] = dc.xf_regs[i] & 0x7FFF;
  u.xf_regs[0x09] = dc.xf_regs[0x09] & 3;
  u.xf_regs[0x12] = dc.xf_regs[0x12] & 1;
  for (uint32_t i = 0; i < u.numTexGens; ++i) { u.xf_regs[0x40 + i] = dc.xf_regs[0x40 + i]; u.xf_regs[0x50 + i] = dc.xf_regs[0x50 + i]; }   // unused texgens: leftover state, not codegen
  u.xf_regs[0x3F] = u.numTexGens;
  return u;
}

PSUid make_ps_uid(const DrawCall& dc) {
  PSUid u{};
  const BPMemory& b = dc.bp;
  // Copy only codegen-relevant registers so equal pipelines share shaders.
  // genmode: texgens, colour channels, TEV and indirect stage counts (cull mode and zfreeze do not reach the shader).
  u.bp.reg[0] = b.reg[0] & 0x7FFFF & ~0xC000u;
  // Only the stages the shader emits: registers of unused stages carry leftover state that
  // would otherwise multiply pipelines (32k pipelines in one session) and keep compiling mid-match.
  const int stages = (int)b.numtevstages() + 1;
  const int indstages = (int)b.numindstages();
  for (int i = 0; i < stages; ++i) { u.bp.reg[BP_TEV_COLOR_ENV + 2 * i] = b.reg[BP_TEV_COLOR_ENV + 2 * i] & 0xFFFFFF; u.bp.reg[BP_TEV_ALPHA_ENV + 2 * i] = b.reg[BP_TEV_ALPHA_ENV + 2 * i] & 0xFFFFFF; u.bp.reg[BP_IND_CMD + i] = indstages ? b.reg[BP_IND_CMD + i] & 0x1FFFFF : 0; }
  for (int i = 0; i < 8; ++i) {
    // tref i covers stages 2i (bits 0-9) and 2i+1 (bits 12-21); ksel i holds konst selects for the
    // same two stages (bits 4-13 and 14-23) plus swap-table entries in bits 0-3 that any stage may use.
    uint32_t tref_mask = (2 * i + 1 < stages) ? 0xFFFFFF : (2 * i < stages) ? 0xFFF : 0;
    uint32_t ksel_mask = (2 * i + 1 < stages) ? 0xFFFFFF : (2 * i < stages) ? 0x3FFF : 0xF;
    u.bp.reg[BP_TREF + i] = b.reg[BP_TREF + i] & tref_mask;
    u.bp.reg[BP_TEV_KSEL + i] = b.reg[BP_TEV_KSEL + i] & ksel_mask;
  }
  u.bp.reg[BP_IREF] = b.reg[BP_IREF];
  u.bp.reg[BP_ALPHACOMPARE] = b.reg[BP_ALPHACOMPARE] & 0xFF0000;   // comparison ops/logic only (refs are constants)
  u.bp.reg[BP_ZTEX2] = b.reg[BP_ZTEX2] & 0xF;
  u.bp.reg[BP_FOGPARAM3] = b.reg[BP_FOGPARAM3] & 0xF00000;         // proj + fsel
  u.bp.reg[BP_FOGRANGE] = b.reg[BP_FOGRANGE] & 0x400;              // range enabled
  u.bp.reg[BP_ZMODE] = b.reg[BP_ZMODE] & 0x1F;
  u.bp.reg[BP_ZCOMPARE] = b.reg[BP_ZCOMPARE] & 0x7F;
  u.numTexGens = dc.xf_regs[0x3F] & 15;
  return u;
}

// ---------------------------------------------------------------- vertex shader
std::string generate_vertex_shader(const VSUid& uid) {
  Code o;
  const uint32_t components = uid.components;
  const uint32_t numTexGens = uid.numTexGens;
  const uint32_t numColorChans = uid.numColorChans;
  bool lightingEnabled = numColorChans > 0;
  o.w("cbuffer VSBlock : register(b0) {\n"
      "float4 projection[4];\nfloat4 depthparams;\nfloat4 viewparams;\nfloat4 materials[4];\nfloat4 lights[40];\n"
      "float4 texmatrices[24];\nfloat4 transformmatrices[64];\nfloat4 normalmatrices[32];\nfloat4 posttransformmatrices[64];\n"
      "float4 unjittered_projection[4];\nfloat4 prev_projection[4];\nfloat4 prev_transformmatrices[64];\n};\n");
  o.w("struct VS_OUTPUT {\nfloat4 pos : SV_Position;\nfloat4 colors_0 : COLOR0;\nfloat4 colors_1 : COLOR1;\n");
  for (uint32_t i = 0; i < numTexGens; ++i) o.w("float3 tex%d : TEXCOORD%d;\n", i, i);
  o.w("float4 clipPos : TEXCOORD%d;\n", numTexGens);
  if (uid.motion_vectors) o.w("float4 curPos : TEXCOORD%d;\nfloat4 prevPos : TEXCOORD%d;\n", numTexGens + 1, numTexGens + 2);
  o.w("};\n");
  o.w("VS_OUTPUT main(float3 rawpos : POSITION, float3 rawnorm0 : NORMAL0, float4 color0 : COLOR0, float4 color1 : COLOR1,\n");
  for (int i = 0; i < 8; ++i) o.w("  float2 rawtex%d : TEXCOORD%d,\n", i, i);
  o.w("  uint4 blend_indices : BLENDINDICES, uint4 blend_indices2 : BLENDINDICES1) {\n");
  o.w("VS_OUTPUT o;\n");
  o.w("int posmtx = int(blend_indices.x);\n");
  o.w("float4 rawpos4 = float4(rawpos, 1.0);\n");
  o.w("float4 pos = float4(dot(transformmatrices[posmtx], rawpos4), dot(transformmatrices[posmtx+1], rawpos4), dot(transformmatrices[posmtx+2], rawpos4), 1);\n");
  if (components & VB_HAS_NRM0) {
    o.w("int normidx = posmtx >= 32 ? (posmtx-32) : posmtx;\n");
    o.w("float3 N0 = normalmatrices[normidx].xyz, N1 = normalmatrices[normidx+1].xyz, N2 = normalmatrices[normidx+2].xyz;\n");
    o.w("float3 _norm0 = normalize(float3(dot(N0, rawnorm0), dot(N1, rawnorm0), dot(N2, rawnorm0)));\n");
  } else {
    o.w("float3 _norm0 = float3(0.0, 0.0, 0.0);\n");
  }
  o.w("o.pos = float4(dot(projection[0], pos), dot(projection[1], pos), dot(projection[2], pos), dot(projection[3], pos));\n");
  if (uid.motion_vectors) {
    o.w("o.curPos = float4(dot(unjittered_projection[0], pos), dot(unjittered_projection[1], pos), dot(unjittered_projection[2], pos), dot(unjittered_projection[3], pos));\n");
    o.w("float4 ppos = float4(dot(prev_transformmatrices[posmtx], rawpos4), dot(prev_transformmatrices[posmtx+1], rawpos4), dot(prev_transformmatrices[posmtx+2], rawpos4), 1);\n");
    o.w("o.prevPos = float4(dot(prev_projection[0], ppos), dot(prev_projection[1], ppos), dot(prev_projection[2], ppos), dot(prev_projection[3], ppos));\n");
  }
  if (lightingEnabled) o.w("float4 mat, lacc;\nfloat3 ldir, h;\nfloat dist, dist2, attn;\nint4 ilacc;\n");
  if (!lightingEnabled) {
    if (components & VB_HAS_COL0) o.w("o.colors_0 = color0;\n"); else o.w("o.colors_0 = float4(1.0, 1.0, 1.0, 1.0);\n");
    if (components & VB_HAS_COL1) o.w("o.colors_1 = color1;\n"); else o.w("o.colors_1 = o.colors_0;\n");
  } else {
    LightUid lu = lighting_uid(uid.xf_regs, numColorChans);
    gen_lighting(o, numColorChans, lu, components);
    if (numColorChans < 2) {
      if (components & VB_HAS_COL1) o.w("o.colors_1 = color1;\n"); else o.w("o.colors_1 = o.colors_0;\n");
    }
  }
  o.w("float4 coord = float4(0.0, 0.0, 1.0, 1.0);\n");
  bool dualTex = uid.xf_regs[0x12] & 1;
  for (uint32_t i = 0; i < numTexGens; ++i) {
    uint32_t info = uid.xf_regs[0x40 + i];
    uint32_t sourcerow = tmi_sourcerow(info), texgentype = tmi_texgentype(info), inputform = tmi_inputform(info), projection = tmi_projection(info);
    o.w("{\ncoord = float4(0.0, 0.0, 1.0, 1.0);\n");
    switch (sourcerow) {
      case 0: o.w("coord.xyz = rawpos.xyz;\n"); break;
      case 1: if (components & VB_HAS_NRM0) o.w("coord.xyz = rawnorm0.xyz;\n"); break;
      case 2: case 3: case 4: break;
      default:
        if (sourcerow >= 5 && sourcerow <= 12 && (components & (VB_HAS_UV0 << (sourcerow - 5)))) o.w("coord.xy = rawtex%d.xy;\n", sourcerow - 5);
        break;
    }
    if (inputform == 0) o.w("coord.z = 1.0;\n");
    switch (texgentype) {
      case 1: {  // emboss
        uint32_t lightshift = tmi_embosslightshift(info), srcshift = tmi_embosssourceshift(info);
        // Without binormals we fall back like Dolphin does.
        o.w("o.tex%d.xyz = float3(coord.xy, 1.0);\n", i);
        (void)lightshift; (void)srcshift;
        break;
      }
      case 2: o.w("o.tex%d.xyz = float3(o.colors_0.x, o.colors_0.y, 1);\n", i); break;
      case 3: o.w("o.tex%d.xyz = float3(o.colors_1.x, o.colors_1.y, 1);\n", i); break;
      default: {
        if (components & (VB_HAS_TEXMTXIDX0 << i)) {
          o.w("int tmp = int(%s);\n", i < 4 ? (i == 0 ? "blend_indices.y" : i == 1 ? "blend_indices.z" : i == 2 ? "blend_indices.w" : "blend_indices2.x")
                                          : (i == 4 ? "blend_indices2.y" : i == 5 ? "blend_indices2.z" : i == 6 ? "blend_indices2.w" : "blend_indices2.w"));
          if (projection == 1) o.w("o.tex%d.xyz = float3(dot(coord, transformmatrices[tmp]), dot(coord, transformmatrices[tmp+1]), dot(coord, transformmatrices[tmp+2]));\n", i);
          else o.w("o.tex%d.xyz = float3(dot(coord, transformmatrices[tmp]), dot(coord, transformmatrices[tmp+1]), 1);\n", i);
        } else {
          if (projection == 1) o.w("o.tex%d.xyz = float3(dot(coord, texmatrices[%d]), dot(coord, texmatrices[%d]), dot(coord, texmatrices[%d]));\n", i, 3 * i, 3 * i + 1, 3 * i + 2);
          else o.w("o.tex%d.xyz = float3(dot(coord, texmatrices[%d]), dot(coord, texmatrices[%d]), 1);\n", i, 3 * i, 3 * i + 1);
        }
        break;
      }
    }
    if (texgentype == 0) {
      if (projection == 1) o.w("if(o.tex%d.z == 0.0f)\n\to.tex%d.xy = clamp(o.tex%d.xy, float2(-2.0f, -2.0f), float2(2.0f, 2.0f));\n", i, i, i);
      if (dualTex) {
        uint32_t post = uid.xf_regs[0x50 + i];
        int postidx = post & 0x3F;
        bool normalize = (post >> 8) & 1;
        o.w("float4 P0 = posttransformmatrices[%d];\nfloat4 P1 = posttransformmatrices[%d];\nfloat4 P2 = posttransformmatrices[%d];\n",
            postidx & 0x3f, (postidx + 1) & 0x3f, (postidx + 2) & 0x3f);
        if (normalize) o.w("o.tex%d.xyz = normalize(o.tex%d.xyz);\n", i, i);
        o.w("o.tex%d.xyz = float3(dot(P0.xyz, o.tex%d.xyz) + P0.w, dot(P1.xyz, o.tex%d.xyz) + P1.w, dot(P2.xyz, o.tex%d.xyz) + P2.w);\n", i, i, i, i);
      }
    }
    o.w("}\n");
  }
  o.w("o.clipPos = float4(pos.x,pos.y,o.pos.z,o.pos.w);\n");
  o.w("o.pos.z = o.pos.w * depthparams.x - o.pos.z * depthparams.y;\n");
  o.w("o.pos.xy *= sign(depthparams.zw * float2(-1.0, 1.0));\n");
  o.w("o.pos.xy = o.pos.xy + o.pos.w * depthparams.zw;\n");
  o.w("if (o.pos.w == 1.0)\n{\n\to.pos.xy = round(o.pos.xy * viewparams.xy) * viewparams.zw;\n}\n");
  o.w("return o;\n}\n");
  return o.s;
}

// ---------------------------------------------------------------- pixel shader
namespace {

const char* kselC[] = {
  "255,255,255", "223,223,223", "191,191,191", "159,159,159", "128,128,128", "96,96,96", "64,64,64", "32,32,32",
  "0,0,0", "0,0,0", "0,0,0", "0,0,0",
  "kcolors[0].rgb", "kcolors[1].rgb", "kcolors[2].rgb", "kcolors[3].rgb",
  "kcolors[0].rrr", "kcolors[1].rrr", "kcolors[2].rrr", "kcolors[3].rrr",
  "kcolors[0].ggg", "kcolors[1].ggg", "kcolors[2].ggg", "kcolors[3].ggg",
  "kcolors[0].bbb", "kcolors[1].bbb", "kcolors[2].bbb", "kcolors[3].bbb",
  "kcolors[0].aaa", "kcolors[1].aaa", "kcolors[2].aaa", "kcolors[3].aaa",
};
const char* kselA[] = {
  "255", "223", "191", "159", "128", "96", "64", "32", "0", "0", "0", "0", "0", "0", "0", "0",
  "kcolors[0].r", "kcolors[1].r", "kcolors[2].r", "kcolors[3].r",
  "kcolors[0].g", "kcolors[1].g", "kcolors[2].g", "kcolors[3].g",
  "kcolors[0].b", "kcolors[1].b", "kcolors[2].b", "kcolors[3].b",
  "kcolors[0].a", "kcolors[1].a", "kcolors[2].a", "kcolors[3].a",
};
const char* cInput[] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                        "tex_t.rgb", "tex_t.aaa", "ras_t.rgb", "ras_t.aaa", "255,255,255", "128,128,128", "konst_t.rgb", "0,0,0"};
const char* aInput[] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex_t.a", "ras_t.a", "konst_t.a", "0"};
const int aInputSource[] = {1, 3, 5, 7, 9, 11, 14, 15};
const char* rasTable[] = {"int4(col0)", "int4(col1)", "int4(0,0,0,0)", "int4(0,0,0,0)", "int4(0,0,0,0)",
                          "int4(a_bump,a_bump,a_bump,a_bump)", "(int4(1,1,1,1)*(a_bump | (a_bump >> 5)))", "int4(0,0,0,0)"};
const char* cOut[] = {"prev.rgb", "c0.rgb", "c1.rgb", "c2.rgb"};
const int cOutSource[] = {0, 2, 4, 6};
const char* aOut[] = {"prev.a", "c0.a", "c1.a", "c2.a"};
const int aOutSource[] = {1, 3, 5, 7};

struct RegState {
  bool overflow[16];
  bool ras0 = false, ras1 = false;
  RegState() {
    static const bool init[16] = {true, true, true, true, true, true, true, true, false, false, true, true, false, false, true, false};
    std::memcpy(overflow, init, sizeof overflow);
  }
};

void write_tev_regular(Code& o, const char* comps, int bias, int op, int shift, int a, int b, int c, int d, bool alpha, int ZERO, int ONE) {
  const char* left[] = {"", " << 1", " << 2", ""};
  const char* right[] = {"", "", "", " >> 1"};
  const char* lerpBias[] = {"", " + 128", "", " + 127"};
  const char* biasTable[] = {"", " + 128", " - 128", ""};
  const char* opTable[] = {"+", "-"};
  int lb = 2 * op + ((shift == 3) == alpha);
  o.w("((((tin_d%s%s)%s)", comps, biasTable[bias], left[shift]);
  o.w(" %s ", opTable[op]);
  if (a == b || c == ZERO) o.w("((((tin_a%s << 8)%s)%s) >> 8)", comps, left[shift], lerpBias[lb]);
  else if (c == ONE) o.w("((((tin_b%s << 8)%s)%s) >> 8)", comps, left[shift], lerpBias[lb]);
  else if (a == ZERO) o.w("((((tin_b%s*tin_c%s)%s)%s) >> 8)", comps, comps, left[shift], lerpBias[lb]);
  else if (b == ZERO) o.w("((((tin_a%s*(256 - tin_c%s))%s)%s) >> 8)", comps, comps, left[shift], lerpBias[lb]);
  else o.w("(((((tin_a%s<<8) + (tin_b%s - tin_a%s) * tin_c%s)%s)%s) >> 8)", comps, comps, comps, comps, left[shift], lerpBias[lb]);
  o.w(")%s)", right[shift]);
}

void write_tev_compare(Code& o, int components, int cmp) {
  static const char* comps[] = {".rgb", ".a"};
  static const char* zero[] = {"int3(0,0,0)", "0"};
  static const char* table[] = {
    "((tin_a.r > tin_b.r) ? tin_c%s : %s)", "((tin_a.r == tin_b.r) ? tin_c%s : %s)",
    "((idot(tin_a.rgb, c16) >  idot(tin_b.rgb, c16)) ? tin_c%s : %s)", "((idot(tin_a.rgb, c16) == idot(tin_b.rgb, c16)) ? tin_c%s : %s)",
    "((idot(tin_a.rgb, c24) >  idot(tin_b.rgb, c24)) ? tin_c%s : %s)", "((idot(tin_a.rgb, c24) == idot(tin_b.rgb, c24)) ? tin_c%s : %s)",
    "(max(int3(sign(tin_a.rgb - tin_b.rgb - 0.5)), int3(0,0,0)) * tin_c.rgb)", "((int3(1,1,1) - sign(abs(tin_a.rgb - tin_b.rgb))) * tin_c.rgb)",
    "((tin_a.a  > tin_b.a) ? tin_c.a : 0)", "((tin_a.a == tin_b.a) ? tin_c.a : 0)",
  };
  o.w("tin_d%s+", comps[components]);
  if (cmp < 6) o.w(table[cmp], comps[components], zero[components]);
  else o.w("%s", table[cmp + (components << 1)]);
}

void sample_texture(Code& o, const char* coords, const char* swizzle, int texmap) {
  o.w("int4(round((Tex[%d].Sample(samp[%d], %s.xy * texdims[%d].xy)).%s * 255.0));\n", texmap, texmap, coords, texmap, swizzle);
}

}  // namespace

std::string generate_pixel_shader(const PSUid& uid) {
  Code o;
  const BPMemory& bp = uid.bp;
  uint32_t numStages = bp.numtevstages() + 1;
  uint32_t numTexgen = uid.numTexGens;
  uint32_t numindStages = bp.numindstages();
  uint32_t at = bp.alpha_test();
  uint32_t comp0 = bits(at, 16, 3), comp1 = bits(at, 19, 3), logic = bits(at, 22, 2);
  // Alpha test pretest (AlphaTest::TestResult)
  int pretest = 0;  // 0 undetermined, 1 fail, 2 pass
  {
    bool a7 = comp0 == 7, b7 = comp1 == 7, a0 = comp0 == 0, b0 = comp1 == 0;
    switch (logic) {
      case 0: if (a7 && b7) pretest = 2; else if (a0 || b0) pretest = 1; break;
      case 1: if (a7 || b7) pretest = 2; else if (a0 && b0) pretest = 1; break;
      case 2: if ((a7 && b0) || (a0 && b7)) pretest = 2; else if ((a7 && b7) || (a0 && b0)) pretest = 1; break;
      case 3: if ((a7 && b0) || (a0 && b7)) pretest = 1; else if ((a7 && b7) || (a0 && b0)) pretest = 2; break;
    }
  }
  uint32_t zmode = bp.zmode();
  bool ztest = zmode & 1;
  bool early_ztest = ((bp.zcontrol() >> 6) & 1) && ztest;
  uint32_t fog_fsel = bits(bp.fogparam3(), 21, 3), fog_proj = bits(bp.fogparam3(), 20, 1);
  bool fog_range = bits(bp.fogrange(0), 10, 1);
  bool forced_early_z = early_ztest && pretest == 2;

  o.w("#define wu int\n#define wu2 int2\n#define wu3 int3\n#define wu4 int4\n");
  o.w("int4 CHK_O_U8(int4 x) { return x & 255; }\n#define BOR(x, n) ((x) | (n))\n#define BSHR(x, n) ((x) >> (n))\n"
      "int2 BSH(int2 x, int n) { if(n >= 0) return x >> n; n = -n; return x << n; }\n"
      "int remainder(int x, int y) { return x & (y - 1); }\n"
      "int idot(int3 x, int3 y) { int3 tmp = x * y; return tmp.x + tmp.y + tmp.z; }\n"
      "int idot(int4 x, int4 y) { int4 tmp = x * y; return tmp.x + tmp.y + tmp.z + tmp.w; }\n"
      "wu wuround(float x) { return wu(round(x)); }\nwu2 wuround(float2 x) { return wu2(round(x)); }\n"
      "wu3 wuround(float3 x) { return wu3(round(x)); }\nwu4 wuround(float4 x) { return wu4(round(x)); }\n");
  o.w("SamplerState samp[8] : register(s0);\nTexture2D Tex[8] : register(t0);\n");
  o.w("cbuffer PSBlock : register(b1) {\nint4 colors[4];\nint4 kcolors[4];\nint4 alpharef;\nfloat4 texdims[8];\nint4 zbias[2];\n"
      "int4 indtexscale[2];\nint4 indtexmtx[6];\nint4 fogcolor;\nint4 fogi;\nfloat4 fogf[2];\nfloat4 zslope;\nint4 flags;\nfloat4 efbscale;\nfloat4 mvscale;\n};\n");
  if (forced_early_z) o.w("[earlydepthstencil]\n");
  if (uid.motion_vectors) o.w("void main(out float4 ocol0 : SV_Target0, out float2 omv : SV_Target1, in float4 rawpos : SV_Position, in float4 colors_0 : COLOR0, in float4 colors_1 : COLOR1");
  else o.w("void main(out float4 ocol0 : SV_Target0, in float4 rawpos : SV_Position, in float4 colors_0 : COLOR0, in float4 colors_1 : COLOR1");
  for (uint32_t i = 0; i < numTexgen; ++i) o.w(", in float3 uv%d : TEXCOORD%d", i, i);
  if (uid.motion_vectors) o.w(", in float4 clipPos : TEXCOORD%d, in float4 curPos : TEXCOORD%d, in float4 prevPos : TEXCOORD%d) {\n", numTexgen, numTexgen + 1, numTexgen + 2);
  else o.w(", in float4 clipPos : TEXCOORD%d) {\n", numTexgen);
  o.w("int2 ditherindex = int2(rawpos.xy) & 3;\n");
  o.w("wu4 c0 = colors[1], c1 = colors[2], c2 = colors[3], prev = colors[0];\n"
      "wu4 tex_ta[%d], tex_t = wu4(0,0,0,0), ras_t = wu4(0,0,0,0), konst_t = wu4(0,0,0,0);\n"
      "wu3 c16 = wu3(1,256,0), c24 = wu3(1,256,256*256);\nwu a_bump=0;\nwu3 tevcoord=wu3(0,0,0);\n"
      "wu2 wrappedcoord=wu2(0,0), t_coord=wu2(0,0),ittmpexp=wu2(0,0);\n"
      "wu4 tin_a = wu4(0,0,0,0), tin_b = wu4(0,0,0,0), tin_c = wu4(0,0,0,0), tin_d = wu4(0,0,0,0);\n\n", numStages);
  if (numTexgen == 0) o.w("float3 uv0 = float3(0.0,0.0,0.0);\n");
  else for (uint32_t i = 0; i < numTexgen; ++i) {
    o.w("uv%d.xy = uv%d.xy / ((uv%d.z == 0.0) ? 2.0 : uv%d.z);\n", i, i, i, i);
    o.w("uv%d.xy = trunc(uv%d.xy * texdims[%d].zw);\n", i, i, i);
  }
  // indirect stage lookups
  uint32_t indUsed = 0;
  for (uint32_t i = 0; i < numStages; ++i) {
    uint32_t ind = bp.tevind(i);
    bool active = bits(ind, 7, 2) != 0 || bits(ind, 9, 4) != 0;
    uint32_t bt = bits(ind, 0, 2);
    if (active && bt < numindStages) indUsed |= 1 << bt;
  }
  for (uint32_t i = 0; i < numindStages; ++i) {
    if (!(indUsed & (1 << i))) continue;
    uint32_t texcoord = bits(bp.iref(), 6 * i + 3, 3), texmap = bits(bp.iref(), 6 * i, 3);
    if (texcoord < numTexgen) o.w("t_coord = BSHR(wu2(uv%d.xy) , indtexscale[%d].%s);\n", texcoord, i / 2, (i & 1) ? "zw" : "xy");
    else o.w("t_coord = wu2(0,0);\n");
    o.w("wu3 indtex%d = ", i);
    o.w("int3(round((Tex[%d].Sample(samp[%d], float2(t_coord) * texdims[%d].xy)).abg * 255.0));\n", texmap, texmap, texmap);
  }
  // fetch stage textures
  for (uint32_t n = 0; n < numStages; ++n) {
    uint32_t texcoord = bp.order_texcoord(n);
    bool hasTexCoord = texcoord < numTexgen;
    if (!hasTexCoord) texcoord = 0;
    uint32_t ind = bp.tevind(n);
    bool hasInd = bits(ind, 0, 2) < numindStages && (bits(ind, 7, 2) != 0 || bits(ind, 9, 4) != 0);
    uint32_t cc = bp.tev_color(n), ac = bp.tev_alpha(n);
    auto ccUses = [&](uint32_t v) { return bits(cc, 12, 4) == v || bits(cc, 8, 4) == v || bits(cc, 4, 4) == v || bits(cc, 0, 4) == v; };
    auto acUses = [&](uint32_t v) { return bits(ac, 13, 3) == v || bits(ac, 10, 3) == v || bits(ac, 7, 3) == v || bits(ac, 4, 3) == v; };
    bool texEnable = bp.order_enable(n) && (ccUses(8) || ccUses(9) || acUses(4));
    o.w("\n{\n");
    if (hasInd) {
      uint32_t bt = bits(ind, 0, 2), fmt = bits(ind, 2, 2), bias = bits(ind, 4, 3), bs = bits(ind, 7, 2), mid = bits(ind, 9, 4);
      uint32_t sw = bits(ind, 13, 3), tw = bits(ind, 16, 3), addprev = bits(ind, 20, 1);
      if (bs != 0) {
        static const char* sel[] = {"", "x", "y", "z"};
        static const char* mask[] = {"248", "224", "240", "248"};
        o.w("a_bump = indtex%d.%s & %s;\n", bt, sel[bs], mask[fmt]);
      }
      if (mid != 0) {
        static const char* fmtMask[] = {"255", "31", "15", "7"};
        o.w("wu3 indtevcrd%d = indtex%d & %s;\n", n, bt, fmtMask[fmt]);
        static const char* biasField[] = {"", "x", "y", "xy", "z", "xz", "yz", "xyz"};
        static const char* biasAdd[] = {"wu(-128)", "wu(1)", "wu(1)", "wu(1)"};
        if (bias == 1 || bias == 2 || bias == 4) o.w("indtevcrd%d.%s += %s;\n", n, biasField[bias], biasAdd[fmt]);
        else if (bias == 3 || bias == 5 || bias == 6) o.w("indtevcrd%d.%s += wu2(%s, %s);\n", n, biasField[bias], biasAdd[fmt], biasAdd[fmt]);
        else if (bias == 7) o.w("indtevcrd%d.%s += wu3(%s, %s, %s);\n", n, biasField[bias], biasAdd[fmt], biasAdd[fmt], biasAdd[fmt]);
        if (mid <= 3) {
          int m = 2 * (mid - 1);
          o.w("wu2 indtevtrans%d = wu2(idot(indtexmtx[%d].xyz, indtevcrd%d), idot(indtexmtx[%d].xyz, indtevcrd%d));\n", n, m, n, m + 1, n);
          o.w("indtevtrans%d = BSHR(indtevtrans%d, wu(3));\nindtevtrans%d = BSH(indtevtrans%d, indtexmtx[%d].w);\n", n, n, n, n, m);
        } else if (mid <= 7 && hasTexCoord) {
          int m = 2 * (mid - 5);
          o.w("wu2 indtevtrans%d = wu2(uv%d.xy * indtevcrd%d.xx);\n", n, texcoord, n);
          o.w("indtevtrans%d = BSHR(indtevtrans%d, wu(8));\nindtevtrans%d = BSH(indtevtrans%d, indtexmtx[%d].w);\n", n, n, n, n, m);
        } else if (mid <= 11 && hasTexCoord) {
          int m = 2 * (mid - 9);
          o.w("wu2 indtevtrans%d = wu2(uv%d.xy * indtevcrd%d.yy);\n", n, texcoord, n);
          o.w("indtevtrans%d = BSHR(indtevtrans%d, wu(8));\nindtevtrans%d = BSH(indtevtrans%d, indtexmtx[%d].w);\n", n, n, n, n, m);
        } else {
          o.w("wu2 indtevtrans%d = wu2(0,0);\n", n);
        }
      } else {
        o.w("wu2 indtevtrans%d = wu2(0,0);\n", n);
      }
      static const char* wrapStart[] = {"wu(0)", "wu(256*128)", "wu(128*128)", "wu(64*128)", "wu(32*128)", "wu(16*128)", "wu(1)", "wu(1)"};
      if (sw == 0) o.w("wrappedcoord.x = wu(uv%d.x);\n", texcoord);
      else if (sw == 6) o.w("wrappedcoord.x = wu(0);\n");
      else o.w("wrappedcoord.x = remainder(wu(uv%d.x), %s);\n", texcoord, wrapStart[sw]);
      if (tw == 0) o.w("wrappedcoord.y = wu(uv%d.y);\n", texcoord);
      else if (tw == 6) o.w("wrappedcoord.y = wu(0);\n");
      else o.w("wrappedcoord.y = remainder(wu(uv%d.y), %s);\n", texcoord, wrapStart[tw]);
      if (addprev) o.w("tevcoord.xy += wrappedcoord + indtevtrans%d;\n", n);
      else o.w("tevcoord.xy = wrappedcoord + indtevtrans%d;\n", n);
      o.w("tevcoord.xy = (tevcoord.xy << 8) >> 8;\n");
    }
    if (texEnable) {
      if (!hasInd) {
        if (hasTexCoord) o.w("tevcoord.xy = wu2(uv%d.xy);\n", texcoord);
        else o.w("tevcoord.xy = wu2(0,0);\n");
      }
      int texmap = bp.order_texmap(n);
      o.w("float2 stagecoord = float2(tevcoord.xy);\n");
      o.w("tex_ta[%d] = ", n);
      sample_texture(o, "stagecoord", "rgba", texmap);
    } else {
      o.w("tex_ta[%d] = wu4(255,255,255,255);\n", n);
    }
    o.w("\n}\n");
  }
  o.w("float4 col0 = colors_0;\nfloat4 col1 = colors_1;\n");
  o.w("clipPos = float4(rawpos.x, rawpos.y, clipPos.z, clipPos.w);\n");

  RegState rs;
  for (uint32_t n = 0; n < numStages; ++n) {
    uint32_t cc = bp.tev_color(n), ac = bp.tev_alpha(n);
    uint32_t cd = bits(cc, 0, 4), ccc = bits(cc, 4, 4), cb = bits(cc, 8, 4), ca = bits(cc, 12, 4);
    uint32_t cbias = bits(cc, 16, 2), cop = bits(cc, 18, 1), cclamp = bits(cc, 19, 1), cshift = bits(cc, 20, 2), cdest = bits(cc, 22, 2);
    uint32_t rswap = bits(ac, 0, 2), tswap = bits(ac, 2, 2);
    uint32_t ad = bits(ac, 4, 3), acc = bits(ac, 7, 3), ab = bits(ac, 10, 3), aa = bits(ac, 13, 3);
    uint32_t abias = bits(ac, 16, 2), aop = bits(ac, 18, 1), aclamp = bits(ac, 19, 1), ashift = bits(ac, 20, 2), adest = bits(ac, 22, 2);
    auto ccUses = [&](uint32_t v) { return ca == v || cb == v || ccc == v || cd == v; };
    auto acUses = [&](uint32_t v) { return aa == v || ab == v || acc == v || ad == v; };
    o.w("// TEV stage %d\n", n);
    if (ccUses(11) || ccUses(10) || acUses(5)) {
      char rasswap[5] = {"rgba"[bp.ksel_swap1(rswap * 2)], "rgba"[bp.ksel_swap2(rswap * 2)], "rgba"[bp.ksel_swap1(rswap * 2 + 1)], "rgba"[bp.ksel_swap2(rswap * 2 + 1)], 0};
      int rasindex = bp.order_colorchan(n);
      if (rasindex == 0 && !rs.ras0) { o.w("col0 = round(col0 * 255.0);\n"); rs.ras0 = true; }
      if (rasindex == 1 && !rs.ras1) { o.w("col1 = round(col1 * 255.0);\n"); rs.ras1 = true; }
      o.w("ras_t = %s.%s;\n", rasTable[rasindex], rasswap);
      rs.overflow[10] = rs.overflow[11] = rasindex < 2;
    }
    char texswap[5] = {"rgba"[bp.ksel_swap1(tswap * 2)], "rgba"[bp.ksel_swap2(tswap * 2)], "rgba"[bp.ksel_swap1(tswap * 2 + 1)], "rgba"[bp.ksel_swap2(tswap * 2 + 1)], 0};
    o.w("tex_t = tex_ta[%d].%s;\n", n, texswap);
    if (ccUses(14) || acUses(6)) {
      int kc = bp.ksel_kc(n), ka = bp.ksel_ka(n);
      o.w("konst_t = wu4(%s,%s);\n", kselC[kc], kselA[ka]);
      rs.overflow[14] = kc > 11 || ka > 15;
    }
    auto chk = [&](uint32_t c, uint32_t a) { return (rs.overflow[c] || rs.overflow[aInputSource[a]]) ? "CHK_O_U8" : ""; };
    o.w("tin_a = %s(wu4(%s,%s));\n", chk(ca, aa), cInput[ca], aInput[aa]);
    o.w("tin_b = %s(wu4(%s,%s));\n", chk(cb, ab), cInput[cb], aInput[ab]);
    o.w("tin_c = %s(wu4(%s,%s));\n", chk(ccc, acc), cInput[ccc], aInput[acc]);
    bool nrgb = ccc != 15 && cbias != 3, na = acc != 7 && abias != 3;
    if (nrgb || na) {
      const char* sw = (nrgb && na) ? "" : (nrgb ? ".rgb" : ".a");
      o.w("tin_c%s = tin_c%s + BSHR(tin_c%s, 7);\n", sw, sw, sw);
    }
    o.w("tin_d = wu4(%s,%s);\n", cInput[cd], aInput[ad]);
    rs.overflow[cOutSource[cdest]] = !cclamp;
    rs.overflow[aOutSource[adest]] = !aclamp;
    o.w("%s = clamp(", cOut[cdest]);
    if (cbias != 3) write_tev_regular(o, ".rgb", cbias, cop, cshift, ca, cb, ccc, cd, false, 15, 12);
    else write_tev_compare(o, 0, (cshift << 1) | cop);
    o.w(cclamp ? ",wu(0),wu(255));\n" : ",wu(-1024),wu(1023));\n");
    o.w("%s = clamp(", aOut[adest]);
    if (abias != 3) write_tev_regular(o, ".a", abias, aop, ashift, aa, ab, acc, ad, true, 7, 8);
    else write_tev_compare(o, 1, (ashift << 1) | aop);
    o.w(aclamp ? ",wu(0),wu(255));\n\n" : ",wu(-1024),wu(1023));\n\n");
  }
  uint32_t lastC = bits(bp.tev_color(numStages - 1), 22, 2), lastA = bits(bp.tev_alpha(numStages - 1), 22, 2);
  if (lastC != 0) o.w("prev.rgb = %s;\n", cOut[lastC]);
  if (lastA != 0) o.w("prev.a = %s;\n", aOut[lastA]);
  if (rs.overflow[cOutSource[lastC]] || rs.overflow[aOutSource[lastA]]) o.w("prev = CHK_O_U8(prev);\n");

  if (pretest != 2) {
    static const char* funcs[] = {"(false)", "(prev.a <  %s)", "(prev.a == %s)", "(prev.a <= %s)", "(prev.a >  %s)", "(prev.a != %s)", "(prev.a >= %s)", "(true)"};
    static const char* logics[] = {" && ", " || ", " != ", " == "};
    o.w("if(!( ");
    o.w(funcs[comp0], "alpharef.r");
    o.w("%s", logics[logic]);
    o.w(funcs[comp1], "alpharef.g");
    o.w(")) {\nocol0 = float4(0.0,0.0,0.0,0.0);\ndiscard;\n}\n");
  }
  // depth value for fog (fast depth: hardware z reversed)
  o.w("wu zCoord = wuround((1.0 - rawpos.z) * 16777216.0);\nzCoord = clamp(zCoord, 0, 0xFFFFFF);\n");
  if (fog_fsel != 0) {
    if (fog_proj == 0) o.w("float ze = (fogf[1].x * 16777216.0) / float(fogi.y - (zCoord >> fogi.w));\n");
    else o.w("float ze = fogf[1].x * (float(zCoord) / 16777216.0);\n");
    if (fog_range) {
      o.w("float x_adjust = (2.0 * (clipPos.x / fogf[0].y)) - 1.0 - fogf[0].x;\n");
      o.w("x_adjust = sqrt(x_adjust * x_adjust + fogf[0].z * fogf[0].z) / fogf[0].z;\nze *= x_adjust;\n");
    }
    o.w("float fog = clamp(ze - fogf[1].z, 0.0, 1.0);\n");
    static const char* fogFuncs[] = {"", "", "", "", "fog = 1.0 - exp2(-8.0 * fog);\n", "fog = 1.0 - exp2(-8.0 * fog * fog);\n",
                                     "fog = exp2(-8.0 * (1.0 - fog));\n", "fog = 1.0 - fog;\n   fog = exp2(-8.0 * fog * fog);\n"};
    if (fog_fsel > 3) o.w("%s", fogFuncs[fog_fsel]);
    o.w("wu ifog = wu(round(fog * 256.0));\n");
    o.w("prev.rgb = BSHR(prev.rgb * (wu(256) - ifog) + fogcolor.rgb * ifog, wu(8));\n");
  }
  o.w("ocol0 = float4(prev) / 255.0;\n");
  if (uid.motion_vectors) o.w("omv = (prevPos.xy / prevPos.w - curPos.xy / curPos.w) * mvscale.xy;\n");
  o.w("}\n");
  return o.s;
}

// ---------------------------------------------------------------- constants
void build_projection(const DrawCall& dc, float m[16]) {
  const float* vp = (const float*)&dc.xf_regs[0x1A];
  const float* proj = (const float*)&dc.xf_regs[0x20];
  uint32_t type = dc.xf_regs[0x26];
  std::memset(m, 0, 16 * sizeof(float));
  if (type == 0) {  // perspective
    m[0] = proj[0]; m[2] = proj[1]; m[5] = proj[2]; m[6] = proj[3]; m[10] = proj[4]; m[11] = proj[5]; m[14] = -1.0f;
  } else {
    m[0] = proj[0]; m[3] = proj[1]; m[5] = proj[2]; m[7] = proj[3]; m[10] = proj[4]; m[11] = proj[5]; m[15] = 1.0f;
  }
  if (vp[0] < 0.0f) for (int i = 0; i < 4; ++i) m[i] *= -1.0f;
  if (vp[1] > 0.0f) for (int i = 4; i < 8; ++i) m[i] *= -1.0f;
}

void fill_vs_constants(const DrawCall& dc, VSConstants& c, int efb_scale, const DrawMatrices* override_matrices, const MotionInfo* motion) {
  std::memset(&c, 0, sizeof c);
  const float* pos_matrices = override_matrices ? override_matrices->pos : dc.posMatrices;
  const float* nrm_matrices = override_matrices ? override_matrices->nrm : dc.normalMatrices;
  const float* vp = (const float*)&dc.xf_regs[0x1A];
  float m[16];
  build_projection(dc, m);
  std::memcpy(c.unjittered_projection, m, sizeof m);
  const float pixel_center_correction = 0.5f - 7.0f / 12.0f;
  float viewport_width = 2.0f * vp[0] * efb_scale, viewport_height = 2.0f * vp[1] * efb_scale;
  if (motion) {
    // Sub-pixel jitter: shift clip space by the jitter in NDC (row 3 is the w row), so the
    // rasterized samples move while the reported matrices stay unjittered.
    float jx = viewport_width != 0.0f ? 2.0f * motion->jitter_x / viewport_width : 0.0f;
    float jy = viewport_height != 0.0f ? 2.0f * motion->jitter_y / viewport_height : 0.0f;
    for (int i = 0; i < 4; ++i) { m[i] += jx * m[12 + i]; m[4 + i] += jy * m[12 + i]; }
    std::memcpy(c.prev_projection, motion->prev_proj ? motion->prev_proj : &c.unjittered_projection[0][0], sizeof c.prev_projection);
    std::memcpy(c.prev_transformmatrices, motion->prev_pos ? motion->prev_pos : pos_matrices, sizeof c.prev_transformmatrices);
  }
  std::memcpy(c.projection, m, sizeof m);
  float psx = 2.0f / viewport_width, psy = 2.0f / viewport_height;
  c.depthparams[0] = 0.0f; c.depthparams[1] = 1.0f;
  c.depthparams[2] = pixel_center_correction * psx; c.depthparams[3] = pixel_center_correction * psy;
  c.viewparams[0] = viewport_width * 0.5f; c.viewparams[1] = viewport_height * 0.5f; c.viewparams[2] = psx; c.viewparams[3] = psy;
  for (int i = 0; i < 2; ++i) {
    uint32_t amb = dc.xf_regs[0x0A + i], mat = dc.xf_regs[0x0C + i];
    c.materials[i][0] = (float)((amb >> 24) & 0xFF); c.materials[i][1] = (float)((amb >> 16) & 0xFF); c.materials[i][2] = (float)((amb >> 8) & 0xFF); c.materials[i][3] = (float)(amb & 0xFF);
    c.materials[i + 2][0] = (float)((mat >> 24) & 0xFF); c.materials[i + 2][1] = (float)((mat >> 16) & 0xFF); c.materials[i + 2][2] = (float)((mat >> 8) & 0xFF); c.materials[i + 2][3] = (float)(mat & 0xFF);
  }
  for (int i = 0; i < 8; ++i) {
    const uint8_t* L = dc.lights[i];
    // Light struct: useless[3] (12), color[4] (16), cosatt[3] (28), distatt[3] (40), dpos[3] (52), ddir[3] (64)
    uint32_t colorword; std::memcpy(&colorword, L + 12, 4);   // stored as big-endian u32 -> host u32 already swapped by xf_load
    // xf_load stored the word in host order; color bytes as Dolphin: color[3]=r? Dolphin: u8 color[4] packed abgr -> use (color>>24)&FF as R? Dolphin uses color[3],color[2],color[1],color[0] = r,g,b,a with little-endian byte order of the u32.
    c.lights[5 * i][0] = (float)((colorword >> 24) & 0xFF);
    c.lights[5 * i][1] = (float)((colorword >> 16) & 0xFF);
    c.lights[5 * i][2] = (float)((colorword >> 8) & 0xFF);
    c.lights[5 * i][3] = (float)(colorword & 0xFF);
    float f[9]; std::memcpy(f, L + 16, 36);
    c.lights[5 * i + 1][0] = f[0]; c.lights[5 * i + 1][1] = f[1]; c.lights[5 * i + 1][2] = f[2];
    if (std::fabs(f[3]) < 0.00001f && std::fabs(f[4]) < 0.00001f && std::fabs(f[5]) < 0.00001f) { c.lights[5 * i + 2][0] = 0.00001f; c.lights[5 * i + 2][1] = f[4]; c.lights[5 * i + 2][2] = f[5]; }
    else { c.lights[5 * i + 2][0] = f[3]; c.lights[5 * i + 2][1] = f[4]; c.lights[5 * i + 2][2] = f[5]; }
    c.lights[5 * i + 3][0] = f[6]; c.lights[5 * i + 3][1] = f[7]; c.lights[5 * i + 3][2] = f[8];
    float d[3]; std::memcpy(d, L + 52, 12);
    double norm = (double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2];
    float nf = norm > 0 ? (float)(1.0 / std::sqrt(norm)) : 0.0f;
    c.lights[5 * i + 4][0] = d[0] * nf; c.lights[5 * i + 4][1] = d[1] * nf; c.lights[5 * i + 4][2] = d[2] * nf;
  }
  uint32_t mia = dc.matrix_index_a, mib = dc.matrix_index_b;
  for (int i = 0; i < 8; ++i) {
    uint32_t idx = i < 4 ? bits(mia, 6 + 6 * i, 6) : bits(mib, 6 * (i - 4), 6);
    std::memcpy(c.texmatrices[3 * i], &pos_matrices[idx * 4], 12 * sizeof(float));
  }
  std::memcpy(c.transformmatrices, pos_matrices, sizeof dc.posMatrices);
  for (int i = 0; i < 32; ++i) std::memcpy(c.normalmatrices[i], &nrm_matrices[3 * i], 12);
  std::memcpy(c.posttransformmatrices, dc.postMatrices, sizeof dc.postMatrices);
}

void fill_ps_constants(const DrawCall& dc, PSConstants& c, int efb_scale) {
  std::memset(&c, 0, sizeof c);
  const BPMemory& bp = dc.bp;
  std::memcpy(c.colors, dc.tev_colors, sizeof c.colors);
  std::memcpy(c.kcolors, dc.tev_kcolors, sizeof c.kcolors);
  uint32_t at = bp.alpha_test();
  c.alpha[0] = at & 0xFF; c.alpha[1] = (at >> 8) & 0xFF; c.alpha[2] = 0; c.alpha[3] = bp.dstalpha() & 0xFF;
  for (int i = 0; i < 8; ++i) {
    const TextureRef& t = dc.textures[i];
    uint32_t w = t.used ? t.width : 1, h = t.used ? t.height : 1;
    c.texdims[i][0] = 1.0f / (float)(w << 7);
    c.texdims[i][1] = 1.0f / (float)(h << 7);
    c.texdims[i][2] = (float)(((bp.texcoord_s(i) & 0xFFFF) + 1) << 7);
    c.texdims[i][3] = (float)(((bp.texcoord_t(i) & 0xFFFF) + 1) << 7);
  }
  const float* vp = (const float*)&dc.xf_regs[0x1A];
  c.zbias[1][0] = (int)vp[5]; c.zbias[1][1] = (int)vp[2]; c.zbias[1][3] = bp.ztex1() & 0xFFFFFF;
  for (int i = 0; i < 2; ++i) {
    uint32_t ts = bp.texscale(i);
    c.indtexscale[i][0] = ts & 15; c.indtexscale[i][1] = (ts >> 4) & 15; c.indtexscale[i][2] = (ts >> 8) & 15; c.indtexscale[i][3] = (ts >> 12) & 15;
  }
  for (int i = 0; i < 3; ++i) {
    uint32_t a = bp.indmtx(i, 0), b = bp.indmtx(i, 1), cc = bp.indmtx(i, 2);
    int scale = (bits(a, 22, 2) << 0) | (bits(b, 22, 2) << 2) | (bits(cc, 22, 2) << 4);
    scale = 17 - scale;
    c.indtexmtx[2 * i][0] = sbits(a, 0, 11); c.indtexmtx[2 * i][1] = sbits(b, 0, 11); c.indtexmtx[2 * i][2] = sbits(cc, 0, 11); c.indtexmtx[2 * i][3] = scale;
    c.indtexmtx[2 * i + 1][0] = sbits(a, 11, 11); c.indtexmtx[2 * i + 1][1] = sbits(b, 11, 11); c.indtexmtx[2 * i + 1][2] = sbits(cc, 11, 11); c.indtexmtx[2 * i + 1][3] = scale;
  }
  uint32_t fc = bp.fogcolor();
  c.fogcolor[0] = (fc >> 16) & 0xFF; c.fogcolor[1] = (fc >> 8) & 0xFF; c.fogcolor[2] = fc & 0xFF;
  c.fogi[1] = bp.fog_bmag() & 0xFFFFFF; c.fogi[3] = bp.fog_bshift() & 0x1F;
  auto f11 = [](uint32_t v) { uint32_t bitsv = ((v >> 20) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12; float f; std::memcpy(&f, &bitsv, 4); return f; };
  c.fogf[1][0] = f11(bp.fogparam0()); c.fogf[1][2] = f11(bp.fogparam3());
  if (bits(bp.fogrange(0), 10, 1)) {
    int center = (int)(bp.fogrange(0) & 0x3FF) - 342;
    float ssc = center / (2.0f * vp[0]); ssc = ssc * 2.0f - 1.0f;
    c.fogf[0][0] = ssc; c.fogf[0][1] = 2.0f * vp[0] * efb_scale; c.fogf[0][2] = ((bp.fogrange(5) >> 12) & 0xFFF) / 256.0f;
  } else {
    c.fogf[0][0] = 0.0f; c.fogf[0][1] = 1.0f; c.fogf[0][2] = 1.0f;
  }
  c.efbscale[0] = 1.0f / efb_scale; c.efbscale[1] = 1.0f / efb_scale;
  { const float* vp = (const float*)&dc.xf_regs[0x1A]; c.mvscale[0] = vp[0] * efb_scale; c.mvscale[1] = vp[1] * efb_scale; }
}

}  // namespace gx
