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
#include "video_bg.h"

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
  else if (key == "videobackgrounds") l.video_backgrounds.store(value != "0" && value != "");
  // The Lab view (upstream's keys).
  else if (key == "labview") l.lab.store(value == "1");
  else if (key == "labskipscene") l.lab_skip_scene.store(value != "0");
  else if (key == "look_lab_fighters") l.lab_fighters.store(std::clamp(std::atoi(value.c_str()), 0, 3));
  else if (key == "look_lab_stage") l.lab_stage.store(std::clamp(std::atoi(value.c_str()), 0, 3));
  else if (key.rfind("lab_colour_", 0) == 0) {
    const uint32_t c = value == "-" ? 0u : 0x1000000u | (uint32_t(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFFu);
    const std::string which = key.substr(11);
    if (which.size() == 2 && which[0] == 'p' && which[1] >= '1' && which[1] <= '4') l.lab_colour[which[1] - '1'].store(c);
    else if (which == "background") l.lab_background.store(c);
    else if (which == "stage") l.lab_stage_colour.store(c);
    else return false;
  }
  else return false;
  return true;
}

void retrace(const Retrace& info) {
  // The Lab view: the game's look snapshot for lab.cpp while it is on (only then is the game asked for it); once more after
  // it is switched off, so the view goes off.
  static bool lab_was_on = false;
  const bool lab_on = live().lab.load(std::memory_order_relaxed);
  if (&fe::gfx::lab_retrace && (lab_on || lab_was_on)) {
    std::string names[4];
    if (lab_on) overlay_names(names);
    fe::gfx::lab_retrace(names);
  }
  lab_was_on = lab_on;
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
void write_lab(Write w) {
  const auto& l = live();
  w("labview", l.lab.load() ? "1" : "0");
  w("look_lab_fighters", std::to_string(l.lab_fighters.load()));
  w("look_lab_stage", std::to_string(l.lab_stage.load()));
  w("labskipscene", l.lab_skip_scene.load() ? "1" : "0");
  const auto colour = [](uint32_t c) { if (!c) return std::string("-"); char b[8]; std::snprintf(b, sizeof b, "%06X", c & 0xFFFFFFu); return std::string(b); };
  for (int i = 0; i < 4; ++i) w((std::string("lab_colour_p") + char('1' + i)).c_str(), colour(l.lab_colour[i].load()));
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
  // Lab view (upstream's VIDEO > Lab view; Ctrl+F3 there).
  add_menu_entry({"Lab view", "Lab view", [] { return on_off(live().lab.load()); },
                  [](int) { live().lab.store(!live().lab.load()); }, nullptr, write_lab});
  add_menu_entry({"Lab view", "  Fighters",
                  [] { static const char* const n[] = {"Silhouettes (download)", "Stick figures", "Capsules", "Blobs"}; return std::string(n[std::clamp(live().lab_fighters.load(), 0, 3)]); },
                  [](int d) { live().lab_fighters.store((live().lab_fighters.load() + (d < 0 ? 3 : 1)) % 4); }, nullptr, write_lab});
  add_menu_entry({"Lab view", "  Stage",
                  [] { static const char* const n[] = {"As drawn", "Outline", "Blocks", "Hidden"}; return std::string(n[std::clamp(live().lab_stage.load(), 0, 3)]); },
                  [](int d) { live().lab_stage.store((live().lab_stage.load() + (d < 0 ? 3 : 1)) % 4); }, nullptr, write_lab});
  add_menu_entry({"Lab view", "  Skip the 3D scene under it", [] { return on_off(live().lab_skip_scene.load()); },
                  [](int) { live().lab_skip_scene.store(!live().lab_skip_scene.load()); }, nullptr, write_lab});
  add_menu_entry({"Lab view", "  Player colours",
                  [] { return std::string(live().lab_colour[0].load() ? "Own" : "The look's"); },
                  [](int) {
                    // The look's colours, or the port colours the other looks use (upstream's colour rows, as one switch: no pointer).
                    static const uint32_t own[4] = {0xFF3838, 0x4873FF, 0xFFD926, 0x40E64D};
                    const bool on = live().lab_colour[0].load() == 0;
                    for (int i = 0; i < 4; ++i) live().lab_colour[i].store(on ? 0x1000000u | own[i] : 0u);
                  }, nullptr, write_lab});
  add_menu_entry({"Lab view", "  Fighter silhouettes",
                  [] { return &fe::gfx::lab_status ? fe::gfx::lab_status() : std::string("-"); }, nullptr,
                  [] { if (&fe::gfx::lab_download) fe::gfx::lab_download(); }, nullptr});
  // Looping menu video backgrounds (upstream's VIDEO > Menu video backgrounds): VideoBackgrounds/css.mp4 and sss.mp4.
  add_menu_entry({"Menu videos", "Menu video backgrounds", [] { return on_off(live().video_backgrounds.load()); },
                  [](int) { live().video_backgrounds.store(!live().video_backgrounds.load()); }, nullptr,
                  [](Write w) { w("videobackgrounds", live().video_backgrounds.load() ? "1" : "0"); }});
  add_menu_entry({"Menu videos", "  Character select (css.mp4)",
                  [] { return &fe::video_bg::status ? fe::video_bg::status(0) : std::string("Not in this build"); }, nullptr, nullptr, nullptr});
  add_menu_entry({"Menu videos", "  Stage select (sss.mp4)",
                  [] { return &fe::video_bg::status ? fe::video_bg::status(1) : std::string("Not in this build"); }, nullptr, nullptr, nullptr});
  add_menu_entry({"Menu videos", "  Learn the stage select's background again", nullptr, nullptr,
                  [] { if (&fe::video_bg::relearn) fe::video_bg::relearn(1); }, nullptr});
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
