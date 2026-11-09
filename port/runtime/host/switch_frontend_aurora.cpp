// Aurora lifecycle adapter for the Switch: Aurora creates the SDL3 window over
// libnx's default NWindow and a Dawn device on Vulkan (Mesa NVK), through the
// Melee-NX patches applied to ref/melee-pc/extern/aurora. The decoded GX frames
// then flow through gx::AuroraBackend exactly as in Dashdance's Metal/Aurora
// diagnostic; only the backend selection and the window checks differ.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_frontend.h"

#include "gx_aurora.h"
#include "host.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/types.h>
#include <dolphin/vi.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace host {
namespace {
void log_aurora(AuroraLogLevel level, const char* module, const char* message, unsigned length) {
  host::log("[aurora:%s] %.*s", module ? module : "unknown", static_cast<int>(length), message);
  if (level == LOG_FATAL) std::fflush(stderr);
}

const char* backend_name(AuroraBackend backend) {
  switch (backend) {
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_NULL: return "null";
    default: return "other";
  }
}

void require_directory(const std::string& value, const char* name) {
  struct stat st{};
  if (value.empty() || stat(value.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    throw std::invalid_argument(std::string(name) + " must be an existing directory: " + value);
}

extern "C" void aurora_warm_pipelines(void);   // aurora lib/gfx/pipeline_cache.cpp

class AuroraSwitchNativeHost final : public MetalNativeHost {
public:
  explicit AuroraSwitchNativeHost(const AuroraMetalConfig& requested) {
    require_directory(requested.user_path, "Aurora user path");
    require_directory(requested.cache_path, "Aurora cache path");
    require_directory(requested.resources_path, "Aurora resources path");

    AuroraConfig config{};
    config.appName = requested.app_name.c_str();
    config.userPath = requested.user_path.c_str();
    config.cachePath = requested.cache_path.c_str();
    config.resourcesPath = requested.resources_path.c_str();
    config.desiredBackend = BACKEND_VULKAN;
    config.vsync = requested.vsync;
    config.windowWidth = requested.window_width;    // the console's own resolution; Aurora sizes to the NWindow
    config.windowHeight = requested.window_height;
    config.allowTextureDumps = false;
    config.allowCpuAdapter = false;
    config.logCallback = log_aurora;
    config.logLevel = LOG_INFO;
    // The recompiled runtime owns guest RAM/ARAM; Aurora owns graphics only.
    config.mem1Size = 0;
    config.mem2Size = 0;
    // Compile the pipelines this console has used before (Aurora's persisted cache) at launch, with every
    // core, before the game runs. Without it the ~650 pipelines were rebuilt in the background during the
    // first half-minute of play, on a compiler thread pinned to the top core - the simulation thread's -
    // and the renderer dropped frames until it finished. shader_warmup=0 in launcher.ini turns it off.
    setenv("AURORA_LAUNCH_WARM_SECONDS", "1", 0);
    info_ = aurora_initialize(0, nullptr, &config);
    initialized_ = true;
    try {
      if (info_.backend != BACKEND_VULKAN)
        throw std::runtime_error(std::string("Aurora selected the ") + backend_name(info_.backend) +
                                 " backend; the Switch build needs Vulkan (Mesa NVK)");
      if (!info_.window) throw std::runtime_error("Aurora did not create an SDL window");
      host::log("aurora: %s backend, window %ux%u, framebuffer %ux%u", backend_name(info_.backend),
                info_.windowSize.width, info_.windowSize.height, info_.windowSize.native_fb_width,
                info_.windowSize.native_fb_height);
      // AuroraBackend requires the 640x480 logical framebuffer Aurora defaults to.
      VIConfigure(nullptr);
      {
        const double t0 = host::now_seconds();
        aurora_warm_pipelines();   // returns at once when there is nothing cached or warm-up is off
        host::log("aurora: launch pipeline warm-up took %.1f s", host::now_seconds() - t0);
      }
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~AuroraSwitchNativeHost() override { shutdown(); }

  MetalNativeEvents pump_events() override {
    require_running();
    MetalNativeEvents result;
    for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event) {
      ++result.events;
      if (event->type == AURORA_EXIT) result.close_requested = true;
      if (event->type == AURORA_WINDOW_RESIZED || event->type == AURORA_DISPLAY_SCALE_CHANGED) {
        info_.windowSize = event->windowSize;   // docking/undocking changes the output size
        ++result.resize_events;
      }
    }
    return result;
  }
  void resize(uint32_t, uint32_t) override { require_running(); }   // the console sizes the window
  MetalWindowSize window_size() const noexcept override {
    return {info_.windowSize.width, info_.windowSize.height, info_.windowSize.native_fb_width, info_.windowSize.native_fb_height};
  }
  void shutdown() noexcept override {
    if (!initialized_) return;
    aurora_shutdown();
    initialized_ = false;
    info_ = {};
  }

private:
  void require_running() const {
    if (!initialized_) throw std::logic_error("Aurora Switch native host is shut down");
  }
  AuroraInfo info_{};
  bool initialized_ = false;
};
}  // namespace

namespace { gx::AuroraBackend* g_switch_backend = nullptr; }

std::unique_ptr<MetalFrontend> create_aurora_switch_frontend(const AuroraMetalConfig& config) {
  auto native_host = std::make_unique<AuroraSwitchNativeHost>(config);
  auto renderer = std::make_unique<gx::AuroraBackend>(gx::AuroraOptions{
      .logical_width = 640, .logical_height = 480, .reset_clear_color = 0, .reset_clear_z = 0xffffff});
  g_switch_backend = renderer.get();
  return std::make_unique<MetalFrontend>(std::move(native_host), std::move(renderer));
}

void switch_frontend_set_overlay(std::function<void()> draw) {
  if (g_switch_backend) g_switch_backend->set_overlay(std::move(draw));
}
void switch_frontend_set_draw_dump(const std::string& path, uint8_t scene_major, uint8_t scene_minor, unsigned after) {
  if (g_switch_backend) g_switch_backend->set_draw_dump(path, scene_major, scene_minor, after);
}
void switch_frontend_set_debug_file(const std::string& path) {
  if (g_switch_backend) g_switch_backend->set_debug_file(path);
}
}  // namespace host
