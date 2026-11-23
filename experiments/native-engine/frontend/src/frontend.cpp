// Picture for the native-engine runner.
//
// The game hands its host a GameCube GX command stream (MuHostApi::gx_fifo) and the headless runner
// throws it away. Here the same bytes go into this repository's GX decoder (port/runtime/gx/gx_core.cpp,
// built with a small patch for the native memory layout), whose finished frames are drawn by the
// renderer the Switch app already uses: gx::AuroraBackend over Aurora, Dawn and Vulkan.
//
//   simulation thread: game -> gx_fifo -> decoder (textures and vertex arrays are copied out of game
//                      memory here, while the game is stopped in its flush) -> frame queue
//   render thread:     owns the window and Aurora; draws the newest queued frame and presents it
//
// Nothing here writes game memory, so the state digest cannot change; the runner still checks it.
// The runner calls mu_frontend_attach through a weak hook in the host services (patches/0101).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host_services.h"       // melee-unlocked: linux_host
#include "host.h"                // shim: host::now_seconds
#include "gx_core.h"
#include "gx_aurora_bridge.h"
#include "async_backend.h"
#if MU_FE_AURORA
#include "gx_aurora.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/types.h>
#include <dolphin/vi.h>
namespace aurora::window { void set_frame_buffer_aspect(float aspect); }   // aurora lib/window.cpp
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
extern "C" void switch_set_pthread_cores(int preferred, uint64_t mask);   // switch/src/switch_threads.c
#endif

namespace fe {
Options options;

namespace {
using Clock = std::chrono::steady_clock;
FILE* log_file = nullptr;
std::mutex log_mutex;
}  // namespace

void say(const char* format, ...) {
  char line[2048];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof line, format, args); va_end(args);
  std::lock_guard<std::mutex> lock(log_mutex);
  std::printf("%s\n", line); std::fflush(stdout);
  if (log_file) { std::fprintf(log_file, "%s\n", line); std::fflush(log_file); }
}

namespace {
struct Series {
  std::vector<float> values;
  void add(double v) { values.push_back(float(v)); }
  void report(const char* label, bool late_count) {
    if (values.empty()) { say("%s: none", label); return; }
    std::vector<float> sorted = values; std::sort(sorted.begin(), sorted.end());
    double sum = 0; size_t late = 0;
    for (float v : values) { sum += v; if (v > 1000.0f / 60) ++late; }
    if (late_count)
      say("%s: %zu frames, avg %.3f ms, median %.3f, p99 %.3f, worst %.3f, over 16.67 ms: %zu", label, sorted.size(), sum / sorted.size(),
          sorted[sorted.size() / 2], sorted[size_t(sorted.size() * 0.99)], sorted.back(), late);
    else
      say("%s: %zu frames, avg %.3f ms, median %.3f, p99 %.3f, worst %.3f", label, sorted.size(), sum / sorted.size(),
          sorted[sorted.size() / 2], sorted[size_t(sorted.size() * 0.99)], sorted.back());
  }
};

// Checks every frame the way the renderer's first stage does (state reconstruction and validation in
// gx::aurora_bridge) without a graphics device. The build without Aurora uses it, and --gfx-null.
class NullBackend final : public gx::Backend {
public:
  void submit_frame(const gx::Frame& frame) override {
    gx::aurora_bridge::prepare_frame(frame, {640, 480}, prepared_);
    for (const auto& cmd : prepared_.commands)
      if (cmd.kind == gx::FrameCommand::Draw && prepared_.draws[cmd.index].vertex_count == 0) ++skipped_draws;
    skipped_copies += prepared_.skipped_copies.size();
  }
  bool wants_raw_vertices() const override { return true; }
  bool wants_state_deltas() const override { return true; }
  uint64_t skipped_draws = 0, skipped_copies = 0;
private:
  gx::aurora_bridge::PreparedFrame prepared_;
};

// Diagnosis: MU_GFX_TEX_DUMP=ADDRESS (hex, as in a draw dump) writes the bytes of that texture as the game
// presented them to <out>/tex-ADDRESS-WxH-fFORMAT-first.bin and -last.bin, to tell a wrong picture caused by
// the renderer from one that is already wrong in game memory.
struct TextureDump {
  bool on = false, first_done = false;
  uint32_t address = 0;
  void init() { if (const char* v = std::getenv("MU_GFX_TEX_DUMP")) { address = uint32_t(std::strtoul(v, nullptr, 16)); on = true; } }
  void frame(const gx::Frame& frame) {
    for (const auto& draw : frame.draws)
      for (const auto& t : draw.textures) {
        if (!t.used || t.addr != address || !t.data) continue;
        char name[160];
        std::snprintf(name, sizeof name, "%s/tex-%08X-%ux%u-f%u-%s.bin", options.out_dir.c_str(), t.addr, t.width, t.height, t.format, first_done ? "last" : "first");
        if (FILE* out = std::fopen(name, "wb")) { std::fwrite(t.data->image.data(), 1, t.data->image.size(), out); std::fclose(out); }
        if (!first_done) say("picture: texture %08X first drawn in frame %llu (scene %u/%u), %zu bytes written", t.addr, (unsigned long long)frame.sequence, frame.scene_major, frame.scene_minor, t.data->image.size());
        first_done = true;
        return;
      }
  }
} texture_dump;

// Times the renderer and keeps one failure from ending the run: the picture stops, the simulation goes on.
class TimedBackend final : public gx::Backend {
public:
  explicit TimedBackend(gx::Backend& inner) : inner_(inner) {}
  void submit_frame(const gx::Frame& frame) override {
    if (failed.load(std::memory_order_relaxed)) return;
    if (texture_dump.on) texture_dump.frame(frame);
    const auto t0 = Clock::now();
    const char* error = nullptr; std::string text;
    try {
      inner_.submit_frame(frame);
    } catch (const std::exception& e) { text = e.what(); error = text.c_str(); }
    catch (...) { error = "unknown exception"; }
    if (error) {
      failed.store(true);
      say("picture: RENDERER STOPPED at frame %llu (scene %u/%u, %zu draws, %zu copies): %s", (unsigned long long)frame.sequence,
          frame.scene_major, frame.scene_minor, frame.draws.size(), frame.copies.size(), error);
      return;
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::lock_guard<std::mutex> lock(mutex);
    all.add(ms);
    if (frame.scene_major == 2 && frame.scene_minor == 2) match.add(ms);
    draws += frame.draws.size();
    for (const auto& copy : frame.copies) if (!copy.to_xfb) ++texture_copies;
    ++frames;
    if (frame.draws.size() > most_draws) most_draws = frame.draws.size();
  }
  bool wants_raw_vertices() const override { return inner_.wants_raw_vertices(); }
  bool wants_state_deltas() const override { return inner_.wants_state_deltas(); }
  std::atomic<bool> failed{false};
  std::mutex mutex;
  Series all, match;
  uint64_t frames = 0, draws = 0, texture_copies = 0;
  size_t most_draws = 0;
private:
  gx::Backend& inner_;
};

struct State {
  bool attached = false;
  pthread_t thread{};
  bool thread_started = false;
  std::mutex mutex;
  std::condition_variable changed;
  bool ready = false;            // the render thread has a backend (or has failed)
  std::string start_error;
  std::atomic<bool> stop{false};
  std::unique_ptr<TimedBackend> timed;
  std::unique_ptr<gx::AsyncBackend> async;
  std::string backend_name;
  // Simulation thread.
  bool decoder_failed = false;
  uint64_t fifo_bytes = 0, fifo_batches = 0;
  double fifo_ms_frame = 0;      // decoder time inside the current retrace
  Clock::time_point resumed;     // when the simulation got the thread back after the last retrace
  bool have_resumed = false;
  Clock::time_point next_deadline;
  bool have_deadline = false;
  uint32_t last_match_frame = 0;
  Series work_match, work_all, decode_match, decode_all;
  uint64_t submitted_before = 0;
} st;

void fifo_feed(const uint8_t* data, uint32_t size) {
  while (gx::g_fifo_len + size + 8 > gx::g_fifo_cap) gx::fifo_grow();
  std::memcpy(gx::g_fifo_data + gx::g_fifo_len, data, size);
  gx::g_fifo_len += size;
  gx::fifo_parse_pending();
}

// The game calls this from its pipe flush (C code without unwind tables): nothing may be thrown through it.
void fifo_sink(const uint8_t* data, uint32_t size, void*) {
  if (st.decoder_failed) return;
  const auto t0 = Clock::now();
  try {
    fifo_feed(data, size);
  } catch (const std::exception& e) {
    st.decoder_failed = true;
    say("picture: DECODER STOPPED after %llu FIFO bytes: %s", (unsigned long long)st.fifo_bytes, e.what());
  } catch (...) {
    st.decoder_failed = true;
    say("picture: DECODER STOPPED after %llu FIFO bytes: unknown exception", (unsigned long long)st.fifo_bytes);
  }
  st.fifo_bytes += size; ++st.fifo_batches;
  st.fifo_ms_frame += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Once per retrace, after the game has simulated the frame and before the runner's observer.
void retrace_sink(void*) {
  const auto now = Clock::now();
  MuStatePod state{};
  linux_host::state_snapshot(&state);
  const bool fighting = state.match_frame && state.match_frame != st.last_match_frame;
  const bool vs_match = fighting && state.scene_major == 2 && state.scene == 2;
  st.last_match_frame = state.match_frame;
  if (st.have_resumed) {
    const double work = std::chrono::duration<double, std::milli>(now - st.resumed).count();
    st.work_all.add(work); st.decode_all.add(st.fifo_ms_frame);
    if (vs_match) { st.work_match.add(work); st.decode_match.add(st.fifo_ms_frame); }
  }
  st.fifo_ms_frame = 0;
  if (options.pace) {
    const auto period = std::chrono::nanoseconds(16666667);
    if (!st.have_deadline || now > st.next_deadline + std::chrono::milliseconds(100)) { st.next_deadline = now; st.have_deadline = true; }   // far behind (loading): start a new grid
    st.next_deadline += period;
    std::this_thread::sleep_until(st.next_deadline);
  }
  st.resumed = Clock::now();
  st.have_resumed = true;
}

#if MU_FE_AURORA
void log_aurora(AuroraLogLevel level, const char* module, const char* message, unsigned length) {
  if (level < LOG_WARNING && !options.verbose) return;
  say("[aurora:%s] %.*s", module ? module : "?", int(length), message);
}
const char* backend_label(::AuroraBackend backend) {
  switch (backend) {
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_NULL: return "null";
    default: return "other";
  }
}
#endif

void* render_main(void*) {
#ifdef __SWITCH__
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1ull << 1);   // the renderer on core 1; the simulation has core 2
#endif
  std::unique_ptr<gx::Backend> backend;
  bool aurora_up = false;
  std::string error;
  try {
#if MU_FE_AURORA
    if (!options.null_backend) {
      const std::string dir = options.out_dir + "/aurora";
      mkdir(dir.c_str(), 0777);
      AuroraConfig config{};
      config.appName = "mu-native-gfx";
      config.userPath = dir.c_str(); config.cachePath = dir.c_str(); config.resourcesPath = dir.c_str();
      config.desiredBackend = BACKEND_VULKAN;
      config.vsync = options.vsync;
      config.windowWidth = uint32_t(options.width); config.windowHeight = uint32_t(options.height);
      config.allowTextureDumps = false;
#ifdef __SWITCH__
      config.allowCpuAdapter = false;
#else
      config.allowCpuAdapter = true;       // the test build draws with Mesa's software Vulkan driver
#endif
      config.logCallback = log_aurora;
      config.logLevel = options.verbose ? LOG_INFO : LOG_WARNING;
      config.mem1Size = 0; config.mem2Size = 0;   // the game owns its memory; Aurora draws only
      setenv("AURORA_LAUNCH_WARM_SECONDS", "1", 0);   // as the app: compile the pipelines this device has drawn with before, at launch
      const AuroraInfo info = aurora_initialize(0, nullptr, &config);
      aurora_up = true;
      if (!info.window) throw std::runtime_error("Aurora did not create a window");
      if (info.backend != BACKEND_VULKAN) throw std::runtime_error(std::string("Aurora selected the ") + backend_label(info.backend) + " backend, not Vulkan");
      say("picture: Aurora on %s, window %ux%u, framebuffer %ux%u", backend_label(info.backend), info.windowSize.width, info.windowSize.height,
          info.windowSize.native_fb_width, info.windowSize.native_fb_height);
      VIConfigure(nullptr);   // the 640x480 logical framebuffer gx::AuroraBackend expects
      if (!options.stretch) aurora::window::set_frame_buffer_aspect(4.0f / 3.0f);   // the game draws for 4:3 (no widescreen code is on)
      {
        const auto t0 = Clock::now();
        aurora_warm_pipelines();   // returns at once when nothing is cached yet (first run)
        say("picture: pipeline warm-up took %.1f s", std::chrono::duration<double>(Clock::now() - t0).count());
      }
      auto aurora_backend = std::make_unique<gx::AuroraBackend>(gx::AuroraOptions{.logical_width = 640, .logical_height = 480, .reset_clear_color = 0, .reset_clear_z = 0xffffff});
      // Diagnosis: MU_GFX_DRAW_DUMP=MAJOR:MINOR:N writes the draw state of the N-th frame drawn in that scene to <out>/draws.txt.
      if (const char* dump = std::getenv("MU_GFX_DRAW_DUMP")) {
        unsigned major = 0, minor = 0, after = 1;
        if (std::sscanf(dump, "%u:%u:%u", &major, &minor, &after) == 3) aurora_backend->set_draw_dump(options.out_dir + "/draws.txt", uint8_t(major), uint8_t(minor), after);
      }
      backend = std::move(aurora_backend);
      st.backend_name = "Aurora/Vulkan";
    }
#endif
    if (!backend) { backend = std::make_unique<NullBackend>(); st.backend_name = "none (frames are decoded and checked, not drawn)"; }
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "unknown exception";
  }
  if (error.empty()) {
    st.timed = std::make_unique<TimedBackend>(*backend);
    st.async = std::make_unique<gx::AsyncBackend>(*st.timed);
#if MU_FE_AURORA
    if (aurora_up)
      st.async->set_idle([] {
        for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
          if (event->type == AURORA_EXIT) linux_host::request_stop(0);
      });
#endif
  }
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.start_error = error; st.ready = true;
  }
  st.changed.notify_all();
  if (error.empty()) {
    try { st.async->run(st.stop); }
    catch (const std::exception& e) { say("picture: render thread stopped: %s", e.what()); st.async->finish(); }
    catch (...) { say("picture: render thread stopped: unknown exception"); st.async->finish(); }
  }
  // The renderer's objects go before Aurora does, on the thread that made them.
  if (auto* null_backend = dynamic_cast<NullBackend*>(backend.get()))
    if (null_backend->skipped_draws || null_backend->skipped_copies)
      say("picture: the frame check left out %llu draws and %llu texture copies", (unsigned long long)null_backend->skipped_draws, (unsigned long long)null_backend->skipped_copies);
#if MU_FE_AURORA
  if (auto* aurora_backend = dynamic_cast<gx::AuroraBackend*>(backend.get())) {
    const auto stats = aurora_backend->stats();
    say("picture: Aurora drew %llu frames, %llu draws, %llu texture copies", (unsigned long long)stats.frames, (unsigned long long)stats.draws, (unsigned long long)stats.texture_copies);
  }
#endif
  backend.reset();
#if MU_FE_AURORA
  if (aurora_up) aurora_shutdown();
#endif
  return nullptr;
}

void attach() {
  if (!options.picture || st.attached) return;
  if (!log_file) { mkdir(options.out_dir.c_str(), 0777); log_file = std::fopen((options.out_dir + "/picture.log").c_str(), "a"); }
  texture_dump.init();
  say("---- picture frontend: %s, %s ----", options.pace ? "simulation paced to 60 Hz" : "simulation unpaced (the picture shows the newest frame)",
      options.vsync ? "vsync" : "no vsync");
  // GX data lives in main memory and in the game image mapped right after it.
  ppc::RAM_SIZE = uint32_t(linux_host::game_image_base - linux_host::mem1_base) + linux_host::game_image_size();
#ifdef __SWITCH__
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1ull << 2);   // this (simulation) thread alone on core 2
  switch_set_pthread_cores(0, 0x3);                         // every thread created from here on (render thread, Aurora, Dawn): cores 0-1
#endif
  st.stop.store(false); st.ready = false; st.start_error.clear();
  pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
  if (pthread_create(&st.thread, &attr, render_main, nullptr) != 0) { pthread_attr_destroy(&attr); say("picture: cannot create the render thread; running without a picture"); return; }
  pthread_attr_destroy(&attr);
  st.thread_started = true;
  {
    std::unique_lock<std::mutex> lock(st.mutex);
    st.changed.wait(lock, [] { return st.ready; });
  }
  if (!st.start_error.empty()) {
    say("picture: NO PICTURE, the renderer did not start: %s", st.start_error.c_str());
    pthread_join(st.thread, nullptr); st.thread_started = false;
    return;
  }
  say("picture: renderer %s; GX data range 80000000..%08X", st.backend_name.c_str(), 0x80000000u + ppc::RAM_SIZE);
  setenv("MELEE_GPU_THREAD", "0", 1);   // decode inside the game's flush: what the commands point at is copied before the game moves on
  gx::init(st.async.get());
  linux_host::set_video_sinks(fifo_sink, retrace_sink, nullptr);
  st.attached = true;
}

void detach() {
  if (!st.attached) return;
  st.attached = false;
  linux_host::set_video_sinks(nullptr, nullptr, nullptr);
  gx::init(nullptr);
  st.stop.store(true);
  if (st.thread_started) { pthread_join(st.thread, nullptr); st.thread_started = false; }
#ifdef __SWITCH__
  switch_set_pthread_cores(-1, 0);
#endif
  uint64_t commands = 0, draws = 0, vertices = 0; uint32_t copies = 0;
  gx::stats(&commands, &draws, &vertices, &copies);
  say("picture: decoded %llu FIFO bytes in %llu batches: %llu commands, %llu draws, %llu vertices, %u EFB copies%s", (unsigned long long)st.fifo_bytes,
      (unsigned long long)st.fifo_batches, (unsigned long long)commands, (unsigned long long)draws, (unsigned long long)vertices, copies,
      st.decoder_failed ? " (DECODER STOPPED EARLY, see above)" : "");
  if (st.timed) {
    auto& t = *st.timed;
    say("picture: %llu frames drawn, %llu stale frames skipped (a newer one was already waiting), most draws in a frame %zu, %llu texture copies%s",
        (unsigned long long)t.frames, (unsigned long long)st.async->dropped(), t.most_draws, (unsigned long long)t.texture_copies,
        t.failed.load() ? " (RENDERER STOPPED EARLY, see above)" : "");
    t.match.report("picture: render thread, VS match frames", false);
    t.all.report("picture: render thread, all frames", false);
  }
  if (options.pace) say("picture: the simulation waited for a 60 Hz timer after every retrace, so the runner's frame times below include that wait; these do not:");
  else say("picture: unpaced, so the runner's frame times below are comparable with the headless runner's; they now include the GX decoder:");
  st.work_match.report("picture: VS match frames, simulation thread work (game + GX decoder + audio mixing)", true);
  st.decode_match.report("picture: VS match frames, GX decoder share of that", false);
  st.work_all.report("picture: all retraces, simulation thread work", true);
  st.async.reset(); st.timed.reset();
  say("---- picture frontend done ----");
  if (log_file) { std::fclose(log_file); log_file = nullptr; }
}
}  // namespace
}  // namespace fe

// Weak hook called by the host services (patches/0101-host-frontend-attach.patch): 1 after the host is
// initialized and before the game runs, 0 after the run and before the game is unmapped. Simulation thread.
extern "C" void mu_frontend_attach(int on) {
  if (on) fe::attach(); else fe::detach();
}
