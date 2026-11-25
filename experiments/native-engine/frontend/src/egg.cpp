// melee-unlocked's character select easter egg (port/runtime/gx/walter_egg.cpp and .h, compiled from the fetched tree,
// GPL-2.0-or-later; build.sh sync copies them into gen/mu_ui/egg): Up Up Down Down Left Right Left Right then B A (Bailey)
// or A B (Walter) on any controller sends a dog's face bouncing around the character select; a hand that presses A on one
// takes hold of it; the C-stick's camera turn tilts them. Local drawing only: the pads are read, never changed, and nothing
// is sent to the other player.
//   - the code: mu_panel.cpp's controller poll (app_menu_frame) hands the merged pads to watch_pads, before the menu's hand-over;
//   - the character select frame command (0xDA, the game image's shim mu_css_frame and its hands, features/egg_command.cpp):
//     css_frame, set_camera, set_hand. The menu sound upstream plays when a face hits a corner is asked for offline only
//     (a sound the game plays is game code: never in an online session);
//   - the drawing: mu_panel.cpp's panel_draw, over the game picture.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "walter_egg.cpp"

#include <cstring>

namespace fe {
void egg_watch_pads(const host::PadState pads[4]) { walter_egg::watch_pads(pads); }
void egg_draw(float x0, float y0, float x1, float y1, double time) {
  walter_egg::Frame f; f.game_x0 = x0; f.game_y0 = y0; f.game_x1 = x1; f.game_y1 = y1; f.time = time;
  walter_egg::draw(f);
}
}  // namespace fe
namespace fe::features {
// features/egg_command.cpp: the 0xDA command (0 bytes: a frame; 52: a frame with the menu camera's 13 floats; 5: a hand).
int egg_css_command(const uint8_t* p, uint32_t n, bool offline, std::vector<uint8_t>& reply) {
  if (n == 5) { walter_egg::set_hand(p[0], int16_t(p[1] << 8 | p[2]), int16_t(p[3] << 8 | p[4])); return 1; }
  if (n == 0) walter_egg::set_camera(nullptr);
  else if (n == 52) { float cam[13]; std::memcpy(cam, p, sizeof cam); walter_egg::set_camera(cam); }
  else return 0;
  const uint8_t sound = walter_egg::css_frame();
  reply.push_back(offline ? sound : 0);
  return 1;
}
}  // namespace fe::features
