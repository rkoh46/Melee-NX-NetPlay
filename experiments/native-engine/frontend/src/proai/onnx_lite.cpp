// onnx_lite: see onnx_lite.h. Written for this repository from the ONNX operator specification (onnx.ai, opset 23)
// and the protobuf wire format; no code from onnxruntime or any other runtime.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "onnx_lite.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define ONNX_LITE_NEON 1
#else
#define ONNX_LITE_NEON 0
#endif

namespace onnx_lite {

size_t dtype_size(DType t) {
  switch (t) {
    case DType::Float: case DType::Int32: return 4;
    case DType::Int64: case DType::Double: return 8;
    case DType::Float16: return 2;
    case DType::UInt8: case DType::Int8: case DType::Bool: return 1;
    default: return 0;
  }
}
const char* dtype_name(DType t) {
  switch (t) {
    case DType::Float: return "float32"; case DType::Int32: return "int32"; case DType::Int64: return "int64";
    case DType::Double: return "float64"; case DType::Float16: return "float16"; case DType::UInt8: return "uint8";
    case DType::Int8: return "int8"; case DType::Bool: return "bool"; default: return "undefined";
  }
}

float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) bits = sign;
    else {   // subnormal: normalise
      int e = -1;
      uint32_t m = man;
      do { ++e; m <<= 1; } while (!(m & 0x400u));
      bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
    }
  } else if (exp == 31) bits = sign | 0x7F800000u | man << 13;
  else bits = sign | (exp + 127 - 15) << 23 | man << 13;
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

size_t Tensor::count() const {
  size_t n = 1;
  for (int64_t d : shape) n *= size_t(d < 0 ? 0 : d);
  return n;
}
void Tensor::reset(DType t, const std::vector<int64_t>& s) {
  type = t;
  if (&s != &shape) shape = s;
  view = nullptr;
  view_bytes = 0;
  const size_t words = (bytes() + 7) / 8;
  if (storage.size() < words) storage.resize(words);
}
double Tensor::get(size_t i) const {
  const uint8_t* p = data();
  switch (type) {
    case DType::Float: { float v; std::memcpy(&v, p + i * 4, 4); return v; }
    case DType::Double: { double v; std::memcpy(&v, p + i * 8, 8); return v; }
    case DType::Int32: { int32_t v; std::memcpy(&v, p + i * 4, 4); return v; }
    case DType::Int64: { int64_t v; std::memcpy(&v, p + i * 8, 8); return double(v); }
    case DType::Float16: { uint16_t v; std::memcpy(&v, p + i * 2, 2); return half_to_float(v); }
    case DType::UInt8: return p[i];
    case DType::Int8: return int8_t(p[i]);
    case DType::Bool: return p[i] ? 1.0 : 0.0;
    default: return 0.0;
  }
}
void Tensor::set(size_t i, double v) {
  uint8_t* p = mutable_data();
  switch (type) {
    case DType::Float: { const float f = float(v); std::memcpy(p + i * 4, &f, 4); break; }
    case DType::Double: std::memcpy(p + i * 8, &v, 8); break;
    case DType::Int32: { const int32_t x = int32_t(v); std::memcpy(p + i * 4, &x, 4); break; }
    case DType::Int64: { const int64_t x = int64_t(v); std::memcpy(p + i * 8, &x, 8); break; }
    case DType::UInt8: p[i] = uint8_t(int(v)); break;
    case DType::Int8: p[i] = uint8_t(int8_t(int(v))); break;
    case DType::Bool: p[i] = v != 0.0 ? 1 : 0; break;
    default: break;
  }
}

namespace {

// ---------------------------------------------------------------------------------------------
// Protobuf wire format: varints, fixed 32/64, length-delimited fields.
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool bad = false;
  bool more() const { return !bad && p < end; }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (p >= end) { bad = true; return 0; }
      const uint8_t b = *p++;
      v |= uint64_t(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    bad = true;
    return 0;
  }
  // The next field: number and wire type; false at the end.
  bool field(uint32_t& number, uint32_t& wire) {
    if (!more()) return false;
    const uint64_t key = varint();
    number = uint32_t(key >> 3);
    wire = uint32_t(key & 7);
    return !bad;
  }
  Reader sub() {
    const uint64_t n = varint();
    if (bad || n > uint64_t(end - p)) { bad = true; return Reader{p, p, true}; }
    Reader r{p, p + n};
    p += n;
    return r;
  }
  std::string str() { Reader r = sub(); return std::string(reinterpret_cast<const char*>(r.p), size_t(r.end - r.p)); }
  float f32() { float f = 0; if (end - p < 4) { bad = true; return 0; } std::memcpy(&f, p, 4); p += 4; return f; }
  void skip(uint32_t wire) {
    switch (wire) {
      case 0: varint(); break;
      case 1: if (end - p < 8) bad = true; else p += 8; break;
      case 2: sub(); break;
      case 5: if (end - p < 4) bad = true; else p += 4; break;
      default: bad = true; break;
    }
  }
};

// Repeated scalar fields come packed (wire 2) or one by one (wire 0).
void read_ints(Reader& r, uint32_t wire, std::vector<int64_t>& out) {
  if (wire == 2) { Reader s = r.sub(); while (s.more()) out.push_back(int64_t(s.varint())); r.bad |= s.bad; }
  else out.push_back(int64_t(r.varint()));
}
void read_floats(Reader& r, uint32_t wire, std::vector<float>& out) {
  if (wire == 2) { Reader s = r.sub(); while (s.more()) out.push_back(s.f32()); r.bad |= s.bad; }
  else out.push_back(r.f32());
}

struct RawTensor {
  std::string name;
  DType type = DType::Undefined;
  std::vector<int64_t> dims;
  const uint8_t* raw = nullptr;
  size_t raw_size = 0;
  std::vector<float> floats;
  std::vector<int64_t> ints;   // int32_data / int64_data (also bools and halves in int32_data)
  bool external = false;
};
RawTensor parse_tensor(Reader r) {
  RawTensor t;
  uint32_t n, w;
  while (r.field(n, w)) {
    switch (n) {
      case 1: read_ints(r, w, t.dims); break;
      case 2: t.type = DType(int32_t(r.varint())); break;
      case 4: read_floats(r, w, t.floats); break;
      case 5: read_ints(r, w, t.ints); break;
      case 7: read_ints(r, w, t.ints); break;
      case 8: t.name = r.str(); break;
      case 9: { Reader s = r.sub(); t.raw = s.p; t.raw_size = size_t(s.end - s.p); break; }
      case 13: r.skip(w); t.external = true; break;    // external_data
      case 14: { const uint64_t loc = r.varint(); if (loc == 1) t.external = true; break; }   // data_location
      default: r.skip(w); break;
    }
  }
  if (r.bad) t.type = DType::Undefined;
  return t;
}

struct Attr {
  std::string name;
  int64_t i = 0;
  float f = 0;
  std::string s;
  std::vector<int64_t> ints;
  std::vector<float> floats;
  bool has_t = false;
  RawTensor t;
};
Attr parse_attr(Reader r) {
  Attr a;
  uint32_t n, w;
  while (r.field(n, w)) {
    switch (n) {
      case 1: a.name = r.str(); break;
      case 2: a.f = r.f32(); break;
      case 3: a.i = int64_t(r.varint()); break;
      case 4: a.s = r.str(); break;
      case 5: a.t = parse_tensor(r.sub()); a.has_t = true; break;
      case 7: read_floats(r, w, a.floats); break;
      case 8: read_ints(r, w, a.ints); break;
      default: r.skip(w); break;
    }
  }
  return a;
}

// ---------------------------------------------------------------------------------------------
enum class Op {
  Add, Sub, Mul, Div, Max, Min, Where, Less, GreaterOrEqual, Greater, LessOrEqual, Equal, Or, And, Not, BitwiseAnd,
  Neg, Sqrt, Log, Exp, Sigmoid, Tanh, Relu, Gelu, Clip, Cast, Reshape, Squeeze, Unsqueeze, Expand, Concat, Split, Gather,
  OneHot, ReduceMean, ReduceSum, ArgMax, Gemm, MatMul, Identity, Shape, Constant
};
bool op_from(const std::string& s, Op& op) {
  static const std::unordered_map<std::string, Op> m = {
      {"Add", Op::Add}, {"Sub", Op::Sub}, {"Mul", Op::Mul}, {"Div", Op::Div}, {"Max", Op::Max}, {"Min", Op::Min},
      {"Where", Op::Where}, {"Less", Op::Less}, {"GreaterOrEqual", Op::GreaterOrEqual}, {"Greater", Op::Greater},
      {"LessOrEqual", Op::LessOrEqual}, {"Equal", Op::Equal}, {"Or", Op::Or}, {"And", Op::And}, {"Not", Op::Not},
      {"BitwiseAnd", Op::BitwiseAnd}, {"Neg", Op::Neg}, {"Sqrt", Op::Sqrt}, {"Log", Op::Log}, {"Exp", Op::Exp},
      {"Sigmoid", Op::Sigmoid}, {"Tanh", Op::Tanh}, {"Relu", Op::Relu}, {"Gelu", Op::Gelu}, {"Clip", Op::Clip},
      {"Cast", Op::Cast}, {"Reshape", Op::Reshape}, {"Squeeze", Op::Squeeze}, {"Unsqueeze", Op::Unsqueeze},
      {"Expand", Op::Expand}, {"Concat", Op::Concat}, {"Split", Op::Split}, {"Gather", Op::Gather}, {"OneHot", Op::OneHot},
      {"ReduceMean", Op::ReduceMean}, {"ReduceSum", Op::ReduceSum}, {"ArgMax", Op::ArgMax}, {"Gemm", Op::Gemm},
      {"MatMul", Op::MatMul}, {"Identity", Op::Identity}, {"Shape", Op::Shape}, {"Constant", Op::Constant}};
  auto it = m.find(s);
  if (it == m.end()) return false;
  op = it->second;
  return true;
}

struct Node {
  Op op;
  std::string op_name, name;
  std::vector<int> in, out;   // value ids, -1 for an omitted optional input
  // Attributes the supported operators read.
  int64_t axis = 0, keepdims = 1, to = 0, transA = 0, transB = 0, select_last = 0, num_outputs = 0, noop_empty = 0;
  int64_t allowzero = 0;
  bool has_axis = false;
  float alpha = 1.0f, beta = 1.0f;
  std::string approximate;
  std::vector<int64_t> axes_attr, split_attr;   // older opsets carry these as attributes
  bool has_axes_attr = false, has_split_attr = false;
  RawTensor value;                               // Constant
  bool folded = false;
};

// Broadcast shape of several inputs (numpy rules); false when they do not broadcast.
bool broadcast_shape(const std::vector<const std::vector<int64_t>*>& shapes, std::vector<int64_t>& out) {
  size_t rank = 0;
  for (auto* s : shapes) rank = std::max(rank, s->size());
  out.assign(rank, 1);
  for (auto* s : shapes) {
    for (size_t i = 0; i < s->size(); ++i) {
      const size_t o = rank - s->size() + i;
      const int64_t d = (*s)[i];
      if (d == 1) continue;
      if (out[o] == 1) out[o] = d;
      else if (out[o] != d) return false;
    }
  }
  return true;
}
// Element strides of `s` against the broadcast shape `out` (0 where the dimension is broadcast).
std::vector<size_t> broadcast_strides(const std::vector<int64_t>& s, const std::vector<int64_t>& out) {
  std::vector<size_t> st(out.size(), 0);
  size_t stride = 1;
  for (size_t i = s.size(); i-- > 0;) {
    const size_t o = out.size() - s.size() + i;
    st[o] = s[i] == 1 ? 0 : stride;
    stride *= size_t(s[i]);
  }
  return st;
}
// Calls fn(out_index, offsets...) over the broadcast shape for up to three inputs.
template <class Fn>
void for_broadcast(const std::vector<int64_t>& out, const std::vector<std::vector<size_t>>& strides, Fn fn) {
  const size_t rank = out.size(), n_in = strides.size();
  size_t total = 1;
  for (int64_t d : out) total *= size_t(d);
  std::vector<size_t> idx(rank, 0);
  size_t off[3] = {0, 0, 0};
  for (size_t i = 0; i < total; ++i) {
    fn(i, off);
    for (size_t d = rank; d-- > 0;) {
      ++idx[d];
      for (size_t k = 0; k < n_in; ++k) off[k] += strides[k][d];
      if (idx[d] < size_t(out[d])) break;
      for (size_t k = 0; k < n_in; ++k) off[k] -= strides[k][d] * size_t(out[d]);
      idx[d] = 0;
    }
  }
}

int64_t norm_axis(int64_t axis, size_t rank) { return axis < 0 ? axis + int64_t(rank) : axis; }
std::vector<int64_t> as_ints(const Tensor& t) {
  std::vector<int64_t> v(t.count());
  for (size_t i = 0; i < v.size(); ++i) v[i] = int64_t(t.get(i));
  return v;
}

// ---------------------------------------------------------------------------------------------
// The worker pool for the large Gemm: the calling thread takes the first share, workers the rest.
class Pool {
 public:
  ~Pool() { stop(); }
  void start(int workers, void (*on_start)(int)) {
    stop();
    quit_ = false;
    for (int i = 0; i < workers; ++i)
      threads_.emplace_back([this, i, on_start] {
        if (on_start) on_start(i);
        uint64_t seen = 0;
        for (;;) {
          std::function<void(int, int)> job;
          int parts;
          {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
            job = job_;
            parts = parts_;
          }
          job(i + 1, parts);
          if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(m_);
            done_cv_.notify_one();
          }
        }
      });
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(m_);
      quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();
  }
  int size() const { return int(threads_.size()) + 1; }
  // job(part, parts) for part 0..parts-1; part 0 on the calling thread.
  void run(const std::function<void(int, int)>& job) {
    const int parts = size();
    if (parts == 1) { job(0, 1); return; }
    {
      std::lock_guard<std::mutex> lk(m_);
      job_ = job;
      parts_ = parts;
      pending_.store(parts - 1, std::memory_order_release);
      ++generation_;
    }
    cv_.notify_all();
    job(0, parts);
    std::unique_lock<std::mutex> lk(m_);
    done_cv_.wait(lk, [&] { return pending_.load(std::memory_order_acquire) == 0; });
  }

 private:
  std::vector<std::thread> threads_;
  std::mutex m_;
  std::condition_variable cv_, done_cv_;
  std::function<void(int, int)> job_;
  int parts_ = 1;
  uint64_t generation_ = 0;
  std::atomic<int> pending_{0};
  bool quit_ = false;
};

// y[0..n) += a * row[0..n), row in half or single precision.
inline void axpy_half(float a, const uint16_t* row, float* y, size_t n) {
  size_t j = 0;
#if ONNX_LITE_NEON
  const float32x4_t va = vdupq_n_f32(a);
  for (; j + 8 <= n; j += 8) {
    const float16x8_t h = vreinterpretq_f16_u16(vld1q_u16(row + j));
    vst1q_f32(y + j, vfmaq_f32(vld1q_f32(y + j), va, vcvt_f32_f16(vget_low_f16(h))));
    vst1q_f32(y + j + 4, vfmaq_f32(vld1q_f32(y + j + 4), va, vcvt_high_f32_f16(h)));
  }
#endif
  for (; j < n; ++j) y[j] += a * half_to_float(row[j]);
}
// Four rows at once: y += a0*r0 + a1*r1 + a2*r2 + a3*r3 (one pass over y).
inline void axpy4_half(const float* a, const uint16_t* const* r, float* y, size_t n) {
  size_t j = 0;
#if ONNX_LITE_NEON
  const float32x4_t a0 = vdupq_n_f32(a[0]), a1 = vdupq_n_f32(a[1]), a2 = vdupq_n_f32(a[2]), a3 = vdupq_n_f32(a[3]);
  for (; j + 8 <= n; j += 8) {
    float32x4_t lo = vld1q_f32(y + j), hi = vld1q_f32(y + j + 4);
    float16x8_t h = vreinterpretq_f16_u16(vld1q_u16(r[0] + j));
    lo = vfmaq_f32(lo, a0, vcvt_f32_f16(vget_low_f16(h))); hi = vfmaq_f32(hi, a0, vcvt_high_f32_f16(h));
    h = vreinterpretq_f16_u16(vld1q_u16(r[1] + j));
    lo = vfmaq_f32(lo, a1, vcvt_f32_f16(vget_low_f16(h))); hi = vfmaq_f32(hi, a1, vcvt_high_f32_f16(h));
    h = vreinterpretq_f16_u16(vld1q_u16(r[2] + j));
    lo = vfmaq_f32(lo, a2, vcvt_f32_f16(vget_low_f16(h))); hi = vfmaq_f32(hi, a2, vcvt_high_f32_f16(h));
    h = vreinterpretq_f16_u16(vld1q_u16(r[3] + j));
    lo = vfmaq_f32(lo, a3, vcvt_f32_f16(vget_low_f16(h))); hi = vfmaq_f32(hi, a3, vcvt_high_f32_f16(h));
    vst1q_f32(y + j, lo);
    vst1q_f32(y + j + 4, hi);
  }
#endif
  for (; j < n; ++j)
    y[j] += a[0] * half_to_float(r[0][j]) + a[1] * half_to_float(r[1][j]) + a[2] * half_to_float(r[2][j]) +
            a[3] * half_to_float(r[3][j]);
}
inline void axpy_float(float a, const float* row, float* y, size_t n) {
  size_t j = 0;
#if ONNX_LITE_NEON
  const float32x4_t va = vdupq_n_f32(a);
  for (; j + 4 <= n; j += 4) vst1q_f32(y + j, vfmaq_f32(vld1q_f32(y + j), va, vld1q_f32(row + j)));
#endif
  for (; j < n; ++j) y[j] += a * row[j];
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct Model::Impl {
  std::vector<uint8_t> file;                     // the .onnx bytes: half-precision weights are views into it
  std::map<std::string, std::string> meta;
  std::unordered_map<std::string, int> ids;
  std::vector<Tensor> values;
  std::vector<bool> constant;
  std::vector<std::string> value_names;
  std::vector<Node> nodes;
  std::vector<int> inputs, outputs;
  std::vector<int> half_alias;                   // value id -> the half-precision constant it stands for (-1 none)
  Pool pool;
  int min_columns = 512;
  uint64_t macs = 0;
  std::vector<int> nz_rows;                      // scratch: the non-zero rows of a Gemm input
  std::vector<float> scratch;                    // scratch: transposed or converted operands

  int id(const std::string& name) {
    if (name.empty()) return -1;
    auto it = ids.find(name);
    if (it != ids.end()) return it->second;
    const int n = int(values.size());
    ids[name] = n;
    values.emplace_back();
    constant.push_back(false);
    value_names.push_back(name);
    half_alias.push_back(-1);
    return n;
  }
  bool tensor_from(const RawTensor& r, Tensor& t, bool allow_view, std::string* error) {
    if (r.external) { if (error) *error = "external tensor data is not supported (" + r.name + ")"; return false; }
    const size_t es = dtype_size(r.type);
    if (!es) { if (error) *error = "unsupported tensor type " + std::to_string(int(r.type)) + " (" + r.name + ")"; return false; }
    t.reset(r.type, r.dims);
    const size_t n = t.count();
    if (r.raw) {
      if (r.raw_size != n * es) { if (error) *error = "tensor " + r.name + " has the wrong size"; return false; }
      if (allow_view && r.type == DType::Float16) {
        t.storage.clear();
        t.storage.shrink_to_fit();
        t.view = r.raw;
        t.view_bytes = r.raw_size;
      } else if (n) {
        std::memcpy(t.mutable_data(), r.raw, n * es);
      }
    } else if (r.type == DType::Float) {
      if (r.floats.size() != n) { if (error) *error = "tensor " + r.name + " has the wrong size"; return false; }
      if (n) std::memcpy(t.mutable_data(), r.floats.data(), n * 4);
    } else {
      if (r.ints.size() != n) { if (error) *error = "tensor " + r.name + " has the wrong size"; return false; }
      if (r.type == DType::Float16) {
        for (size_t i = 0; i < n; ++i) { const uint16_t h = uint16_t(r.ints[i]); std::memcpy(t.mutable_data() + i * 2, &h, 2); }
      } else {
        for (size_t i = 0; i < n; ++i) t.set(i, double(r.ints[i]));
      }
    }
    return true;
  }

  bool parse(std::string* error, std::vector<std::string>& in_names, std::vector<std::string>& out_names) {
    Reader model{file.data(), file.data() + file.size()};
    Reader graph{nullptr, nullptr, true};
    uint32_t n, w;
    while (model.field(n, w)) {
      if (n == 7 && w == 2) graph = model.sub();
      else if (n == 14 && w == 2) {
        Reader e = model.sub();
        std::string k, v;
        uint32_t en, ew;
        while (e.field(en, ew)) {
          if (en == 1) k = e.str();
          else if (en == 2) v = e.str();
          else e.skip(ew);
        }
        meta[k] = v;
      } else model.skip(w);
    }
    if (model.bad || graph.bad || !graph.p) { if (error) *error = "not an ONNX model"; return false; }
    std::vector<std::string> graph_inputs;
    std::vector<RawTensor> inits;
    while (graph.field(n, w)) {
      switch (n) {
        case 1: {   // node
          Reader nr = graph.sub();
          Node node{};
          std::vector<std::string> ins, outs;
          std::string op_type, domain;
          std::vector<Attr> attrs;
          uint32_t fn, fw;
          while (nr.field(fn, fw)) {
            switch (fn) {
              case 1: ins.push_back(nr.str()); break;
              case 2: outs.push_back(nr.str()); break;
              case 3: node.name = nr.str(); break;
              case 4: op_type = nr.str(); break;
              case 5: attrs.push_back(parse_attr(nr.sub())); break;
              case 7: domain = nr.str(); break;
              default: nr.skip(fw); break;
            }
          }
          if (nr.bad) { if (error) *error = "bad node"; return false; }
          if (!domain.empty() && domain != "ai.onnx") { if (error) *error = "operator domain " + domain + " is not supported"; return false; }
          if (!op_from(op_type, node.op)) { if (error) *error = "operator " + op_type + " is not supported"; return false; }
          node.op_name = op_type;
          for (const Attr& a : attrs) {
            if (a.name == "axis") { node.axis = a.i; node.has_axis = true; }
            else if (a.name == "keepdims") node.keepdims = a.i;
            else if (a.name == "to") node.to = a.i;
            else if (a.name == "transA") node.transA = a.i;
            else if (a.name == "transB") node.transB = a.i;
            else if (a.name == "alpha") node.alpha = a.f;
            else if (a.name == "beta") node.beta = a.f;
            else if (a.name == "select_last_index") node.select_last = a.i;
            else if (a.name == "num_outputs") node.num_outputs = a.i;
            else if (a.name == "noop_with_empty_axes") node.noop_empty = a.i;
            else if (a.name == "allowzero") node.allowzero = a.i;
            else if (a.name == "approximate") node.approximate = a.s;
            else if (a.name == "axes") { node.axes_attr = a.ints; node.has_axes_attr = true; }
            else if (a.name == "split") { node.split_attr = a.ints; node.has_split_attr = true; }
            else if (a.name == "value" && a.has_t) node.value = a.t;
            else if (a.name == "min" || a.name == "max") { if (error) *error = "Clip with attributes (opset < 11) is not supported"; return false; }
          }
          for (const std::string& s : ins) node.in.push_back(id(s));
          for (const std::string& s : outs) node.out.push_back(id(s));
          nodes.push_back(std::move(node));
          break;
        }
        case 5: inits.push_back(parse_tensor(graph.sub())); break;
        case 11: case 12: {   // input, output: ValueInfoProto, name = field 1
          Reader v = graph.sub();
          uint32_t vn, vw;
          std::string name;
          while (v.field(vn, vw)) { if (vn == 1) name = v.str(); else v.skip(vw); }
          (n == 11 ? graph_inputs : out_names).push_back(name);
          break;
        }
        default: graph.skip(w); break;
      }
    }
    if (graph.bad) { if (error) *error = "bad graph"; return false; }
    for (const RawTensor& r : inits) {
      const int v = id(r.name);
      if (!tensor_from(r, values[size_t(v)], true, error)) return false;
      constant[size_t(v)] = true;
    }
    for (const std::string& s : graph_inputs) {
      const int v = id(s);
      if (constant[size_t(v)]) continue;   // an initializer listed as an input (older exporters)
      in_names.push_back(s);
      inputs.push_back(v);
    }
    for (const std::string& s : out_names) outputs.push_back(id(s));
    return true;
  }

  // ---- operators ----
  Tensor& V(int i) { return values[size_t(i)]; }
  const Tensor* in(const Node& nd, size_t k) {
    if (k >= nd.in.size() || nd.in[k] < 0) return nullptr;
    return &values[size_t(nd.in[k])];
  }

  bool binary(const Node& nd, std::string* error) {
    const size_t n_in = nd.in.size();
    if (n_in < 1) return false;
    std::vector<const std::vector<int64_t>*> shapes;
    for (size_t k = 0; k < n_in; ++k) shapes.push_back(&V(nd.in[k]).shape);
    std::vector<int64_t> shape;
    if (!broadcast_shape(shapes, shape)) { if (error) *error = nd.op_name + " " + nd.name + ": shapes do not broadcast"; return false; }
    const Tensor& a = V(nd.in[0]);
    DType out_t = a.type;
    switch (nd.op) {
      case Op::Less: case Op::GreaterOrEqual: case Op::Greater: case Op::LessOrEqual: case Op::Equal: out_t = DType::Bool; break;
      default: break;
    }
    // Max and Min take any number of inputs: fold them pairwise.
    if ((nd.op == Op::Max || nd.op == Op::Min) && n_in > 2) {
      Tensor acc = V(nd.in[0]);
      for (size_t k = 1; k < n_in; ++k) {
        Tensor next;
        if (!pair(nd.op, acc, V(nd.in[k]), next, a.type, error)) return false;
        acc = std::move(next);
      }
      V(nd.out[0]) = std::move(acc);
      return true;
    }
    if (n_in == 1) { V(nd.out[0]) = a; return true; }
    Tensor& out = V(nd.out[0]);
    return pair(nd.op, a, V(nd.in[1]), out, out_t, error);
  }
  static bool pair(Op op, const Tensor& a, const Tensor& b, Tensor& out, DType out_t, std::string* error) {
    std::vector<int64_t> shape;
    if (!broadcast_shape({&a.shape, &b.shape}, shape)) { if (error) *error = "shapes do not broadcast"; return false; }
    // `out` may be one of the inputs only through pairwise folding, which uses a separate tensor.
    out.reset(out_t, shape);
    const size_t total = out.count();
    // Fast path: float, same shape or a scalar on one side.
    if (a.type == DType::Float && b.type == DType::Float && out_t == DType::Float) {
      const float* x = a.as<float>();
      const float* y = b.as<float>();
      float* o = out.as<float>();
      const size_t na = a.count(), nb = b.count();
      if ((na == total || na == 1) && (nb == total || nb == 1)) {
        const size_t sa = na == 1 ? 0 : 1, sb = nb == 1 ? 0 : 1;
        switch (op) {
          case Op::Add: for (size_t i = 0; i < total; ++i) o[i] = x[i * sa] + y[i * sb]; return true;
          case Op::Sub: for (size_t i = 0; i < total; ++i) o[i] = x[i * sa] - y[i * sb]; return true;
          case Op::Mul: for (size_t i = 0; i < total; ++i) o[i] = x[i * sa] * y[i * sb]; return true;
          case Op::Div: for (size_t i = 0; i < total; ++i) o[i] = x[i * sa] / y[i * sb]; return true;
          case Op::Max: for (size_t i = 0; i < total; ++i) o[i] = std::max(x[i * sa], y[i * sb]); return true;
          case Op::Min: for (size_t i = 0; i < total; ++i) o[i] = std::min(x[i * sa], y[i * sb]); return true;
          default: break;
        }
      }
    }
    const auto sa = broadcast_strides(a.shape, shape), sb = broadcast_strides(b.shape, shape);
    const bool integer = a.type == DType::Int32 || a.type == DType::Int64 || a.type == DType::UInt8 || a.type == DType::Int8;
    for_broadcast(shape, {sa, sb}, [&](size_t i, const size_t* off) {
      const double x = a.get(off[0]), y = b.get(off[1]);
      double r = 0;
      switch (op) {
        case Op::Add: r = x + y; break;
        case Op::Sub: r = x - y; break;
        case Op::Mul: r = x * y; break;
        case Op::Div: r = integer ? (y != 0 ? std::trunc(x / y) : 0) : x / y; break;
        case Op::Max: r = std::max(x, y); break;
        case Op::Min: r = std::min(x, y); break;
        case Op::Less: r = x < y; break;
        case Op::Greater: r = x > y; break;
        case Op::LessOrEqual: r = x <= y; break;
        case Op::GreaterOrEqual: r = x >= y; break;
        case Op::Equal: r = x == y; break;
        case Op::Or: r = (x != 0) || (y != 0); break;
        case Op::And: r = (x != 0) && (y != 0); break;
        case Op::BitwiseAnd: r = double(int64_t(x) & int64_t(y)); break;
        default: break;
      }
      if (out.type == DType::Float) { const float f = float(r); std::memcpy(out.mutable_data() + i * 4, &f, 4); }
      else out.set(i, r);
    });
    // Float results computed through double: recompute in single precision for the arithmetic operators so they round
    // as a float operation would.
    if (out.type == DType::Float && a.type == DType::Float && b.type == DType::Float &&
        (op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div)) {
      const float* x = a.as<float>();
      const float* y = b.as<float>();
      float* o = out.as<float>();
      for_broadcast(shape, {sa, sb}, [&](size_t i, const size_t* off) {
        switch (op) {
          case Op::Add: o[i] = x[off[0]] + y[off[1]]; break;
          case Op::Sub: o[i] = x[off[0]] - y[off[1]]; break;
          case Op::Mul: o[i] = x[off[0]] * y[off[1]]; break;
          default: o[i] = x[off[0]] / y[off[1]]; break;
        }
      });
    }
    return true;
  }

  bool where(const Node& nd, std::string* error) {
    const Tensor& c = V(nd.in[0]);
    const Tensor& x = V(nd.in[1]);
    const Tensor& y = V(nd.in[2]);
    std::vector<int64_t> shape;
    if (!broadcast_shape({&c.shape, &x.shape, &y.shape}, shape)) { if (error) *error = "Where " + nd.name + ": shapes do not broadcast"; return false; }
    Tensor& out = V(nd.out[0]);
    out.reset(x.type, shape);
    const size_t es = dtype_size(x.type);
    const uint8_t* cd = c.data();
    const uint8_t* xd = x.data();
    const uint8_t* yd = y.data();
    uint8_t* od = out.mutable_data();
    for_broadcast(shape, {broadcast_strides(c.shape, shape), broadcast_strides(x.shape, shape), broadcast_strides(y.shape, shape)},
                  [&](size_t i, const size_t* off) {
                    std::memcpy(od + i * es, cd[off[0]] ? xd + off[1] * es : yd + off[2] * es, es);
                  });
    return true;
  }

  bool unary(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    Tensor& out = V(nd.out[0]);
    if (nd.op == Op::Not) {
      out.reset(DType::Bool, a.shape);
      for (size_t i = 0; i < a.count(); ++i) out.set(i, a.get(i) == 0);
      return true;
    }
    if (a.type != DType::Float) {
      if (nd.op == Op::Neg) { out.reset(a.type, a.shape); for (size_t i = 0; i < a.count(); ++i) out.set(i, -a.get(i)); return true; }
      if (error) *error = nd.op_name + " " + nd.name + " on " + dtype_name(a.type) + " is not supported";
      return false;
    }
    out.reset(DType::Float, a.shape);
    const float* x = a.as<float>();
    float* o = out.as<float>();
    const size_t n = a.count();
    switch (nd.op) {
      case Op::Neg: for (size_t i = 0; i < n; ++i) o[i] = -x[i]; break;
      case Op::Sqrt: for (size_t i = 0; i < n; ++i) o[i] = std::sqrt(x[i]); break;
      case Op::Log: for (size_t i = 0; i < n; ++i) o[i] = std::log(x[i]); break;
      case Op::Exp: for (size_t i = 0; i < n; ++i) o[i] = std::exp(x[i]); break;
      case Op::Relu: for (size_t i = 0; i < n; ++i) o[i] = std::max(x[i], 0.0f); break;
      case Op::Sigmoid:
        for (size_t i = 0; i < n; ++i) o[i] = x[i] >= 0 ? 1.0f / (1.0f + std::exp(-x[i])) : std::exp(x[i]) / (1.0f + std::exp(x[i]));
        break;
      case Op::Tanh: for (size_t i = 0; i < n; ++i) o[i] = std::tanh(x[i]); break;
      case Op::Gelu:
        if (nd.approximate == "tanh") {
          const float k = std::sqrt(2.0f / 3.14159265358979323846f);
          for (size_t i = 0; i < n; ++i) o[i] = 0.5f * x[i] * (1.0f + std::tanh(k * (x[i] + 0.044715f * x[i] * x[i] * x[i])));
        } else {
          for (size_t i = 0; i < n; ++i) o[i] = 0.5f * x[i] * (1.0f + std::erf(x[i] * 0.70710678118654752440f));
        }
        break;
      default: return false;
    }
    return true;
  }

  bool clip(const Node& nd) {
    const Tensor& a = V(nd.in[0]);
    const Tensor* lo = in(nd, 1);
    const Tensor* hi = in(nd, 2);
    Tensor& out = V(nd.out[0]);
    out.reset(a.type, a.shape);
    const double l = lo && lo->count() ? lo->get(0) : -INFINITY, h = hi && hi->count() ? hi->get(0) : INFINITY;
    if (a.type == DType::Float) {
      const float lf = float(l), hf = float(h);
      const float* x = a.as<float>();
      float* o = out.as<float>();
      for (size_t i = 0; i < a.count(); ++i) o[i] = std::min(std::max(x[i], lf), hf);
    } else {
      for (size_t i = 0; i < a.count(); ++i) out.set(i, std::min(std::max(a.get(i), l), h));
    }
    return true;
  }

  bool cast(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    const DType to = DType(int32_t(nd.to));
    if (!dtype_size(to)) { if (error) *error = "Cast to type " + std::to_string(nd.to) + " is not supported"; return false; }
    Tensor& out = V(nd.out[0]);
    if (&a == &out) return true;
    out.reset(to, a.shape);
    const size_t n = a.count();
    if (a.type == to) { std::memcpy(out.mutable_data(), a.data(), a.bytes()); return true; }
    if (a.type == DType::Float16 && to == DType::Float) {
      const uint8_t* src = a.data();
      float* o = out.as<float>();
      for (size_t i = 0; i < n; ++i) { uint16_t h; std::memcpy(&h, src + i * 2, 2); o[i] = half_to_float(h); }
      return true;
    }
    if (to == DType::Float16) { if (error) *error = "Cast to float16 is not supported"; return false; }
    for (size_t i = 0; i < n; ++i) {
      double v = a.get(i);
      if (to == DType::Int32 || to == DType::Int64 || to == DType::Int8 || to == DType::UInt8) v = std::trunc(v);
      out.set(i, v);
    }
    return true;
  }

  bool reshape(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    std::vector<int64_t> shape = as_ints(V(nd.in[1]));
    size_t known = 1;
    int infer = -1;
    for (size_t i = 0; i < shape.size(); ++i) {
      if (shape[i] == 0 && !nd.allowzero) shape[i] = i < a.shape.size() ? a.shape[i] : 1;
      if (shape[i] == -1) infer = int(i);
      else known *= size_t(shape[i]);
    }
    if (infer >= 0) shape[size_t(infer)] = known ? int64_t(a.count() / known) : 0;
    return copy_as(nd, a, shape, error);
  }
  bool copy_as(const Node& nd, const Tensor& a, const std::vector<int64_t>& shape, std::string* error) {
    Tensor& out = V(nd.out[0]);
    size_t n = 1;
    for (int64_t d : shape) n *= size_t(d);
    if (n != a.count()) { if (error) *error = nd.op_name + " " + nd.name + ": element count changes"; return false; }
    if (&out == &a) { out.shape = shape; return true; }
    out.reset(a.type, shape);
    std::memcpy(out.mutable_data(), a.data(), a.bytes());
    return true;
  }
  std::vector<int64_t> axes_of(const Node& nd, size_t input) {
    if (const Tensor* t = in(nd, input)) return as_ints(*t);
    return nd.axes_attr;
  }
  bool squeeze(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    std::vector<int64_t> axes = axes_of(nd, 1);
    std::vector<int64_t> shape;
    for (size_t i = 0; i < a.shape.size(); ++i) {
      bool drop = false;
      if (axes.empty()) drop = a.shape[i] == 1;
      else for (int64_t ax : axes) if (norm_axis(ax, a.shape.size()) == int64_t(i)) drop = true;
      if (!drop) shape.push_back(a.shape[i]);
    }
    return copy_as(nd, a, shape, error);
  }
  bool unsqueeze(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    std::vector<int64_t> axes = axes_of(nd, 1);
    const size_t rank = a.shape.size() + axes.size();
    for (int64_t& ax : axes) ax = norm_axis(ax, rank);
    std::vector<int64_t> shape;
    size_t k = 0;
    for (size_t i = 0; i < rank; ++i) {
      if (std::find(axes.begin(), axes.end(), int64_t(i)) != axes.end()) shape.push_back(1);
      else shape.push_back(k < a.shape.size() ? a.shape[k++] : 1);
    }
    return copy_as(nd, a, shape, error);
  }
  bool expand(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    const std::vector<int64_t> want = as_ints(V(nd.in[1]));
    std::vector<int64_t> shape;
    if (!broadcast_shape({&a.shape, &want}, shape)) { if (error) *error = "Expand " + nd.name + ": shapes do not broadcast"; return false; }
    Tensor& out = V(nd.out[0]);
    out.reset(a.type, shape);
    const size_t es = dtype_size(a.type);
    const uint8_t* src = a.data();
    uint8_t* dst = out.mutable_data();
    for_broadcast(shape, {broadcast_strides(a.shape, shape)}, [&](size_t i, const size_t* off) { std::memcpy(dst + i * es, src + off[0] * es, es); });
    return true;
  }
  bool concat(const Node& nd, std::string* error) {
    const Tensor& first = V(nd.in[0]);
    const size_t rank = first.shape.size();
    const int64_t axis = norm_axis(nd.axis, rank);
    std::vector<int64_t> shape = first.shape;
    shape[size_t(axis)] = 0;
    for (int i : nd.in) {
      if (i < 0) continue;
      const Tensor& t = V(i);
      if (t.shape.size() != rank || t.type != first.type) { if (error) *error = "Concat " + nd.name + ": inputs differ"; return false; }
      shape[size_t(axis)] += t.shape[size_t(axis)];
    }
    Tensor& out = V(nd.out[0]);
    out.reset(first.type, shape);
    const size_t es = dtype_size(first.type);
    size_t outer = 1, inner = 1;
    for (size_t d = 0; d < size_t(axis); ++d) outer *= size_t(shape[d]);
    for (size_t d = size_t(axis) + 1; d < rank; ++d) inner *= size_t(shape[d]);
    const size_t out_row = size_t(shape[size_t(axis)]) * inner * es;
    size_t at = 0;
    for (int i : nd.in) {
      if (i < 0) continue;
      const Tensor& t = V(i);
      const size_t row = size_t(t.shape[size_t(axis)]) * inner * es;
      for (size_t o = 0; o < outer; ++o) std::memcpy(out.mutable_data() + o * out_row + at, t.data() + o * row, row);
      at += row;
    }
    return true;
  }
  bool split(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    const size_t rank = a.shape.size();
    const int64_t axis = norm_axis(nd.axis, rank);
    const int64_t dim = a.shape[size_t(axis)];
    std::vector<int64_t> parts;
    if (const Tensor* s = in(nd, 1)) parts = as_ints(*s);
    else if (nd.has_split_attr) parts = nd.split_attr;
    else {
      const int64_t n = int64_t(nd.out.size()), each = (dim + n - 1) / n;
      for (int64_t k = 0, left = dim; k < n; ++k) { parts.push_back(std::min(each, left)); left -= parts.back(); }
    }
    if (parts.size() != nd.out.size()) { if (error) *error = "Split " + nd.name + ": wrong number of parts"; return false; }
    const size_t es = dtype_size(a.type);
    size_t outer = 1, inner = 1;
    for (size_t d = 0; d < size_t(axis); ++d) outer *= size_t(a.shape[d]);
    for (size_t d = size_t(axis) + 1; d < rank; ++d) inner *= size_t(a.shape[d]);
    const size_t in_row = size_t(dim) * inner * es;
    size_t at = 0;
    for (size_t k = 0; k < parts.size(); ++k) {
      std::vector<int64_t> shape = a.shape;
      shape[size_t(axis)] = parts[k];
      Tensor& out = V(nd.out[k]);
      out.reset(a.type, shape);
      const size_t row = size_t(parts[k]) * inner * es;
      for (size_t o = 0; o < outer; ++o) std::memcpy(out.mutable_data() + o * row, a.data() + o * in_row + at, row);
      at += row;
    }
    return true;
  }
  bool gather(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    const Tensor& idx = V(nd.in[1]);
    const size_t rank = a.shape.size();
    const int64_t axis = norm_axis(nd.axis, rank);
    std::vector<int64_t> shape;
    for (size_t d = 0; d < size_t(axis); ++d) shape.push_back(a.shape[d]);
    for (int64_t d : idx.shape) shape.push_back(d);
    for (size_t d = size_t(axis) + 1; d < rank; ++d) shape.push_back(a.shape[d]);
    Tensor& out = V(nd.out[0]);
    out.reset(a.type, shape);
    const size_t es = dtype_size(a.type);
    size_t outer = 1, inner = 1;
    for (size_t d = 0; d < size_t(axis); ++d) outer *= size_t(a.shape[d]);
    for (size_t d = size_t(axis) + 1; d < rank; ++d) inner *= size_t(a.shape[d]);
    const int64_t dim = a.shape[size_t(axis)];
    const size_t n_idx = idx.count();
    for (size_t o = 0; o < outer; ++o)
      for (size_t k = 0; k < n_idx; ++k) {
        int64_t j = int64_t(idx.get(k));
        if (j < 0) j += dim;
        if (j < 0 || j >= dim) { if (error) *error = "Gather " + nd.name + ": index out of range"; return false; }
        std::memcpy(out.mutable_data() + ((o * n_idx + k) * inner) * es, a.data() + ((o * size_t(dim) + size_t(j)) * inner) * es, inner * es);
      }
    return true;
  }
  bool onehot(const Node& nd, std::string* error) {
    const Tensor& idx = V(nd.in[0]);
    const int64_t depth = int64_t(V(nd.in[1]).get(0));
    const Tensor& vals = V(nd.in[2]);
    if (depth <= 0 || vals.count() != 2) { if (error) *error = "OneHot " + nd.name + ": bad depth or values"; return false; }
    const size_t rank = idx.shape.size() + 1;
    const int64_t axis = nd.has_axis ? norm_axis(nd.axis, rank) : int64_t(rank - 1);
    std::vector<int64_t> shape = idx.shape;
    shape.insert(shape.begin() + axis, depth);
    Tensor& out = V(nd.out[0]);
    out.reset(vals.type, shape);
    const double off = vals.get(0), on = vals.get(1);
    const size_t total = out.count();
    if (vals.type == DType::Float) { float* o = out.as<float>(); const float f = float(off); for (size_t i = 0; i < total; ++i) o[i] = f; }
    else for (size_t i = 0; i < total; ++i) out.set(i, off);
    size_t outer = 1, inner = 1;
    for (size_t d = 0; d < size_t(axis); ++d) outer *= size_t(idx.shape[d]);
    for (size_t d = size_t(axis); d < idx.shape.size(); ++d) inner *= size_t(idx.shape[d]);
    for (size_t o = 0; o < outer; ++o)
      for (size_t i = 0; i < inner; ++i) {
        int64_t j = int64_t(idx.get(o * inner + i));
        if (j < 0) j += depth;
        if (j < 0 || j >= depth) continue;   // out of range: every value stays off
        out.set((o * size_t(depth) + size_t(j)) * inner + i, on);
      }
    return true;
  }
  bool reduce(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    std::vector<int64_t> axes = axes_of(nd, 1);
    const size_t rank = a.shape.size();
    std::vector<bool> red(rank, false);
    if (axes.empty()) {
      if (nd.noop_empty) { Tensor& out = V(nd.out[0]); out = a; return true; }
      red.assign(rank, true);
    }
    for (int64_t ax : axes) {
      const int64_t k = norm_axis(ax, rank);
      if (k < 0 || k >= int64_t(rank)) { if (error) *error = nd.op_name + " " + nd.name + ": bad axis"; return false; }
      red[size_t(k)] = true;
    }
    std::vector<int64_t> kept, out_shape;
    for (size_t d = 0; d < rank; ++d) {
      kept.push_back(red[d] ? 1 : a.shape[d]);
      if (!red[d]) out_shape.push_back(a.shape[d]);
      else if (nd.keepdims) out_shape.push_back(1);
    }
    size_t out_n = 1, group = 1;
    for (size_t d = 0; d < rank; ++d) { out_n *= size_t(kept[d]); if (red[d]) group *= size_t(a.shape[d]); }
    std::vector<double> acc(out_n, 0.0);
    // Map each input element to its output slot (kept dims only).
    const auto st = broadcast_strides(kept, a.shape);
    for_broadcast(a.shape, {st}, [&](size_t i, const size_t* off) { acc[off[0]] += a.get(i); });
    Tensor& out = V(nd.out[0]);
    out.reset(a.type, out_shape);
    for (size_t i = 0; i < out_n; ++i) out.set(i, nd.op == Op::ReduceMean ? acc[i] / double(group) : acc[i]);
    // Single-precision sums for float input, in index order (as a straightforward kernel adds them).
    if (a.type == DType::Float) {
      std::vector<float> facc(out_n, 0.0f);
      const float* x = a.as<float>();
      for_broadcast(a.shape, {st}, [&](size_t i, const size_t* off) { facc[off[0]] += x[i]; });
      float* o = out.as<float>();
      for (size_t i = 0; i < out_n; ++i) o[i] = nd.op == Op::ReduceMean ? facc[i] / float(group) : facc[i];
    }
    return true;
  }
  bool argmax(const Node& nd) {
    const Tensor& a = V(nd.in[0]);
    const size_t rank = a.shape.size();
    const int64_t axis = norm_axis(nd.axis, rank);
    std::vector<int64_t> shape;
    size_t outer = 1, inner = 1;
    for (size_t d = 0; d < rank; ++d) {
      if (int64_t(d) == axis) { if (nd.keepdims) shape.push_back(1); continue; }
      shape.push_back(a.shape[d]);
      if (int64_t(d) < axis) outer *= size_t(a.shape[d]);
      else inner *= size_t(a.shape[d]);
    }
    const size_t dim = size_t(a.shape[size_t(axis)]);
    Tensor& out = V(nd.out[0]);
    out.reset(DType::Int64, shape);
    int64_t* o = out.as<int64_t>();
    for (size_t p = 0; p < outer; ++p)
      for (size_t q = 0; q < inner; ++q) {
        size_t best = 0;
        double bv = a.get(p * dim * inner + q);
        for (size_t k = 1; k < dim; ++k) {
          const double v = a.get((p * dim + k) * inner + q);
          if (v > bv || (nd.select_last && v == bv)) { bv = v; best = k; }
        }
        o[p * inner + q] = int64_t(best);
      }
    return true;
  }

  // Y = alpha * A' B' + beta * C. A is float; B float or (a view of) half-precision weights.
  bool gemm(const Node& nd, std::string* error) {
    const Tensor& a = V(nd.in[0]);
    const int b_id = half_alias[size_t(nd.in[1])] >= 0 ? half_alias[size_t(nd.in[1])] : nd.in[1];
    const Tensor& b = V(b_id);
    const Tensor* c = nd.op == Op::Gemm ? in(nd, 2) : nullptr;
    if (a.type != DType::Float || (b.type != DType::Float && b.type != DType::Float16) || a.shape.size() != 2 || b.shape.size() != 2) {
      if (error) *error = nd.op_name + " " + nd.name + ": operands must be 2-D float";
      return false;
    }
    const bool ta = nd.op == Op::Gemm && nd.transA, tb = nd.op == Op::Gemm && nd.transB;
    const size_t M = size_t(ta ? a.shape[1] : a.shape[0]), K = size_t(ta ? a.shape[0] : a.shape[1]);
    const size_t Kb = size_t(tb ? b.shape[1] : b.shape[0]), N = size_t(tb ? b.shape[0] : b.shape[1]);
    if (K != Kb) { if (error) *error = nd.op_name + " " + nd.name + ": inner sizes differ"; return false; }
    Tensor& out = V(nd.out[0]);
    out.reset(DType::Float, {int64_t(M), int64_t(N)});
    float* y = out.as<float>();
    const float alpha = nd.op == Op::Gemm ? nd.alpha : 1.0f, beta = nd.op == Op::Gemm ? nd.beta : 0.0f;
    std::fill(y, y + M * N, 0.0f);
    const float* A = a.as<float>();
    std::vector<float> at;
    if (ta) {   // rare: make A row-major M x K
      at.resize(M * K);
      for (size_t i = 0; i < M; ++i) for (size_t k = 0; k < K; ++k) at[i * K + k] = A[k * M + i];
      A = at.data();
    }
    if (tb) {   // B stored N x K: dot products
      for (size_t i = 0; i < M; ++i)
        for (size_t j = 0; j < N; ++j) {
          float s = 0;
          for (size_t k = 0; k < K; ++k) s += A[i * K + k] * float(b.get(j * K + k));
          y[i * N + j] = s;
        }
      macs += M * N * K;
    } else {
      const bool half = b.type == DType::Float16;
      const uint16_t* Bh = half ? reinterpret_cast<const uint16_t*>(b.data()) : nullptr;
      const float* Bf = half ? nullptr : b.as<float>();
      for (size_t i = 0; i < M; ++i) {
        const float* ai = A + i * K;
        nz_rows.clear();
        for (size_t k = 0; k < K; ++k) if (ai[k] != 0.0f) nz_rows.push_back(int(k));
        macs += nz_rows.size() * N;
        float* yi = y + i * N;
        const auto cols = [&](size_t j0, size_t j1) {
          const size_t w = j1 - j0;
          if (w == 0) return;
          size_t r = 0;
          if (half) {
            for (; r + 4 <= nz_rows.size(); r += 4) {
              const float av[4] = {ai[nz_rows[r]], ai[nz_rows[r + 1]], ai[nz_rows[r + 2]], ai[nz_rows[r + 3]]};
              const uint16_t* rows[4] = {Bh + size_t(nz_rows[r]) * N + j0, Bh + size_t(nz_rows[r + 1]) * N + j0,
                                         Bh + size_t(nz_rows[r + 2]) * N + j0, Bh + size_t(nz_rows[r + 3]) * N + j0};
              axpy4_half(av, rows, yi + j0, w);
            }
            for (; r < nz_rows.size(); ++r) axpy_half(ai[nz_rows[r]], Bh + size_t(nz_rows[r]) * N + j0, yi + j0, w);
          } else {
            for (; r < nz_rows.size(); ++r) axpy_float(ai[nz_rows[r]], Bf + size_t(nz_rows[r]) * N + j0, yi + j0, w);
          }
        };
        if (pool.size() > 1 && N >= size_t(min_columns) && nz_rows.size() * N >= 65536) {
          pool.run([&](int part, int parts) {
            // Columns in blocks of 8 so that each share stays on the vector width.
            const size_t blocks = (N + 7) / 8;
            const size_t b0 = blocks * size_t(part) / size_t(parts), b1 = blocks * size_t(part + 1) / size_t(parts);
            cols(std::min(N, b0 * 8), std::min(N, b1 * 8));
          });
        } else {
          cols(0, N);
        }
      }
    }
    if (alpha != 1.0f) for (size_t i = 0; i < M * N; ++i) y[i] *= alpha;
    if (c && beta != 0.0f) {
      if (c->type != DType::Float) { if (error) *error = "Gemm " + nd.name + ": C must be float"; return false; }
      const std::vector<int64_t> shape = {int64_t(M), int64_t(N)};
      std::vector<int64_t> bs;
      if (!broadcast_shape({&c->shape, &shape}, bs) || bs != shape) { if (error) *error = "Gemm " + nd.name + ": C does not broadcast"; return false; }
      const float* cd = c->as<float>();
      for_broadcast(shape, {broadcast_strides(c->shape, shape)}, [&](size_t i, const size_t* off) { y[i] += beta * cd[off[0]]; });
    }
    return true;
  }

  bool exec(Node& nd, std::string* error) {
    for (int i : nd.in)
      if (i >= 0 && values[size_t(i)].type == DType::Undefined && half_alias[size_t(i)] < 0) {
        if (error) *error = nd.op_name + " " + nd.name + ": input " + value_names[size_t(i)] + " has no value";
        return false;
      }
    switch (nd.op) {
      case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Max: case Op::Min: case Op::Less: case Op::GreaterOrEqual:
      case Op::Greater: case Op::LessOrEqual: case Op::Equal: case Op::Or: case Op::And: case Op::BitwiseAnd:
        return binary(nd, error);
      case Op::Where: return where(nd, error);
      case Op::Not: case Op::Neg: case Op::Sqrt: case Op::Log: case Op::Exp: case Op::Relu: case Op::Sigmoid: case Op::Tanh: case Op::Gelu:
        return unary(nd, error);
      case Op::Clip: return clip(nd);
      case Op::Cast: return cast(nd, error);
      case Op::Reshape: return reshape(nd, error);
      case Op::Squeeze: return squeeze(nd, error);
      case Op::Unsqueeze: return unsqueeze(nd, error);
      case Op::Expand: return expand(nd, error);
      case Op::Concat: return concat(nd, error);
      case Op::Split: return split(nd, error);
      case Op::Gather: return gather(nd, error);
      case Op::OneHot: return onehot(nd, error);
      case Op::ReduceMean: case Op::ReduceSum: return reduce(nd, error);
      case Op::ArgMax: return argmax(nd);
      case Op::Gemm: case Op::MatMul: return gemm(nd, error);
      case Op::Identity: { const Tensor& a = V(nd.in[0]); return copy_as(nd, a, a.shape, error); }
      case Op::Shape: {
        const Tensor& a = V(nd.in[0]);
        Tensor& out = V(nd.out[0]);
        const std::vector<int64_t> s = a.shape;
        out.reset(DType::Int64, {int64_t(s.size())});
        for (size_t i = 0; i < s.size(); ++i) out.as<int64_t>()[i] = s[i];
        return true;
      }
      case Op::Constant: return tensor_from(nd.value, V(nd.out[0]), false, error);
    }
    return false;
  }

  // Every node whose inputs are all constant is run once now. A Cast of half-precision weights that only Gemm/MatMul
  // read as their B operand is not run: the Gemm reads the half-precision constant itself.
  bool fold(std::string* error, Stats& stats) {
    std::vector<std::vector<std::pair<const Node*, size_t>>> readers(values.size());
    for (const Node& nd : nodes)
      for (size_t k = 0; k < nd.in.size(); ++k) if (nd.in[k] >= 0) readers[size_t(nd.in[k])].push_back({&nd, k});
    for (int o : outputs) readers[size_t(o)].push_back({nullptr, 0});
    for (Node& nd : nodes) {
      bool all = true;
      for (int i : nd.in) if (i >= 0 && !constant[size_t(i)]) all = false;
      if (!all) continue;
      if (nd.op == Op::Cast && nd.to == int64_t(DType::Float) && V(nd.in[0]).type == DType::Float16) {
        bool only_b = !readers[size_t(nd.out[0])].empty();
        for (const auto& [r, k] : readers[size_t(nd.out[0])])
          if (!r || !(r->op == Op::Gemm || r->op == Op::MatMul) || k != 1 || (r->op == Op::Gemm && r->transB)) only_b = false;
        if (only_b) {
          half_alias[size_t(nd.out[0])] = nd.in[0];
          constant[size_t(nd.out[0])] = true;
          nd.folded = true;
          ++stats.folded;
          continue;
        }
      }
      if (!exec(nd, error)) return false;
      for (int o : nd.out) constant[size_t(o)] = true;
      nd.folded = true;
      ++stats.folded;
    }
    // Constants nobody reads any more (the inputs of folded nodes) are freed, views included.
    std::vector<bool> needed(values.size(), false);
    for (const Node& nd : nodes) {
      if (nd.folded) continue;
      for (int i : nd.in) if (i >= 0) { needed[size_t(i)] = true; if (half_alias[size_t(i)] >= 0) needed[size_t(half_alias[size_t(i)])] = true; }
    }
    for (int o : outputs) needed[size_t(o)] = true;
    stats.weight_bytes = 0;
    for (size_t v = 0; v < values.size(); ++v) {
      if (!constant[v]) continue;
      if (!needed[v]) { values[v] = Tensor(); continue; }
      stats.weight_bytes += values[v].view ? values[v].view_bytes : values[v].bytes();
    }
    return true;
  }
};

Model::Model() : impl_(new Impl) {}
Model::~Model() = default;

bool Model::load_file(const std::string& path, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { if (error) *error = "cannot open " + path; return false; }
  const std::streamoff size = f.tellg();
  if (size <= 0 || size > (std::streamoff(1) << 31)) { if (error) *error = "bad size " + path; return false; }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) { if (error) *error = "cannot read " + path; return false; }
  return load_memory(std::move(bytes), error);
}

bool Model::load_memory(std::vector<uint8_t> bytes, std::string* error) {
  impl_.reset(new Impl);
  input_names_.clear();
  output_names_.clear();
  stats_ = Stats{};
  impl_->file = std::move(bytes);
  if (!impl_->parse(error, input_names_, output_names_)) return false;
  stats_.nodes = impl_->nodes.size();
  if (!impl_->fold(error, stats_)) return false;
  stats_.runtime_nodes = stats_.nodes - stats_.folded;
  return true;
}

const std::string* Model::metadata(const std::string& key) const {
  auto it = impl_->meta.find(key);
  return it == impl_->meta.end() ? nullptr : &it->second;
}

bool Model::set_input(const std::string& name, DType t, const std::vector<int64_t>& s, const void* data, size_t bytes) {
  auto it = impl_->ids.find(name);
  if (it == impl_->ids.end() || impl_->constant[size_t(it->second)]) return false;
  Tensor& v = impl_->values[size_t(it->second)];
  v.reset(t, s);
  if (v.bytes() != bytes) return false;
  std::memcpy(v.mutable_data(), data, bytes);
  return true;
}

bool Model::run(std::string* error) {
  impl_->macs = 0;
  for (Node& nd : impl_->nodes) {
    if (nd.folded) continue;
    if (!impl_->exec(nd, error)) return false;
  }
  stats_.gemm_macs = impl_->macs;
  return true;
}

const Tensor* Model::output(const std::string& name) const {
  auto it = impl_->ids.find(name);
  return it == impl_->ids.end() ? nullptr : &impl_->values[size_t(it->second)];
}

void Model::set_threads(int threads, int min_columns) {
  impl_->min_columns = std::max(8, min_columns);
  impl_->pool.start(std::max(0, threads - 1), nullptr);
}
void Model::set_thread_start(void (*fn)(int)) {
  const int workers = impl_->pool.size() - 1;
  impl_->pool.start(workers, fn);
}

}  // namespace onnx_lite
