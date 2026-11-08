#pragma once
#include "Assets.h"
#include "Texture.h"
#include <algorithm>
#include <set>
namespace NativeMelee {
using Vec=std::array<float,3>;
using Matrix=std::array<float,12>;
inline Matrix Identity() { return {{1,0,0,0,0,1,0,0,0,0,1,0}}; }
inline Matrix Multiply(const Matrix& a,const Matrix& b) {
  Matrix c{};
  for (int r=0;r<3;++r) for (int col=0;col<4;++col) {
    for (int k=0;k<3;++k) c[r*4+col]+=a[r*4+k]*b[k*4+col];
    if (col==3) c[r*4+col]+=a[r*4+3];
  }
  return c;
}
inline Vec Transform(const Matrix& m,const Vec& v) {
  return {{m[0]*v[0]+m[1]*v[1]+m[2]*v[2]+m[3],
           m[4]*v[0]+m[5]*v[1]+m[6]*v[2]+m[7],
           m[8]*v[0]+m[9]*v[1]+m[10]*v[2]+m[11]}};
}
inline Matrix SRT(Vec scale,Vec rotation,Vec position,const Vec& parent_scale) {
  const float sx=std::sin(rotation[0]),cx=std::cos(rotation[0]);
  const float sy=std::sin(rotation[1]),cy=std::cos(rotation[1]);
  const float sz=std::sin(rotation[2]),cz=std::cos(rotation[2]);
  Matrix m={{cz*cy,cz*sx*sy-cx*sz,cz*cx*sy+sx*sz,position[0],
             sz*cy,sz*sx*sy+cx*cz,sz*cx*sy-sx*cz,position[1],
             -sy,cy*sx,cy*cx,position[2]}};
  for (int r=0;r<3;++r) for (int c=0;c<3;++c) {
    if (std::abs(parent_scale[r])<1e-10f) throw std::runtime_error("Zero inherited scale");
    m[r*4+c]*=scale[c]*parent_scale[c]/parent_scale[r];
  }
  return m;
}
inline std::vector<Matrix> Pose(const Rig& rig,const Animation* animation,float frame) {
  std::vector<Matrix> matrices;
  std::vector<Vec> scales;
  for (size_t i=0;i<rig.joints.size();++i) {
    const auto& j=rig.joints[i];
    Vec rotation=j.rotation,scale=j.scale,position=j.position;
    bool classical=(j.flags&8)!=0;
    if (animation && i<animation->nodes.size() && !animation->nodes[i].empty()) {
      classical=(animation->type&1)!=0;
      for (const auto& track:animation->nodes[i]) {
        float value;
        if (!SamplePacked(track,frame,value)) continue;
        if (track.channel>=1 && track.channel<=3) rotation[track.channel-1]=value;
        else if (track.channel>=5 && track.channel<=7) position[track.channel-5]=value;
        else if (track.channel>=8 && track.channel<=10) scale[track.channel-8]=value;
      }
    }
    const Vec parent_scale=j.parent<0 ? Vec{{1,1,1}} : scales[j.parent];
    Vec accumulated=parent_scale;
    if (!classical) for (int k=0;k<3;++k) accumulated[k]*=scale[k];
    scales.push_back(accumulated);
    Matrix local=SRT(scale,rotation,position,parent_scale);
    matrices.push_back(j.parent<0 ? local : Multiply(matrices[j.parent],local));
  }
  return matrices;
}
struct Influence { size_t joint; float weight; bool inverse_bind; };
struct Vertex { Vec position; Vec color; Vec normal{{0,1,0}}; std::array<float,2> uv{}; std::vector<Influence> influences; };
struct Batch { size_t start,count,texture; };
struct Mesh { std::vector<Vertex> triangles; std::vector<Texture> textures; std::vector<Batch> batches; size_t polygons=0; };
struct Attribute { uint32_t attr,type,count,format; uint8_t frac; uint16_t stride; uint32_t data; };
inline unsigned ComponentBytes(const Attribute& a) {
  if (a.attr<=8) return 1;
  if (a.attr==11 || a.attr==12) {
    static const unsigned sizes[]={2,3,4,2,3,4};
    if (a.format>5) throw std::runtime_error("Bad color format");
    return sizes[a.format];
  }
  if (a.format>4) throw std::runtime_error("Bad component format");
  unsigned count=a.attr==9 ? (a.count==0?2:3) : a.attr==10 ? (a.count==0?3:9) : (a.count==0?1:2);
  return count*(a.format<2?1:a.format<4?2:4);
}
inline float Component(const Archive& a,size_t offset,uint32_t format,uint8_t frac) {
  if (format==4) return a.F32(offset);
  float value= format==0 ? float(a.U8(offset)) : format==1 ? float(static_cast<int8_t>(a.U8(offset))) :
               format==2 ? float(a.U16(offset)) : float(static_cast<int16_t>(a.U16(offset)));
  return std::ldexp(value,-int(frac));
}
inline Mesh LoadMesh(const Archive& a,const Rig& rig) {
  Mesh mesh;
  mesh.textures.emplace_back();std::map<uint32_t,size_t> texture_indices;
  std::set<uint32_t> all_dobjs,all_pobjs;
  for (size_t owner=0;owner<rig.joints.size();++owner) {
    const auto& joint=rig.joints[owner];
    if (joint.flags & ((1<<5)|(1<<14))) continue;
    for (uint32_t dobj=joint.display;dobj;dobj=a.U32(dobj+4)) {
      if (!all_dobjs.insert(dobj).second) throw std::runtime_error("Repeated display object");
      Vec color{{0.65f,0.7f,0.8f}};
      size_t texture_index=0;
      if(auto material=a.U32(dobj+8)) if(auto texture=a.U32(material+8)) {
        auto found=texture_indices.find(texture);
        if(found==texture_indices.end()) {
          texture_index=mesh.textures.size();texture_indices[texture]=texture_index;
          mesh.textures.push_back(DecodeTexture(a,texture));
        }else texture_index=found->second;
      }
      if (auto material=a.U32(dobj+8)) if (auto mat=a.U32(material+12))
        for (int k=0;k<3;++k) color[k]=a.U8(mat+4+k)/255.f;
      for (uint32_t pobj=a.U32(dobj+12);pobj;pobj=a.U32(pobj+4)) {
        if (!all_pobjs.insert(pobj).second) throw std::runtime_error("Repeated polygon object");
        ++mesh.polygons;
        const size_t batch_start=mesh.triangles.size();
        const unsigned flags=a.U16(pobj+12), kind=flags&0x3000;
        if (kind==0x1000) throw std::runtime_error("Shape animation requires morph decoder");
        std::vector<std::vector<Influence>> palettes;
        if (kind==0x2000) {
          if (!(joint.flags&2)) throw std::runtime_error("Non-root envelope transform not implemented");
          auto table=a.U32(pobj+20);
          for (size_t slot=0;slot<10;++slot) {
            auto envelope=a.U32(table+slot*4); if (!envelope) break;
            std::vector<Influence> weights;
            for (size_t n=0;n<rig.joints.size();++n,envelope+=8) {
              auto reference=a.U32(envelope); if (!reference) break;
              auto found=rig.index.find(reference);
              if (found==rig.index.end()) throw std::runtime_error("Unknown envelope joint");
              weights.push_back({found->second,a.F32(envelope+4),true});
            }
            if (weights.empty()) throw std::runtime_error("Empty envelope");
            if (weights.front().weight>=1.0f-1.192092896e-7f) {
              weights.resize(1); weights.front().inverse_bind=false;
            }
            palettes.push_back(weights);
          }
        } else {
          palettes.push_back({{owner,1,false}});
          if (auto shared=a.U32(pobj+20)) palettes.push_back({{rig.index.at(shared),1,false}});
        }
        std::vector<Attribute> attributes;
        for (size_t v=a.U32(pobj+8);;v+=24) {
          if (attributes.size()>32) throw std::runtime_error("Unterminated vertex attributes");
          auto attr=a.U32(v); if (attr==255) break;
          attributes.push_back({attr,a.U32(v+4),a.U32(v+8),a.U32(v+12),a.U8(v+16),a.U16(v+18),a.U32(v+20)});
        }
        size_t cursor=a.U32(pobj+16),end=cursor+size_t(a.U16(pobj+14))*32;
        a.Data(cursor,end-cursor);
        while (cursor<end) {
          uint8_t command=a.U8(cursor++); if (!command) continue;
          unsigned primitive=command&0xf8;
          if (primitive!=0x80 && primitive!=0x90 && primitive!=0x98 && primitive!=0xa0)
            throw std::runtime_error("Unsupported display-list primitive " + std::to_string(command));
          if (cursor+2>end) throw std::runtime_error("Truncated primitive");
          const unsigned count=a.U16(cursor); cursor+=2;
          std::vector<Vertex> vertices;
          for (unsigned n=0;n<count;++n) {
            Vertex vertex{}; vertex.color=color; unsigned matrix=0; bool has_position=false;
            for (const auto& attribute:attributes) {
              if (!attribute.type) continue;
              size_t at=cursor;
              if (attribute.type==1) cursor+=ComponentBytes(attribute);
              else if (attribute.type==2 || attribute.type==3) {
                const unsigned index=attribute.type==2 ? a.U8(cursor++) : a.U16(cursor);
                if (attribute.type==3) cursor+=2;
                at=attribute.data+size_t(index)*attribute.stride;
                if (attribute.attr==10 && attribute.count==2) cursor+=attribute.type==2?2:4;
              } else throw std::runtime_error("Unknown vertex attribute type");
              if (cursor>end) throw std::runtime_error("Vertex overran display list");
              if (attribute.attr==0) matrix=a.U8(at)/3;
              if (attribute.attr==9) {
                const unsigned width=attribute.format<2?1:attribute.format<4?2:4;
                for (unsigned k=0;k<(attribute.count==0?2u:3u);++k)
                  vertex.position[k]=Component(a,at+k*width,attribute.format,attribute.frac);
                has_position=true;
              }
              if(attribute.attr==13) {
                const unsigned width=attribute.format<2?1:attribute.format<4?2:4;
                vertex.uv[0]=Component(a,at,attribute.format,attribute.frac);
                if(attribute.count)vertex.uv[1]=Component(a,at+width,attribute.format,attribute.frac);
              }
              if(attribute.attr==10 && attribute.count==0) {
                const unsigned width=attribute.format<2?1:attribute.format<4?2:4;
                const uint8_t fraction=attribute.format==1?6:attribute.format==3?14:0;
                for(unsigned k=0;k<3;++k)vertex.normal[k]=Component(a,at+k*width,attribute.format,fraction);
              }
            }
            if (!has_position || matrix>=palettes.size()) throw std::runtime_error("Invalid vertex position/matrix");
            vertex.influences=palettes[matrix]; vertices.push_back(std::move(vertex));
          }
          auto triangle=[&](unsigned x,unsigned y,unsigned z) {
            mesh.triangles.push_back(vertices.at(x)); mesh.triangles.push_back(vertices.at(y)); mesh.triangles.push_back(vertices.at(z));
          };
          if (primitive==0x90) { if (count%3) throw std::runtime_error("Partial triangle"); for (unsigned i=0;i<count;i+=3) triangle(i,i+1,i+2); }
          if (primitive==0x80) { if (count%4) throw std::runtime_error("Partial quad"); for (unsigned i=0;i<count;i+=4) {triangle(i,i+1,i+2);triangle(i,i+2,i+3);} }
          if (primitive==0x98) for (unsigned i=2;i<count;++i) {if(i&1)triangle(i-1,i-2,i);else triangle(i-2,i-1,i);}
          if (primitive==0xa0) for (unsigned i=2;i<count;++i) triangle(0,i-1,i);
        }
        mesh.batches.push_back({batch_start,mesh.triangles.size()-batch_start,texture_index});
      }
    }
  }
  return mesh;
}
inline Vec Skin(const Vertex& vertex,const Rig& rig,const std::vector<Matrix>& pose) {
  Vec output{};
  for (const auto& influence:vertex.influences) {
    Matrix matrix=pose.at(influence.joint);
    if (influence.inverse_bind) {
      if (!rig.joints[influence.joint].has_inverse_bind) throw std::runtime_error("Missing inverse bind matrix");
      matrix=Multiply(matrix,rig.joints[influence.joint].inverse_bind);
    }
    const auto point=Transform(matrix,vertex.position);
    for (int k=0;k<3;++k) output[k]+=influence.weight*point[k];
  }
  return output;
}
inline std::vector<Matrix> SkinMatrices(const Rig& rig,const std::vector<Matrix>& pose) {
  auto matrices=pose;
  for(size_t i=0;i<matrices.size();++i) if(rig.joints[i].has_inverse_bind)
    matrices[i]=Multiply(pose[i],rig.joints[i].inverse_bind);
  return matrices;
}
inline void SkinVertex(const Vertex& vertex,const std::vector<Matrix>& pose,
                       const std::vector<Matrix>& bound,Vec& position,Vec& normal) {
  position={};normal={};
  for(const auto& influence:vertex.influences) {
    const auto& matrix=influence.inverse_bind?bound.at(influence.joint):pose.at(influence.joint);
    const auto point=Transform(matrix,vertex.position);
    // Direction transform; nonuniform scale needs full inverse-transpose parity work.
    for(int r=0;r<3;++r) {
      position[r]+=influence.weight*point[r];
      for(int c=0;c<3;++c)normal[r]+=influence.weight*matrix[r*4+c]*vertex.normal[c];
    }
  }
}
}
