// See slippi_match_quality.h. Port of match-quality/src/lib.rs (slippi-rust-extensions 934f373).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_match_quality.h"
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#include "host.h"
#include <algorithm>
#include <optional>

namespace slippi::match_quality {
namespace {
constexpr int32_t ROLLBACK_MAX_FRAMES = 7;
constexpr uint64_t FRAME_TIME_US = 16683;
constexpr int32_t WARMUP_FRAMES = 120;      // nothing before this frame is judged
constexpr int32_t INTERVAL_FRAMES = 150;    // the debt is updated every 2.5 seconds after the warmup
constexpr int32_t LATE_GAME_FRAME = 3600;
constexpr double LATE_GAME_MODIFIER = 1.15;
constexpr int32_t TERMINATE_DEBT = 30;
constexpr int32_t MILD_DEBT = 4, BAD_DEBT = 8, SEVERE_DEBT = 15, HEALTHY_DEBT = -1;
constexpr double OVERRUN_TIERS[3] = {0.1, 0.5, 0.75};
constexpr double DEPTH_TIERS[3] = {2.0, 3.0, 4.75};
constexpr int32_t MIN_JUDGED_DELAY = 2;
constexpr uint64_t STALL_MERGE_GAP_US = 500000;
constexpr uint32_t MIN_STALL_PASSES = 5;
constexpr int32_t STALL_DEBT = 9, SHORT_STALL_DEBT = 3;

int32_t tier_debt(double value, const double tiers[3], double modifier) {
  if (value >= tiers[2] * modifier) return SEVERE_DEBT;
  if (value >= tiers[1] * modifier) return BAD_DEBT;
  if (value >= tiers[0] * modifier) return MILD_DEBT;
  return HEALTHY_DEBT;
}

struct Game {
  GameConfig config;
  bool verdict_reached = false;
  int32_t debt = 0;
  std::optional<uint64_t> interval_start_us;
  int32_t last_interval_frame = 0;
  uint64_t depth_sum = 0, depth_samples = 0, ping_sum_us = 0, ping_samples = 0;
  std::optional<uint64_t> last_stall_us;
  uint32_t current_stall_passes = 0, interval_stall_events = 0, interval_stalled_passes = 0, stall_events = 0;

  void on_stall(const Frame& frame) {
    if (frame.frame <= WARMUP_FRAMES) return;
    ++interval_stalled_passes;
    const bool is_new_event = !last_stall_us || (frame.time_us >= *last_stall_us ? frame.time_us - *last_stall_us : 0) > STALL_MERGE_GAP_US;
    last_stall_us = frame.time_us;
    if (is_new_event) current_stall_passes = 0;
    ++current_stall_passes;
    if (current_stall_passes != MIN_STALL_PASSES) return;   // an event counts once, on the pass that reaches the minimum length
    ++interval_stall_events;
    ++stall_events;
    host::log("slippi: match quality: stall event at frame %d (newest remote input %d), %u this game", frame.frame, frame.latest_remote_frame, stall_events);
  }

  // Returns true when the debt has reached the termination threshold.
  bool on_interval(const Frame& frame) {
    const uint64_t d_sum = depth_sum, d_samples = depth_samples, p_sum = ping_sum_us, p_samples = ping_samples;
    const uint32_t events = interval_stall_events, stalled = interval_stalled_passes;
    depth_sum = depth_samples = ping_sum_us = ping_samples = 0;
    interval_stall_events = interval_stalled_passes = 0;
    const std::optional<uint64_t> start = interval_start_us;
    interval_start_us = frame.time_us;
    if (!start) return false;   // the first boundary only starts the clock
    const uint64_t expected_us = FRAME_TIME_US * (uint64_t)INTERVAL_FRAMES;
    const double ratio = (double)(frame.time_us >= *start ? frame.time_us - *start : 0) / (double)expected_us;
    const double avg_depth = (double)d_sum / (double)std::max<uint64_t>(d_samples, 1);
    const double avg_ping_ms = (double)p_sum / (double)std::max<uint64_t>(p_samples, 1) / 1000.0;
    const double modifier = frame.frame > LATE_GAME_FRAME ? LATE_GAME_MODIFIER : 1.0;
    const int32_t speed_debt = tier_debt(ratio - 1.0, OVERRUN_TIERS, modifier);
    const int32_t depth_debt = tier_debt(avg_depth, DEPTH_TIERS, modifier);
    const int32_t stall_debt = events > 0 ? STALL_DEBT : stalled > 0 ? SHORT_STALL_DEBT : HEALTHY_DEBT;
    // The worst of the three, not the sum: a bad connection stalls, deepens rollbacks and stretches the interval at once.
    const int32_t change = std::max(speed_debt, std::max(depth_debt, stall_debt));
    debt = std::max(debt + change, 0);
    if (change > 0 || debt > 0)
      host::log("slippi: match quality: frame %d debt %+d (speed %d, depth %d, stall %d; ratio %.2f, avg depth %.2f, delay %u, avg ping %.1f ms, stall events %u, stalled passes %u), now %d/%d",
                frame.frame, change, speed_debt, depth_debt, stall_debt, ratio, avg_depth, config.delay, avg_ping_ms, events, stalled, debt, TERMINATE_DEBT);
    return debt >= TERMINATE_DEBT;
  }

  bool on_frame(const Frame& frame) {
    const int32_t delay_shortfall = std::max(MIN_JUDGED_DELAY - (int32_t)config.delay, 0);
    // A stalled frame is past the rollback limit by definition, so cap there in case the input queue is empty.
    const int32_t depth = std::clamp(frame.frame - frame.latest_remote_frame - delay_shortfall, 0, ROLLBACK_MAX_FRAMES + 1);
    depth_sum += (uint64_t)depth;
    ++depth_samples;
    if (frame.ping_us > 0) { ping_sum_us += frame.ping_us; ++ping_samples; }
    bool terminate = false;
    if (frame.is_stall) on_stall(frame);
    else if (frame.frame >= WARMUP_FRAMES && (frame.frame - WARMUP_FRAMES) % INTERVAL_FRAMES == 0 && frame.frame != last_interval_frame) {
      last_interval_frame = frame.frame;
      terminate = on_interval(frame);
    }
    if (!terminate || verdict_reached) return false;   // a match that cannot be terminated keeps being tracked
    verdict_reached = true;
    if (!config.can_terminate) { host::log("slippi: match quality: a ranked match would have been terminated at frame %d (debt %d/%d)", frame.frame, debt, TERMINATE_DEBT); return false; }
    host::log("slippi: match quality: terminating the match at frame %d (debt %d/%d, %u stall events)", frame.frame, debt, TERMINATE_DEBT, stall_events);
    return true;
  }
};
std::optional<Game> g_game;
}  // namespace

void start_game(const GameConfig& config) { g_game.emplace(); g_game->config = config; }
void end_game() { g_game.reset(); }
bool on_frame(const Frame& frame) { return g_game && g_game->on_frame(frame); }
}  // namespace slippi::match_quality
#else
namespace slippi::match_quality {
void start_game(const GameConfig&) {}
void end_game() {}
bool on_frame(const Frame&) { return false; }
}  // namespace slippi::match_quality
#endif
