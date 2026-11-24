// NOT onnxruntime. The few names of onnxruntime's C API that melee-unlocked's phillip_ai.cpp calls, declared here so
// that file compiles unchanged around them, and implemented on this frontend's own engine (../onnx_lite.cpp, in
// ../ort_lite.cpp). Only what phillip_ai.cpp uses exists; the names and argument orders follow the public C API so its
// calls read the same, but nothing of onnxruntime's code or headers is here.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#define ORT_API_VERSION 1
#define ORT_API_CALL

typedef enum ONNXTensorElementDataType {
  ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED = 0,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT = 1,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8 = 2,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8 = 3,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 = 6,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 = 7,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL = 9,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 = 10,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE = 11,
} ONNXTensorElementDataType;

typedef enum OrtLoggingLevel { ORT_LOGGING_LEVEL_VERBOSE, ORT_LOGGING_LEVEL_INFO, ORT_LOGGING_LEVEL_WARNING, ORT_LOGGING_LEVEL_ERROR, ORT_LOGGING_LEVEL_FATAL } OrtLoggingLevel;
typedef enum GraphOptimizationLevel { ORT_DISABLE_ALL = 0, ORT_ENABLE_BASIC = 1, ORT_ENABLE_EXTENDED = 2, ORT_ENABLE_ALL = 99 } GraphOptimizationLevel;
typedef enum OrtAllocatorType { OrtInvalidAllocator = -1, OrtDeviceAllocator = 0, OrtArenaAllocator = 1 } OrtAllocatorType;
typedef enum OrtMemType { OrtMemTypeCPUInput = -2, OrtMemTypeCPUOutput = -1, OrtMemTypeDefault = 0 } OrtMemType;

struct OrtStatus;
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtModelMetadata;
struct OrtMemoryInfo;
struct OrtValue;
struct OrtRunOptions;
typedef struct OrtAllocator {
  uint32_t version;
  void*(ORT_API_CALL* Alloc)(struct OrtAllocator* self, size_t size);
  void(ORT_API_CALL* Free)(struct OrtAllocator* self, void* p);
} OrtAllocator;

typedef struct OrtApi {
  const char*(ORT_API_CALL* GetErrorMessage)(const OrtStatus* status);
  void(ORT_API_CALL* ReleaseStatus)(OrtStatus* status);
  OrtStatus*(ORT_API_CALL* CreateEnv)(OrtLoggingLevel level, const char* logid, OrtEnv** out);
  OrtStatus*(ORT_API_CALL* CreateSessionOptions)(OrtSessionOptions** out);
  OrtStatus*(ORT_API_CALL* SetIntraOpNumThreads)(OrtSessionOptions* options, int threads);
  OrtStatus*(ORT_API_CALL* SetInterOpNumThreads)(OrtSessionOptions* options, int threads);
  OrtStatus*(ORT_API_CALL* SetSessionGraphOptimizationLevel)(OrtSessionOptions* options, GraphOptimizationLevel level);
  OrtStatus*(ORT_API_CALL* CreateSession)(const OrtEnv* env, const char* path, const OrtSessionOptions* options, OrtSession** out);
  void(ORT_API_CALL* ReleaseSessionOptions)(OrtSessionOptions* options);
  void(ORT_API_CALL* ReleaseSession)(OrtSession* session);
  OrtStatus*(ORT_API_CALL* GetAllocatorWithDefaultOptions)(OrtAllocator** out);
  OrtStatus*(ORT_API_CALL* SessionGetModelMetadata)(const OrtSession* session, OrtModelMetadata** out);
  OrtStatus*(ORT_API_CALL* ModelMetadataLookupCustomMetadataMap)(const OrtModelMetadata* metadata, OrtAllocator* allocator, const char* key, char** value);
  void(ORT_API_CALL* ReleaseModelMetadata)(OrtModelMetadata* metadata);
  OrtStatus*(ORT_API_CALL* SessionGetInputCount)(const OrtSession* session, size_t* out);
  OrtStatus*(ORT_API_CALL* SessionGetInputName)(const OrtSession* session, size_t index, OrtAllocator* allocator, char** value);
  OrtStatus*(ORT_API_CALL* CreateCpuMemoryInfo)(OrtAllocatorType type, OrtMemType mem_type, OrtMemoryInfo** out);
  void(ORT_API_CALL* ReleaseMemoryInfo)(OrtMemoryInfo* info);
  OrtStatus*(ORT_API_CALL* CreateTensorWithDataAsOrtValue)(const OrtMemoryInfo* info, void* data, size_t bytes, const int64_t* shape,
                                                            size_t shape_len, ONNXTensorElementDataType type, OrtValue** out);
  OrtStatus*(ORT_API_CALL* Run)(OrtSession* session, const OrtRunOptions* run_options, const char* const* input_names,
                                const OrtValue* const* inputs, size_t input_count, const char* const* output_names, size_t output_count,
                                OrtValue** outputs);
  OrtStatus*(ORT_API_CALL* GetTensorMutableData)(OrtValue* value, void** out);
  void(ORT_API_CALL* ReleaseValue)(OrtValue* value);
} OrtApi;

typedef struct OrtApiBase {
  const OrtApi*(ORT_API_CALL* GetApi)(uint32_t version);
  const char*(ORT_API_CALL* GetVersionString)(void);
} OrtApiBase;

// The engine behind it (ort_lite.cpp).
const OrtApiBase* OrtGetApiBase(void);
