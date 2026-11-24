// The Practice menu's render-thread half: melee-unlocked's practice_menu.cpp (build-time copy with patches/0113) draws
// the menu in the look settings.ini practicestyle picks and reads its controller, once per drawn frame inside Aurora's
// ImGui pass (overlay_draw.cpp calls this), as upstream's pc_settings.cpp calls it inside its overlay. What it needs from
// the game side comes from features/proai.cpp (practice_state.h); what it asks for goes back there.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "practice_menu.h"   // build-time copy (build/native-engine-gfx/gen/proai)
#include "practice_state.h"

#include <imgui.h>

namespace fe {
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
  f.keyboard = false;              // the controller drives it (no keyboard on the console)
  f.hint = v.hint && v.training;   // the corner hint in Training (upstream: every Training match unless turned off)
  f.intro = false;                 // upstream's first-open banner is about its search, which this menu does not start
  f.unlocked = v.unlocked;
  f.time = ImGui::GetTime();
  // The Search page: this app searches from Online Play (and keeps Training going there with L + R on the select).
  f.search.available = false;
  const practice_menu::Result r = practice_menu::frame(v.style, f);
  proai::menu_result(r.close, r.changed, r.stronger_changed);
}
}  // namespace fe
