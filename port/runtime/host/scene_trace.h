// Raw scene observations; no game-state writes or inferred scene transitions.
// Source: GALE01 routingInfo curr_mode +0 / curr_state_id +3 in gm_1A3F.c,
// state_machine 0x80479D30; raw byte loads confirmed by Slippi GetCommonMinorID.
// The combined diagnostic value is explicitly defined here as 0xSSMM.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace host {
constexpr uint32_t SCENE_MODE_ADDRESS = 0x80479D30u;
constexpr uint32_t SCENE_STATE_ADDRESS = 0x80479D33u;
struct SceneTransition {
  uint32_t retrace = 0;
  uint8_t mode_byte = 0, state_byte = 0;
  uint16_t combined() const { return uint16_t((uint16_t(state_byte) << 8) | mode_byte); }
};
class SceneTrace {
public:
  bool observe(uint32_t retrace, uint8_t mode, uint8_t state) {
    uint16_t combined = uint16_t((uint16_t(state) << 8) | mode);
    seen_.set(combined);
    if (initialized_ && latest_.combined() == combined) return false;
    initialized_ = true;
    latest_ = {retrace, mode, state};
    ++transitions_;
    if (size_ < events_.size()) events_[size_++] = latest_;
    else ++unrecorded_;
    return true;
  }
  bool saw(uint16_t combined) const { return seen_.test(combined); }
  const SceneTransition& latest() const { return latest_; }
  const std::array<SceneTransition, 256>& events() const { return events_; }
  size_t size() const { return size_; }
  uint64_t transitions() const { return transitions_; }
  uint64_t unrecorded() const { return unrecorded_; }
private:
  bool initialized_ = false;
  SceneTransition latest_{};
  std::array<SceneTransition, 256> events_{};
  std::bitset<65536> seen_;
  size_t size_ = 0;
  uint64_t transitions_ = 0, unrecorded_ = 0;
};
}  // namespace host
