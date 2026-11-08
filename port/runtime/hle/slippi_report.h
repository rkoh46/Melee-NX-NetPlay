// Slippi game reporting (port of SlippiRustExtensions' game-reporter): after each online game the
// result is POSTed to Slippi's GraphQL API and, when the server asks for it, the replay is
// uploaded. Ranked play depends on this; unranked reports feed stats. Match status updates
// (connecting, game setup/start, completion, abandoned, poor performance) go the same way.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::report {
struct PlayerReport {
  std::string uid;
  uint8_t slot_type = 0, stocks_remaining = 0, character_id = 0, color_id = 0;
  double damage_done = 0;
  int starting_stocks = 0, starting_percent = 0;
};
struct GameReport {
  std::string uid, play_key, match_id, replay_path;
  int online_mode = 0;
  uint32_t duration_frames = 0, game_index = 0, tiebreak_index = 0;
  int8_t winner_index = -1, lras_initiator = -1;
  uint8_t game_end_method = 0;
  int stage_id = 0;
  std::vector<PlayerReport> players;
};
void init(const std::string& iso_path, const std::string& cache_dir);   // starts the worker; hashes the ISO in the background
void shutdown();                                                         // flushes queued reports (one attempt each)
void log_game(const GameReport& report);
void match_status(const std::string& uid, const std::string& play_key, const std::string& match_id, const std::string& status, bool background);

// Ranked: the player's rank (port of the Rust user crate's rank fetcher). fetch_user_rank runs at
// login (users REST API), fetch_match_result after a ranked game (GraphQL), both in the background.
struct RankInfo {
  int8_t rank = 0;              // SlippiRank index (0 unranked ... 19 grandmaster)
  float rating_ordinal = 0;
  uint16_t global_placing = 0, regional_placing = 0;
  uint32_t rating_update_count = 0;
  float rating_change = 0;
  int8_t rank_change = 0;
};
enum class RankFetchStatus : uint8_t { Fetching = 0, Fetched = 1, Error = 2 };
void fetch_user_rank(const std::string& uid);
void fetch_match_result(const std::string& match_id, const std::string& uid, const std::string& play_key);
RankFetchStatus rank_info(RankInfo* out);
}  // namespace slippi::report
