// Looks: the look presets, their parts, the Lightweight ladder and Visual effects (melee-unlocked's Video > "Low poly and
// lightweight"). The renderer applies them (frontend/patches/0116 with src/looks_live.h); this module keeps the settings,
// tells the game when to mark draw owners and when to draw its low poly fighters, offers the menu rows, and prints what
// the looks cost at the end of a run.
//
// Everything here is display only. The game is asked for two things, both picture-only: MuHostApi.native_owner_tracking
// (the game writes draw-owner tokens 0xF1/0xF2/0xF7 into its GX stream; asked only while a look needs them) and the
// display word's MU_DISPLAY_OPTION_LOW_POLY (Lightweight 1 and up; upstream's union of the low poly setting and the
// ladder). Neither changes the game's state: the reference scripts never get here, and picture parity holds with the
// looks on as well as off (frontend/README.md, Looks).
//
// settings.ini keys (upstream's port-settings.ini names): lightweight 0..5, effects 0..3, look_fighter, look_stage,
// look_menus (0 textured, 1 64 style, 2 flat colour, 3 white, 4 posterize, 5 wire), look_unfiltered, look_colours
// (1: port colours), look_background (0 the stage's, 1 black, 2 plain colour), look_background_rgb. The preset is the
// combination (render_options.h look_preset_of); "look NAME|N" sets one by name or number as a shorthand.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "looks_live.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fe::features {
namespace {
using fe::looks::live;

int clamp_int(const std::string& v, int lo, int hi) { return std::clamp(std::atoi(v.c_str()), lo, hi); }
bool setting(const std::string& key, const std::string& value) {
  auto& l = live();
  if (key == "lightweight") l.lightweight.store(uint8_t(clamp_int(value, 0, 5)));
  else if (key == "effects") l.effects.store(uint8_t(clamp_int(value, 0, 3)));
  else if (key == "look_fighter") l.fighter.store(uint8_t(clamp_int(value, 0, 5)));
  else if (key == "look_stage") l.stage.store(uint8_t(clamp_int(value, 0, 5)));
  else if (key == "look_menus") l.menus.store(uint8_t(clamp_int(value, 0, 4)));   // no wire menus (upstream's panel offers none)
  else if (key == "look_unfiltered") l.unfiltered.store(value == "1");
  else if (key == "look_colours") l.colours.store(uint8_t(clamp_int(value, 0, 1)));
  else if (key == "look_background") l.background.store(uint8_t(clamp_int(value, 0, 2)));
  else if (key == "look_background_rgb") l.background_rgb.store(uint32_t(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFFu);
  else if (key == "look") {
    int n = 0; const fe::looks::Preset* p = fe::looks::presets(&n);
    char* end = nullptr; const long index = std::strtol(value.c_str(), &end, 10);
    if (end && *end == 0 && index >= 0 && index < n) fe::looks::apply_preset(int(index));
    else for (int i = 0; i < n; ++i) if (value == p[i].name) fe::looks::apply_preset(i);
  } else return false;
  return true;
}

MuHostApi* g_host = nullptr;
uint32_t (*orig_display)() = nullptr;
int32_t owner_tracking() { return fe::looks::active() ? 1 : 0; }   // read by the game for every draw: follows the menu at once
// True widescreen (settings.cpp truewidescreen): how much wider the picture is than the game's cameras (320/219 at 16:9);
// the game widens the camera Fountain of Dreams' reflection is rendered with by it (mu_host.h picture_widen).
float picture_widen() { return live().true_widescreen.load(std::memory_order_relaxed) ? 320.0f / 219.0f : 1.0f; }
uint32_t display_options() {
  const uint32_t word = orig_display ? orig_display() : 0;
  return word | (fe::looks::load(live().lightweight) >= 1 ? MU_DISPLAY_OPTION_LOW_POLY : 0u);
}
void tables(MuHostApi* host, MuGameApi*) {
  if (!host) return;
  g_host = host;
  host->native_owner_tracking = owner_tracking;
  host->picture_widen = picture_widen;
  orig_display = host->display_options; host->display_options = display_options;
}

std::string preset_name() {
  int n = 0; const fe::looks::Preset* p = fe::looks::presets(&n);
  const int i = fe::looks::preset_of();
  return i < 0 ? std::string("Custom") : std::string(p[i].name);
}
void run_end() {
  auto& l = live();
  const uint64_t frames = l.frames.load();
  if (!frames && !fe::looks::active()) return;
  say("looks: %s (lightweight %u, effects %u, surfaces fighter %u stage %u menus %u, colours %u, background %u, arcade backdrop %u)", preset_name().c_str(),
      fe::looks::load(l.lightweight), fe::looks::load(l.effects), fe::looks::load(l.fighter), fe::looks::load(l.stage), fe::looks::load(l.menus),
      fe::looks::load(l.colours), fe::looks::load(l.background), fe::looks::load(l.arcade_backdrop));
  if (frames)
    say("looks: %llu match frames: the look rules took %.3f ms a frame on the render thread; %llu draws skipped, %llu drawn (%.1f skipped a frame); "
        "%llu tinted, %llu as lines; %llu textures treated", (unsigned long long)frames, l.rule_ns.load() / 1e6 / double(frames),
        (unsigned long long)l.skipped.load(), (unsigned long long)l.drawn.load(), double(l.skipped.load()) / double(frames),
        (unsigned long long)l.tinted.load(), (unsigned long long)l.wired.load(), (unsigned long long)l.treated_textures.load());
}

const char* const kSurfaces[] = {"Textured", "64 style", "Flat colour", "White", "Posterize", "Wire"};
const char* const kEffects[] = {"Full", "Reduced", "Minimal", "No decorative particles"};
const char* const kBackgrounds[] = {"The stage's own", "Black", "Plain colour"};
using Write = MenuEntry::Write;
void write_all(Write w) {
  const auto& l = live();
  w("lightweight", std::to_string(fe::looks::load(l.lightweight)));
  w("effects", std::to_string(fe::looks::load(l.effects)));
  w("look_fighter", std::to_string(fe::looks::load(l.fighter)));
  w("look_stage", std::to_string(fe::looks::load(l.stage)));
  w("look_menus", std::to_string(fe::looks::load(l.menus)));
  w("look_unfiltered", l.unfiltered.load() ? "1" : "0");
  w("look_colours", std::to_string(fe::looks::load(l.colours)));
  w("look_background", std::to_string(fe::looks::load(l.background)));
}
void step(std::atomic<uint8_t>& a, int d, int count) { a.store(uint8_t((int(a.load()) + (d < 0 ? count - 1 : 1)) % count)); }
}  // namespace

void register_looks() {
  add_setting(setting);
  add_tables(tables);
  add_run_end(run_end);
  // Looks page: the preset row first (it sets every part), then the parts. Thin rows over looks_live.h.
  add_menu_entry({"Looks", "Look", [] { return preset_name(); },
                  [](int d) {
                    int n = 0; fe::looks::presets(&n);
                    const int now = fe::looks::preset_of();
                    fe::looks::apply_preset(now < 0 ? 0 : (now + (d < 0 ? n - 1 : 1)) % n);
                  }, nullptr, write_all});
  add_menu_entry({"Looks", "  Lightweight", [] { return std::to_string(fe::looks::load(live().lightweight)); },
                  [](int d) { step(live().lightweight, d, 6); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Visual effects", [] { return std::string(kEffects[fe::looks::load(live().effects) & 3]); },
                  [](int d) { step(live().effects, d, 4); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Fighters", [] { return std::string(kSurfaces[std::min<int>(5, fe::looks::load(live().fighter))]); },
                  [](int d) { step(live().fighter, d, 6); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Stage", [] { return std::string(kSurfaces[std::min<int>(5, fe::looks::load(live().stage))]); },
                  [](int d) { step(live().stage, d, 6); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Menus", [] { return std::string(kSurfaces[std::min<int>(4, fe::looks::load(live().menus))]); },
                  [](int d) { step(live().menus, d, 5); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Port colours on fighters", [] { return std::string(fe::looks::load(live().colours) ? "On" : "Off"); },
                  [](int) { live().colours.store(fe::looks::load(live().colours) ? 0 : 1); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Background", [] { return std::string(kBackgrounds[std::min<int>(2, fe::looks::load(live().background))]); },
                  [](int d) { step(live().background, d, 3); }, nullptr, write_all});
  add_menu_entry({"Looks", "  Sharp texels", [] { return std::string(live().unfiltered.load() ? "On" : "Off"); },
                  [](int) { live().unfiltered.store(!live().unfiltered.load()); }, nullptr, write_all});
}
}  // namespace fe::features
