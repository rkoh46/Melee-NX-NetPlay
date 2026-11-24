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
  int port = 0;               // the controller that opened it
};
// Render thread, once per drawn frame.
MenuView menu_view();
// Render thread, after practice_menu::frame: what the menu asked for.
void menu_result(bool close, bool changed, bool stronger_changed);

// The Practice menu's Search page (features/search.cpp; upstream native_practice.cpp start_search and its snapshot): a Slippi
// search started from an offline Training match, which goes on while the player keeps training (the kept search).
struct SearchView {
  bool available = false;     // the page can be used here: the services program, an offline Training match, or a search running
  bool can_start = false;     // nothing running: Unranked, Direct and Teams can start
  bool searching = false;     // a search runs (started here or kept from the select)
  const char* mode = "";      // "Unranked", "Direct", "Teams"
  uint32_t elapsed_ms = 0;
  int state = 0;              // the netplay object's search state (0 idle .. 4 connected, 5 error)
  char opponent[40] = {};
  char code[24] = {};
  char note[96] = {};         // why the last start was refused (shown once in the menu's description box)
};
SearchView search_view();     // render thread
// Render thread: start a search (1 Unranked, 2 Direct with `code`, 3 Teams with `code`), or stop the one running; acted on at
// the next retrace on the simulation thread. `port` is the controller that drives the menu.
void search_ask(int mode, const char* code, int port);
void search_stop();

}  // namespace fe::proai

// overlay_draw.cpp (render thread, inside Aurora's ImGui frame): the Practice menu, open or closed (its corner hint).
// Weak: a build without the menu draws nothing.
namespace fe {
__attribute__((weak)) void practice_menu_draw(float display_w, float display_h, float game_x0, float game_y0, float game_x1,
                                              float game_y1);
}
