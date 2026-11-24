// onnx_lite: a small inference engine for exactly the ONNX graphs Phillip's published policies use (slippi-ai by
// Vlad Firoiu, MIT; models on Hugging Face vladfi/phillip-models, MIT), so that "Fight a pro" runs on the Switch with
// no third-party runtime. Plain C++17, NEON where the target has it.
//
// What it does: reads the .onnx file (a minimal protobuf reader: the model's graph, its initializers, its metadata),
// folds every node whose inputs are all constant once at load, and runs the remaining nodes in the file's order on
// tensors that keep their storage between steps (no allocation after the first step). The weights stay in the file's
// half-precision form; the Gemm kernel widens them as it reads them and skips rows whose input is zero (the one-hot
// embeddings). Shapes are worked out at run time, so any static batch-1 graph of the supported operators runs.
// Operators: Add Sub Mul Div Max Min Where Less GreaterOrEqual Or And BitwiseAnd Neg Sqrt Log Sigmoid Tanh Gelu Clip
// Cast Reshape Squeeze Unsqueeze Expand Concat Split Gather OneHot ReduceMean ReduceSum ArgMax Gemm MatMul Identity.
// Anything else is refused at load with its name.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace onnx_lite {

enum class DType : int32_t { Undefined = 0, Float = 1, UInt8 = 2, Int8 = 3, Int32 = 6, Int64 = 7, Bool = 9, Float16 = 10, Double = 11 };
size_t dtype_size(DType t);
const char* dtype_name(DType t);

struct Tensor {
  DType type = DType::Undefined;
  std::vector<int64_t> shape;
  // Owned storage (8-byte aligned), or a view into the model file (`view`, half-precision weights only).
  std::vector<uint64_t> storage;
  const uint8_t* view = nullptr;
  size_t view_bytes = 0;

  size_t count() const;
  size_t bytes() const { return count() * dtype_size(type); }
  const uint8_t* data() const { return view ? view : reinterpret_cast<const uint8_t*>(storage.data()); }
  uint8_t* mutable_data() { return reinterpret_cast<uint8_t*>(storage.data()); }
  template <class T> T* as() { return reinterpret_cast<T*>(storage.data()); }
  template <class T> const T* as() const { return reinterpret_cast<const T*>(data()); }
  // Shape and type set, storage sized (kept when it is already large enough: no allocation in the steady state).
  void reset(DType t, const std::vector<int64_t>& s);
  double get(size_t i) const;            // any type as a double (generic paths)
  void set(size_t i, double v);
};

struct Stats {
  size_t nodes = 0, folded = 0, runtime_nodes = 0;
  size_t weight_bytes = 0;        // initializers kept (views included)
  uint64_t gemm_macs = 0;         // multiply-adds the last step did in Gemm/MatMul (after skipping zero rows)
};

class Model {
 public:
  Model();
  ~Model();
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  bool load_file(const std::string& path, std::string* error);
  bool load_memory(std::vector<uint8_t> bytes, std::string* error);

  // The model's metadata_props value for `key` (slippi-ai keeps its JSON under "slippi_ai"); null when absent.
  const std::string* metadata(const std::string& key) const;

  // Graph inputs and outputs in the file's order.
  const std::vector<std::string>& input_names() const { return input_names_; }
  const std::vector<std::string>& output_names() const { return output_names_; }
  // Before run(): the input's bytes, as type `t` and shape `s` (copied).
  bool set_input(const std::string& name, DType t, const std::vector<int64_t>& s, const void* data, size_t bytes);
  bool run(std::string* error);
  const Tensor* output(const std::string& name) const;

  // Worker threads for the large Gemm (0 or 1: the calling thread only). Each Gemm with at least `min_columns` output
  // columns is split by columns between the calling thread and the workers.
  void set_threads(int threads, int min_columns = 512);
  // Called on each worker as it starts (thread placement on the console).
  void set_thread_start(void (*fn)(int index));
  const Stats& stats() const { return stats_; }

  struct Impl;
 private:
  std::unique_ptr<Impl> impl_;
  std::vector<std::string> input_names_, output_names_;
  Stats stats_;
};

// IEEE half to float (exact).
float half_to_float(uint16_t h);

}  // namespace onnx_lite
