// The onnxruntime-shaped calls of include/onnxruntime_c_api.h (what melee-unlocked's phillip_ai.cpp calls) on this
// frontend's own engine, onnx_lite. A session is one onnx_lite::Model; a value is a tensor that either borrows the
// caller's buffer (inputs) or holds the engine's output (outputs, copied out of the model after the step).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "onnxruntime_c_api.h"
#include "onnx_lite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Thread placement for the engine's Gemm workers on the console (features/proai.cpp); weak: none elsewhere.
__attribute__((weak)) void fe_proai_place_worker(int index);
__attribute__((weak)) int fe_proai_threads();

struct OrtStatus { std::string message; };
struct OrtEnv { int unused = 0; };
struct OrtSessionOptions { int threads = 1; };
struct OrtSession { onnx_lite::Model model; std::string path; };
struct OrtModelMetadata { const OrtSession* session = nullptr; };
struct OrtMemoryInfo { int unused = 0; };
struct OrtValue {
  void* data = nullptr;            // inputs: the caller's buffer
  size_t bytes = 0;
  ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  std::vector<int64_t> shape;
  std::vector<uint8_t> owned;      // outputs
};

namespace {
OrtStatus* fail(const std::string& m) { return new OrtStatus{m}; }

void* alloc_fn(OrtAllocator*, size_t size) { return std::malloc(size); }
void free_fn(OrtAllocator*, void* p) { std::free(p); }
OrtAllocator g_alloc = {1, alloc_fn, free_fn};
char* dup(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

const char* get_error_message(const OrtStatus* s) { return s ? s->message.c_str() : ""; }
void release_status(OrtStatus* s) { delete s; }
OrtStatus* create_env(OrtLoggingLevel, const char*, OrtEnv** out) { *out = new OrtEnv; return nullptr; }
OrtStatus* create_session_options(OrtSessionOptions** out) { *out = new OrtSessionOptions; return nullptr; }
OrtStatus* set_intra(OrtSessionOptions* o, int n) { o->threads = n < 1 ? 1 : n; return nullptr; }
OrtStatus* set_inter(OrtSessionOptions*, int) { return nullptr; }
OrtStatus* set_level(OrtSessionOptions*, GraphOptimizationLevel) { return nullptr; }
OrtStatus* create_session(const OrtEnv*, const char* path, const OrtSessionOptions* o, OrtSession** out) {
  std::unique_ptr<OrtSession> s(new OrtSession);
  std::string error;
  if (!s->model.load_file(path, &error)) return fail(error);
  s->path = path;
  // MELEE_PRO_AI_THREADS=<n> (tests, timing) overrides the session's own count.
  // The app's setting (settings.ini proai_threads, default 1) rather than upstream's request: on the console the Gemm
  // workers share cores 0 and 1 with the renderer and the decoder.
  int threads = &fe_proai_threads ? fe_proai_threads() : (o ? o->threads : 1);
  if (const char* v = std::getenv("MELEE_PRO_AI_THREADS")) threads = std::max(1, std::atoi(v));
  s->model.set_threads(threads);
  if (threads > 1 && &fe_proai_place_worker) s->model.set_thread_start(fe_proai_place_worker);
  *out = s.release();
  return nullptr;
}
void release_session_options(OrtSessionOptions* o) { delete o; }
void release_session(OrtSession* s) { delete s; }
OrtStatus* get_allocator(OrtAllocator** out) { *out = &g_alloc; return nullptr; }
OrtStatus* get_metadata(const OrtSession* s, OrtModelMetadata** out) { *out = new OrtModelMetadata{s}; return nullptr; }
OrtStatus* lookup_custom(const OrtModelMetadata* m, OrtAllocator*, const char* key, char** value) {
  const std::string* v = m->session->model.metadata(key);
  *value = v ? dup(*v) : nullptr;
  return nullptr;
}
void release_metadata(OrtModelMetadata* m) { delete m; }
OrtStatus* input_count(const OrtSession* s, size_t* out) { *out = s->model.input_names().size(); return nullptr; }
OrtStatus* input_name(const OrtSession* s, size_t i, OrtAllocator*, char** value) {
  if (i >= s->model.input_names().size()) return fail("input index out of range");
  *value = dup(s->model.input_names()[i]);
  return nullptr;
}
OrtStatus* create_cpu_info(OrtAllocatorType, OrtMemType, OrtMemoryInfo** out) { *out = new OrtMemoryInfo; return nullptr; }
void release_info(OrtMemoryInfo* i) { delete i; }
OrtStatus* create_tensor(const OrtMemoryInfo*, void* data, size_t bytes, const int64_t* shape, size_t len, ONNXTensorElementDataType type,
                         OrtValue** out) {
  OrtValue* v = new OrtValue;
  v->data = data;
  v->bytes = bytes;
  v->type = type;
  v->shape.assign(shape, shape + len);
  *out = v;
  return nullptr;
}
// MELEE_PRO_AI_RECORD=<file> (diagnosis): every step's packed inputs and the engine's outputs, in tools/proai_record.py's
// format (one session only; MELEE_PRO_AI_RECORD_MODEL picks it), so tools/proai_replay.py can run the very inputs the game built through onnxruntime.
void record(OrtSession* s, const char* const* in_names, const OrtValue* const* ins, size_t n_in, const char* const* out_names,
            OrtValue* const* outs, size_t n_out) {
  static const char* path = std::getenv("MELEE_PRO_AI_RECORD");
  static FILE* f = nullptr;
  static const OrtSession* owner = nullptr;
  static long steps_at = 0;
  static uint32_t steps = 0;
  if (!path) return;
  // MELEE_PRO_AI_RECORD_MODEL=<part of a file name>: that policy's session only (default: the first one stepped).
  static const char* only = std::getenv("MELEE_PRO_AI_RECORD_MODEL");
  if (!f && only && s->path.find(only) == std::string::npos) return;
  if (!f) {
    f = std::fopen(path, "wb");
    if (!f) { path = nullptr; return; }
    owner = s;
    if (FILE* m = std::fopen((std::string(path) + ".model").c_str(), "w")) { std::fprintf(m, "%s\n", s->path.c_str()); std::fclose(m); }
    std::fwrite("PAIREC1\n", 1, 8, f);
    const auto packs = [&](const char* const* names, const OrtValue* const* v, size_t n) {
      const uint32_t count = uint32_t(n);
      std::fwrite(&count, 4, 1, f);
      for (size_t i = 0; i < n; ++i) {
        const uint16_t len = uint16_t(std::strlen(names[i]));
        const int32_t type = int32_t(v[i]->type);
        const size_t es = onnx_lite::dtype_size(onnx_lite::DType(type));
        const uint32_t width = uint32_t(es ? v[i]->bytes / es : 0);
        std::fwrite(&len, 2, 1, f); std::fwrite(names[i], 1, len, f); std::fwrite(&type, 4, 1, f); std::fwrite(&width, 4, 1, f);
      }
    };
    packs(in_names, ins, n_in);
    packs(out_names, outs, n_out);
    steps_at = std::ftell(f);
    std::fwrite(&steps, 4, 1, f);
  }
  if (s != owner) return;
  for (size_t i = 0; i < n_in; ++i) std::fwrite(ins[i]->data, 1, ins[i]->bytes, f);
  for (size_t o = 0; o < n_out; ++o) std::fwrite(outs[o]->data, 1, outs[o]->bytes, f);
  ++steps;
  const long end = std::ftell(f);
  std::fseek(f, steps_at, SEEK_SET);
  std::fwrite(&steps, 4, 1, f);
  std::fseek(f, end, SEEK_SET);
  std::fflush(f);
}

OrtStatus* run(OrtSession* s, const OrtRunOptions*, const char* const* in_names, const OrtValue* const* ins, size_t n_in,
               const char* const* out_names, size_t n_out, OrtValue** outs) {
  for (size_t i = 0; i < n_in; ++i)
    if (!s->model.set_input(in_names[i], onnx_lite::DType(int32_t(ins[i]->type)), ins[i]->shape, ins[i]->data, ins[i]->bytes))
      return fail(std::string("input ") + in_names[i] + " does not fit the model");
  std::string error;
  if (!s->model.run(&error)) return fail(error);
  for (size_t o = 0; o < n_out; ++o) {
    const onnx_lite::Tensor* t = s->model.output(out_names[o]);
    if (!t || t->type == onnx_lite::DType::Undefined) return fail(std::string("no output ") + out_names[o]);
    OrtValue* v = new OrtValue;
    v->type = ONNXTensorElementDataType(int32_t(t->type));
    v->shape = t->shape;
    v->owned.assign(t->data(), t->data() + t->bytes());
    v->data = v->owned.data();
    v->bytes = v->owned.size();
    outs[o] = v;
  }
  record(s, in_names, ins, n_in, out_names, outs, n_out);
  return nullptr;
}
OrtStatus* mutable_data(OrtValue* v, void** out) { *out = v->data; return nullptr; }
void release_value(OrtValue* v) { delete v; }

const OrtApi g_api = {get_error_message, release_status, create_env, create_session_options, set_intra, set_inter, set_level,
                      create_session, release_session_options, release_session, get_allocator, get_metadata, lookup_custom,
                      release_metadata, input_count, input_name, create_cpu_info, release_info, create_tensor, run, mutable_data,
                      release_value};
const OrtApi* get_api(uint32_t) { return &g_api; }
const char* version() { return "onnx_lite (Melee-NX-NetPlay)"; }
const OrtApiBase g_base = {get_api, version};
}  // namespace

const OrtApiBase* OrtGetApiBase(void) { return &g_base; }
