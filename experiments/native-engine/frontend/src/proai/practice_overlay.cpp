// The Practice menu's render-thread half: melee-unlocked's practice_menu.cpp (build-time copy with patches/0113) draws
// the menu in the look settings.ini practicestyle picks and reads its controller, once per drawn frame inside Aurora's
// ImGui pass (overlay_draw.cpp calls this), as upstream's pc_settings.cpp calls it inside its overlay. What it needs from
// the game side comes from features/proai.cpp (practice_state.h); what it asks for goes back there.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "practice_menu.h"   // build-time copy (build/native-engine-gfx/gen/proai)
#include "practice_state.h"
#include "native_practice_model.h"   // upstream's connect code rules (fetched tree, compiled with the menu)

#include <cstdio>
#include <string>

#include <imgui.h>

namespace fe {
__attribute__((weak)) bool kb_seen();   // usb_keyboard.cpp: a USB keyboard has been used this session
void practice_menu_draw(float display_w, float display_h, float game_x0, float game_y0, float game_x1, float game_y1) {
  const proai::MenuView v = proai::menu_view();
  if (!v.open && !v.training) return;   // nothing of it outside offline Training
  if (v.just_opened) practice_menu::opened();
  practice_menu::Frame f;
  f.display_w = display_w;
  f.display_h = display_h;
  f.game_x0 = game_x0;
  f.game_y0 = game_y0;
  f.game_x1 = game_x1;
  f.game_y1 = game_y1;
  f.native_source = true;          // the Training tools and Fight a pro run in this engine
  f.open = v.open;
  f.armed = v.armed;
  f.buttons = v.buttons;
  f.stick_x = v.stick_x;
  f.stick_y = v.stick_y;
  // A USB keyboard in use (usb_keyboard.cpp feeds its keys to ImGui): arrows, Enter, Escape and typing drive it too, as upstream's
  // keyboard does (the connect-code keyboard takes typed letters and digits). Otherwise the controller drives it.
  f.keyboard = &fe::kb_seen && fe::kb_seen();
  f.hint = v.hint && v.training;   // the corner hint in Training (upstream: every Training match unless turned off)
  f.intro = false;                 // upstream disables its banner too (kBanner false); the corner hint stays
  f.unlocked = v.unlocked;
  f.time = ImGui::GetTime();
  // The Search page (features/search.cpp): Unranked, Direct and Teams from Training in the services program; elsewhere the
  // page says to search from Online Play (patches/0113).
  const proai::SearchView s = proai::search_view();
  f.search.available = s.available;
  f.search.can_start = s.can_start;
  f.search.searching = s.searching;
  f.search.mode = s.mode;
  char elapsed[16];
  std::snprintf(elapsed, sizeof elapsed, "%u:%02u", s.elapsed_ms / 60000, s.elapsed_ms / 1000 % 60);
  f.search.elapsed = elapsed;
  f.search.state = s.state == 1 ? "Starting the search" : s.state == 2 ? "Looking for an opponent" : s.state == 3 ? "Connecting to the opponent"
                 : s.state == 4 ? "Connected" : "";
  f.search.opponent = s.opponent;
  static std::string last_note;
  if (last_note != s.note) { last_note = s.note; if (!last_note.empty()) practice_menu::show_error(last_note); }
  practice_menu::Result r = practice_menu::frame(v.style, f);
  // Upstream's own rules for the typed codes (native_practice_model.cpp); a bad code stays on the keyboard with the reason.
  if (r.start_unranked) { proai::search_ask(1, "", v.port); r.close = true; }
  if (r.start_direct || r.start_teams) {
    std::string code, error;
    const bool ok = r.start_direct ? slippi::native_practice::normalize_direct_code(r.direct_code, &code, &error)
                                   : slippi::native_practice::normalize_teams_code(r.direct_code, &code, &error);
    if (!ok) practice_menu::show_error(error);
    else { proai::search_ask(r.start_direct ? 2 : 3, code.c_str(), v.port); r.close = true; }
  }
  if (r.cancel_search) proai::search_stop();
  proai::menu_result(r.close, r.changed, r.stronger_changed);
}
}  // namespace fe
