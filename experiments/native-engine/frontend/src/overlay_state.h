// What the overlay module (src/features/overlays.cpp) hands the overlay drawer (src/overlay_draw.cpp): the settings of
// the on-screen overlays and picture options, and a copy of what the game reported in the newest retrace. The module
// fills it on the simulation thread from what the game hands its host anyway (the pads it read, MuHostApi.hud_player
// and hud_combo, MuGameApi.lcancel_view, the state snapshot); the drawer reads it on the render thread. Nothing here
// is ever written into the game: the overlays are pictures over the presented frame.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace fe::overlay {

// settings.ini keys (upstream's port-settings.ini names where upstream has the option; see frontend/README.md).
struct Config {
  // Controller input overlay (inputoverlay*): one pad display per controller port in `ports` (bit per port).
  bool input = false, input_values = false, input_hide_border = false;
  int input_ports = 1, input_size = 5;          // input_size 1..10 (upstream's inputoverlaystick)
  bool fps = false, graph = false;              // showfps, performance
  bool nicknames = false;                       // playernicknames
  bool combo = false;                           // arcade_combo_counter
  int lcancel = 0;                              // lcancel_flash_mode: 0 off, 1 missed, 5 made, 6 both (upstream's renderer modes)
  int lcancel_success = 2;                      // lcancel_success_flash: 0 none, 1 white, 2 green
  bool latency_test = false;                    // latencytest: X + D-pad Down paints black, then white, X + D-pad Up ends
  bool adapter_rate = false;                    // adapterrate: the GameCube adapter's measured report rate on screen
  // Picture (renderer). aspect: 0 Auto (4:3, 16:9 with the widescreen code), 1 73:60 (the console's own pixel shape),
  // 2 4:3, 3 16:9, 4 stretched to the screen (upstream's AspectMode numbers).
  int aspect = 0;
  float brightness = 1.0f, contrast = 1.0f, vibrance = 1.0f, sharpness = 0.0f;
  int crt = 0;                                  // crt_filter: 0 off, 1 studio monitor, 2 home television
  float crt_curve = 1.0f;                       // crt_curve 0..3
  std::string border;                           // borderart: empty none, "grid", or a .bmp file in <folder>/Borders
  int border_edge = 1;                          // borderedge: 1 the picture casts a soft shadow on the art
  int scale = 0;                                // scale: internal resolution, 0 Auto (the display), 1..4 times 640x480
  uint32_t generation = 0;                      // bumped whenever a picture option changes
};

struct Pad { bool used = false; uint16_t button = 0; int8_t sx = 0, sy = 0, cx = 0, cy = 0; uint8_t l = 0, r = 0; };
struct Player {
  bool present = false;                         // in the match (hud_player)
  int damage = 0, stocks = 0;
  float tag_x = NAN, tag_y = NAN;               // the name tag's anchor (or the fighter's place) in 640x480 units
  bool tag_visible = false;
  std::string name;                             // the Slippi display name online (empty offline)
  bool fighter = false;                         // the state snapshot has this player's fighter
  uint32_t action = 0;
  float anim_frame = 0, percent = 0, vel_x = 0, vel_y = 0;
};
struct Combo { int attacker = -1, hits = 0; bool live = false; uint32_t changed = 0, ended = 0; };   // by the player hit
struct Flash { bool on = false, success = false; uint32_t start = 0; };                               // by player slot
struct Snapshot {
  bool valid = false;
  uint32_t retrace = 0;
  bool match = false;                           // fighters on screen in a match
  bool training = false, online = false, replay = false;
  int local_player = -1;                        // online: this console's player
  Pad pads[4];                                  // what the game got from each controller port this retrace
  Player players[4];
  Combo combos[4];
  Flash flashes[4];
  bool advantage = false; int advantage_frames = 0, advantage_kind = 0; uint32_t advantage_retrace = 0;
  bool lab_panel = false;                       // Training with "Inputs and frame advantage" (lab_overlay)
  bool te_inputs = false;                       // 20XX TE's input display bit (MU_GAME_OPTION2_TE_INPUT_DISPLAY), offline matches and replays
  int latency_fill = -1;                        // -1 the picture, 0 black, 1 white
  float game_fps = 0;                           // retraces per second, measured on the simulation thread
};

// One online tick for the network panel (src/features/netoverlay.cpp; melee-unlocked's net_trace::Record, the fields drawn).
struct NetSample {
  double wall = 0;                              // seconds, steady clock
  float sim_ms = 0;                             // the tick's work, rollbacks included
  uint16_t ping_ms = 0;
  uint8_t rollbacks = 0, depth = 0;
  uint8_t flags = 0;                            // net_trace's: 1 wait, 2 halted by time sync, 4 advanced, 8 mark, 16/32/64 marks from the D-pad
};

}  // namespace fe::overlay

#include <vector>
namespace fe::features {
// netoverlay.cpp; weak: the newest ticks while the network panel is on (netoverlay 1) and an online match runs.
__attribute__((weak)) bool net_overlay_samples(std::vector<overlay::NetSample>& out);
// overlays.cpp; weak in the drawer, so the program works without the feature modules.
__attribute__((weak)) overlay::Config overlay_config();
__attribute__((weak)) void overlay_snapshot(overlay::Snapshot& out);
}
