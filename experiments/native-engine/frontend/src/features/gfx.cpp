// The display wave's graphics options (0.4.8): the settings and menu rows; the renderer side is src/gfx_render.cpp (render
// thread, Aurora), the shared state src/gfx_live.h. Every option here is display only and off by default, as melee-unlocked
// ships it; none of it reaches game memory, so the reference scripts, replays and online play are untouched by any of them.
//   - texture packs: Dolphin-format packs ("tex1_" PNG/DDS files, XXH64 names) in Load/Textures/GALE01 and TexturePacks, each
//     folder a pack that can be switched off (melee-unlocked gx/texture_pack.cpp's folders and keys); dumps in Dump/Textures/GALE01;
//   - screen-space ambient occlusion (ssao), in Aurora's present pass;
//   - the frame capture burst (upstream's Ctrl+F2: the next 90 presented frames) and the one-frame glitch detector (flickerscan);
// settings.ini keys (upstream's port-settings.ini names): customtextures, dumptextures, texpackoff, prefetchtextures (accepted:
// Aurora loads replacements on worker threads as they are first drawn), ssao, flickerscan, capture_burst_frames (ours).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "gfx_live.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fe::features {
namespace {
using fe::gfx::live;

using fe::gfx::PackInfo;
using fe::gfx::scan_packs;
std::vector<PackInfo> g_packs;     // menu thread / simulation thread
size_t g_pack_cursor = 0;          // the pack the menu's "This pack" row switches
bool pack_off(const std::string& name) {
  std::lock_guard<std::mutex> lock(live().mutex);
  return std::find(live().packs_off.begin(), live().packs_off.end(), name) != live().packs_off.end();
}
void bump_textures() { live().textures_generation.fetch_add(1, std::memory_order_relaxed); }

float to_float(const std::string& v, float lo, float hi, float fallback) {
  char* end = nullptr;
  const float f = std::strtof(v.c_str(), &end);
  return end && end != v.c_str() && std::isfinite(f) ? std::clamp(f, lo, hi) : fallback;
}
bool setting(const std::string& key, const std::string& value) {
  auto& l = live();
  if (key == "customtextures") { l.textures.store(value == "1"); bump_textures(); }
  else if (key == "dumptextures") { l.dump.store(value == "1"); bump_textures(); }
  else if (key == "prefetchtextures") {}   // Aurora streams each replacement in on its workers when it is first drawn
  else if (key == "texpackoff") {
    std::lock_guard<std::mutex> lock(l.mutex);
    size_t at = 0;
    while (at <= value.size()) {   // one name per line upstream; several separated by '|' when this app writes them
      const size_t bar = value.find('|', at);
      const std::string name = value.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
      if (!name.empty() && name != "-" && std::find(l.packs_off.begin(), l.packs_off.end(), name) == l.packs_off.end()) l.packs_off.push_back(name);
      if (bar == std::string::npos) break;
      at = bar + 1;
    }
    bump_textures();
  }
  else if (key == "ssao") l.ssao.store(to_float(value, 0.0f, 1.0f, 0.0f));
  else if (key == "flickerscan") l.flicker.store(value == "1");
  else if (key == "capture_burst_frames") l.capture_size.store(uint32_t(std::clamp(std::atoi(value.c_str()), 1, 600)));
  else return false;
  return true;
}

void retrace(const Retrace& info) {
  // Test aid: MU_CAPTURE_AT=RETRACE[:FRAMES] starts a capture burst at that retrace (verification runs).
  static const char* at = std::getenv("MU_CAPTURE_AT");
  static bool done = false;
  if (at && !done && info.retrace >= uint32_t(std::atoi(at))) {
    done = true;
    const char* colon = std::strchr(at, ':');
    live().capture_request.store(colon ? uint32_t(std::max(1, std::atoi(colon + 1))) : live().capture_size.load());
    say("gfx: capture burst of %u frames asked for at retrace %u (MU_CAPTURE_AT)", live().capture_request.load(), info.retrace);
  }
}

void run_end() {
  const auto& l = live();
  if (l.textures.load() || l.dump.load())
    say("gfx: texture packs: %u loaded, %u replacements; %llu textures dumped", l.packs_loaded.load(), l.replacements.load(), (unsigned long long)l.dumped.load());
  if (l.captured.load()) say("gfx: capture burst: %llu frames written", (unsigned long long)l.captured.load());
  if (l.flicker.load())
    say("gfx: glitch detector: %llu frames scanned: %llu whole-picture glitches, %llu frames with a part of the picture popping out and back (the log's flicker: lines)",
        (unsigned long long)l.flicker_frames.load(), (unsigned long long)l.flicker_hits.load(), (unsigned long long)l.flicker_cells.load());
}

using Write = MenuEntry::Write;
std::string on_off(bool v) { return v ? "On" : "Off"; }
void write_packs(Write w) {
  std::string joined;
  {
    std::lock_guard<std::mutex> lock(live().mutex);
    for (const auto& n : live().packs_off) { if (!joined.empty()) joined += "|"; joined += n; }
  }
  w("texpackoff", joined.empty() ? "-" : joined);
}
}  // namespace

void register_gfx() {
  add_setting(setting);
  add_retrace(retrace);
  add_run_end(run_end);
  g_packs = scan_packs();
  // Picture page: ambient occlusion with the other present-pass options.
  add_menu_entry({"Picture", "Screen-space ambient occlusion",
                  [] { const float v = live().ssao.load(); return v <= 0.0f ? std::string("Off") : std::to_string(int(std::lround(v * 100))) + "%"; },
                  [](int d) { live().ssao.store(std::clamp(std::round((live().ssao.load() + 0.1f * d) * 10.0f) / 10.0f, 0.0f, 1.0f)); }, nullptr,
                  [](Write w) { char b[16]; std::snprintf(b, sizeof b, "%.2f", live().ssao.load()); w("ssao", b); }});
  // Texture packs page.
  add_menu_entry({"Texture packs", "Custom textures", [] { return on_off(live().textures.load()); },
                  [](int) { live().textures.store(!live().textures.load()); bump_textures(); }, nullptr,
                  [](Write w) { w("customtextures", live().textures.load() ? "1" : "0"); }});
  add_menu_entry({"Texture packs", "Pack",
                  [] {
                    if (g_packs.empty()) return std::string("None installed");
                    const PackInfo& p = g_packs[g_pack_cursor % g_packs.size()];
                    return p.name + " (" + std::to_string(p.files) + " files)";
                  },
                  [](int d) {
                    g_packs = scan_packs();
                    if (g_packs.empty()) return;
                    g_pack_cursor = (g_pack_cursor + (d < 0 ? g_packs.size() - 1 : 1)) % g_packs.size();
                  }, nullptr, nullptr});
  add_menu_entry({"Texture packs", "  This pack",
                  [] { if (g_packs.empty()) return std::string("-"); return on_off(!pack_off(g_packs[g_pack_cursor % g_packs.size()].name)); },
                  [](int) {
                    if (g_packs.empty()) return;
                    const std::string name = g_packs[g_pack_cursor % g_packs.size()].name;
                    {
                      std::lock_guard<std::mutex> lock(live().mutex);
                      auto& off = live().packs_off;
                      const auto it = std::find(off.begin(), off.end(), name);
                      if (it == off.end()) off.push_back(name); else off.erase(it);
                    }
                    bump_textures();
                  }, nullptr, write_packs});
  add_menu_entry({"Texture packs", "Dump textures", [] { return on_off(live().dump.load()); },
                  [](int) { live().dump.store(!live().dump.load()); bump_textures(); }, nullptr,
                  [](Write w) { w("dumptextures", live().dump.load() ? "1" : "0"); }});
  add_menu_entry({"Texture packs", "Status",
                  [] { std::lock_guard<std::mutex> lock(live().mutex); return live().status.empty() ? std::string("-") : live().status; }, nullptr, nullptr, nullptr});
  // Diagnostics (upstream's Ctrl+F2 and --flicker-scan).
  add_menu_entry({"Diagnostics", "Capture frames (starts 3 s after the menu closes)",
                  [] { return std::to_string(live().capture_size.load()) + " frames"; },
                  [](int d) {
                    const uint32_t n = live().capture_size.load();
                    live().capture_size.store(uint32_t(std::clamp(int(n) + (d < 0 ? -30 : 30), 30, 600)));
                  },
                  [] { live().capture_request.store(0x80000000u | live().capture_size.load()); say("gfx: capture burst of %u frames asked for from the menu", live().capture_size.load()); },
                  [](Write w) { w("capture_burst_frames", std::to_string(live().capture_size.load())); }});
  add_menu_entry({"Diagnostics", "One-frame glitch detector", [] { return on_off(live().flicker.load()); },
                  [](int) { live().flicker.store(!live().flicker.load()); }, nullptr,
                  [](Write w) { w("flickerscan", live().flicker.load() ? "1" : "0"); }});
}
}  // namespace fe::features
