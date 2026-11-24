// Network and timing overlay and the session trace file, as melee-unlocked has them (its port/runtime/host/net_trace.h, a record
// per online tick in a ring of the last 12 seconds; port/runtime/host/net_trace_file.cpp, the .trace file beside the replay;
// port/runtime/gx/net_overlay.cpp, the graphs). Upstream fills its records inside its Slippi device; here the netplay host's
// answers are read as play mode hands them over (feature_hooks.h add_exchange): each online tick is the game's 0xB0 (the online
// inputs: frame, the local pad, and in the answer whether the frame ran, halted or advanced), the netplay object's wait or time
// sync reason and offset (netplay_bridge::frame_trace, patches/net-0005), the 0xB2 state loads after it (the rollbacks and how
// deep they went) and the simulation thread's work in that frame (rollbacks included, pacing sleep out). Nothing is sent to
// the other player and nothing is written into the game.
//
// settings.ini:
//   netoverlay 1        during an online match: upstream's panel at the top right, the last 10 s (frame time with the tick's work
//                       inside it, waits for the other player, time sync halts and advances, rollbacks by depth, ping, marks),
//                       drawn by the overlay drawer (src/overlay_draw.cpp); a build without a picture overlay shows one
//                       summary line in the HUD instead (and the log has it either way)
//   session_trace 0|1|2 the session trace (upstream's CSV, net_trace.h csv_header): beside each online match's replay as
//                       Game_*.trace. 1 (default) keeps it as upstream does: only when the match had a mark (D-pad Left, Right or
//                       Down on this player's controller during the match: "that looked wrong", "my input came out wrong",
//                       "that sounded wrong"; or a wait of 6 frames or more for the other player's inputs); 2 always; 0 never.
//                       Rows are kept in memory during the match and written with the replay, after it (no card writes while
//                       playing). Columns upstream fills that this host does not know: presents and desync are 0.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "overlay_state.h"
#include "netplay_bridge.h"                // runner: ping, time sync speed, frame_trace
#include "../host/net_trace.h"             // melee-unlocked (fetched tree), port/runtime/host: Record, Ring, the CSV row (header only)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace fe::features {
namespace {
using Clock = std::chrono::steady_clock;
bool enabled = false;
int trace_mode = 1;
double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// The tick being built: opened by the 0xB0, closed at the end of the frame.
bool open = false;
net_trace::Record pending;
double pending_start = 0;
int32_t current_frame = 0;
uint16_t wait_run = 0;
uint8_t input_port = 0;
uint16_t last_dpad = 0;
uint8_t carry_rollbacks = 0, carry_depth = 0;   // state loads before this frame's 0xB0 (the game rolls back first, then asks)

std::mutex mutex;                          // the ring (overlay, any thread) and the match's rows (the replay writer's thread)
net_trace::Ring ring;
std::vector<net_trace::Record> rows;       // this online match, every tick
bool rows_marked = false;
uint32_t rows_waits6 = 0, rows_marks = 0;
std::string text;
uint32_t lines_logged = 0;
std::atomic<double> panel_shown_at{0.0};    // the overlay drawer drew the panel then: the HUD line steps aside

bool setting(const std::string& key, const std::string& value) {
  if (key == "netoverlay") enabled = value != "0";
  else if (key == "session_trace") trace_mode = std::clamp(std::atoi(value.c_str()), 0, 2);
  else return false;
  return true;
}

int32_t be32s(const uint8_t* p) { return int32_t(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]); }

void exchange(uint8_t command, const uint8_t* payload, uint32_t payload_size, const uint8_t* response, uint32_t response_size, int32_t result,
              bool resim) {
  if (result != 0 || !payload) return;
  if (command == 0xB2 && payload_size >= 4) {
    // A rollback's state load: counted in the tick it happens in (before or after that tick's 0xB0), as deep as the frames
    // it goes back from the newest frame asked for.
    const int depth = current_frame - be32s(payload);
    uint8_t& count = open ? pending.rollbacks : carry_rollbacks;
    uint8_t& deepest = open ? pending.rollback_depth : carry_depth;
    if (count < 0xFF) ++count;
    if (depth > 0) deepest = uint8_t(std::max<int>(deepest, std::min(depth, 0xFF)));
    return;
  }
  if (resim) return;
  if (command == 0xB0 && payload_size >= 26 && response && response_size >= 1) {
    if (response[0] == 3) return;          // disconnected: no tick
    const int32_t frame = be32s(payload);
    if (frame == 1) {                      // a new online match
      std::lock_guard<std::mutex> lock(mutex);
      ring.clear(); rows.clear(); rows_marked = false; rows_waits6 = 0; rows_marks = 0; wait_run = 0;
    }
    const netplay_bridge::FrameTrace t = netplay_bridge::frame_trace();
    pending = net_trace::Record{};
    pending.wall = now_s();
    pending.frame = frame;
    pending.offset_us = t.offset_us;
    pending.ping_ms = uint16_t(std::clamp(netplay_bridge::ping_ms(), 0, 0xFFFF));
    if (response[0] == 2) pending.flags |= t.skip == 1 ? net_trace::kWait : net_trace::kShed;
    else if (response[0] == 4) pending.flags |= net_trace::kAdvance;
    wait_run = (pending.flags & net_trace::kWait) ? uint16_t(std::min(wait_run + 1, 0xFFFF)) : 0;
    pending.wait_frames = wait_run;
    for (int i = 0; i < 8; ++i) pending.pad[i] = payload[13 + i];
    input_port = payload[25] < 4 ? payload[25] : 0;
    pending.rollbacks = carry_rollbacks; pending.rollback_depth = carry_depth;
    carry_rollbacks = carry_depth = 0;
    pending_start = pending.wall;
    current_frame = frame;
    open = true;
  }
}

void summarise(uint32_t retrace) {
  // The HUD line, twice a second: the last 10 s.
  if (retrace % 30) return;
  static net_trace::Record recs[600];      // simulation thread only
  size_t n;
  { std::lock_guard<std::mutex> lock(mutex); n = ring.copy(recs, 600); }
  if (!n) return;
  unsigned rollbacks = 0, deepest = 0, waited = 0, late = 0;
  float worst = 0, sum = 0;
  for (size_t i = 0; i < n; ++i) {
    rollbacks += recs[i].rollbacks; deepest = std::max<unsigned>(deepest, recs[i].rollback_depth);
    if (recs[i].flags & net_trace::kWait) ++waited;
    if (i) { const float gap = float((recs[i].wall - recs[i - 1].wall) * 1000.0); sum += gap; worst = std::max(worst, gap); late += gap > 25.0f; }
  }
  char line[200];
  std::snprintf(line, sizeof line, "%.0f s: ping %u ms, %u rollbacks (deepest %u), %u waits, time sync x%.4f, frames %.1f ms (worst %.1f, %u late)",
                double(n) / 60.0, (unsigned)recs[n - 1].ping_ms, rollbacks, deepest, waited, netplay_bridge::emulation_speed(),
                n > 1 ? sum / float(n - 1) : 0.0f, worst, late);
  { std::lock_guard<std::mutex> lock(mutex); text = line; }
  if (lines_logged < 3 || retrace % 1800 == 0) { ++lines_logged; say("netoverlay: %s", line); }
}

void on_retrace(const Retrace& info) {
  if (open) {
    open = false;
    // The tick's work: the simulation thread's time for this frame with the pacing sleep taken out (frontend.cpp hud_frame,
    // measured just before this call), rollbacks included; upstream's sim_ms.
    float work = 0;
    pending.sim_ms = perf_history(&work, 1) == 1 ? work : float((now_s() - pending_start) * 1000.0);
    // Marks from this player's controller (upstream's D-pad Left, Right, Down), on the press.
    if (const MuPadStatus* p = pads_raw()) {
      const uint16_t dpad = p[input_port].button & 0x0007u;
      const uint16_t pressed = dpad & ~last_dpad;
      last_dpad = dpad;
      if (pressed & 0x0001u) pending.flags |= net_trace::kMarkVisual;      // D-pad Left: that looked wrong
      if (pressed & 0x0002u) pending.flags |= net_trace::kMarkInput;       // D-pad Right: my input came out wrong or late
      if (pressed & 0x0004u) pending.flags |= net_trace::kMarkAudio;       // D-pad Down: that sounded wrong
    }
    if (pending.wait_frames >= 6) pending.flags |= net_trace::kMark;       // upstream: a long wait marks the trace itself
    std::lock_guard<std::mutex> lock(mutex);
    ring.push(pending);
    if (trace_mode) {
      if (rows.size() < 60u * 60u * 30u) rows.push_back(pending);         // 30 minutes at most
      if (pending.flags & (net_trace::kMark | net_trace::kMarkVisual | net_trace::kMarkInput | net_trace::kMarkAudio)) {
        rows_marked = true;
        if (pending.flags & net_trace::kMark) ++rows_waits6; else ++rows_marks;
      }
    }
  }
  if (enabled && info.online && info.fighting) summarise(info.retrace);
  if (!info.online) { std::lock_guard<std::mutex> lock(mutex); text.clear(); }
}

void hud(std::vector<HudLine>& out) {
  if (now_s() - panel_shown_at.load(std::memory_order_relaxed) < 0.5) return;   // the panel says it all (a build with a picture)
  std::lock_guard<std::mutex> lock(mutex);
  if (enabled && !text.empty()) out.push_back({text, 0xC8F0C8FFu});
}
}  // namespace

// The replay writer (replays.cpp, its own thread) wrote a match to `slp_path`: the session trace of that match goes beside it.
void net_trace_replay_saved(const std::string& slp_path) {
  std::vector<net_trace::Record> taken;
  bool marked;
  uint32_t waits6, marks;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (rows.empty()) return;
    taken.swap(rows);
    marked = rows_marked; waits6 = rows_waits6; marks = rows_marks;
    rows_marked = false; rows_waits6 = rows_marks = 0;
  }
  if (trace_mode == 0) return;
  if (trace_mode == 1 && !marked) { say("nettrace: session trace of %s not kept (no mark, no long wait; session_trace 2 keeps every one)", slp_path.c_str()); return; }
  // Upstream's name (net_trace_file.cpp trace_path): the replay's name with .trace for .slp or .slpz.
  std::string path = slp_path;
  if (path.size() > 5 && path.compare(path.size() - 5, 5, ".slpz") == 0) path.resize(path.size() - 5);
  else if (path.size() > 4 && path.compare(path.size() - 4, 4, ".slp") == 0) path.resize(path.size() - 4);
  path += ".trace";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { say("nettrace: cannot create %s", path.c_str()); return; }
  std::fputs(net_trace::csv_header(), f);
  char line[200];
  size_t written = 0;
  for (const auto& r : taken) { const size_t n = net_trace::csv_row(r, line, sizeof line); if (n && std::fwrite(line, 1, n, f) == n) ++written; }
  const bool ok = std::fclose(f) == 0;
  say("nettrace: session trace %s %s (%zu rows; %u marks, %u long waits)", ok ? "saved to" : "WRITE FAILED for", path.c_str(), written, marks, waits6);
}

// The overlay drawer (src/overlay_draw.cpp, render thread): the newest records while the panel is on.
bool net_overlay_samples(std::vector<overlay::NetSample>& out) {
  if (!enabled) return false;
  static net_trace::Record recs[600];      // the render thread only
  size_t n;
  { std::lock_guard<std::mutex> lock(mutex); n = ring.copy(recs, 600); }
  if (!n || now_s() - recs[n - 1].wall > 2.0) return false;   // the panel leaves with the match
  panel_shown_at.store(now_s(), std::memory_order_relaxed);
  out.resize(n);
  for (size_t i = 0; i < n; ++i) {
    auto& s = out[i];
    s.wall = recs[i].wall; s.sim_ms = recs[i].sim_ms; s.ping_ms = recs[i].ping_ms;
    s.rollbacks = recs[i].rollbacks; s.depth = recs[i].rollback_depth; s.flags = recs[i].flags;
  }
  return true;
}

void register_netoverlay() {
  add_setting(setting);
  add_exchange(exchange);
  add_retrace(on_retrace);
  add_hud(hud);
  add_menu_entry({"Online", "Network overlay", [] { return std::string(enabled ? "On" : "Off"); }, [](int) { enabled = !enabled; }, nullptr,
                  [](MenuEntry::Write w) { w("netoverlay", enabled ? "1" : "0"); }});
  add_menu_entry({"Online", "Session trace beside replays",
                  [] { return std::string(trace_mode == 2 ? "Every match" : trace_mode == 1 ? "Marked matches" : "Off"); },
                  [](int d) { trace_mode = (trace_mode + (d < 0 ? 2 : 1)) % 3; }, nullptr,
                  [](MenuEntry::Write w) { w("session_trace", std::to_string(trace_mode)); }});
}
}  // namespace fe::features
