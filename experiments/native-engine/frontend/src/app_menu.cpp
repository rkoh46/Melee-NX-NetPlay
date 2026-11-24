// The app's in-game menu (port/runtime/host/game_menu.cpp, with patches/0104) wired to the native engine:
// the app's own rows (volume, HUD, online delay, screen filter, antialiasing, texture filtering, light-shield
// and stick range, controller remapping and deadzones on the Controls page, Quit), and pages for the
// melee-unlocked options settings.ini already supports, written back to settings.ini when changed, plus the
// feature modules' pages (feature_hooks.h). Options upstream only allows offline are greyed while the session
// is online. Opened with L + R + Start held for half a second, as in the app. Aurora builds only.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "app/game_menu.h"       // the build-time copy with patches/0104 (build/native-engine-gfx/gen/app)
#include "audio.h"
#include "feature_calls.h"
#include "host_services.h"       // melee-unlocked: linux_host::request_stop
#include "netplay_bridge.h"      // set_delay: the input delay from the next game on
#include <aurora/aurora.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef __SWITCH__
extern "C" int melee_nx_gc_stick_range;
extern "C" int melee_nx_gc_trigger_scale;
#endif

namespace fe {
namespace {
// One row of a settings.ini page.
enum class Kind { Toggle, Range, Choice, Bit, Grid };
struct Option {
  const char* key;
  const char* label;
  Kind kind;
  int low = 0, high = 1, step = 1;          // Range; Bit: the bit in `low` (community_codes)
  std::vector<const char*> names = {};      // Choice
  bool online_locked = false, next_start = false;
  const char* suffix = "";
};
struct Page { const char* title; std::vector<Option> options; };

const std::vector<Page>& pages() {
  static const std::vector<Page> list = {
    {"Game options", {
      {"results_screen", "Results screen", Kind::Toggle, 0, 1, 1, {}, true},
      {"unlock_all", "Everything unlocked", Kind::Toggle, 0, 1, 1, {}, true, true},
      {"gamelanguage", "Language", Kind::Choice, 0, 2, 1, {"The save's", "Japanese", "English"}, false, true},
      {"music", "Music volume", Kind::Range, 0, 100, 10, {}, false, false, "%"},
      {"mute_voices", "Mute voices", Kind::Toggle},
      {"audio_early_keyon", "Faster sound effects", Kind::Toggle, 0, 1, 1, {}, true},
      {"stadium_frozen", "Frozen Pokemon Stadium", Kind::Toggle, 0, 1, 1, {}, true},
      {"freeze_fd", "Frozen Final Destination", Kind::Toggle},
      {"vanilla", "Retail game (no built-in codes)", Kind::Toggle, 0, 1, 1, {}, true, true},
      {"slippi_menus", "Slippi menus (Online Play)", Kind::Toggle, 0, 1, 1, {}, true, true},
    }},
    {"Picture and HUD", {
      {"palstockicons", "PAL stock icons", Kind::Toggle},
      {"noscreenshake", "No screen shake", Kind::Toggle},
      {"screenshake", "Screen shake", Kind::Range, 0, 200, 10, {}, false, false, "%"},
      {"stockhudscale", "Stock icons size", Kind::Range, 75, 175, 25, {}, false, false, "%"},
      {"damagehudscale", "Damage numbers size", Kind::Range, 75, 175, 25, {}, false, false, "%"},
      {"playertags", "Player tags stay up", Kind::Toggle},
      {"hide_ko_explosions", "Hide KO explosions", Kind::Toggle},
      {"hide_hud_emblems", "Hide HUD emblems", Kind::Toggle},
      {"arcade_hitboxes", "Show hitboxes", Kind::Toggle, 0, 1, 1, {}, true},
      {"arcade_projectile_hitboxes", "Projectile hitboxes", Kind::Toggle, 0, 1, 1, {}, true},
      {"arcade_hitboxes_behind", "Hitboxes behind fighters", Kind::Toggle, 0, 1, 1, {}, true},
      {"arcade_camera_2d", "2D camera", Kind::Toggle, 0, 1, 1, {}, true},
      {"arcade_backdrop", "Grid behind the stage", Kind::Grid},
    }},
    {"Community codes", {
      {"community_codes", "HUD fades behind fighters", Kind::Bit, 0x1000},
      {"community_codes", "Respawn platform colours", Kind::Bit, 0x2000},
      {"community_codes", "Nana's percent", Kind::Bit, 0x4000},
      {"community_codes", "No GO! at the start", Kind::Bit, 0x10000},
      {"community_codes", "Hitbox colours by id", Kind::Bit, 0x20000},
      {"community_codes", "Hitbox order", Kind::Bit, 0x40000},
      {"community_codes", "Hitbox angles", Kind::Bit, 0x80000},
      {"community_codes", "V-cancel sound", Kind::Bit, 0x200000},
      {"community_codes", "No fighter shadows", Kind::Bit, 0x1000000},
      {"community_codes", "Centred 2-player HUD", Kind::Bit, 0x4000000},
      {"community_codes", "Full Fountain of Dreams reflections", Kind::Bit, 0x100000},
      {"community_codes", "Instant Melee (offline)", Kind::Bit, 0x400000, 1, 1, {}, true},
      {"community_codes", "Free pause camera (offline)", Kind::Bit, 0x800000, 1, 1, {}, true},
      {"community_codes", "Boot to character select (offline)", Kind::Bit, 0x2000000, 1, 1, {}, true, true},
    }},
    {"Computer players", {
      {"cpu_20xx", "20XX computer players", Kind::Toggle, 0, 1, 1, {}, true},
      {"cpu_tech", "Tech", Kind::Choice, 0, 5, 1, {"Normal", "In place", "Roll forward", "Roll back", "Miss", "Random"}, true},
      {"cpu_getup", "Get-up", Kind::Choice, 0, 5, 1, {"Normal", "Stand", "Roll forward", "Roll back", "Attack", "Random"}, true},
      {"cpu_di", "Directional influence", Kind::Choice, 0, 3, 1, {"Normal", "None", "Random", "Survival"}, true},
      {"cpu_sdi", "Smash DI", Kind::Choice, 0, 6, 1, {"Normal", "None", "Random", "Against the hit", "With the hit", "Up", "Down"}, true},
      {"cpu_notaunt", "No taunts", Kind::Toggle, 0, 1, 1, {}, true},
      {"cpu_lcancel", "Always L-cancel", Kind::Toggle, 0, 1, 1, {}, true},
      {"cpu_nojab", "No rapid jabs", Kind::Toggle, 0, 1, 1, {}, true},
      {"cpu_notransform", "No transforming", Kind::Toggle, 0, 1, 1, {}, true},
    }},
  };
  return list;
}

int to_int(const std::string& v, int base = 10) { return int(std::strtol(v.c_str(), nullptr, base)); }
std::string shown(const Option& o) {
  const std::string v = settings_get(o.key);
  switch (o.kind) {
    case Kind::Toggle: return v == "1" ? "On" : "Off";
    case Kind::Grid: return to_int(v) == 2 ? "On" : "Off";
    case Kind::Bit: return (uint32_t(to_int(v, 16)) & uint32_t(o.low)) ? "On" : "Off";
    case Kind::Range: return v + o.suffix;
    case Kind::Choice: { const int i = std::clamp(to_int(v), 0, int(o.names.size()) - 1); return o.names.empty() ? v : o.names[size_t(i)]; }
  }
  return v;
}
// Steps a row; true when settings.ini was written.
bool step(const Option& o, int dir) {
  const std::string v = settings_get(o.key);
  std::string next;
  char hex[16];
  switch (o.kind) {
    case Kind::Toggle: next = v == "1" ? "0" : "1"; break;
    case Kind::Grid: next = to_int(v) == 2 ? "0" : "2"; break;
    case Kind::Bit: std::snprintf(hex, sizeof hex, "%X", uint32_t(to_int(v, 16)) ^ uint32_t(o.low)); next = hex; break;
    case Kind::Range: { const int n = std::clamp(to_int(v) + (dir ? dir : 1) * o.step, o.low, o.high); if (std::to_string(n) == v) return false; next = std::to_string(n); break; }
    case Kind::Choice: { const int count = int(o.names.size()); next = std::to_string(((to_int(v) + (dir ? dir : 1)) % count + count) % count); break; }
  }
  return settings_change(o.key, next);
}

// The feature modules' rows, grouped by page.
struct FeaturePage { std::string title; std::vector<features::MenuEntry> entries; };
std::vector<FeaturePage>& feature_pages() { static std::vector<FeaturePage> list; return list; }

void apply(const host::RuntimeSettings& s, host::MenuChange what) {
  switch (what) {
    case host::MenuChange::Volume: host::audio_set_volume(s.volume); app_ini_set("volume", std::to_string(s.volume)); break;
    case host::MenuChange::Hud: app_ini_set("hud", s.hud ? "1" : "0"); break;
    case host::MenuChange::OnlineDelay:
      options.online_delay = s.online_delay <= 0 ? 0 : s.online_delay;
      app_ini_set("online_delay", s.online_delay <= 0 ? "auto" : std::to_string(s.online_delay));
      netplay_bridge::set_delay(s.online_delay, s.online_delay <= 0);   // the netplay device reads it at each game's start
      say("menu: online input delay %s from the next match", s.online_delay <= 0 ? "automatic" : (std::to_string(s.online_delay) + " frames").c_str());
      break;
    case host::MenuChange::Widescreen:
      settings_change("widescreen", s.widescreen ? "1" : "0");
      app_ini_set("widescreen", s.widescreen ? "1" : "0");
      break;
    case host::MenuChange::Filter:
      aurora_set_resampler(s.filter == 1 ? SAMPLER_AREA : s.filter == 2 ? SAMPLER_CRT : s.filter == 3 ? SAMPLER_VIBRANT : SAMPLER_BILINEAR);
      app_ini_set("filter", std::to_string(s.filter));
      break;
    case host::MenuChange::Msaa: app_ini_set("msaa", std::to_string(s.msaa)); break;
    case host::MenuChange::Anisotropy: app_ini_set("anisotropy", std::to_string(s.anisotropy)); break;
    case host::MenuChange::Trigger:
#ifdef __SWITCH__
      melee_nx_gc_trigger_scale = s.trigger_range;
#endif
      app_ini_set("trigger_range", std::to_string(s.trigger_range));
      break;
    case host::MenuChange::Stick:
#ifdef __SWITCH__
      melee_nx_gc_stick_range = s.stick_range;
#endif
      app_ini_set("stick_range", std::to_string(s.stick_range));
      break;
    default: break;
  }
}
}  // namespace

void app_menu_setup() {
  host::RuntimeSettings rs;
  rs.volume = options.volume >= 0 ? options.volume : 100;
  rs.hud = app_ini("hud", "0") == "1";
  rs.online_delay = options.online_delay >= 0 ? options.online_delay : settings_online_delay();
  rs.widescreen = settings_get("widescreen") == "1";
  rs.filter = std::clamp(std::atoi(app_ini("filter", "0").c_str()), 0, 3);
  rs.msaa = app_ini("msaa", "1") == "4" ? 4 : 1;
  rs.anisotropy = std::clamp(std::atoi(app_ini("anisotropy", "16").c_str()), 1, 16);
  rs.trigger_range = std::clamp(std::atoi(app_ini("trigger_range", "4").c_str()), 2, 8);
  rs.stick_range = std::clamp(std::atoi(app_ini("stick_range", "100").c_str()), 80, 120);
  rs.full_speed = true;
  host::menu_init(rs, apply);
  // Rows of the earlier renderer that mean nothing here: this renderer always draws the newest frame.
  host::menu_hide(host::MenuChange::FullSpeed);
  host::menu_hide(host::MenuChange::Graphics);
  host::menu_set_label(host::MenuChange::Widescreen, "Aspect ratio *");
  host::menu_set_quit([] {
    say("menu: quit");
    linux_host::request_stop(0);
  });
  host::menu_set_hud_extra([]() -> std::string {
    std::string text;
    if (session_online()) {
      const uint32_t delay = online_delay_frames();
      text = delay ? "delay " + std::to_string(delay) : std::string("delay auto");
    }
    return text;
  });
  std::vector<host::MenuExtraPage> extra;
  for (size_t p = 0; p < pages().size(); ++p) {
    host::MenuExtraPage page;
    page.title = pages()[p].title;
    page.items = [p] {
      std::vector<host::MenuExtraItem> items;
      const bool online = session_online();
      for (const Option& o : pages()[p].options) items.push_back({o.label, shown(o), !(o.online_locked && online), o.next_start});
      return items;
    };
    page.change = [p](int item, int dir) {
      const auto& list = pages()[p].options;
      if (item < 0 || item >= int(list.size())) return false;
      if (list[size_t(item)].online_locked && session_online()) return false;
      return step(list[size_t(item)], dir);
    };
    extra.push_back(std::move(page));
  }
  // The feature modules' pages (feature_hooks.h menu_entries), in the order their rows were registered.
  auto& groups = feature_pages();
  groups.clear();
  for (const auto& entry : features::call_menu_entries()) {
    const std::string title = entry.page ? entry.page : "More";
    auto it = std::find_if(groups.begin(), groups.end(), [&](const FeaturePage& g) { return g.title == title; });
    if (it == groups.end()) { groups.push_back({title, {}}); it = groups.end() - 1; }
    it->entries.push_back(entry);
  }
  for (size_t g = 0; g < groups.size(); ++g) {
    host::MenuExtraPage page;
    page.title = groups[g].title;
    page.items = [g] {
      std::vector<host::MenuExtraItem> items;
      for (const auto& e : feature_pages()[g].entries) items.push_back({e.label ? e.label : "", e.value ? e.value() : std::string()});
      return items;
    };
    page.change = [g](int item, int dir) {
      const auto& list = feature_pages()[g].entries;
      if (item < 0 || item >= int(list.size())) return false;
      const auto& e = list[size_t(item)];
      if (dir == 0 && e.press) e.press();
      else if (e.change) e.change(dir ? dir : 1);
      if (!e.persist) return false;   // nothing of this row is saved
      e.persist([](const char* key, const std::string& value) { settings_write_key(key, value); });
      return true;
    };
    extra.push_back(std::move(page));
  }
  // The controller setup beyond the Controls page's button remap (controls.cpp): setup, bindings, profiles; last, so
  // they sit right above Controls.
  controls_menu_pages(&extra);
  host::menu_set_extra_pages(std::move(extra));
  say("app: in-game menu ready (L + R + Start): %zu settings pages, %zu feature pages", pages().size(), groups.size());
}
}  // namespace fe
