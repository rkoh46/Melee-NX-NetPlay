#pragma once
#include "PackedAnimation.h"
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>

namespace NativeMelee {
class Archive {
public:
  std::vector<uint8_t> bytes;
  size_t base=0, length=0, data_size=0;
  explicit Archive(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + path);
    const auto size=file.tellg();
    if (size < 32 || size > 64*1024*1024) throw std::runtime_error("Invalid asset size");
    bytes.resize(static_cast<size_t>(size)); file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Asset read failed");
    Select(0);
  }
  uint32_t Absolute32(size_t at) const {
    if (at > bytes.size() || bytes.size()-at < 4) throw std::runtime_error("Asset bounds");
    return uint32_t(bytes[at])<<24 | uint32_t(bytes[at+1])<<16 | uint32_t(bytes[at+2])<<8 | bytes[at+3];
  }
  void Select(size_t position) {
    base=position; length=Absolute32(base); data_size=Absolute32(base+4);
    if (length < 32 || length > bytes.size()-base || data_size > length-32)
      throw std::runtime_error("Invalid archive");
  }
  const uint8_t* Data(size_t at, size_t count) const {
    if (at > data_size || count > data_size-at) throw std::runtime_error("Archive data bounds");
    return bytes.data()+base+32+at;
  }
  uint8_t U8(size_t at) const { return *Data(at,1); }
  uint16_t U16(size_t at) const { const auto* p=Data(at,2); return uint16_t(p[0])<<8|p[1]; }
  uint32_t U32(size_t at) const { Data(at,4); return Absolute32(base+32+at); }
  float F32(size_t at) const {
    uint32_t bits=U32(at); float value; std::memcpy(&value,&bits,4);
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite asset float");
    return value;
  }
  std::vector<std::pair<std::string,uint32_t>> Symbols() const {
    const size_t reloc=Absolute32(base+8), count=Absolute32(base+12), external=Absolute32(base+16);
    const size_t table=32+data_size+reloc*4, names=table+(count+external)*8;
    if (names > length) throw std::runtime_error("Invalid symbol tables");
    std::vector<std::pair<std::string,uint32_t>> result;
    for (size_t i=0;i<count;++i) {
      const auto offset=Absolute32(base+table+i*8), name=Absolute32(base+table+i*8+4);
      size_t start=names+name, end=start;
      if (start>=length || offset>=data_size) throw std::runtime_error("Invalid public symbol");
      while (end<length && bytes[base+end]) ++end;
      if (end==length) throw std::runtime_error("Unterminated symbol");
      result.emplace_back(std::string(reinterpret_cast<const char*>(bytes.data()+base+start),end-start),offset);
    }
    return result;
  }
};
struct Joint {
  uint32_t address, flags, display;
  int parent;
  std::array<float,3> rotation, scale, position;
  std::array<float,12> inverse_bind{};
  bool has_inverse_bind=false;
};
struct Rig {
  std::vector<Joint> joints;
  std::map<uint32_t,size_t> index;
  void Visit(const Archive& a, uint32_t offset, int parent) {
    if (joints.size()>=4096 || index.count(offset)) throw std::runtime_error("Invalid joint hierarchy");
    a.Data(offset,64);
    Joint j{}; j.address=offset; j.flags=a.U32(offset+4); j.display=a.U32(offset+16); j.parent=parent;
    for (int i=0;i<3;++i) { j.rotation[i]=a.F32(offset+20+4*i); j.scale[i]=a.F32(offset+32+4*i); j.position[i]=a.F32(offset+44+4*i); }
    if (auto matrix=a.U32(offset+56)) {
      j.has_inverse_bind=true;
      for (int i=0;i<12;++i) j.inverse_bind[i]=a.F32(matrix+4*i);
    }
    int current=static_cast<int>(joints.size()); index[offset]=joints.size(); joints.push_back(j);
    if (auto child=a.U32(offset+8)) Visit(a,child,current);
    if (auto next=a.U32(offset+12)) Visit(a,next,parent);
  }
  explicit Rig(const Archive& a) {
    for (const auto& symbol:a.Symbols()) {
      if (symbol.first.find("_Share_joint") != std::string::npos) { Visit(a,symbol.second,-1); return; }
    }
    throw std::runtime_error("Missing model root");
  }
};
struct Animation {
  std::string name;
  float frames;
  uint32_t type, flags;
  std::vector<std::vector<PackedTrack>> nodes;
};
inline std::vector<Animation> LoadAnimations(Archive& archive) {
  std::vector<Animation> animations;
  for (size_t offset=0;offset<archive.bytes.size();) {
    archive.Select(offset);
    for (const auto& symbol:archive.Symbols()) {
      if (symbol.first.find("_figatree")==std::string::npos) continue;
      Animation animation; animation.name=symbol.first;
      const size_t tree=symbol.second;
      animation.type=archive.U32(tree); animation.flags=archive.U32(tree+4);
      animation.frames=archive.F32(tree+8);
      size_t counts=archive.U32(tree+12), tracks=archive.U32(tree+16);
      for (size_t node=0;;++node) {
        if (node>=4096) throw std::runtime_error("Unterminated animation node table");
        const auto count=archive.U8(counts+node);
        if (count==255) break;
        if (count>127) throw std::runtime_error("Invalid node track count");
        animation.nodes.emplace_back();
        for (size_t i=0;i<count;++i,tracks+=12) {
          PackedTrack track;
          const auto size=archive.U16(tracks);
          track.start_frame=static_cast<int16_t>(archive.U16(tracks+2));
          track.channel=archive.U8(tracks+4); track.value_format=archive.U8(tracks+5); track.slope_format=archive.U8(tracks+6);
          const auto* data=archive.Data(archive.U32(tracks+8),size);
          track.bytes.assign(data,data+size);
          animation.nodes.back().push_back(std::move(track));
        }
      }
      animations.push_back(std::move(animation));
    }
    offset+=(archive.length+31)&~size_t(31);
  }
  return animations;
}
}
