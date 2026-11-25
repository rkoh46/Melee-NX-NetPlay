// The "PC Settings" row in Melee's Options menu (melee-unlocked's settings_guest_options_frame, gx/pc_settings.cpp). Upstream
// (its Static Recomp) shows a box while Melee's Options menu (menu kind 4) is open and opens the panel when A is pressed on the
// menu's fourth slot (selection 3), reading the menu at its console addresses (0x804A04F0). The game image's state snapshot
// carries the same fields here (MuStatePod menu_kind, menu_selection: mn_804A04F0), but in the native game that fourth slot is
// one the cursor never stops on (it goes from the third option to the fifth: test-app-linux.sh pcsettings, feature_test
// menu-trace), so here the box names Z, which Melee's Options menu does not use: Z anywhere in the Options menu opens the
// settings panel, and the Z is kept from the game.
// settings.ini pcsettings_row 0 turns it off. Display and menus only: never in a match, a replay or online.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"

#include <atomic>
#include <string>

namespace host { __attribute__((weak)) void menu_open_extra(const std::string& title); }

namespace fe::features {
namespace {
constexpr uint32_t kMenuOptions = 4;   // MENU_KIND_SETTINGS, upstream's menu 4
constexpr uint16_t kZ = 0x0010;
std::atomic<bool> g_on{true}, g_in_options{false};
uint16_t g_swallow = 0;
bool g_logged = false;

bool setting(const std::string& key, const std::string& value) {
  if (key != "pcsettings_row") return false;
  g_on = value != "0";
  return true;
}
void on_retrace(const Retrace& info) {
  const bool in = g_on && options.app && info.state && info.scene_major == 1 && !info.fighting && !info.online && !replay_playing() &&
                  info.state->menu_kind == kMenuOptions;
  if (in && !g_in_options) { if (!g_logged) say("pc settings: Melee's Options menu: Z opens the settings panel"); g_logged = true; }
  g_in_options = in;
  if (feature_test("menu-trace") && info.state) {   // verification: the main menu's kind and row as they change
    static uint32_t last = ~0u;
    const uint32_t now = info.state->menu_kind << 16 | info.state->menu_selection;
    if (info.scene_major == 1 && now != last) { last = now; say("pc settings: menu %u row %u", info.state->menu_kind, info.state->menu_selection); }
  }
}
void hud(std::vector<HudLine>& out) {
  if (!g_in_options) return;
  out.push_back({"OPTIONS  >  PC Settings: Z", 0xFFE070FFu});
}
}  // namespace

// options.cpp pad_read, before the game reads the controllers.
void pcsettings_pad_hook(MuPadStatus pads[4]) {
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0 || !(g_swallow & kZ)) continue;
    if (!(pads[i].button & kZ)) g_swallow = 0;
    pads[i].button &= uint16_t(~kZ);
  }
  if (!g_in_options || !&host::menu_open_extra) return;
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0 || !(pads[i].button & kZ) || (g_swallow & kZ)) continue;
    g_swallow = kZ;
    for (int j = 0; j < 4; ++j) pads[j].button &= uint16_t(~kZ);
    say("pc settings: Z in Melee's Options menu: the settings panel opens");
    host::menu_open_extra("");
    return;
  }
}

void register_pcsettings() {
  add_setting(setting);
  add_retrace(on_retrace);
  add_hud(hud);
}
}  // namespace fe::features
