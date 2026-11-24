// What the replay module (src/features/replays.cpp) hands the overlay drawer (src/overlay_draw.cpp) and the render loop
// (src/frontend.cpp) while a replay plays: where playback is, the viewer's state, the takeover and the coach's tips. The
// module fills it from its own state on request (render thread); nothing here reaches the game.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace fe::replay {

struct Tip {                       // replay_coach::Moment, as the drawer needs it
  int32_t frame = 0;
  std::string what, value, advice;
  int sign = 0;                    // -1 minus, 0 even, 1 plus (colours the value)
  int type = 0;                    // 0 unsafe on shield, 1 L-cancel, 2 combo, 3 tech (the marker's colour)
};

struct View {
  bool active = false;             // a replay is being played back
  bool hidden = false;             // B: the viewer draws nothing
  bool paused = false, fast = false, seeking = false;
  int slow = 1;                    // 1, 2, 4: 1/slow speed
  int32_t frame = 0, first = 0, last = 0;
  double idle = 0;                 // seconds since the viewer last pressed something (the bar fades while playing)
  int port = 0;                    // the player A takes over (watching) or the live port (takeover)
  bool live = false;               // a takeover is in progress
  int32_t live_frame = 0;          // the takeover's frame
  std::string name;                // "P1 Fox"
  uint32_t ports = 0;              // bit per port present in the replay
  bool tips_on = true;
  std::vector<Tip> tips;           // the shown player's tips, by frame
  // The newest hit or shield hit the coach saw, with how long ago (fades out after 5 s).
  bool readout = false;
  double readout_age = 0;
  std::string head, value, detail;
  int sign = 0;
  // Takeover.
  int tries = 0;
  int seconds_left = -1;           // a try with a time limit: what is left; -1 none
  bool result = false;             // the try ended: its result is shown until the next one
  std::string result_head, result_value, result_detail;
  int result_sign = 0;
  int next_try_in = 0;
  // Analyze (the whole replay run once unseen).
  bool analyzing = false;
  float analyze_progress = 0;
};

}  // namespace fe::replay

namespace fe::features {
// replays.cpp; weak in the drawer and the render loop, so the program works without the feature modules.
__attribute__((weak)) void replay_view(replay::View& out);
// The picture stands still (the replay is paused, or a takeover's result is shown) but the overlay and the menu still
// change: the render loop draws its last frame again.
__attribute__((weak)) bool replay_repaint_wanted();
// Frames on the way to a jump's target (simulated and drawn by the game, for exactness): the render loop does not show them.
__attribute__((weak)) bool replay_frames_unseen();
}
