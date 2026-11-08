// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings.h"
#include "jukebox.h"
#include "window.h"
#include "audio.h"
#include "host.h"
#include "updater.h"
#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace gx {
void load_pc_settings(D3D12Options& options, int& volume) {
  std::ifstream file(options.settings_path);
  // First launch (no saved settings yet): open the PC settings panel so nobody has to find it.
  options.settings_open = true;   // opens at every launch unless "startup 0" was saved
  std::string key, value;
  while (file >> key >> value) {
    try {
      if (key == "fps") { double rate = std::stod(value); if (rate == -1 || rate == 0 || (rate >= 30 && rate <= 2000)) options.fps_cap = rate; }
      else if (key == "scale") { int scale = std::stoi(value); if (scale >= 0 && scale <= 8) options.efb_scale = scale; }
      else if (key == "fullscreen") options.fullscreen = value == "1";
      else if (key == "vsync") options.vsync = value == "1";
      else if (key == "widescreen") options.widescreen = value == "1";
      else if (key == "sharpness") options.sharpness = std::clamp(std::stof(value), 0.0f, 1.0f);
      else if (key == "anisotropy") { int a = std::stoi(value); if (a == 1 || a == 2 || a == 4 || a == 8 || a == 16) options.anisotropy = a; }
      else if (key == "ssaa") { int a = std::stoi(value); if (a == 1 || a == 2) options.ssaa = a; }
      else if (key == "subframe") options.subframe = value == "0" ? SubFrameMode::Off : value == "2" ? SubFrameMode::AuthoredInterpolate : SubFrameMode::Authored;
      else if (key == "music") slippi::jukebox::set_user_volume(std::stoi(value));
      else if (key == "performance") options.performance_overlay = value == "1";
      else if (key == "startup") options.settings_open = value != "0";
      else if (key == "dlss") { int m = std::stoi(value); if (m >= 0 && m <= 5) options.dlss_mode = m; }
      else if (key == "volume") volume = std::clamp(std::stoi(value), 0, 100);
    } catch (...) { /* Ignore a malformed preference, retaining the safe default. */ }
  }
}

struct PcSettingsUI::Impl {
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  std::array<bool, 64> used{};
  UINT stride = 0;
  bool open = false, saved = false;
  int volume = 0;
  std::array<float, 180> intervals{};
  unsigned cursor = 0;
};

PcSettingsUI::PcSettingsUI(void* window, ID3D12Device* device, ID3D12CommandQueue* queue, const D3D12Options& options)
    : impl_(std::make_unique<Impl>()) {
  auto& state = *impl_;
  state.open = options.settings_open;
  IMGUI_CHECKVERSION(); ImGui::CreateContext();
  auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImGui::StyleColorsDark(); ImGui::GetStyle().ScaleAllSizes(1.25f);
  D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = (UINT)state.used.size(); desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&state.heap)))) host::die("PC settings descriptor heap creation failed");
  state.stride = device->GetDescriptorHandleIncrementSize(desc.Type);
  ImGui_ImplWin32_Init(window);
  ImGui_ImplDX12_InitInfo info{}; info.Device = device; info.CommandQueue = queue; info.NumFramesInFlight = 3;
  info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  info.SrvDescriptorHeap = state.heap.Get(); info.UserData = &state;
  info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto& s = *(Impl*)i->UserData;
    for (size_t slot = 0; slot < s.used.size(); ++slot) if (!s.used[slot]) {
      s.used[slot] = true; *cpu = s.heap->GetCPUDescriptorHandleForHeapStart(); *gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
      cpu->ptr += slot*s.stride; gpu->ptr += slot*s.stride; return;
    }
    host::die("PC settings descriptor heap exhausted");
  };
  info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto& s = *(Impl*)i->UserData;
    size_t slot = (cpu.ptr-s.heap->GetCPUDescriptorHandleForHeapStart().ptr)/s.stride;
    if (slot < s.used.size()) s.used[slot] = false;
  };
  if (!ImGui_ImplDX12_Init(&info)) host::die("PC settings renderer initialization failed");
  host::window_set_message_callback([](void* w, uint32_t m, uintptr_t a, intptr_t b) {
    return ImGui_ImplWin32_WndProcHandler((HWND)w, m, a, b) != 0;
  });
}

PcSettingsUI::~PcSettingsUI() {
  host::window_set_message_callback({}); host::window_input_capture(false);
  ImGui_ImplDX12_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
}

bool PcSettingsUI::begin(D3D12Options& options) {
  auto& state = *impl_;
  ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame();
  host::PadState pad{};
  if (host::window_ui_gamecube_pad(pad)) {
    auto& io = ImGui::GetIO(); io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.AddKeyEvent(ImGuiKey_GamepadStart, (pad.button & 0x1000) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadBack, (pad.button & 0x10) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (pad.button & 0x100) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (pad.button & 0x200) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (pad.button & 8) || pad.stick_y > 40);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (pad.button & 4) || pad.stick_y < -40);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (pad.button & 1) || pad.stick_x < -40);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (pad.button & 2) || pad.stick_x > 40);
  }
  ImGui::NewFrame();
  if (ImGui::IsKeyPressed(ImGuiKey_F1) ||
      (ImGui::IsKeyDown(ImGuiKey_GamepadBack) && ImGui::IsKeyPressed(ImGuiKey_GamepadStart))) state.open = !state.open;
  if (state.open && ImGui::IsKeyPressed(ImGuiKey_Escape)) state.open = false;
  host::window_input_capture(state.open);
  state.intervals[state.cursor++ % state.intervals.size()] = ImGui::GetIO().DeltaTime*1000.f;
  bool changed = false;
  if (state.open) {
    ImGui::SetNextWindowSize(ImVec2(510, 430), ImGuiCond_FirstUseEver);
    ImGui::Begin("PC settings", &state.open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("F1 / Back + Start / Z + Start: settings    Escape: return to game");
    ImGui::Separator();
    changed |= ImGui::Checkbox("Borderless fullscreen", &options.fullscreen);
    const double rates[] = {-1, 0, 60, 120, 144, 165, 200, 240, 360, 480};
    const char* names[] = {"Match monitor", "Unlocked", "60", "120", "144", "165", "200", "240", "360", "480"};
    int selected = -1; for (int i = 0; i < 10; ++i) if (options.fps_cap == rates[i]) selected = i;
    if (ImGui::Combo("Frame rate", &selected, names, 10)) { options.fps_cap = rates[selected]; changed = true; }
    changed |= ImGui::Checkbox("VSync", &options.vsync);
    changed |= ImGui::Checkbox("Widescreen 16:9 (Slippi code, online safe)", &options.widescreen);
    // Same numbers Dolphin shows (EFB 640x528 per multiplier). Auto = the smallest multiplier
    // whose 640x480 image covers the window, like Dolphin's "Auto (Window Size)".
    float win_w = ImGui::GetIO().DisplaySize.x, win_h = ImGui::GetIO().DisplaySize.y;
    float aspect = options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    float vw = win_w, vh = win_w / aspect; if (vh > win_h) { vh = win_h; vw = win_h * aspect; }
    int auto_scale = std::clamp(std::max((int)std::ceil(vw / (480.0f * aspect)), (int)std::ceil(vh / 480.0f)), 1, 8);
    char auto_label[64]; std::snprintf(auto_label, sizeof auto_label, "Auto (%dx = %dx%d for this window)", auto_scale, 640 * auto_scale, 528 * auto_scale);
    const char* scales[] = {auto_label, "Native (640x528)", "2x (1280x1056) for 720p", "3x (1920x1584) for 1080p", "4x (2560x2112) for 1440p",
                            "5x (3200x2640)", "6x (3840x3168) for 4K", "7x (4480x3696)", "8x (5120x4224)"};
    if (options.dlss_mode) ImGui::BeginDisabled();
    changed |= ImGui::Combo("Internal resolution", &options.efb_scale, scales, 9);
    if (options.dlss_mode) ImGui::EndDisabled();
    const char* aa[] = {"None", "4x SSAA (supersampling)"};
    int aa_index = options.ssaa == 2 ? 1 : 0;
    if (ImGui::Combo("Anti-aliasing", &aa_index, aa, 2)) { options.ssaa = aa_index ? 2 : 1; changed = true; }
    const char* anis[] = {"1x", "2x", "4x", "8x", "16x"};
    int an_index = options.anisotropy >= 16 ? 4 : options.anisotropy >= 8 ? 3 : options.anisotropy >= 4 ? 2 : options.anisotropy >= 2 ? 1 : 0;
    if (ImGui::Combo("Anisotropic filtering", &an_index, anis, 5)) { options.anisotropy = 1 << an_index; changed = true; }
    const char* upscalers[] = {"Native", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance"};
    if (ImGui::Combo("Upscaling (NVIDIA DLSS)", &options.dlss_mode, upscalers, 6)) changed = true;
    if (options.dlss_mode) {
      static const char* ratios[] = {"", "100% (DLAA: full resolution, anti-aliasing only)", "67% (Quality)", "58% (Balanced)", "50% (Performance)", "33% (Ultra Performance)"};
      ImGui::TextWrapped("DLSS renders the game at %s of the window size (at 1080p about 1280x960) and upscales it. That is what DLSS is for in heavy games; Melee is cheap to render, so here it is a downgrade in sharpness, and Internal resolution and Anti-aliasing above are ignored while it is on. For the sharpest image choose Native, set Internal resolution to 3x or higher and Anti-aliasing to 4x SSAA (the Dolphin look), or choose DLAA (full resolution, DLSS used only as anti-aliasing).", ratios[options.dlss_mode]);
    }
    int sharp = (int)std::lround(options.sharpness * 100.0f);
    if (ImGui::SliderInt("Sharpening", &sharp, 0, 100, "%d%%")) { options.sharpness = sharp / 100.0f; changed = true; }
    const char* subframe_modes[] = {"Off (60 Hz poses only)", "Predict ahead (no delay, can overshoot on speed changes)", "Interpolate (exact, one frame of delay)"};
    int sf = options.subframe == SubFrameMode::Off ? 0 : options.subframe == SubFrameMode::AuthoredInterpolate ? 2 : 1;
    if (ImGui::Combo("Sub-frame animation", &sf, subframe_modes, 3)) { options.subframe = sf == 0 ? SubFrameMode::Off : sf == 2 ? SubFrameMode::AuthoredInterpolate : SubFrameMode::Authored; changed = true; }
    int music = slippi::jukebox::user_volume();
    if (ImGui::SliderInt("Music", &music, 0, 100, "%d%%")) slippi::jukebox::set_user_volume(music);
    state.volume = host::audio_volume();
    if (ImGui::SliderInt("Volume", &state.volume, 0, 100, "%d%%")) host::audio_set_volume(state.volume);
    ImGui::Checkbox("Performance overlay", &options.performance_overlay);
    ImGui::Checkbox("Open this panel at startup", &options.settings_open);
    ImGui::Separator();
    {
      auto st = host::updater::state();
      if (st == host::updater::State::Idle) host::updater::check(MELEE_PORT_VERSION);
      ImGui::Text("Version %s. %s", MELEE_PORT_VERSION, host::updater::message().c_str());
      if (st == host::updater::State::UpdateAvailable) { ImGui::SameLine(); if (ImGui::Button("Update and restart")) host::updater::download_and_install(); }
      if (st == host::updater::State::Failed) { ImGui::SameLine(); if (ImGui::Button("Retry")) host::updater::check(MELEE_PORT_VERSION); }
    }
    ImGui::Separator();
    if (ImGui::Button("Save settings")) {
      std::filesystem::path path(options.settings_path), temporary = path; temporary += ".tmp";
      std::ofstream file(temporary);
      file << "fps " << options.fps_cap << "\nscale " << options.efb_scale << "\nfullscreen " << options.fullscreen
           << "\nvsync " << options.vsync << "\nwidescreen " << options.widescreen << "\nvolume " << state.volume << "\nperformance " << options.performance_overlay
           << "\ndlss " << options.dlss_mode << "\nsharpness " << options.sharpness << "\nanisotropy " << options.anisotropy << "\nssaa " << options.ssaa
           << "\nsubframe " << (options.subframe == SubFrameMode::Off ? 0 : options.subframe == SubFrameMode::AuthoredInterpolate ? 2 : 1) << "\nmusic " << slippi::jukebox::user_volume()
           << "\nstartup " << (options.settings_open ? 1 : 0) << '\n';
      file.close();
      state.saved = file.good() && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    ImGui::SameLine(); if (ImGui::Button("Return to game")) state.open = false;
    if (state.saved) ImGui::TextUnformatted("Settings saved");
    ImGui::End();
  }
  if (!state.open) {
    // Always-visible way in: a small button in the corner (mouse), plus the key hint.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(ImGui::GetTime() < 20.0 ? 0.8f : 0.35f);
    ImGui::Begin("SettingsButton", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Settings  (F1 / Z+Start)")) state.open = true;
    ImGui::End();
  }
  if (options.performance_overlay) {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("Performance", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::Text("%.0f presentations/s | %.2f ms", ImGui::GetIO().Framerate, 1000.f/std::max(1.f, ImGui::GetIO().Framerate));
    ImGui::PlotLines("##frametimes", state.intervals.data(), (int)state.intervals.size(), state.cursor % state.intervals.size(), nullptr, 0, 33.4f, ImVec2(250, 60));
    ImGui::End();
  }
  host::window_input_capture(state.open);
  ImGui::Render();
  return changed;
}

void PcSettingsUI::draw(ID3D12GraphicsCommandList* list) {
  ID3D12DescriptorHeap* heap = impl_->heap.Get(); list->SetDescriptorHeaps(1, &heap);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}
}
