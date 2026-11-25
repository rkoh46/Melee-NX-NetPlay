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
#include "replay_view.h"                // a paused replay asks for repaints (feature module, weak)
#include "features/present/present.h"   // presentation at the display's rate, sub-frames (feature module, weak)
#include "hostprof.h"                // the runner's host service profiler (--profile)
#if MU_FE_AURORA
#include "gx_aurora.h"
#include "metal_frontend.h"          // the screens before the game take the app's MetalFrontend (switch_signin.h)
#include "overlay.h"
#include "overlay_imgui.h"
#include "mu_ui.h"
#include "gfx_live.h"                // the display wave's renderer side (gfx_render.cpp): its threads end before Aurora
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_video.h>
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

namespace linux_host { uint64_t slept_nanoseconds(); }   // ../runner/mu_elf_bridge.h: the host's own 60 Hz waits so far

namespace fe {
namespace features { __attribute__((weak)) bool practice_menu_open(); }   // features/proai.cpp: the Practice menu is open
Options options;
uint64_t sim_cost[3] = {};
bool in_resim = false;

namespace {
FILE* log_file = nullptr;
std::mutex log_mutex;
inline uint64_t now_ns() { return host::now_ns(); }
inline double ms(uint64_t ns) { return double(ns) * 1e-6; }
}  // namespace

// Whole lines to the console (stdout: nxlink on the console) and to <out>/picture.log.
void log_write(const char* text, size_t size, bool to_console, bool to_file) {
  std::lock_guard<std::mutex> lock(log_mutex);
  if (to_console) { std::fwrite(text, 1, size, stdout); std::fflush(stdout); }
  if (to_file && log_file) { std::fwrite(text, 1, size, log_file); std::fflush(log_file); }
}
void log_open() {
  std::lock_guard<std::mutex> lock(log_mutex);
  if (log_file) return;
  mkdir(options.out_dir.c_str(), 0777);
  log_file = std::fopen((options.log_path.empty() ? options.out_dir + "/picture.log" : options.log_path).c_str(), "a");
  if (log_file) std::setvbuf(log_file, nullptr, _IOFBF, 1u << 16);
}
void log_close() {
  std::lock_guard<std::mutex> lock(log_mutex);
  if (log_file) { std::fclose(log_file); log_file = nullptr; }
}
void say(const char* format, ...) {
  char line[2048];
  va_list args; va_start(args, format); const int n = std::vsnprintf(line, sizeof line - 1, format, args); va_end(args);
  if (log_queue_push(line)) return;        // play mode, while the game runs: written by the log thread (log_queue.cpp)
  size_t length = n < 0 ? 0 : std::min<size_t>(size_t(n), sizeof line - 2);
  line[length++] = '\n';
  log_write(line, length, true, true);
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
  bool wants_state_deltas() const override { return !(present::linked() && present::wants_full_state()); }   // sub-frames need every draw's full state
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
  hostprof::Sample prof_first{}, prof_last{}; uint64_t prof_frames = 0;   // --profile: totals at the first and the last match frame
  uint64_t prev_pad_ns = 0;      // the controller read that the frame now being simulated uses
  // What was inside each match frame's work, to explain the slow ones (all on the simulation thread).
  struct FrameCost { uint32_t retrace; float work, feed, wait, pad, store, sync, command; uint32_t lines, disc_reads, resim_bytes; };
  std::vector<FrameCost> costs;
  uint64_t last_slept = 0, last_cost[3] = {}, last_lines = 0, last_disc = 0;
  uint64_t resim_fifo_bytes = 0, resim_fifo_frame = 0, sync_calls = 0;
  Series work_match, work_all, feed_match, wait_match, wait_all, decode_match, snapshot_match, record_match, handoff_match, pad_match, decode_all, padcost_all, padcost_match;
#ifdef __SWITCH__
  s32 sim_priority_before = 0x2C;
  bool clocks_changed = false;
  SwitchClockResult clocks_before{};
#endif
  // The render thread started before the game (app mode: screens_start). Guarded by mutex.
  bool early = false, early_ready = false, screens_ok = false, go = false, quit = false;
  void (*job)(void*, void*) = nullptr;
  void* job_user = nullptr;
  std::atomic<int> win_w{1280}, win_h{720};
} st;

void decoder_stopped(const char* reason) {
  if (st.decoder_failed) return;
  st.decoder_failed = true;
  say("picture: DECODER STOPPED after %llu FIFO bytes: %s", (unsigned long long)st.fifo_bytes, reason);
}

// The game calls this from its pipe flush (C code without unwind tables): nothing may be thrown through it.
void fifo_sink(const uint8_t* data, uint32_t size, void*) {
  if (st.decoder_failed) return;
  if (in_resim) { st.resim_fifo_bytes += size; st.resim_fifo_frame += size; }
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
  const bool vs_match = fighting;      // any mode with fighters simulating (VS, training, the single-player modes)
  io_retrace(linux_host::counters().retraces, state.scene_major, state.scene, fighting);
  const double pad_ms = io_take_pad_ms();
  st.last_match_frame = state.match_frame;
  const uint64_t decode = host::sim_cost_ns(host::SIM_DECODE), snapshot = host::sim_cost_ns(host::SIM_SNAPSHOT), record = host::sim_cost_ns(host::SIM_RECORD);
  const uint64_t handoff = st.queue ? st.queue->handoff_ns.load(std::memory_order_relaxed) : 0;
  // With netplay the host paces (its waits lie inside the frame, before this function): not work.
  const uint64_t slept = linux_host::slept_nanoseconds(), lines = log_queue_lines(), disc = linux_host::counters().disc_reads;
  if (st.have_resumed) {
    const double work = ms(entered - st.resumed) - ms(slept - st.last_slept);
    hud_frame(work);
    st.work_all.add(work); st.wait_all.add(ms(waited)); st.decode_all.add(ms(decode - st.last_decode)); st.padcost_all.add(pad_ms);
    if (vs_match) {
      st.work_match.add(work); st.feed_match.add(ms(st.feed_ns_frame)); st.wait_match.add(ms(waited)); st.padcost_match.add(pad_ms);
      st.decode_match.add(ms(decode - st.last_decode)); st.snapshot_match.add(ms(snapshot - st.last_snapshot));
      st.record_match.add(ms(record - st.last_record)); st.handoff_match.add(ms(handoff - st.last_handoff));
      // The game samples the controllers in its retrace handler, just before this function runs; the frame
      // simulated from that sample is the one decoded by the next retrace.
      if (st.prev_pad_ns) st.pad_match.add(ms(entered + waited - st.prev_pad_ns));
      st.costs.push_back({linux_host::counters().retraces, float(work), float(ms(st.feed_ns_frame)), float(ms(waited)), float(pad_ms),
                          float(ms(sim_cost[0] - st.last_cost[0])), float(ms(sim_cost[1] - st.last_cost[1])), float(ms(sim_cost[2] - st.last_cost[2])),
                          uint32_t(lines - st.last_lines), uint32_t(disc - st.last_disc), uint32_t(st.resim_fifo_frame)});
    }
  }
  st.last_slept = slept; st.last_lines = lines; st.last_disc = disc; st.resim_fifo_frame = 0;
  for (int i = 0; i < 3; ++i) st.last_cost[i] = sim_cost[i];
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
  if (hostprof::installed()) {
    // This function runs inside a host service (the retrace): its waits are not that service's work.
    hostprof::discount((now_ns() - entered) * hostprof::frequency() / 1000000000ull);
    if (vs_match) { hostprof::read(&st.prof_last); if (!st.prof_frames++) st.prof_first = st.prof_last; }
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

void render_one(gx::Backend& backend, const gx::Frame& frame, const gx::DrawMatrices* overrides = nullptr) {
  auto& r = st.render;
  if (r.failed) return;
  if (texture_dump.on) texture_dump.frame(frame);
  const uint64_t t0 = now_ns(), end0 = end_frame_ns, begin0 = begin_frame_ns;
  const char* error = nullptr; std::string text;
  try { if (overrides) backend.submit_frame(frame, overrides); else backend.submit_frame(frame); }
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
  if (gx::frame_in_match(frame)) { r.total_match.add(total); r.convert_match.add(convert); r.end_frame_match.add(end); r.age_match.add(age); }
  r.draws += frame.draws.size();
  for (const auto& copy : frame.copies) if (!copy.to_xfb) ++r.texture_copies;
  ++r.frames;
  r.most_draws = std::max(r.most_draws, frame.draws.size());
}

// The window's events, once per presented frame of the presenter's loop (features/present).
void pump_window() {
#if MU_FE_AURORA
  for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
    if (event->type == AURORA_EXIT) linux_host::request_stop(0);
    else if (event->type == AURORA_WINDOW_RESIZED || event->type == AURORA_DISPLAY_SCALE_CHANGED) { st.win_w = int(event->windowSize.width); st.win_h = int(event->windowSize.height); }
#endif
}
void render_with(gx::Backend& backend, const gx::Frame& frame, const gx::DrawMatrices* overrides) { render_one(backend, frame, overrides); }
PresentHook present_hook;
std::atomic<double> refresh_hz{60.0};
void render_loop(gx::Backend& backend, bool pump) {
  auto& queue = st.queue->queue;
  gx::Frame frame, last;                   // last: the newest frame drawn, kept for a repaint
  bool have_last = false;
  uint64_t last_draw_ns = 0;
  for (;;) {
#if MU_FE_AURORA
    if (pump)
      for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
        if (event->type == AURORA_EXIT) linux_host::request_stop(0);
        else if (event->type == AURORA_WINDOW_RESIZED || event->type == AURORA_DISPLAY_SCALE_CHANGED) { st.win_w = int(event->windowSize.width); st.win_h = int(event->windowSize.height); }
#else
    (void)pump;
#endif
    if (!queue.pop(frame)) {               // waits up to 2 ms
      if (st.stop.load(std::memory_order_acquire) && queue.size() == 0) break;
      // A paused replay sends no frames, but its bar, its tips and the in-game menu still change (replay_view.h):
      // the newest frame is drawn again, at most 60 times a second.
      if (have_last && &features::replay_repaint_wanted && features::replay_repaint_wanted() && now_ns() - last_draw_ns >= 16'000'000) {
        last_draw_ns = now_ns();
        render_one(backend, last);
      }
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
    // A replay jumping to a frame (replay_view.h): its frames on the way are simulated but not shown.
    if (&features::replay_frames_unseen && features::replay_frames_unseen()) {
      if (have_last) queue.recycle(std::move(last));
      last = std::move(frame); have_last = true; frame = gx::Frame{};
      continue;
    }
    render_one(backend, frame);
    // A sub-frame presenter (present_hook: display-rate features) may present this frame again before the next
    // finished one, as long as no newer frame is waiting. Without one: one present per finished game frame.
    if (present_hook.presents_per_frame && present_hook.present_again) {
      const int n = present_hook.presents_per_frame();
      for (int i = 1; i < n && queue.size() == 0 && !st.render.failed; ++i)
        if (!present_hook.present_again(&backend, &frame, i)) break;
    }
    last_draw_ns = now_ns();
    if (have_last) queue.recycle(std::move(last));
    last = std::move(frame); have_last = true;
    frame = gx::Frame{};
  }
  if (have_last) queue.recycle(std::move(last));
}

#if MU_FE_AURORA
// The screens before the game (app mode) use the app's own drawing code (switch_signin.cpp), which takes a
// host::MetalFrontend: one made here over Aurora as this thread already runs it, drawing no game frames.
class ScreenHost final : public host::MetalNativeHost {
public:
  host::MetalNativeEvents pump_events() override {
    host::MetalNativeEvents result;
    for (auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event) {
      ++result.events;
      if (event->type == AURORA_EXIT) result.close_requested = true;
      if (event->type == AURORA_WINDOW_RESIZED || event->type == AURORA_DISPLAY_SCALE_CHANGED) {
        st.win_w = int(event->windowSize.width); st.win_h = int(event->windowSize.height); ++result.resize_events;
      }
    }
    return result;
  }
  void resize(uint32_t, uint32_t) override {}
  host::MetalWindowSize window_size() const noexcept override {
    const uint32_t w = uint32_t(st.win_w.load()), h = uint32_t(st.win_h.load());
    return {w, h, w, h};
  }
  void shutdown() noexcept override {}   // Aurora stays up: the game is drawn next on this thread
};
class NoFrames final : public gx::Backend {
public:
  void submit_frame(const gx::Frame&) override {}
};
// Runs screen jobs until attach() (go) or screens_stop() (quit).
void screens_loop() {
  auto frontend = std::make_unique<host::MetalFrontend>(std::make_unique<ScreenHost>(), std::make_unique<NoFrames>());
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.early_ready = true; st.screens_ok = true;
  }
  st.changed.notify_all();
  for (;;) {
    std::unique_lock<std::mutex> lock(st.mutex);
    st.changed.wait(lock, [] { return st.job || st.go || st.quit; });
    if (!st.job) break;
    auto job = st.job; void* user = st.job_user;
    lock.unlock();
    try { job(frontend.get(), user); } catch (const std::exception& e) { say("picture: a screen stopped: %s", e.what()); } catch (...) { say("picture: a screen stopped"); }
    lock.lock();
    st.job = nullptr;
    lock.unlock();
    st.changed.notify_all();
  }
  frontend.reset();
}
#endif

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
      // The app's launcher.ini msaa and anisotropy (app.cpp sets these; fixed when the device is created).
      if (const char* m = std::getenv("MELEE_MSAA")) config.msaa = uint32_t(std::max(1, std::atoi(m)));
      if (const char* a = std::getenv("MELEE_ANISOTROPY")) config.maxTextureAnisotropy = uint16_t(std::max(1, std::atoi(a)));
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
      st.win_w = int(info.windowSize.width); st.win_h = int(info.windowSize.height);
      if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); mode && mode->refresh_rate > 1.0f) refresh_hz = double(mode->refresh_rate);
      say("picture: display refresh %.2f Hz (the simulation stays at 60 Hz)", refresh_hz.load());
      VIConfigure(nullptr);   // the 640x480 logical framebuffer gx::AuroraBackend expects
      if (options.app) mu_ui::fonts_init();   // melee-unlocked's fonts for its panel and launcher (mu_ui.h), before the first frame
      if (const char* f = std::getenv("MU_APP_FILTER")) {   // launcher.ini filter (the app's screen filter)
        const int filter = std::atoi(f);
        aurora_set_resampler(filter == 1 ? SAMPLER_AREA : filter == 2 ? SAMPLER_CRT : filter == 3 ? SAMPLER_VIBRANT : SAMPLER_BILINEAR);
      }
      {
        const uint64_t t0 = now_ns();
        aurora_warm_pipelines();   // returns at once when nothing is cached yet (first run)
        say("picture: pipeline warm-up took %.1f s%s", ms(now_ns() - t0) / 1000.0, options.warm_all ? " (--warm-all: every pipeline in the cache)" : "");
      }
      if (st.early) {
        screens_loop();                // the app's screens before the game, until attach() or screens_stop()
        if (st.quit) throw std::runtime_error("the game was not started");
      }
      // Settled only now: in the app the 16:9 setting is known once settings.ini has been read.
      if (!options.stretch) aurora::window::set_frame_buffer_aspect(4.0f / 3.0f);   // the game draws for 4:3 (no widescreen code is on)
      auto aurora_backend = std::make_unique<gx::AuroraBackend>(gx::AuroraOptions{.logical_width = 640, .logical_height = 480, .reset_clear_color = 0, .reset_clear_z = 0xffffff});
      // Diagnosis: MU_GFX_DRAW_DUMP=MAJOR:MINOR:N writes the draw state of the N-th frame drawn in that scene to <out>/draws.txt.
      if (const char* dump = std::getenv("MU_GFX_DRAW_DUMP")) {
        unsigned major = 0, minor = 0, after = 1;
        if (std::sscanf(dump, "%u:%u:%u", &major, &minor, &after) == 3) aurora_backend->set_draw_dump(options.out_dir + "/draws.txt", uint8_t(major), uint8_t(minor), after);
      }
      // The app's in-game menu and HUD (game_menu.cpp, drawn through Aurora's ImGui pass as the app draws them).
      // The feature modules' lines (frame advantage...) in every play session, the menu and HUD in the app.
      if (options.play) aurora_backend->set_overlay([] {
        int w = 1280, h = 720; window_size(&w, &h);
        overlay_draw(w, h);   // the overlays and picture options (overlay_draw.cpp), under the menu and HUD
        host::OverlayFrame overlay;
        if (options.app) host::game_overlay(overlay);
        feature_overlay(&overlay, w, h);
        if (!overlay.shapes.empty() || !overlay.texts.empty()) host::overlay_imgui_draw(overlay);
        if (options.app) mu_ui::panel_draw(w, h);   // melee-unlocked's settings panel (mu_panel.cpp), over everything
        // A USB keyboard (usb_keyboard.cpp): its keys to ImGui, and the box a screen's words are typed in, over the panel. The
        // Practice menu's code keyboard takes typed letters itself while it is open.
        kb_frame(w, h, &features::practice_menu_open && features::practice_menu_open());
      });
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
  if (st.early) {
    // Aurora did not come up (or there is none in this build): no screens; wait for the game or the end.
    std::unique_lock<std::mutex> lock(st.mutex);
    if (!st.early_ready) { st.early_ready = true; st.screens_ok = false; lock.unlock(); st.changed.notify_all(); lock.lock(); }
    st.changed.wait(lock, [] { return st.go || st.quit; });
    if (st.quit) {
      lock.unlock();
      backend.reset();
#if MU_FE_AURORA
      if (aurora_up) { aurora_flush_caches(); aurora_shutdown(); }
#endif
      return nullptr;
    }
  }
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.start_error = error; st.ready = true;
  }
  st.changed.notify_all();
  if (error.empty()) {
    // One presentation path: this loop presents each finished frame (and, through present_hook, again at a display
    // rate above 60 Hz); with sub-frames on, the presenter's loop takes its place and draws the in-between poses.
    try { if (present::linked() && present::wants_full_state()) present::loop(*backend, st.queue->queue, st.stop, aurora_up ? pump_window : nullptr, render_with); else render_loop(*backend, aurora_up); }
    catch (const std::exception& e) { say("picture: render thread stopped: %s", e.what()); st.queue->queue.finish(true); }
    catch (...) { say("picture: render thread stopped: unknown exception"); st.queue->queue.finish(true); }
  }
  // The renderer's objects go before Aurora does, on the thread that made them.
  if (auto* null_backend = dynamic_cast<NullBackend*>(backend.get()))
    if (null_backend->skipped_draws || null_backend->skipped_copies)
      say("picture: the frame check left out %llu draws and %llu texture copies", (unsigned long long)null_backend->skipped_draws, (unsigned long long)null_backend->skipped_copies);
#if MU_FE_AURORA
  if (aurora_up) gfx::render_end();
#endif
  backend.reset();
#if MU_FE_AURORA
  if (aurora_up) { aurora_flush_caches(); aurora_shutdown(); }
#endif
  return nullptr;
}

void attach() {
  if (!options.picture || st.attached) return;
  log_open();
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
  if (st.early && st.thread_started) {
    // The render thread has been running the screens before the game (screens_start): it draws the game now.
    { std::lock_guard<std::mutex> lock(st.mutex); st.go = true; }
    st.changed.notify_all();
  } else {
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&st.thread, &attr, render_main, nullptr) != 0) { pthread_attr_destroy(&attr); say("picture: cannot create the render thread; running without a picture"); return; }
    pthread_attr_destroy(&attr);
    st.thread_started = true;
  }
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
  st.early = false; st.early_ready = false; st.screens_ok = false; st.go = false; st.quit = false;
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
  say("picture: timing of match frames (fighters simulating), ms per frame, by thread%s", options.pace ? " (the runner's own frame times below include the 60 Hz wait; these do not)" : "");
  st.work_match.report("picture: simulation thread: work (game, handing the stream over, audio mixing)", true);
  st.feed_match.report(st.threaded ? "picture: simulation thread:   of that, handing the stream to the decoder" : "picture: simulation thread:   of that, the decoder (--gfx-sync)");
  if (options.play) st.padcost_match.report("picture: simulation thread:   of that, reading the controllers");
  if (st.threaded) st.wait_match.report("picture: simulation thread: then waiting for the decoder at the retrace (inside the 60 Hz wait when paced)");
  if (st.threaded) st.decode_match.report("picture: decoder thread: all work");
  st.snapshot_match.report("picture: decoder:   of that, texture copies out of game memory");
  st.record_match.report("picture: decoder:   of that, draw records (state, vertex and array copies)");
  st.handoff_match.report("picture: decoder:   of that, handing the frame to the render queue");
  r.age_match.report("picture: render thread: frame waited in the queue");
  r.total_match.report("picture: render thread: all work on a frame");
  r.convert_match.report("picture: render thread:   of that, converting the frame and feeding Aurora");
  r.end_frame_match.report("picture: render thread:   of that, aurora_end_frame (waits for Aurora's FIFO and render workers; they present)");
  if (!st.costs.empty()) {
    // The slowest match frames and what was inside them. Work is the simulation thread's time from one retrace to
    // the next without the 60 Hz wait: the game, the stream hand-over, the mixer, and with netplay the state saves
    // and loads, every repeated frame of a rollback and the netplay device.
    size_t over8 = 0, over16 = 0, with_store = 0, with_sync = 0, with_command = 0, with_disc = 0, with_lines = 0, with_pad = 0, none = 0;
    for (const auto& f : st.costs) {
      if (f.work < 8) continue;
      ++over8; if (f.work > 1000.0f / 60) ++over16;
      const bool a = f.store >= 1, b = f.sync >= 1, c = f.command >= 1, d = f.disc_reads != 0, e = f.lines >= 20, g = f.pad >= 1;
      with_store += a; with_sync += b; with_command += c; with_disc += d; with_lines += e; with_pad += g; none += !(a || b || c || d || e || g);
    }
    say("picture: match frames with 8 ms or more of simulation-thread work: %zu of %zu (%zu over 16.67 ms); of those, with 1 ms or more in rollback state saves and loads %zu, "
        "waiting for the decoder before them %zu, in the netplay device %zu, reading controllers %zu, with a disc read %zu, with 20 or more log lines queued %zu, none of these %zu",
        over8, st.costs.size(), over16, with_store, with_sync, with_command, with_pad, with_disc, with_lines, none);
    std::vector<size_t> order(st.costs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    const size_t top = std::min<size_t>(10, order.size());
    std::partial_sort(order.begin(), order.begin() + top, order.end(), [&](size_t x, size_t y) { return st.costs[x].work > st.costs[y].work; });
    for (size_t i = 0; i < top; ++i) {
      const auto& f = st.costs[order[i]];
      say("picture: slow frame %zu: %.3f ms at retrace %u (%zu frames into the fighting): stream hand-over %.3f, controllers %.3f, state saves and loads %.3f, decoder wait before them %.3f, "
          "netplay device %.3f ms; %u log lines queued, %u disc reads, %u stream bytes from repeated frames; then %.3f ms waiting for the decoder at the retrace",
          i + 1, f.work, f.retrace, order[i], f.feed, f.pad, f.store, f.sync, f.command, f.lines, f.disc_reads, f.resim_bytes, f.wait);
    }
  }
  if (sim_cost[0] || st.sync_calls)
    say("picture: netplay: the decoder was brought up to date %llu times before a state save or load (%.1f ms in all); %llu stream bytes arrived from repeated frames of rollbacks",
        (unsigned long long)st.sync_calls, ms(sim_cost[1]), (unsigned long long)st.resim_fifo_bytes);
  if (!st.pad_match.values.empty()) {
    st.pad_match.report("picture: controllers read -> the frame simulated from them decoded (one 60 Hz period when paced, plus the decoder wait)");
    say("picture: input to picture is the three lines \"controllers read -> decoded\", \"waited in the queue\" and \"render thread: all work\" added up, "
        "plus Aurora's render worker and the display's own wait for its next refresh, which are not measured here");
  }
  if (st.prof_frames > 1) {
    // Where the simulation thread's time goes in a match frame: each host service the game calls (exclusive
    // time, the frontend's sinks included in theirs) and each game function the host calls.
    const double frames = double(st.prof_frames - 1), tick_ms = 1000.0 / double(hostprof::frequency());
    say("picture: profile of the simulation thread over %llu match frames (calls and ms per frame; the game's own code is the rest of the work line above):", (unsigned long long)st.prof_frames - 1);
    double services = 0;
    for (int i = 0; i < hostprof::kSlots; ++i) {
      const double calls = double(st.prof_last.calls[i] - st.prof_first.calls[i]) / frames, ms_frame = double(st.prof_last.ticks[i] - st.prof_first.ticks[i]) * tick_ms / frames;
      if (ms_frame < 0.005) continue;
      say("picture:   %-34s %9.2f calls %8.3f ms", hostprof::name(i) ? hostprof::name(i) : "?", calls, ms_frame);
      if (hostprof::name(i) && !std::strncmp(hostprof::name(i), "host ", 5)) services += ms_frame;
    }
    say("picture:   host services together %.3f ms", services);
  }
  say("picture: all frames (menus, loading, match):");
  st.work_all.report("picture: simulation thread: work", true);
  if (options.play) st.padcost_all.report("picture: simulation thread:   of that, reading the controllers");
  if (st.threaded) st.decode_all.report("picture: decoder thread: all work");
  if (st.threaded) st.wait_all.report("picture: simulation thread: waiting for the decoder");
  r.total.report("picture: render thread: all work on a frame");
  st.queue.reset();
  say("---- picture frontend done ----");
  if (!options.play) log_close();          // play mode has more to say (play_main.cpp closes it)
}
}  // namespace
}  // namespace fe

// Weak hook called by the host services (patches/0101-host-frontend-hooks.patch): 1 after the host is
// initialized and before the game runs, 0 after the run and before the game is unmapped. Simulation thread.
namespace fe {
// The rollback store copies and (on the console) remaps pages of game memory: the decoder thread must not be
// reading them. Called on the simulation thread before every state save and load.
void decoder_sync() {
  if (!st.attached || !st.threaded || st.decoder_failed) return;
  ++st.sync_calls;
  try { gx::gpu_sync_all(); } catch (...) {}
}
}  // namespace fe

namespace fe {
bool screens_start() {
  if (!options.picture || options.null_backend) return false;
#if !MU_FE_AURORA
  return false;
#else
  if (st.thread_started) return st.early && st.screens_ok;
#ifdef __SWITCH__
  switch_set_pthread_cores(0, 0x3);   // Aurora's workers stay off the game's core 2, as attach() arranges it
#endif
  st.early = true; st.early_ready = false; st.go = false; st.quit = false;
  pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
  const int rc = pthread_create(&st.thread, &attr, render_main, nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0) { st.early = false; say("picture: cannot create the render thread for the screens"); return false; }
  st.thread_started = true;
  std::unique_lock<std::mutex> lock(st.mutex);
  st.changed.wait(lock, [] { return st.early_ready; });
  return st.screens_ok;
#endif
}
bool screens_running() { return st.early && st.screens_ok && !st.go && !st.quit; }
bool screens_run(void (*job)(void*, void*), void* user) {
  if (!screens_running()) return false;
  std::unique_lock<std::mutex> lock(st.mutex);
  st.job = job; st.job_user = user;
  st.changed.notify_all();
  st.changed.wait(lock, [] { return st.job == nullptr; });
  return true;
}
void screens_stop() {
  if (!st.early || !st.thread_started || st.go) return;
  { std::lock_guard<std::mutex> lock(st.mutex); st.quit = true; }
  st.changed.notify_all();
  pthread_join(st.thread, nullptr);
  st.thread_started = false; st.early = false; st.early_ready = false; st.screens_ok = false; st.quit = false;
#ifdef __SWITCH__
  switch_set_pthread_cores(-1, 0);
#endif
}
void window_size(int* w, int* h) { *w = st.win_w.load(); *h = st.win_h.load(); }
void set_present_hook(const PresentHook& hook) { present_hook = hook; }   // before the game starts
double display_refresh_hz() { return refresh_hz.load(); }
}  // namespace fe

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
