// Streamline / DLSS integration (see gx_streamline.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_streamline.h"
#include "host.h"
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cmath>
#include <cstring>
#include <atomic>
#include <string>

#ifdef GX_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_security.h>
#endif

namespace gx {

const char* dlss_mode_name(DlssMode m) {
  switch (m) {
    case DlssMode::Off: return "Native";
    case DlssMode::DLAA: return "DLAA";
    case DlssMode::Quality: return "DLSS Quality";
    case DlssMode::Balanced: return "DLSS Balanced";
    case DlssMode::Performance: return "DLSS Performance";
    case DlssMode::UltraPerformance: return "DLSS Ultra Performance";
  }
  return "?";
}

namespace streamline {

void jitter(uint32_t index, float* jx, float* jy) {
  auto halton = [](uint32_t i, uint32_t base) { float f = 1.0f, r = 0.0f; while (i > 0) { f /= base; r += f * (i % base); i /= base; } return r; };
  uint32_t i = (index % 32) + 1;
  *jx = halton(i, 2) - 0.5f;
  *jy = halton(i, 3) - 0.5f;
}

#ifndef GX_STREAMLINE
bool init(const std::wstring&) { host::log("dlss: built without the Streamline SDK"); return false; }
void shutdown() {}
bool available() { return false; }
long create_dxgi_factory2(uint32_t flags, const void* riid, void** out) { return CreateDXGIFactory2(flags, *(const IID*)riid, out); }
long d3d12_create_device(void* adapter, int fl, const void* riid, void** out) { return D3D12CreateDevice((IUnknown*)adapter, (D3D_FEATURE_LEVEL)fl, *(const IID*)riid, out); }
void set_device(ID3D12Device*) {}
bool dlss_supported(IDXGIAdapter*) { return false; }
bool dlss_optimal_size(DlssMode, uint32_t, uint32_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*) { return false; }
bool dlss_set_options(DlssMode, uint32_t, uint32_t) { return false; }
void new_frame(uint32_t) {}
bool set_constants(const FrameConstants&) { return false; }
bool evaluate(ID3D12GraphicsCommandList*, const EvaluateInputs&) { return false; }
#else

namespace {
HMODULE g_module = nullptr;
bool g_ready = false, g_dlss_ok = false;
sl::FrameToken* g_token = nullptr;
sl::ViewportHandle g_viewport{0u};
typedef HRESULT(WINAPI* PFunCreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFunD3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFunCreateDXGIFactory2 g_create_factory2 = nullptr;
PFunD3D12CreateDevice g_create_device = nullptr;
sl::DLSSMode g_mode = sl::DLSSMode::eOff;
uint32_t g_out_w = 0, g_out_h = 0;
float g_prev_proj[16] = {};
bool g_have_prev = false;

void log_callback(sl::LogType type, const char* msg) {
  if (type == sl::LogType::eInfo) return;
  static std::atomic<uint64_t> count{0};
  uint64_t n = count.fetch_add(1);
  if (n >= 20 && n % 1000 != 0) return;   // a failing feature logs every frame; keep the log usable
  std::string s(msg);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  host::log("streamline: %s", s.c_str());
}

sl::DLSSMode to_sl(DlssMode m) {
  switch (m) {
    case DlssMode::DLAA: return sl::DLSSMode::eDLAA;
    case DlssMode::Quality: return sl::DLSSMode::eMaxQuality;
    case DlssMode::Balanced: return sl::DLSSMode::eBalanced;
    case DlssMode::Performance: return sl::DLSSMode::eMaxPerformance;
    case DlssMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
    default: return sl::DLSSMode::eOff;
  }
}

// Row-major 4x4 inverse (general).
bool invert4x4(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
  inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
  inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
  inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
  inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
  inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
  inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
  inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
  inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
  inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
  inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
  inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
  inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
  inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
  inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
  inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
  float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
  if (std::fabs(det) < 1e-20f) return false;
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
  return true;
}
sl::float4x4 to_sl_matrix(const float m[16]) {
  sl::float4x4 r;
  for (int i = 0; i < 4; ++i) r.setRow(i, sl::float4(m[4 * i], m[4 * i + 1], m[4 * i + 2], m[4 * i + 3]));
  return r;
}
void mul4x4(const float a[16], const float b[16], float out[16]) {
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { float s = 0; for (int k = 0; k < 4; ++k) s += a[4 * r + k] * b[4 * k + c]; out[4 * r + c] = s; }
}
}  // namespace

bool init(const std::wstring& exe_dir) {
  std::wstring path = exe_dir + L"\\sl.interposer.dll";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { host::log("dlss: sl.interposer.dll not found next to the executable; DLSS unavailable"); return false; }
  if (!sl::security::verifyEmbeddedSignature(path.c_str())) { host::log("dlss: sl.interposer.dll signature check failed; DLSS unavailable"); return false; }
  g_module = LoadLibraryW(path.c_str());
  if (!g_module) { host::log("dlss: cannot load sl.interposer.dll (%lu)", GetLastError()); return false; }
  g_create_factory2 = (PFunCreateDXGIFactory2)GetProcAddress(g_module, "CreateDXGIFactory2");
  g_create_device = (PFunD3D12CreateDevice)GetProcAddress(g_module, "D3D12CreateDevice");
  static const wchar_t* plugin_dirs[1];
  static std::wstring dir_copy;
  dir_copy = exe_dir;
  plugin_dirs[0] = dir_copy.c_str();
  static const sl::Feature features[] = {sl::kFeatureDLSS};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = plugin_dirs;
  pref.numPathsToPlugins = 1;
  static std::wstring logs; logs = exe_dir + L"\\streamline-logs"; CreateDirectoryW(logs.c_str(), nullptr);
  pref.pathToLogsAndData = logs.c_str();
  pref.logMessageCallback = log_callback;
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = 1;
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "melee-port";
  pref.applicationId = 231313132;   // NVIDIA sample application id: valid for development builds of non-registered titles
  pref.projectId = "5d3d9a7e-1c2b-4c9e-9a0e-2f6b8c1d4e70";
  pref.renderAPI = sl::RenderAPI::eD3D12;
  sl::Result res = slInit(pref, sl::kSDKVersion);
  if (res != sl::Result::eOk) { host::log("dlss: slInit failed (%d); DLSS unavailable", (int)res); return false; }
  g_ready = true;
  host::log("dlss: Streamline initialised (SDK %llu)", (unsigned long long)sl::kSDKVersion);
  return true;
}

void shutdown() {
  if (g_ready) { slShutdown(); g_ready = false; }
}
bool available() { return g_ready && g_dlss_ok; }

long create_dxgi_factory2(uint32_t flags, const void* riid, void** out) {
  if (g_ready && g_create_factory2) return g_create_factory2(flags, *(const IID*)riid, out);
  return CreateDXGIFactory2(flags, *(const IID*)riid, out);
}
long d3d12_create_device(void* adapter, int fl, const void* riid, void** out) {
  if (g_ready && g_create_device) return g_create_device((IUnknown*)adapter, (D3D_FEATURE_LEVEL)fl, *(const IID*)riid, out);
  return D3D12CreateDevice((IUnknown*)adapter, (D3D_FEATURE_LEVEL)fl, *(const IID*)riid, out);
}
void set_device(ID3D12Device* device) {
  if (!g_ready) return;
  sl::Result res = slSetD3DDevice(device);
  if (res != sl::Result::eOk) { host::log("dlss: slSetD3DDevice failed (%d)", (int)res); g_dlss_ok = false; return; }
  sl::FeatureRequirements req{};
  g_dlss_ok = slGetFeatureRequirements(sl::kFeatureDLSS, req) == sl::Result::eOk;
  host::log("dlss: %s", g_dlss_ok ? "available (off until selected under Upscaling in PC settings)" : "feature failed to initialise; native rendering only");
}
bool dlss_supported(IDXGIAdapter* adapter) {
  if (!g_ready) return false;
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) return false;
  sl::AdapterInfo info{};
  info.deviceLUID = (uint8_t*)&desc.AdapterLuid;
  info.deviceLUIDSizeInBytes = sizeof(LUID);
  sl::Result res = slIsFeatureSupported(sl::kFeatureDLSS, info);
  if (res != sl::Result::eOk) {
    const char* why = res == sl::Result::eErrorOSOutOfDate ? "OS out of date" : res == sl::Result::eErrorDriverOutOfDate ? "driver out of date"
                    : res == sl::Result::eErrorNoSupportedAdapterFound || res == sl::Result::eErrorAdapterNotSupported ? "adapter not supported" : "not supported";
    host::log("dlss: not available on this adapter (%s, %d)", why, (int)res);
  }
  g_dlss_ok = res == sl::Result::eOk;
  return g_dlss_ok;
}

bool dlss_optimal_size(DlssMode mode, uint32_t out_w, uint32_t out_h, uint32_t* rw, uint32_t* rh, uint32_t* min_w, uint32_t* min_h, uint32_t* max_w, uint32_t* max_h) {
  if (!available() || mode == DlssMode::Off) return false;
  sl::DLSSOptions o{};
  o.mode = to_sl(mode); o.outputWidth = out_w; o.outputHeight = out_h;
  sl::DLSSOptimalSettings s{};
  if (slDLSSGetOptimalSettings(o, s) != sl::Result::eOk) return false;
  *rw = s.optimalRenderWidth; *rh = s.optimalRenderHeight;
  *min_w = s.renderWidthMin; *min_h = s.renderHeightMin; *max_w = s.renderWidthMax; *max_h = s.renderHeightMax;
  return true;
}

bool dlss_set_options(DlssMode mode, uint32_t out_w, uint32_t out_h) {
  if (!available()) return false;
  sl::DLSSOptions o{};
  o.mode = to_sl(mode); o.outputWidth = out_w; o.outputHeight = out_h;
  o.colorBuffersHDR = sl::Boolean::eFalse;
  o.useAutoExposure = sl::Boolean::eTrue;
  o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = sl::DLSSPreset::ePresetK;
  o.ultraPerformancePreset = sl::DLSSPreset::ePresetF;
  sl::Result res = slDLSSSetOptions(g_viewport, o);
  if (res != sl::Result::eOk) { host::log("dlss: slDLSSSetOptions failed (%d)", (int)res); return false; }
  g_mode = o.mode; g_out_w = out_w; g_out_h = out_h;
  if (mode == DlssMode::Off) g_have_prev = false;
  return true;
}

void new_frame(uint32_t frame_index) {
  if (!available()) return;
  g_token = nullptr;
  if (slGetNewFrameToken(g_token, &frame_index) != sl::Result::eOk) g_token = nullptr;
}

bool set_constants(const FrameConstants& c) {
  if (!available() || !g_token) return false;
  sl::Constants k{};
  float inv[16];
  bool ok = invert4x4(c.projection, inv);
  k.cameraViewToClip = to_sl_matrix(c.projection);
  k.clipToCameraView = to_sl_matrix(ok ? inv : c.projection);
  float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // Motion vectors carry camera motion (they come from the per-draw model-view matrices), so the
  // clip-to-previous-clip transform only needs to describe the projection change, if any.
  float clip_to_prev[16] = {}, prev_to_clip[16] = {};
  if (g_have_prev && ok) { mul4x4(inv, g_prev_proj, clip_to_prev); if (!invert4x4(clip_to_prev, prev_to_clip)) std::memcpy(prev_to_clip, ident, sizeof ident); }
  else { std::memcpy(clip_to_prev, ident, sizeof ident); std::memcpy(prev_to_clip, ident, sizeof ident); }
  k.clipToPrevClip = to_sl_matrix(clip_to_prev);
  k.prevClipToClip = to_sl_matrix(prev_to_clip);
  k.clipToLensClip = to_sl_matrix(ident);
  k.jitterOffset = sl::float2(c.jitter_x, c.jitter_y);
  k.mvecScale = sl::float2(1.0f / (float)c.render_w, 1.0f / (float)c.render_h);
  k.cameraPinholeOffset = sl::float2(0, 0);
  k.cameraPos = sl::float3(0, 0, 0);
  k.cameraUp = sl::float3(0, 1, 0);
  k.cameraRight = sl::float3(1, 0, 0);
  k.cameraFwd = sl::float3(0, 0, -1);
  k.cameraNear = 1.0f; k.cameraFar = 10000.0f;
  k.cameraFOV = c.orthographic ? 0.0f : 2.0f * std::atan(1.0f / std::max(1e-6f, std::fabs(c.projection[5])));
  k.cameraAspectRatio = std::fabs(c.projection[5]) > 1e-6f ? std::fabs(c.projection[5] / std::max(1e-6f, std::fabs(c.projection[0]))) : 4.0f / 3.0f;
  k.depthInverted = sl::Boolean::eTrue;
  k.cameraMotionIncluded = sl::Boolean::eTrue;
  k.motionVectors3D = sl::Boolean::eFalse;
  k.reset = (c.reset || !g_have_prev) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  k.orthographicProjection = c.orthographic ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  k.motionVectorsDilated = sl::Boolean::eFalse;
  k.motionVectorsJittered = sl::Boolean::eFalse;
  sl::Result res = slSetConstants(k, *g_token, g_viewport);
  std::memcpy(g_prev_proj, c.projection, sizeof g_prev_proj);
  g_have_prev = true;
  if (res != sl::Result::eOk) { host::log("dlss: slSetConstants failed (%d)", (int)res); return false; }
  return true;
}

bool evaluate(ID3D12GraphicsCommandList* list, const EvaluateInputs& in) {
  if (!available() || !g_token) return false;
  sl::Resource color_in(sl::ResourceType::eTex2d, in.color_in, in.color_state);
  sl::Resource depth(sl::ResourceType::eTex2d, in.depth, in.depth_state);
  sl::Resource mvec(sl::ResourceType::eTex2d, in.mvec, in.mvec_state);
  sl::Resource color_out(sl::ResourceType::eTex2d, in.color_out, in.out_state);
  sl::Extent render_extent{in.in_top, in.in_left, in.in_w, in.in_h};
  sl::Extent out_extent{0, 0, in.out_w, in.out_h};
  sl::ResourceTag tags[] = {
      sl::ResourceTag(&color_in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent),
      sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent),
      sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent),
      sl::ResourceTag(&color_out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &out_extent),
  };
  const sl::BaseStructure* inputs[] = {&g_viewport, &tags[0], &tags[1], &tags[2], &tags[3]};
  sl::Result res = slEvaluateFeature(sl::kFeatureDLSS, *g_token, inputs, (uint32_t)(sizeof inputs / sizeof inputs[0]), list);
  if (res != sl::Result::eOk) {
    static int logged = 0;
    if (logged++ < 5) host::log("dlss: slEvaluateFeature failed (%d)", (int)res);
    return false;
  }
  return true;
}
#endif

}  // namespace streamline
}  // namespace gx
