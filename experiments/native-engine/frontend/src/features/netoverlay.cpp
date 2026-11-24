// Network and timing overlay, text form (settings.ini netoverlay 1, upstream's key; its gx/net_overlay.cpp draws graphs on
// its ImGui layer, which this renderer does not have). During an online match one line near the top sums up the last ten
// seconds: the round trip to the opponent, the rollbacks, the frame rate Slippi's time sync asks for, and the time between
// frames on the simulation thread (average, worst, how many came later than 1.5 frames).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "netplay_bridge.h"                // runner: ping, time sync speed
namespace net_host { uint64_t rollbacks(); }   // runner/net_host.h: the rollback store's loads

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace fe::features {
namespace {
using Clock = std::chrono::steady_clock;
bool enabled = false;
constexpr size_t kWindow = 600;            // ten seconds of retraces

struct Sample { uint64_t rollbacks; float interval_ms; };
std::vector<Sample> ring;
size_t next = 0;
bool have_last = false;
Clock::time_point last;
std::mutex text_mutex;
std::string text;
uint32_t lines_logged = 0;

bool setting(const std::string& key, const std::string& value) {
  if (key == "netoverlay") enabled = value != "0";
  else return false;
  return true;
}

void on_retrace(const Retrace& info) {
  if (!enabled) return;
  const auto now = Clock::now();
  const float interval = have_last ? std::chrono::duration<float, std::milli>(now - last).count() : 0.0f;
  last = now; have_last = true;
  if (!info.online || !info.fighting) {
    if (!ring.empty()) { ring.clear(); next = 0; std::lock_guard<std::mutex> lock(text_mutex); text.clear(); }
    return;
  }
  const Sample s{net_host::rollbacks(), interval};
  if (ring.size() < kWindow) ring.push_back(s); else ring[next] = s;
  next = (next + 1) % kWindow;
  if (info.retrace % 30) return;           // the line changes twice a second
  const Sample& oldest = ring.size() < kWindow ? ring.front() : ring[next];
  float sum = 0, worst = 0; int late = 0, n = 0;
  for (const auto& r : ring) { if (r.interval_ms <= 0) continue; sum += r.interval_ms; worst = std::max(worst, r.interval_ms); late += r.interval_ms > 25.0f; ++n; }
  const int ping = netplay_bridge::ping_ms();
  char line[160];
  std::snprintf(line, sizeof line, "%.0f s: ping %s, %llu rollbacks, time sync x%.4f, frames %.1f ms (worst %.1f, %d late)",
                double(ring.size()) / 60.0, ping >= 0 ? (std::to_string(ping) + " ms").c_str() : "-",
                (unsigned long long)(s.rollbacks - oldest.rollbacks), netplay_bridge::emulation_speed(), n ? sum / float(n) : 0.0f, worst, late);
  { std::lock_guard<std::mutex> lock(text_mutex); text = line; }
  if (lines_logged < 3 || info.retrace % 1800 == 0) { ++lines_logged; say("netoverlay: %s", line); }
}

void hud(std::vector<HudLine>& out) {
  std::lock_guard<std::mutex> lock(text_mutex);
  if (enabled && !text.empty()) out.push_back({text, 0xC8F0C8FFu});
}
}  // namespace

void register_netoverlay() {
  add_setting(setting);
  add_retrace(on_retrace);
  add_hud(hud);
  add_menu_entry({"Online", "Network overlay", [] { return std::string(enabled ? "On" : "Off"); }, [](int) { enabled = !enabled; }, nullptr,
                  [](MenuEntry::Write w) { w("netoverlay", enabled ? "1" : "0"); }});
}
}  // namespace fe::features
