// proai_check: runs the steps tools/proai_record.py recorded through onnxruntime again through the frontend's own
// inference engine (frontend/src/proai/onnx_lite.cpp) and compares the outputs, step by step, with the same inputs.
//
//   proai_check MODEL.onnx RECORD.rec [--threads N] [--repeat N]
//
// Prints, per output: the largest absolute difference of float outputs (and where), how many integer and boolean
// outputs differ, and the engine's step time. Exit code 0 when every float output is within the tolerance
// (|a - b| <= 1e-3 + 1e-3 * |b|) and at most 1% of the steps have a differing sampled (integer or boolean) output: the
// policy samples its controller by an argmax over logits plus noise, so two correct engines that add in a different
// order can still pick differently when two choices are within rounding of each other.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "onnx_lite.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct Pack { std::string name; onnx_lite::DType type; uint32_t width; };
bool read_packs(std::ifstream& f, std::vector<Pack>& out) {
  uint32_t n = 0;
  if (!f.read(reinterpret_cast<char*>(&n), 4) || n > 64) return false;
  for (uint32_t i = 0; i < n; ++i) {
    uint16_t len = 0;
    f.read(reinterpret_cast<char*>(&len), 2);
    std::string s(len, '\0');
    f.read(&s[0], len);
    int32_t t = 0;
    uint32_t w = 0;
    f.read(reinterpret_cast<char*>(&t), 4);
    f.read(reinterpret_cast<char*>(&w), 4);
    if (!f) return false;
    out.push_back({s, onnx_lite::DType(t), w});
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: proai_check MODEL.onnx RECORD.rec [--threads N] [--repeat N]\n"); return 2; }
  int threads = 1, repeat = 1;
  for (int i = 3; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--threads")) threads = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--repeat")) repeat = std::max(1, std::atoi(argv[i + 1]));
  }
  onnx_lite::Model model;
  std::string error;
  const auto t0 = std::chrono::steady_clock::now();
  if (!model.load_file(argv[1], &error)) { std::fprintf(stderr, "load: %s\n", error.c_str()); return 1; }
  const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  model.set_threads(threads);
  const auto& st = model.stats();
  std::printf("model %s: %zu nodes, %zu folded at load, %zu run per step, %.1f MB of weights kept, loaded in %.2f s\n", argv[1],
              st.nodes, st.folded, st.runtime_nodes, st.weight_bytes / 1e6, load_s);

  std::ifstream f(argv[2], std::ios::binary);
  char magic[8];
  if (!f.read(magic, 8) || std::memcmp(magic, "PAIREC1\n", 8)) { std::fprintf(stderr, "not a record file\n"); return 1; }
  std::vector<Pack> ins, outs;
  uint32_t steps = 0;
  if (!read_packs(f, ins) || !read_packs(f, outs) || !f.read(reinterpret_cast<char*>(&steps), 4)) { std::fprintf(stderr, "bad record header\n"); return 1; }
  struct Diff { double max_abs = 0, max_rel = 0, worst_ref = 0; uint32_t worst_step = 0; size_t mismatches = 0, steps_differing = 0, values = 0; bool tol_fail = false; };
  std::vector<Diff> diffs(outs.size());
  std::vector<std::vector<uint8_t>> in_bytes(ins.size()), ref(outs.size());
  std::vector<double> times;
  size_t steps_with_sample_diff = 0;
  for (uint32_t s = 0; s < steps; ++s) {
    for (size_t i = 0; i < ins.size(); ++i) {
      in_bytes[i].resize(ins[i].width * onnx_lite::dtype_size(ins[i].type));
      f.read(reinterpret_cast<char*>(in_bytes[i].data()), std::streamsize(in_bytes[i].size()));
    }
    for (size_t o = 0; o < outs.size(); ++o) {
      ref[o].resize(outs[o].width * onnx_lite::dtype_size(outs[o].type));
      f.read(reinterpret_cast<char*>(ref[o].data()), std::streamsize(ref[o].size()));
    }
    if (!f) { std::fprintf(stderr, "record ends early at step %u\n", s); return 1; }
    for (int r = 0; r < repeat; ++r) {
      for (size_t i = 0; i < ins.size(); ++i)
        if (!model.set_input(ins[i].name, ins[i].type, {1, int64_t(ins[i].width)}, in_bytes[i].data(), in_bytes[i].size())) {
          std::fprintf(stderr, "input %s refused\n", ins[i].name.c_str());
          return 1;
        }
      const auto a = std::chrono::steady_clock::now();
      if (!model.run(&error)) { std::fprintf(stderr, "step %u: %s\n", s, error.c_str()); return 1; }
      times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    bool sample_diff = false;
    for (size_t o = 0; o < outs.size(); ++o) {
      const onnx_lite::Tensor* t = model.output(outs[o].name);
      if (!t || t->count() != outs[o].width || t->type != outs[o].type) {
        std::fprintf(stderr, "output %s: missing or of another shape/type\n", outs[o].name.c_str());
        return 1;
      }
      Diff& d = diffs[o];
      size_t differing = 0;
      for (size_t k = 0; k < outs[o].width; ++k) {
        ++d.values;
        if (outs[o].type == onnx_lite::DType::Float) {
          float want, got;
          std::memcpy(&want, ref[o].data() + k * 4, 4);
          std::memcpy(&got, t->data() + k * 4, 4);
          const double e = std::fabs(double(got) - double(want));
          if (e > d.max_abs) { d.max_abs = e; d.worst_ref = want; d.worst_step = s; }
          if (std::fabs(want) > 1e-3) d.max_rel = std::max(d.max_rel, e / std::fabs(double(want)));
          if (e > 1e-3 + 1e-3 * std::fabs(double(want))) { d.tol_fail = true; ++differing; }
        } else {
          const size_t es = onnx_lite::dtype_size(outs[o].type);
          if (std::memcmp(ref[o].data() + k * es, t->data() + k * es, es)) { ++d.mismatches; ++differing; }
        }
      }
      if (differing) { ++d.steps_differing; if (outs[o].type != onnx_lite::DType::Float) sample_diff = true; }
    }
    if (sample_diff) ++steps_with_sample_diff;
  }
  bool ok = true;
  for (size_t o = 0; o < outs.size(); ++o) {
    const Diff& d = diffs[o];
    if (outs[o].type == onnx_lite::DType::Float)
      std::printf("  %-16s float x%-5u max |diff| %.3g (step %u, reference %.4g), max relative %.3g%s\n", outs[o].name.c_str(), outs[o].width,
                  d.max_abs, d.worst_step, d.worst_ref, d.max_rel, d.tol_fail ? "  OUT OF TOLERANCE" : "");
    else
      std::printf("  %-16s %s x%-3u %zu of %zu values differ, in %zu of %u steps\n", outs[o].name.c_str(), onnx_lite::dtype_name(outs[o].type),
                  outs[o].width, d.mismatches, d.values, d.steps_differing, steps);
    if (d.tol_fail) ok = false;
  }
  if (steps_with_sample_diff * 100 > steps) ok = false;
  std::sort(times.begin(), times.end());
  double sum = 0;
  for (double t : times) sum += t;
  std::printf("  sampled controller differs in %zu of %u steps; step time %.3f ms mean, %.3f median, %.3f p99, %.3f worst (%d thread%s)\n",
              steps_with_sample_diff, steps, sum / times.size(), times[times.size() / 2], times[times.size() * 99 / 100], times.back(),
              threads, threads == 1 ? "" : "s");
  std::printf("%s\n", ok ? "RESULT: same outputs within tolerance" : "RESULT: DIFFERENT");
  return ok ? 0 : 1;
}
