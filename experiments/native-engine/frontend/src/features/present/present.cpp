// Presentation at the display's rate, with optional sub-frame poses (melee-unlocked's frame-rate unlock,
// FEATURES.md section 2). The game keeps simulating at 60 Hz; this only decides what the render thread draws
// between two game frames. Nothing here touches game memory or any simulation state.
//
//   subframe 0   Off: every presented frame is the newest game frame as it is. On a 120 Hz display each game
//                frame is presented twice (once per refresh), on 60 Hz once.
//   subframe 1   Predict: poses extrapolated from the newest game frame by the time since it arrived (up to one
//                frame ahead); no added delay. The mode that lowers latency; the one to use at 120 Hz.
//   subframe 2   Interpolate: poses between the two newest game frames; one game frame of added delay.
//   fps N        presentation cap (N presented frames per second), e.g. 120 to test without a 120 Hz display;
//                -1 (default) the display's own rate.
// Upstream's settings keys (port-settings.ini `subframe`, `fps`). The poses come from this repository's sub-frame
// solver (port/runtime/gx/subframe.cpp: per-draw matrix deltas between paired draws, cuts held), i.e. upstream's
// matrix modes; upstream's authored-pose path (native_pose_snapshot) is not wired.
//
// The display rate is measured at start: the first game frame is presented back to back a few dozen times and
// the median interval between presents is the refresh period (with vsync the swap chain blocks once per
// refresh). The log line is `present: display refresh measured N Hz (median interval M ms over K presents)`.
// A rollback, a Training Lab load or a replay jump (mark_discontinuity) is never blended across. Online, when
// the render thread runs late for 90% of the presented frames of the last 3 s, sub-frames pause (as upstream's
// online_frame_budget.h) and come back below 50%.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "feature_hooks.h"
#include "features/present/present.h"
#include "replay_view.h"            // a paused replay asks for repaints

#include "frame_queue.h"
#include "subframe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace fe::features { bool online(); }

// The solver's authored-pose path (authored_pose.cpp) samples the game's packed animation tracks; this build
// has no authored poses (no native_pose_snapshot), so that path is never taken. The definition the app's
// static recompilation provides is not part of this build: a sampler that never samples.
#include "PackedAnimation.h"
namespace NativeMelee { bool SamplePacked(const PackedTrack&, float, float&) { return false; } }

namespace fe::present {
namespace {
std::atomic<int> mode{0};               // 0 off, 1 predict, 2 interpolate
std::atomic<int> fps_cap{-1};
std::atomic<uint64_t> discontinuity{0};
std::atomic<double> measured_hz{0.0};
constexpr double kGamePeriod = 1.0 / 60.0;
double now_s();
// A repaint of a paused replay (replay_view.h): at most 60 a second.
bool start_repaint_due(double& last) {
  const double t = now_s();
  if (t - last < kGamePeriod) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); return false; }
  last = t;
  return true;
}

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool setting(const std::string& key, const std::string& value) {
  const int n = std::atoi(value.c_str());
  if (key == "subframe") mode = n == 0 ? 0 : n == 2 ? 2 : 1;        // upstream: 0 off, 2 interpolate, other predict
  else if (key == "fps") fps_cap = n <= 0 ? -1 : std::clamp(n, 30, 480);
  else return false;
  return true;
}
const char* mode_name(int m) { return m == 0 ? "off" : m == 1 ? "predict" : "interpolate"; }

struct Series {
  std::vector<float> v;
  void add(double x) { if (v.size() < 2000000) v.push_back(float(x)); }
  void report(const char* label) {
    if (v.empty()) { say("%s: none", label); return; }
    std::vector<float> s = v; std::sort(s.begin(), s.end());
    double sum = 0; for (float x : s) sum += x;
    say("%s: %zu, avg %.3f ms, median %.3f, p99 %.3f, worst %.3f", label, s.size(), sum / double(s.size()), s[s.size() / 2], s[size_t(double(s.size()) * 0.99)], s.back());
  }
};
}  // namespace

void configure_from_arguments(const std::vector<std::string>& args) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--frame-mode") { const std::string& v = args[i + 1]; mode = v == "off" ? 0 : (v == "authored-interpolate" || v == "interpolate") ? 2 : 1; }
    else if (args[i] == "--fps") setting("fps", args[i + 1]);
  }
}
// The command line counts in every run (also the scripted ones, which never read settings.ini): taken once here.
void from_command_line() {
  static const bool once = [] {
    configure_from_arguments(options.rest);
    // The scripted runs pass their arguments to the runner, which takes no others: MU_FRAME_MODE and MU_PRESENT_FPS.
    std::vector<std::string> env;
    if (const char* m = std::getenv("MU_FRAME_MODE")) { env.push_back("--frame-mode"); env.push_back(m); }
    if (const char* f = std::getenv("MU_PRESENT_FPS")) { env.push_back("--fps"); env.push_back(f); }
    configure_from_arguments(env);
    return true;
  }();
  (void)once;
}
bool active() { from_command_line(); return mode.load() != 0 || fps_cap.load() > 0; }
bool wants_full_state() { from_command_line(); return mode.load() != 0; }
void mark_discontinuity() { discontinuity.fetch_add(1, std::memory_order_relaxed); }
double display_hz() { return measured_hz.load(); }

void loop(gx::Backend& backend, gx::FrameQueue& queue, const std::atomic<bool>& stop, void (*pump)(), Render render) {
  const int m0 = mode.load();
  gx::Frame prev, cur, incoming;
  bool have_cur = false, have_prev = false;
  double cur_arrival = 0;
  uint64_t seen_discontinuity = discontinuity.load();
  gx::SubFrameSolver solver;
  std::vector<gx::DrawMatrices> mats;
  Series cost, cost_match, interval, phase;
  uint64_t presents = 0, repeats = 0, game_frames = 0, stale = 0, paused_presents = 0, late = 0;
  std::deque<bool> late_window;
  bool paused = false;
  // Display rate: presents of the first frame back to back.
  bool calibrated = fps_cap.load() > 0;          // with a cap the rate is the cap's; the display is not measured
  if (calibrated) say("present: presentation capped at %d frames per second; the display's refresh is not measured", fps_cap.load());
  std::vector<double> calib;
  double last_present = 0, next_tick = 0, pace_hz = 0;
  say("present: frames presented at the display's rate%s, sub-frames %s", fps_cap.load() > 0 ? (" (capped at " + std::to_string(fps_cap.load()) + " per second)").c_str() : "",
      mode_name(m0));
  for (;;) {
    if (pump) pump();
    // Take the newest finished game frame. Before the first one, and in Off mode on a display no faster than the
    // game, wait for it; otherwise only look.
    const double hz_now = fps_cap.load() > 0 ? double(fps_cap.load()) : measured_hz.load();   // 0 until measured
    // One present per game tick (a 60 Hz display, the console's panel and dock, or a cap of 60): sub-frame re-posing has no
    // smoothness to add there, only delay, so it is skipped as upstream skips it (threaded_backend.cpp: presented rate 60).
    // MU_SUBFRAME_AT_60=1 keeps it (tests of the solver on a 60 Hz display).
    static const bool keep_at_60 = std::getenv("MU_SUBFRAME_AT_60") != nullptr;
    const bool one_per_tick = m0 != 0 && calibrated && hz_now > 1.0 && hz_now < 70.0 && !keep_at_60;
    {
      static int told = -1;
      if (calibrated && int(one_per_tick) != told) {
        told = one_per_tick;
        if (one_per_tick) say("present: %.1f presents a second, one per game frame: sub-frame re-posing skipped (no smoothness to gain, only delay)", hz_now);
      }
    }
    const bool off_like = m0 == 0 || one_per_tick;
    const bool wait_for_new = !have_cur || (calibrated && off_like && fps_cap.load() <= 0 && hz_now < 70.0);
    bool got = false;
    if (wait_for_new) got = queue.pop(incoming);
    else got = queue.try_pop(incoming);
    if (!got && stop.load(std::memory_order_acquire) && queue.size() == 0) break;
    if (got) {
      for (gx::Frame newer; queue.try_pop(newer);) { queue.recycle(std::move(incoming)); incoming = std::move(newer); newer = gx::Frame{}; ++stale; }
      if (have_prev) queue.recycle(std::move(prev));
      prev = std::move(cur); have_prev = have_cur;
      cur = std::move(incoming); incoming = gx::Frame{};
      have_cur = true; ++game_frames;
      cur_arrival = now_s();
      const uint64_t d = discontinuity.load(std::memory_order_relaxed);
      const bool cut = d != seen_discontinuity || !have_prev;
      seen_discontinuity = d;
      if (m0) {
        solver.set_menu_mode(!gx::frame_in_match(cur));
        solver.set_frames(cut ? &cur : &prev, &cur);    // a cut pairs the frame with itself: every draw holds
      }
    }
    if (!have_cur) continue;
    // A replay jumping to a frame: its frames on the way are not shown (replay_view.h).
    if (&features::replay_frames_unseen && features::replay_frames_unseen()) { if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
    // Pacing: with a cap (or when presents do not wait for vsync), the next tick of that rate's grid; otherwise the
    // swap chain's vsync blocks in the present.
    if (fps_cap.load() > 0 || pace_hz > 0) {
      const double period = 1.0 / (fps_cap.load() > 0 ? double(fps_cap.load()) : pace_hz), t = now_s();
      if (next_tick == 0 || t - next_tick > 4 * period) next_tick = t;
      if (next_tick > t) std::this_thread::sleep_for(std::chrono::duration<double>(next_tick - t));
      next_tick += period;
    } else if (calibrated && !got && off_like && hz_now < 70.0) {
      // 60 Hz display, Off: nothing new to show, unless a paused replay's overlay or the menu changes (replay_view.h).
      static double repainted = 0;
      if (!(&features::replay_repaint_wanted && features::replay_repaint_wanted() && start_repaint_due(repainted))) continue;
    }
    // Draw.
    const double start = now_s();
    const bool online_now = features::online();
    bool sub = m0 != 0 && !paused && calibrated && !one_per_tick;
    if (sub) {
      double t = std::clamp((start - cur_arrival) / kGamePeriod, 0.0, 1.0);
      solver.build(t, m0 == 2, mats);
      phase.add(t * 1000.0);
      render(backend, cur, mats.data());
    } else {
      render(backend, cur, nullptr);
      if (m0) ++paused_presents;
    }
    const double end = now_s();
    const double work = (end - start) * 1000.0;
    cost.add(work);
    if (gx::frame_in_match(cur)) cost_match.add(work);
    if (!got) ++repeats;
    ++presents;
    if (last_present > 0) interval.add((end - last_present) * 1000.0);
    // The display's rate, from presents back to back of the first frame (vsync blocks once per refresh).
    if (!calibrated) {
      if (last_present > 0) calib.push_back(end - last_present);
      if (calib.size() >= 40) {
        std::vector<double> s(calib.begin() + 8, calib.end());
        std::sort(s.begin(), s.end());
        const double median = s[s.size() / 2];
        const double hz = median > 0.0015 ? 1.0 / median : 0.0;
        calibrated = true;
        if (hz >= 45.0 && hz <= 130.0 && options.vsync) {
          measured_hz = hz;
          say("present: display refresh measured %.1f Hz (median interval %.3f ms over %zu presents)", hz, median * 1000.0, s.size());
        } else {
          // Presents do not wait for the display (no vsync, or a virtual display): pace to the rate the system reports.
          pace_hz = display_refresh_hz() > 1.0 ? display_refresh_hz() : 60.0;
          measured_hz = pace_hz;
          say("present: presents do not wait for a refresh (%.1f per second back to back%s); paced to the display's reported %.1f Hz",
              hz, options.vsync ? "" : ", vsync off", pace_hz);
        }
      }
      last_present = end;
      continue;
    }
    last_present = end;
    // Online: pause sub-frames while the render thread cannot keep up (upstream: 90% of frames late in 3 s).
    const double budget = 1000.0 / std::max(30.0, hz_now > 0 ? hz_now : 60.0);
    const bool is_late = work > budget;
    if (is_late) ++late;
    late_window.push_back(is_late);
    const size_t window = size_t(std::max(30.0, (hz_now > 0 ? hz_now : 60.0) * 3.0));
    while (late_window.size() > window) late_window.pop_front();
    if (m0 && late_window.size() == window) {
      const double share = double(std::count(late_window.begin(), late_window.end(), true)) / double(window);
      if (!paused && online_now && share >= 0.9) { paused = true; say("present: sub-frames paused (online, %.0f%% of presented frames late)", share * 100.0); }
      else if (paused && (share < 0.5 || !online_now)) { paused = false; say("present: sub-frames back"); }
    }
  }
  if (have_prev) queue.recycle(std::move(prev));
  if (have_cur) queue.recycle(std::move(cur));
  say("present: %llu frames presented for %llu game frames (%llu repeats of a frame already shown, %llu game frames never shown), sub-frames %s%s",
      (unsigned long long)presents, (unsigned long long)game_frames, (unsigned long long)repeats, (unsigned long long)stale, mode_name(m0),
      paused_presents ? (", " + std::to_string(paused_presents) + " presented without them").c_str() : "");
  cost.report("present: render thread work per presented frame");
  cost_match.report("present: render thread work per presented frame in a match");
  interval.report("present: time between presents");
  if (m0) {
    phase.report("present: sub-frame phase in thousandths of a game frame (read ms as thousandths)");
    const auto& s = solver.stats();
    say("present: last frame's solver: %u draws, %u paired, %u rigid, %u blended, %u cuts, %u held for geometry, %u implausible", s.draws, s.paired, s.rigid, s.blended, s.cuts, s.geometry, s.insane);
  }
  say("present: %llu presented frames over the budget of one refresh", (unsigned long long)late);
}

// Off mode (and the decoder's per-draw deltas) through play mode's own render loop: after a frame is drawn it is
// presented again until the next refresh's worth of presents for one game frame is reached, n = rate / 60.
int presents_per_frame() {
  if (mode.load() != 0) return 1;                       // sub-frames run in loop(), not here
  const double hz = fps_cap.load() > 0 ? double(fps_cap.load()) : std::max(display_refresh_hz(), measured_hz.load());
  const int n = std::clamp(int(hz / 60.0 + 0.5), 1, 8);
  static int told = 0;
  if (n != told) { told = n; say("present: sub-frames off; each game frame presented %d time%s (%s %.1f Hz)", n, n == 1 ? "" : "s", fps_cap.load() > 0 ? "cap" : "display", hz); }
  return n;
}
bool present_again(void* backend, const void* frame, int index) {
  static double next = 0;
  if (fps_cap.load() > 0) {                             // no vsync to pace the repeat: the cap's grid
    const double period = 1.0 / double(fps_cap.load()), t = now_s();
    if (index == 1 || next < t - 4 * period) next = t + period;
    if (next > t) std::this_thread::sleep_for(std::chrono::duration<double>(next - t));
    next += period;
  }
  static_cast<gx::Backend*>(backend)->submit_frame(*static_cast<const gx::Frame*>(frame));
  return true;
}
void register_settings() {
  features::add_setting(setting);
  PresentHook hook;
  hook.presents_per_frame = presents_per_frame;
  hook.present_again = present_again;
  set_present_hook(hook);
}
}  // namespace fe::present
