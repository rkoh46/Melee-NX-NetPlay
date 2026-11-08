"""Build a host-only adapter from the pinned decomp's FObj interpreter.

Keep the original state machine and Hermite expression. Replace unsafe stream reads
with bounded little-endian reads, assertions with errors, and guest callbacks with
a local scalar sink. No allocator, JObj, game memory, or emulation is linked.
"""
from pathlib import Path
import argparse
import hashlib
import json

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--decomp-root', type=Path, default=root / 'melee',
                    help='explicit doldecomp/melee source root; no files are copied')
args = parser.parse_args()
pins = json.loads((root / 'tools/port_source_pins.json').read_text(encoding='utf-8'))
base = args.decomp_root.resolve() / 'src/sysdolphin/baselib'
for name in ('fobj.c', 'fobj.h', 'spline.c'):
    path = base / name
    relative = 'src/sysdolphin/baselib/' + name
    if not path.is_file():
        parser.error(f'missing {path}; set --decomp-root to the pinned decomp checkout')
    if hashlib.sha256(path.read_bytes()).hexdigest() != pins['decomp']['files_sha256'][relative]:
        parser.error(f'{path} does not match pinned decomp revision {pins["decomp"]["commit"]}')
source = (base / 'fobj.c').read_text()
source = source[source.index('u32 HSD_FObjSetState'):source.index('HSD_FObj* HSD_FObjLoadDesc')]
header = (base / 'fobj.h').read_text()
constants = header[header.index('#define HSD_A_OP_NONE'):header.index('struct HSD_FObj {')]
structure = header[header.index('struct HSD_FObj {'):header.index('typedef struct _HSD_FObjDesc')]
spline = (base / 'spline.c').read_text()
spline = spline[spline.index('f32 splGetHelmite'):spline.index('static inline void splGetCardinalPoint')]
start, end = source.index('static f32 parseFloat'), source.index('static u32 FObjLoadWait')
source = source[:start] + r'''
static thread_local const u8* stream_end;
static u8 readByte(u8** p) {
  if (*p >= stream_end) throw std::runtime_error("truncated animation track");
  return *(*p)++;
}
static f32 parseFloat(u8** p, u8 format) {
  if (format == 0) {
    u32 bits = 0;
    for (unsigned i=0; i<4; ++i) bits |= u32(readByte(p)) << (8*i);
    float value; std::memcpy(&value, &bits, 4);
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite track value");
    return value;
  }
  const unsigned kind = format >> 5;
  int value = readByte(p);
  if (kind == 1 || kind == 2) value |= int(readByte(p)) << 8;
  if (kind == 1 && value >= 32768) value -= 65536;
  if (kind == 3 && value >= 128) value -= 256;
  if (kind < 1 || kind > 4) throw std::runtime_error("unsupported track value format");
  return std::ldexp(float(value), -int(format & 31));
}
static u8 parseOpCode(u8** p) {
  if (*p >= stream_end) throw std::runtime_error("missing track opcode");
  return **p & 15;
}
static u32 parsePackInfo(u8** p) {
  u8 byte = readByte(p);
  u32 count = ((byte >> 4) & 7) + 1;
  for (unsigned shift=3; byte & 128; shift+=7) {
    if (shift > 10) throw std::runtime_error("track packet count overflow");
    byte = readByte(p); count += u32(byte & 127) << shift;
  }
  if (count > 65535) throw std::runtime_error("track packet count overflow");
  return count;
}
static void FObjLaunchKeyData(HSD_FObj* fobj) {
  if (fobj->flags & 0x40) {
    fobj->op_intrp=fobj->op; fobj->flags &= ~0x40;
    fobj->flags |= 0x80; fobj->p0=fobj->p1;
  }
}
static s32 parseWait(u8** p) {
  u32 value = 0;
  for (unsigned shift=0; shift<=14; shift+=7) {
    const u8 byte=readByte(p); value |= u32(byte & 127) << shift;
    if (!(byte & 128)) {
      if (value > 65535) throw std::runtime_error("track wait overflow");
      return value;
    }
  }
  throw std::runtime_error("track wait overflow");
}
''' + source[end:]
# The original default branch leaves the callback value undefined; reject it.
source = source.replace('    default:\n        break;\n    }\n    obj_update', '    default:\n        return;\n    }\n    obj_update')
preamble = r'''
// Generated from pinned doldecomp/melee fobj.c and spline.c; see generator for adaptations.
#include "PackedAnimation.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <cstddef>
namespace NativeMelee { namespace {
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t;
using s8=int8_t; using s16=int16_t; using s32=int32_t; using f32=float;
struct HSD_FObj;
union HSD_ObjData { float fv; int iv; };
using HSD_ObjUpdateFunc = void(*)(void*, u8, HSD_ObjData*);
void HSD_FObjInterpretAnim(HSD_FObj*, void*, HSD_ObjUpdateFunc, float);
#define HSD_ASSERT(line, condition) if (!(condition)) throw std::runtime_error("animation state error")
'''
footer = r'''
struct Result { float value=0; bool sampled=false; };
void Receive(void* object, u8, HSD_ObjData* data) {
  auto& result=*static_cast<Result*>(object);
  result.value=data->fv; result.sampled=true;
}
} // anonymous namespace
bool SamplePacked(const PackedTrack& track, float frame, float& value) {
  if (!std::isfinite(frame)) throw std::invalid_argument("invalid animation time");
  if (track.bytes.empty()) return false;
  if (track.bytes.size() > 65535) throw std::invalid_argument("oversized fighter track");
  HSD_FObj fobj{};
  fobj.startframe=track.start_frame; fobj.obj_type=track.channel;
  fobj.frac_value=track.value_format; fobj.frac_slope=track.slope_format;
  // The original interpreter only reads byte data; its mutable pointer is local.
  fobj.ad_head=const_cast<u8*>(track.bytes.data());
  fobj.length=static_cast<u32>(track.bytes.size());
  stream_end=track.bytes.data()+track.bytes.size();
  HSD_FObjReqAnim(&fobj, frame);
  Result result;
  HSD_FObjInterpretAnim(&fobj, &result, Receive, 0);
  if (result.sampled) value=result.value;
  return result.sampled;
}
}
'''
output = args.output
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(preamble + constants + structure + spline + source + footer)
