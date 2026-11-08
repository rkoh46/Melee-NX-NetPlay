// Local match history from the app's own .slp replays (Slippi has no public match API).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::history {
struct Player { int port = 0; int character = -1; int type = 3; std::string name, code; int stocks = -1; };   // type: 0 human, 1 cpu, 3 empty
struct Game {
  std::string path, started_at;   // ISO 8601 from the file name / metadata
  int stage = -1;
  int last_frame = 0;             // duration = last_frame / 60 seconds
  std::vector<Player> players;
  int winner_port = -1;           // -1 unknown
  bool online = false;            // connect codes present
};
// Newest `limit` replays under `dir`, parsed from the raw event stream (game start, post-frame stocks, game end).
std::vector<Game> recent_games(const std::string& dir, size_t limit);
}  // namespace slippi::history
