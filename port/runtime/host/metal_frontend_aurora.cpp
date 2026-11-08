// Pinned Aurora Cocoa/Metal lifecycle adapter.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_frontend.h"

#include "gx_aurora.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/types.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace host {
namespace {
namespace fs = std::filesystem;

constexpr const char* AuroraMetalViewProperty = "aurora.metal_view";

std::string explicit_directory(const std::string& value, const char* name) {
  const fs::path path(value);
  if (value.empty() || !path.is_absolute())
    throw std::invalid_argument(std::string(name) + " must be an explicit absolute directory");
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || status.type() == fs::file_type::symlink || !fs::is_directory(status))
    throw std::invalid_argument(std::string(name) + " must be an existing non-symlink directory");
  const auto canonical = fs::canonical(path, error);
  if (error) throw std::invalid_argument(std::string("cannot canonicalize ") + name);
  return canonical.string();
}

void log_aurora(AuroraLogLevel level, const char* module, const char* message, unsigned length) {
  std::fprintf(stderr, "[aurora:%s] %.*s\n", module ? module : "unknown", static_cast<int>(length), message);
  if (level == LOG_FATAL) std::fflush(stderr);
}

class AuroraMetalNativeHost final : public MetalNativeHost {
public:
  explicit AuroraMetalNativeHost(const AuroraMetalConfig& requested) {
    if (requested.app_name.empty()) throw std::invalid_argument("Metal app name must not be empty");
    if (!requested.window_width || !requested.window_height)
      throw std::invalid_argument("Metal window dimensions must be non-zero");
    const auto user_path = explicit_directory(requested.user_path, "Metal user path");
    const auto cache_path = explicit_directory(requested.cache_path, "Metal cache path");
    const auto resources_path = explicit_directory(requested.resources_path, "Metal resources path");
    if (user_path == cache_path)
      throw std::invalid_argument("Metal user and cache paths must be distinct");

    AuroraConfig config{};
    config.appName = requested.app_name.c_str();
    config.userPath = user_path.c_str();
    config.cachePath = cache_path.c_str();
    config.resourcesPath = resources_path.c_str();
    config.desiredBackend = BACKEND_METAL;
    config.vsync = requested.vsync;
    config.windowWidth = requested.window_width;
    config.windowHeight = requested.window_height;
    config.allowTextureDumps = false;
    config.allowCpuAdapter = false;
    config.logCallback = log_aurora;
    config.logLevel = LOG_INFO;
    // The translated runtime owns guest RAM/ARAM; Aurora owns graphics only.
    config.mem1Size = 0;
    config.mem2Size = 0;
    info_ = aurora_initialize(0, nullptr, &config);
    initialized_ = true;

    try {
      if (info_.backend != BACKEND_METAL || aurora_get_backend() != BACKEND_METAL)
        throw std::runtime_error("Aurora did not select its native Metal backend");
      if (!info_.window) throw std::runtime_error("Aurora did not create an SDL window");
      const auto properties = SDL_GetWindowProperties(info_.window);
      if (!properties ||
          !SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr))
        throw std::runtime_error("Aurora SDL window is not backed by a Cocoa NSWindow");
      auto* metal_view = SDL_GetPointerProperty(properties, AuroraMetalViewProperty, nullptr);
      if (!metal_view || !SDL_Metal_GetLayer(static_cast<SDL_MetalView>(metal_view)))
        throw std::runtime_error("Aurora Cocoa window has no CAMetalLayer-backed Metal view");
      // Aurora defaults to the adapter's required 640x480 logical framebuffer;
      // make that owner configuration explicit before AuroraBackend validates it.
      VIConfigure(nullptr);
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~AuroraMetalNativeHost() override { shutdown(); }

  MetalNativeEvents pump_events() override {
    require_running();
    MetalNativeEvents result;
    for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event) {
      ++result.events;
      if (event->type == AURORA_EXIT) result.close_requested = true;
      if (event->type == AURORA_WINDOW_RESIZED || event->type == AURORA_DISPLAY_SCALE_CHANGED) {
        info_.windowSize = event->windowSize;
        ++result.resize_events;
      }
    }
    return result;
  }

  void resize(uint32_t width, uint32_t height) override {
    require_running();
    if (!SDL_SetWindowSize(info_.window, static_cast<int>(width), static_cast<int>(height)))
      throw std::runtime_error(std::string("cannot resize Cocoa Metal window: ") + SDL_GetError());
  }

  MetalWindowSize window_size() const noexcept override {
    return {
        info_.windowSize.width,
        info_.windowSize.height,
        info_.windowSize.native_fb_width,
        info_.windowSize.native_fb_height,
    };
  }

  void shutdown() noexcept override {
    if (!initialized_) return;
    aurora_shutdown();
    initialized_ = false;
    info_ = {};
  }

private:
  void require_running() const {
    if (!initialized_) throw std::logic_error("Aurora Metal native host is shut down");
  }

  AuroraInfo info_{};
  bool initialized_ = false;
};
} // namespace

std::unique_ptr<MetalFrontend> create_aurora_metal_frontend(const AuroraMetalConfig& config) {
  auto native_host = std::make_unique<AuroraMetalNativeHost>(config);
  auto renderer = std::make_unique<gx::AuroraBackend>(gx::AuroraOptions{
      .logical_width = 640,
      .logical_height = 480,
      .reset_clear_color = 0,
      .reset_clear_z = 0xffffff,
  });
  return std::make_unique<MetalFrontend>(std::move(native_host), std::move(renderer));
}

} // namespace host
