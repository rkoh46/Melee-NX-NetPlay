// Match quality for Slippi Online: decides when a ranked match has degraded enough to be terminated,
// from stall events, rollback depth and frame timing (never ping). C++ port of the `match-quality`
// crate of project-slippi/slippi-rust-extensions (GPL-2.0, commit 934f373); same constants, same order.
// All calls come from the simulation thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace slippi::match_quality {
struct GameConfig { bool can_terminate = false; uint8_t delay = 2; };
// One pass through the online frame handler. A halted frame is reported again on every pass until it goes through.
struct Frame { int32_t frame = 0; uint64_t time_us = 0; bool is_stall = false; int32_t latest_remote_frame = 0; uint64_t ping_us = 0; };
void start_game(const GameConfig& config);
void end_game();
bool on_frame(const Frame& frame);   // true once, when the match should be terminated
}  // namespace slippi::match_quality
