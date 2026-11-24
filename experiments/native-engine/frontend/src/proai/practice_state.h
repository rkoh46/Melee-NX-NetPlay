// Between the Practice menu's two halves: features/proai.cpp (simulation thread: the chord, the controller while the
// menu is open, settings.ini, Training again for a pro) and proai/practice_overlay.cpp (render thread: melee-unlocked's
// practice_menu.cpp draws the menu and reads its controller inside Aurora's ImGui pass). Plain functions; the state
// behind them is guarded in proai.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fe::proai {

struct MenuView {
  bool open = false;          // the menu is open
  bool armed = false;         // its controller was let go since it opened
  bool just_opened = false;   // first frame drawn since it opened (practice_menu::opened)
  uint16_t buttons = 0;       // GameCube buttons of the controller that opened it
  int8_t stick_x = 0, stick_y = 0;
  bool training = false;      // an offline Training match: the corner hint may show
  bool hint = false;          // settings.ini matchmakinghint
  uint32_t unlocked = 0xFFFFFFFFu;   // characters the memory card has
  int style = 0;              // settings.ini practicestyle (0..3)
};
// Render thread, once per drawn frame.
MenuView menu_view();
// Render thread, after practice_menu::frame: what the menu asked for.
void menu_result(bool close, bool changed, bool stronger_changed);

}  // namespace fe::proai

// overlay_draw.cpp (render thread, inside Aurora's ImGui frame): the Practice menu, open or closed (its corner hint).
// Weak: a build without the menu draws nothing.
namespace fe {
__attribute__((weak)) void practice_menu_draw(float display_w, float display_h, float game_x0, float game_y0, float game_x1,
                                              float game_y1);
}
