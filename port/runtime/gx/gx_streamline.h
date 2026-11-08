// NVIDIA Streamline (DLSS Super Resolution / DLAA) integration for the D3D12 backend.
// The SDK is optional at build time (GX_STREAMLINE); at run time the signed sl.interposer.dll next
// to the executable is loaded and D3D12/DXGI creation goes through its proxies so Streamline can
// manage the swap chain and command lists. Everything degrades to the native path when
// unavailable.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct IDXGIAdapter;

namespace gx {

enum class DlssMode : int { Off = 0, DLAA = 1, Quality = 2, Balanced = 3, Performance = 4, UltraPerformance = 5 };
const char* dlss_mode_name(DlssMode m);

namespace streamline {

// Loads the interposer and initialises Streamline with DLSS requested. Must run before any
// DXGI/D3D12 call. Returns false (with a logged reason) when Streamline cannot be used.
bool init(const std::wstring& exe_dir);
void shutdown();
bool available();

// D3D/DXGI creation proxies (fall back to the system functions when Streamline is off).
long create_dxgi_factory2(uint32_t flags, const void* riid, void** out);
long d3d12_create_device(void* adapter, int feature_level, const void* riid, void** out);
void set_device(ID3D12Device* device);
bool dlss_supported(IDXGIAdapter* adapter);

// Per-mode optimal render size for an output size. Returns false when DLSS is unavailable.
bool dlss_optimal_size(DlssMode mode, uint32_t out_w, uint32_t out_h, uint32_t* render_w, uint32_t* render_h,
                       uint32_t* min_w, uint32_t* min_h, uint32_t* max_w, uint32_t* max_h);
bool dlss_set_options(DlssMode mode, uint32_t out_w, uint32_t out_h);

// Frame flow on the render thread: new_frame() -> set_constants() -> ... draws ... -> evaluate().
struct FrameConstants {
  float projection[16];       // row-major clip = P * view (as the vertex shader dots rows with the position)
  float jitter_x, jitter_y;   // pixel-space jitter applied to this frame's projection
  uint32_t render_w, render_h;
  bool reset;                 // no relation to the previous frame (scene cut, first frame)
  bool orthographic;
};
void new_frame(uint32_t frame_index);
bool set_constants(const FrameConstants& c);
struct EvaluateInputs {
  ID3D12Resource* color_in; uint32_t color_state;      // render-resolution jittered color
  ID3D12Resource* depth; uint32_t depth_state;
  ID3D12Resource* mvec; uint32_t mvec_state;           // pixel-space motion vectors (previous - current)
  ID3D12Resource* color_out; uint32_t out_state;       // output-resolution result (UAV capable)
  uint32_t in_left, in_top, in_w, in_h;                // extent within the render-resolution textures
  uint32_t out_w, out_h;
};
bool evaluate(ID3D12GraphicsCommandList* list, const EvaluateInputs& in);
// Halton (2,3) jitter for a frame index, centred, in pixels.
void jitter(uint32_t index, float* jx, float* jy);

}  // namespace streamline
}  // namespace gx
