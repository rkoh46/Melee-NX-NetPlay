// Picture for the native-engine runner.
//
// The game hands its host a GameCube GX command stream (MuHostApi::gx_fifo) and the headless runner
// throws it away. Here the same bytes go into this repository's GX decoder (port/runtime/gx/gx_core.cpp,
// built with a small patch for the native memory layout), whose finished frames are drawn by the
// renderer the Switch app already uses: gx::AuroraBackend over Aurora, Dawn and Vulkan.
//
// Three threads, arranged as the app arranges them (port/app/main_switch.cpp):
//   simulation  the game. It only appends its stream to a chunk queue, and waits for the decoder once
//               per frame, at the retrace, before it may change what the frame's commands point at.
//               Core 2 on the console, the highest application priority.
//   decoder     gx_core's own worker ("GPU thread"): parses the stream, copies the textures and vertex
//               arrays the commands reference out of game memory, builds frames. Core 0.
//   render      owns the window and Aurora; draws the newest finished frame and presents it. Core 1;
//               Aurora's workers on cores 0-1.
// --gfx-sync decodes inside the game's flush instead (no decoder thread): exact even if the game
// rewrote data between two draws of one frame, at the cost of the decoder's time on the simulation thread.
//
// Nothing here writes game memory, so the state digest cannot change; the runner still checks it.
// The runner calls mu_frontend_attach through a weak hook in the host services (patches/0101).
// Nothing is logged while a match runs: every line is printed before the game starts or after it stops.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host_services.h"       // melee-unlocked: linux_host
#include "host.h"                // shim: clock and cost counters
#include "gx_core.h"
#include "gx_aurora_bridge.h"
#include "frame_queue.h"
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
#include "switch_clocks.h"                                                 // switch/src: the runner has opened the clock service
extern "C" void switch_set_pthread_cores(int preferred, uint64_t mask);   // switch/src/switch_threads.c
#endif

namespace fe {
Options options;

namespace {
FILE* log_file = nullptr;
std::mutex log_mutex;
inline uint64_t now_ns() { return host::now_ns(); }
inline double ms(uint64_t ns) { return double(ns) * 1e-6; }
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
  void report(const char* label, bool late_count = false) {
    if (values.empty()) { say("%s: none", label); return; }
    std::vector<float> sorted = values; std::sort(sorted.begin(), sorted.end());
    double sum = 0; size_t late = 0;
    for (float v : values) { sum += v; if (v > 1000.0f / 60) ++late; }
    char tail[64] = "";
    if (late_count) std::snprintf(tail, sizeof tail, ", over 16.67 ms: %zu", late);
    say("%s: %zu frames, avg %.3f ms, median %.3f, p99 %.3f, worst %.3f%s", label, sorted.size(), sum / sorted.size(),
        sorted[sorted.size() / 2], sorted[size_t(sorted.size() * 0.99)], sorted.back(), tail);
  }
};

// Time the render thread spends inside Aurora's frame calls (the linker wraps them, see CMakeLists.txt).
uint64_t end_frame_ns = 0, begin_frame_ns = 0;

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
        first_done = true;
        return;
      }
  }
} texture_dump;

// The decoder's finished frames, on their way to the render thread. On the console a full queue gives up
// its oldest frame instead of making the decoder (and with it the game) wait (gx::FrameQueue).
class QueueBackend final : public gx::Backend {
public:
  void submit_frame(const gx::Frame& frame) override { gx::Frame copy = frame; queue.push(std::move(copy)); }
  void submit_and_recycle(gx::Frame& frame) override {
    const uint64_t t0 = now_ns();
    queue.push_and_recycle(frame);
    handoff_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    finished.fetch_add(1, std::memory_order_relaxed);
  }
  bool wants_raw_vertices() const override { return true; }   // Aurora consumes the hardware vertex format
  bool wants_state_deltas() const override { return true; }
  gx::FrameQueue queue;
  std::atomic<uint64_t> handoff_ns{0}, finished{0};
};

struct RenderStats {
  std::mutex mutex;
  Series total, convert, end_frame, age, total_match, convert_match, end_frame_match, age_match;
  uint64_t frames = 0, draws = 0, texture_copies = 0, stale = 0;
  size_t most_draws = 0;
  bool failed = false;
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
  std::unique_ptr<QueueBackend> queue;
  RenderStats render;
  std::string backend_name;
  bool threaded = true;          // decoder on its own thread
  // Simulation thread.
  bool decoder_failed = false;
  uint64_t fifo_bytes = 0, fifo_batches = 0;
  uint64_t feed_ns_frame = 0;    // time inside the FIFO sink during the current retrace
  uint64_t resumed = 0;          // when the simulation got the thread back after the last retrace
  bool have_resumed = false;
  std::chrono::steady_clock::time_point next_deadline;
  bool have_deadline = false;
  uint32_t last_match_frame = 0;
  uint64_t last_decode = 0, last_snapshot = 0, last_record = 0, last_handoff = 0;
  uint64_t prev_pad_ns = 0;      // the controller read that the frame now being simulated uses
  Series work_match, work_all, feed_match, wait_match, wait_all, decode_match, snapshot_match, record_match, handoff_match, pad_match;
#ifdef __SWITCH__
  s32 sim_priority_before = 0x2C;
  bool clocks_changed = false;
  SwitchClockResult clocks_before{};
#endif
} st;

void decoder_stopped(const char* reason) {
  if (st.decoder_failed) return;
  st.decoder_failed = true;
  say("picture: DECODER STOPPED after %llu FIFO bytes: %s", (unsigned long long)st.fifo_bytes, reason);
}

// The game calls this from its pipe flush (C code without unwind tables): nothing may be thrown through it.
void fifo_sink(const uint8_t* data, uint32_t size, void*) {
  if (st.decoder_failed) return;
  const uint64_t t0 = now_ns();
  try {
    if (st.threaded) {
      // gx_core's producer side: append; a chunk goes to the decoder thread every 8 KiB.
      for (uint32_t at = 0; at < size;) {
        const uint32_t part = std::min<uint32_t>(size - at, 32768);
        gx::fifo_store_words(reinterpret_cast<const uint32_t*>(data + at), part / 4);
        for (uint32_t i = part & ~3u; i < part; ++i) gx::fifo_store8(data[at + i]);
        at += part;
      }
    } else {
      while (gx::g_fifo_len + size + 8 > gx::g_fifo_cap) gx::fifo_grow();
      std::memcpy(gx::g_fifo_data + gx::g_fifo_len, data, size);
      gx::g_fifo_len += size;
      gx::fifo_parse_pending();
    }
  } catch (const std::exception& e) { decoder_stopped(e.what()); }
  catch (...) { decoder_stopped("unknown exception"); }
  st.fifo_bytes += size; ++st.fifo_batches;
  const uint64_t spent = now_ns() - t0;
  st.feed_ns_frame += spent;
  if (!st.threaded) host::sim_cost_add(host::SIM_DECODE, double(spent) * 1e-9);
}

// Once per retrace, after the game has simulated and drawn the frame and before the runner's observer.
void retrace_sink(void*) {
  const uint64_t entered = now_ns();
  // The game is about to run its next update, which rewrites what this frame's commands point at:
  // the decoder must have read it all first (the app synchronises at the same place, the guest's wait
  // for the draw-done interrupt).
  uint64_t waited = 0;
  if (st.threaded && !st.decoder_failed) {
    try { gx::gpu_sync_all(); } catch (...) {}
    waited = now_ns() - entered;
    std::string reason;
    if (host::take_background_failure(reason)) decoder_stopped(reason.c_str());
  }
  MuStatePod state{};
  linux_host::state_snapshot(&state);
  const bool fighting = state.match_frame && state.match_frame != st.last_match_frame;
  const bool vs_match = fighting && state.scene_major == 2 && state.scene == 2;
  st.last_match_frame = state.match_frame;
  const uint64_t decode = host::sim_cost_ns(host::SIM_DECODE), snapshot = host::sim_cost_ns(host::SIM_SNAPSHOT), record = host::sim_cost_ns(host::SIM_RECORD);
  const uint64_t handoff = st.queue ? st.queue->handoff_ns.load(std::memory_order_relaxed) : 0;
  if (st.have_resumed) {
    const double work = ms(entered - st.resumed);
    st.work_all.add(work); st.wait_all.add(ms(waited));
    if (vs_match) {
      st.work_match.add(work); st.feed_match.add(ms(st.feed_ns_frame)); st.wait_match.add(ms(waited));
      st.decode_match.add(ms(decode - st.last_decode)); st.snapshot_match.add(ms(snapshot - st.last_snapshot));
      st.record_match.add(ms(record - st.last_record)); st.handoff_match.add(ms(handoff - st.last_handoff));
      // The game samples the controllers in its retrace handler, just before this function runs; the frame
      // simulated from that sample is the one decoded by the next retrace.
      if (st.prev_pad_ns) st.pad_match.add(ms(entered + waited - st.prev_pad_ns));
    }
  }
  st.prev_pad_ns = io_last_pad_ns();
  st.last_decode = decode; st.last_snapshot = snapshot; st.last_record = record; st.last_handoff = handoff;
  st.feed_ns_frame = 0;
  if (options.pace) {
    // The app's retrace timing (port/runtime/host/host.cpp): a 16.667 ms grid on the steady clock; after a
    // stall of more than two frames the grid starts again from now instead of sprinting to catch up.
    const auto now = std::chrono::steady_clock::now();
    if (!st.have_deadline) { st.next_deadline = now; st.have_deadline = true; }
    st.next_deadline += std::chrono::microseconds(16667);
    if (st.next_deadline > now) std::this_thread::sleep_until(st.next_deadline);
    else if (now - st.next_deadline > std::chrono::milliseconds(34)) st.next_deadline = now;
  }
  st.resumed = now_ns();
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

void render_one(gx::Backend& backend, const gx::Frame& frame) {
  auto& r = st.render;
  if (r.failed) return;
  if (texture_dump.on) texture_dump.frame(frame);
  const uint64_t t0 = now_ns(), end0 = end_frame_ns, begin0 = begin_frame_ns;
  const char* error = nullptr; std::string text;
  try { backend.submit_frame(frame); }
  catch (const std::exception& e) { text = e.what(); error = text.c_str(); }
  catch (...) { error = "unknown exception"; }
  if (error) {
    r.failed = true;
    say("picture: RENDERER STOPPED at frame %llu (scene %u/%u, %zu draws, %zu copies): %s", (unsigned long long)frame.sequence,
        frame.scene_major, frame.scene_minor, frame.draws.size(), frame.copies.size(), error);
    return;
  }
  const uint64_t t1 = now_ns();
  const double total = ms(t1 - t0), end = ms(end_frame_ns - end0), convert = total - end - ms(begin_frame_ns - begin0);
  const double age = frame.time > 0 ? double(t0) * 1e-6 - frame.time * 1000.0 : 0;   // from the decoder finishing the frame to the renderer starting on it
  std::lock_guard<std::mutex> lock(r.mutex);
  r.total.add(total); r.convert.add(convert); r.end_frame.add(end); r.age.add(age);
  if (frame.scene_major == 2 && frame.scene_minor == 2) { r.total_match.add(total); r.convert_match.add(convert); r.end_frame_match.add(end); r.age_match.add(age); }
  r.draws += frame.draws.size();
  for (const auto& copy : frame.copies) if (!copy.to_xfb) ++r.texture_copies;
  ++r.frames;
  r.most_draws = std::max(r.most_draws, frame.draws.size());
}

void render_loop(gx::Backend& backend, bool pump) {
  auto& queue = st.queue->queue;
  gx::Frame frame;
  for (;;) {
#if MU_FE_AURORA
    if (pump)
      for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
        if (event->type == AURORA_EXIT) linux_host::request_stop(0);
#else
    (void)pump;
#endif
    if (!queue.pop(frame)) {               // waits up to 2 ms
      if (st.stop.load(std::memory_order_acquire) && queue.size() == 0) break;
      continue;
    }
    // Behind the game: only the newest finished frame is worth drawing.
    while (queue.size() > 0) {
      gx::Frame newer;
      if (!queue.try_pop(newer)) break;
      queue.recycle(std::move(frame));
      frame = std::move(newer);
      ++st.render.stale;
    }
    render_one(backend, frame);
    queue.recycle(std::move(frame));
    frame = gx::Frame{};
  }
}

void* render_main(void*) {
#ifdef __SWITCH__
  // As the app's owner thread: core 1, priority 0x28. Aurora's workers are created from here and inherit the
  // priority; switch_set_pthread_cores has confined them to cores 0-1.
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1ull << 1);
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28);
#endif
  std::unique_ptr<gx::Backend> backend;
  bool aurora_up = false;
  std::string error;
  try {
#if MU_FE_AURORA
    if (!options.null_backend) {
      const std::string dir = options.cache_dir.empty() ? options.out_dir + "/aurora" : options.cache_dir;
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
      // As the app (port/app/main_switch.cpp): the shader compilers never use the game's core, and the
      // pipelines this device has drawn with before are compiled at launch, before the game runs.
      setenv("AURORA_RESERVED_CORE_MASK", "0x4", 0);
      setenv("AURORA_LAUNCH_WARM_SECONDS", "1", 0);
      if (options.warm_all) { setenv("AURORA_LAUNCH_WARM_ALL", "1", 1); setenv("AURORA_LAUNCH_WARM_HELPERS", "2", 1); }
      const AuroraInfo info = aurora_initialize(0, nullptr, &config);
      aurora_up = true;
      if (!info.window) throw std::runtime_error("Aurora did not create a window");
      if (info.backend != BACKEND_VULKAN) throw std::runtime_error(std::string("Aurora selected the ") + backend_label(info.backend) + " backend, not Vulkan");
      say("picture: Aurora on %s, window %ux%u, framebuffer %ux%u", backend_label(info.backend), info.windowSize.width, info.windowSize.height,
          info.windowSize.native_fb_width, info.windowSize.native_fb_height);
      VIConfigure(nullptr);   // the 640x480 logical framebuffer gx::AuroraBackend expects
      if (!options.stretch) aurora::window::set_frame_buffer_aspect(4.0f / 3.0f);   // the game draws for 4:3 (no widescreen code is on)
      {
        const uint64_t t0 = now_ns();
        aurora_warm_pipelines();   // returns at once when nothing is cached yet (first run)
        say("picture: pipeline warm-up took %.1f s%s", ms(now_ns() - t0) / 1000.0, options.warm_all ? " (--warm-all: every pipeline in the cache)" : "");
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
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.start_error = error; st.ready = true;
  }
  st.changed.notify_all();
  if (error.empty()) {
    try { render_loop(*backend, aurora_up); }
    catch (const std::exception& e) { say("picture: render thread stopped: %s", e.what()); st.queue->queue.finish(true); }
    catch (...) { say("picture: render thread stopped: unknown exception"); st.queue->queue.finish(true); }
  }
  // The renderer's objects go before Aurora does, on the thread that made them.
  if (auto* null_backend = dynamic_cast<NullBackend*>(backend.get()))
    if (null_backend->skipped_draws || null_backend->skipped_copies)
      say("picture: the frame check left out %llu draws and %llu texture copies", (unsigned long long)null_backend->skipped_draws, (unsigned long long)null_backend->skipped_copies);
  backend.reset();
#if MU_FE_AURORA
  if (aurora_up) { aurora_flush_caches(); aurora_shutdown(); }
#endif
  return nullptr;
}

void attach() {
  if (!options.picture || st.attached) return;
  if (!log_file) { mkdir(options.out_dir.c_str(), 0777); log_file = std::fopen((options.out_dir + "/picture.log").c_str(), "a"); }
  texture_dump.init();
  st.threaded = !options.sync_decode;
  say("---- picture frontend: %s, %s, %s ----", options.pace ? "simulation paced to 60 Hz" : "simulation unpaced (the picture shows the newest frame)",
      options.vsync ? "vsync" : "no vsync", st.threaded ? "decoder on its own thread" : "decoder inside the game's flush (--gfx-sync)");
  // GX data lives in main memory and in the game image mapped right after it.
  ppc::RAM_SIZE = uint32_t(linux_host::game_image_base - linux_host::mem1_base) + linux_host::game_image_size();
#ifdef __SWITCH__
  // The app's placement: the game alone on core 2 at the highest priority an application may use; every
  // thread created from here on (render thread, decoder, Aurora, Dawn) on cores 0-1.
  svcGetThreadPriority(&st.sim_priority_before, CUR_THREAD_HANDLE);
  const Result core_rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1ull << 2);
  Result priority_rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x1C);
  int priority = 0x1C;
  if (R_FAILED(priority_rc)) { priority = 0x28; priority_rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28); }
  say("picture: simulation thread on core 2 (rc %x) at priority 0x%02X (rc %x, was 0x%02X); renderer core 1 at 0x28; decoder core 0 at 0x26; Aurora cores 0-1",
      core_rc, priority, priority_rc, st.sim_priority_before);
  switch_set_pthread_cores(0, 0x3);
  // The app also raises the GPU and memory clocks (the runner raises the CPU clock only).
  if (options.overclock) {
    st.clocks_before = switch_clocks_read();
    // Only ever upwards: a console that already runs a clock higher than the app's request keeps it.
    SwitchClockRequest request;
    if (st.clocks_before.gpu_hz / 1000000u < kSwitchClocksErista.gpu_mhz) request.gpu_mhz = kSwitchClocksErista.gpu_mhz;
    if (st.clocks_before.mem_hz / 1000000u < kSwitchClocksErista.mem_mhz) request.mem_mhz = kSwitchClocksErista.mem_mhz;
    if (request.gpu_mhz || request.mem_mhz) {
      switch_clocks_apply(request);
      st.clocks_changed = true;
    }
    const SwitchClockResult now = switch_clocks_read();
    say("picture: clocks: gpu %u -> %u MHz, memory %u -> %u MHz, cpu %u MHz reported", st.clocks_before.gpu_hz / 1000000u, now.gpu_hz / 1000000u,
        st.clocks_before.mem_hz / 1000000u, now.mem_hz / 1000000u, now.cpu_hz / 1000000u);
  }
#endif
  st.queue = std::make_unique<QueueBackend>();
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
  setenv("MELEE_GPU_THREAD", st.threaded ? "1" : "0", 1);
  gx::init(st.queue.get());        // starts the decoder thread (it pins itself to core 0 on the console)
  linux_host::set_video_sinks(fifo_sink, retrace_sink, nullptr);
  st.attached = true;
}

void detach() {
  if (!st.attached) return;
  st.attached = false;
  linux_host::set_video_sinks(nullptr, nullptr, nullptr);
  try { gx::init(nullptr); } catch (...) {}      // drains and joins the decoder thread
  st.stop.store(true);
  if (st.thread_started) { pthread_join(st.thread, nullptr); st.thread_started = false; }
#ifdef __SWITCH__
  switch_set_pthread_cores(-1, 0);
  svcSetThreadPriority(CUR_THREAD_HANDLE, st.sim_priority_before);
  if (st.clocks_changed) {
    SwitchClockRequest back; back.gpu_mhz = st.clocks_before.gpu_hz / 1000000u; back.mem_mhz = st.clocks_before.mem_hz / 1000000u;
    switch_clocks_apply(back);
    const SwitchClockResult restored = switch_clocks_read();
    say("picture: clocks: gpu put back to %u MHz, memory to %u MHz", restored.gpu_hz / 1000000u, restored.mem_hz / 1000000u);
    st.clocks_changed = false;
  }
#endif
  uint64_t commands = 0, draws = 0, vertices = 0; uint32_t copies = 0;
  gx::stats(&commands, &draws, &vertices, &copies);
  auto& r = st.render;
  say("picture: decoded %llu FIFO bytes in %llu batches: %llu commands, %llu draws, %llu vertices, %u EFB copies%s", (unsigned long long)st.fifo_bytes,
      (unsigned long long)st.fifo_batches, (unsigned long long)commands, (unsigned long long)draws, (unsigned long long)vertices, copies,
      st.decoder_failed ? " (DECODER STOPPED EARLY, see above)" : "");
  say("picture: %llu frames finished by the decoder, %llu drawn, %llu skipped as stale (a newer one was waiting), %llu given up because the queue was full; "
      "most draws in a frame %zu, %llu texture copies drawn%s", (unsigned long long)st.queue->finished.load(), (unsigned long long)r.frames, (unsigned long long)r.stale,
      (unsigned long long)st.queue->queue.overflow_dropped.load(), r.most_draws, (unsigned long long)r.texture_copies, r.failed ? " (RENDERER STOPPED EARLY, see above)" : "");
  say("picture: timing of VS match frames, ms per frame, by thread%s", options.pace ? " (the runner's own frame times below include the 60 Hz wait; these do not)" : "");
  st.work_match.report("picture: simulation thread: work (game, handing the stream over, audio mixing)", true);
  st.feed_match.report(st.threaded ? "picture: simulation thread:   of that, handing the stream to the decoder" : "picture: simulation thread:   of that, the decoder (--gfx-sync)");
  if (st.threaded) st.wait_match.report("picture: simulation thread: then waiting for the decoder at the retrace (inside the 60 Hz wait when paced)");
  if (st.threaded) st.decode_match.report("picture: decoder thread: all work");
  st.snapshot_match.report("picture: decoder:   of that, texture copies out of game memory");
  st.record_match.report("picture: decoder:   of that, draw records (state, vertex and array copies)");
  st.handoff_match.report("picture: decoder:   of that, handing the frame to the render queue");
  r.age_match.report("picture: render thread: frame waited in the queue");
  r.total_match.report("picture: render thread: all work on a frame");
  r.convert_match.report("picture: render thread:   of that, converting the frame and feeding Aurora");
  r.end_frame_match.report("picture: render thread:   of that, aurora_end_frame (waits for Aurora's FIFO and render workers; they present)");
  if (!st.pad_match.values.empty()) {
    st.pad_match.report("picture: controllers read -> the frame simulated from them decoded (one 60 Hz period when paced, plus the decoder wait)");
    say("picture: input to picture is the three lines \"controllers read -> decoded\", \"waited in the queue\" and \"render thread: all work\" added up, "
        "plus Aurora's render worker and the display's own wait for its next refresh, which are not measured here");
  }
  say("picture: all frames (menus, loading, match):");
  st.work_all.report("picture: simulation thread: work", true);
  if (st.threaded) st.wait_all.report("picture: simulation thread: waiting for the decoder");
  r.total.report("picture: render thread: all work on a frame");
  st.queue.reset();
  say("---- picture frontend done ----");
  if (log_file) { std::fclose(log_file); log_file = nullptr; }
}
}  // namespace
}  // namespace fe

// Weak hook called by the host services (patches/0101-host-frontend-hooks.patch): 1 after the host is
// initialized and before the game runs, 0 after the run and before the game is unmapped. Simulation thread.
extern "C" void mu_frontend_attach(int on) {
  if (on) { fe::attach(); fe::io_start(fe::st.attached); }
  else { fe::io_stop(); fe::detach(); }
}

#if MU_FE_AURORA
// -Wl,--wrap=aurora_begin_frame,--wrap=aurora_end_frame: gx::AuroraBackend's calls come through here so the
// render thread's time can be split. Render thread only.
extern "C" bool __real_aurora_begin_frame();
extern "C" void __real_aurora_end_frame();
extern "C" bool __wrap_aurora_begin_frame() {
  const uint64_t t0 = host::now_ns();
  const bool ok = __real_aurora_begin_frame();
  fe::begin_frame_ns += host::now_ns() - t0;
  return ok;
}
extern "C" void __wrap_aurora_end_frame() {
  const uint64_t t0 = host::now_ns();
  __real_aurora_end_frame();
  fe::end_frame_ns += host::now_ns() - t0;
}
#endif
