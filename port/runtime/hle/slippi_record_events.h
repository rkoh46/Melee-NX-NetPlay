// Pure recording-event telemetry; frame survival is not gameplay evidence.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace slippi {
struct RecordingEvents {
  uint64_t commands = 0;
  uint64_t game_start = 0, pre_frame = 0, post_frame = 0, game_end = 0;
  bool has_frames = false;
  int32_t min_frame = 0, max_frame = 0;
  bool match_input_started = false;

  enum Transition : uint8_t { None = 0, Commands = 1, GameStart = 2, GameEnd = 4, MatchInputStart = 8 };

  // `record` includes the command byte. The caller validates the command-table
  // payload size first. A new command table starts a new per-game observation.
  uint8_t observe(const uint8_t* record, size_t size) {
    if (!record || size == 0) return None;
    switch (record[0]) {
      case 0x35: {
        if (size < 2 || record[1] < 1 || size != size_t(record[1]) + 1 || (record[1] - 1) % 3 != 0) return None;
        const uint64_t sessions = commands + 1;
        *this = {};
        commands = sessions;
        return Commands;
      }
      case 0x36:
        if (!commands || size < 5) return None;
        ++game_start;
        return GameStart;
      case 0x37: case 0x38: {
        if (!commands || !game_start || size < 5) return None;
        const uint32_t word = (uint32_t(record[1]) << 24) | (uint32_t(record[2]) << 16) |
                              (uint32_t(record[3]) << 8) | record[4];
        const int32_t frame = static_cast<int32_t>(word);
        if (!has_frames) min_frame = max_frame = frame;
        else { min_frame = std::min(min_frame, frame); max_frame = std::max(max_frame, frame); }
        has_frames = true;
        if (record[0] == 0x37) ++pre_frame;
        else {
          ++post_frame;
          if (frame == 1 && !match_input_started) { match_input_started = true; return MatchInputStart; }
        }
        return None;
      }
      case 0x39:
        if (!commands || !game_start || size < 2) return None;
        ++game_end;
        return GameEnd;
      default: return None;
    }
  }
};
}  // namespace slippi
