#pragma once
#include "Assets.h"
namespace NativeMelee {
using Pixel=std::array<uint8_t,4>;
struct Texture { unsigned width=1,height=1; std::vector<Pixel> pixels={Pixel{{255,255,255,255}}}; };
inline uint8_t Expand(unsigned v,unsigned bits) {return static_cast<uint8_t>((v*255)/((1u<<bits)-1));}
inline Pixel RGB565(uint16_t c) {return {{Expand(c>>11,5),Expand((c>>5)&63,6),Expand(c&31,5),255}};}
inline Pixel RGB5A3(uint16_t c) {
  if(c&0x8000)return {{Expand((c>>10)&31,5),Expand((c>>5)&31,5),Expand(c&31,5),255}};
  return {{Expand((c>>8)&15,4),Expand((c>>4)&15,4),Expand(c&15,4),Expand((c>>12)&7,3)}};
}
inline Texture DecodeTexture(const Archive& a,uint32_t descriptor) {
  if(!descriptor)return {};
  const auto image=a.U32(descriptor+76),palette=a.U32(descriptor+80);
  if(!image)return {};
  Texture texture;texture.width=a.U16(image+4);texture.height=a.U16(image+6);
  if(!texture.width || !texture.height || texture.width>4096 || texture.height>4096)throw std::runtime_error("Invalid texture dimensions");
  const auto format=a.U32(image+8);size_t cursor=a.U32(image);
  texture.pixels.assign(size_t(texture.width)*texture.height,Pixel{});
  auto store=[&](unsigned x,unsigned y,Pixel pixel) {if(x<texture.width && y<texture.height)texture.pixels[y*texture.width+x]=pixel;};
  auto lookup=[&](unsigned index)->Pixel {
    if(!palette || index>=a.U16(palette+12))throw std::runtime_error("Invalid texture palette index");
    const auto color=a.U16(a.U32(palette)+index*2);
    const auto kind=a.U32(palette+4);
    if(kind==0)return {{uint8_t(color&255),uint8_t(color&255),uint8_t(color&255),uint8_t(color>>8)}};
    if(kind==1)return RGB565(color);
    if(kind==2)return RGB5A3(color);
    throw std::runtime_error("Unknown palette format");
  };
  unsigned bw=4,bh=4;
  if(format==0 || format==8 || format==14)bw=bh=8;
  else if(format==1 || format==2 || format==9)bw=8;
  for(unsigned by=0;by<texture.height;by+=bh)for(unsigned bx=0;bx<texture.width;bx+=bw) {
    if(format==14) {
      for(unsigned block=0;block<4;++block,cursor+=8) {
        const auto c0=a.U16(cursor),c1=a.U16(cursor+2);
        Pixel colors[4]={RGB565(c0),RGB565(c1),{},{} };
        for(int k=0;k<3;++k) {
          if(c0>c1) {colors[2][k]=uint8_t((2*colors[0][k]+colors[1][k])/3);colors[3][k]=uint8_t((colors[0][k]+2*colors[1][k])/3);}
          else {colors[2][k]=uint8_t((colors[0][k]+colors[1][k])/2);colors[3][k]=colors[2][k];}
        }
        colors[2][3]=255;colors[3][3]=c0>c1?255:0;
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
          store(bx+(block&1)*4+x,by+(block>>1)*4+y,colors[(a.U8(cursor+4+y)>>(6-2*x))&3]);
      }
      continue;
    }
    for(unsigned y=0;y<bh;++y)for(unsigned x=0;x<bw;++x) {
      const unsigned index=y*bw+x;Pixel pixel{};
      if(format==0 || format==8) {
        unsigned v=(a.U8(cursor+index/2)>>(index%2?0:4))&15;
        pixel=format==8?lookup(v):Pixel{{Expand(v,4),Expand(v,4),Expand(v,4),Expand(v,4)}};
      } else if(format==1) {auto v=a.U8(cursor+index);pixel={{v,v,v,v}};}
      else if(format==2) {auto v=a.U8(cursor+index);pixel={{Expand(v&15,4),Expand(v&15,4),Expand(v&15,4),Expand(v>>4,4)}};}
      else if(format==3) {auto v=a.U16(cursor+2*index);pixel={{uint8_t(v&255),uint8_t(v&255),uint8_t(v&255),uint8_t(v>>8)}};}
      else if(format==4)pixel=RGB565(a.U16(cursor+2*index));
      else if(format==5)pixel=RGB5A3(a.U16(cursor+2*index));
      else if(format==6)pixel={{a.U8(cursor+2*index+1),a.U8(cursor+32+2*index),a.U8(cursor+32+2*index+1),a.U8(cursor+2*index)}};
      else if(format==9)pixel=lookup(a.U8(cursor+index));
      else if(format==10)pixel=lookup(a.U16(cursor+2*index)&0x3fff);
      else throw std::runtime_error("Unsupported texture format " + std::to_string(format));
      store(bx+x,by+y,pixel);
    }
    cursor+=format==6?64:32;
  }
  return texture;
}
}
