// Between the PhobGCC menu's two halves: features/phob.cpp (simulation thread: opening it from the panel's PhobGCC page, the
// controller while it is open, back to the panel) and src/phob_menu_glue.cpp (render thread: melee-unlocked's gx/phob_menu.cpp,
// PhobVision's screens, PhobScope and Phob Check, drawn inside Aurora's ImGui pass). Plain functions; the state behind them is
// guarded in features/phob.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fe::phob_menu {
struct View {
  bool open = false;
  bool just_opened = false;   // first drawn frame since it opened (the menu is asked for this socket then)
  bool scopes_only = false;   // opened as "Controller test": PhobScope is the top screen
  int port = 0;               // the adapter socket it sets up (0..3)
  uint16_t buttons = 0;       // the controllers' GameCube buttons (every connected port's, as the game would read them)
  int8_t stick_x = 0, stick_y = 0;
  bool armed = false;         // let go since it opened
};
View view();                                      // render thread, once per drawn frame
bool is_open();
void result(bool back_to_panel, bool close_all, bool changed);   // render thread, after the menu's frame
}  // namespace fe::phob_menu

// overlay_draw.cpp (render thread, inside Aurora's ImGui frame). Weak: a build without the menu (null) draws nothing.
namespace fe {
__attribute__((weak)) void phob_menu_draw(float display_w, float display_h, float game_x0, float game_y0, float game_x1, float game_y1);
}
