// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad_rumble.h"

namespace hle::pad {
namespace {

constexpr uint16_t kOnlineInGameScene = 0x0208;
constexpr uint32_t kOdbPointerR13Offset = 0x49E4;
constexpr uint32_t kGuestAddressMask = 0x3FFFFFFF;
constexpr uint8_t kPortCount = 4;

bool span(const uint8_t* ram, size_t ram_size, uint32_t address, size_t bytes,
          size_t& offset) {
  offset = address & kGuestAddressMask;
  return ram != nullptr && offset <= ram_size && bytes <= ram_size - offset;
}

bool read_u8(const uint8_t* ram, size_t ram_size, uint32_t address,
             uint8_t& value) {
  size_t offset = 0;
  if (!span(ram, ram_size, address, 1, offset)) return false;
  value = ram[offset];
  return true;
}

bool read_be32(const uint8_t* ram, size_t ram_size, uint32_t address,
               uint32_t& value) {
  size_t offset = 0;
  if (!span(ram, ram_size, address, 4, offset)) return false;
  value = (uint32_t(ram[offset]) << 24) |
          (uint32_t(ram[offset + 1]) << 16) |
          (uint32_t(ram[offset + 2]) << 8) | uint32_t(ram[offset + 3]);
  return true;
}

}  // namespace

RumbleDecision decide_rumble(uint32_t logical_port, uint32_t command,
                             uint8_t mode, uint8_t state_id,
                             uint32_t r13, const uint8_t* ram,
                             size_t ram_size) {
  RumbleDecision result{};
  result.on = command == 1;
  if (logical_port >= kPortCount) return result;

  // Common.s:getMinorMajor rotates the word at 0x80479D30 so the state ID
  // from byte +3 is high and the game mode from byte +0 is low.
  const uint16_t scene = uint16_t(uint16_t(state_id) << 8) | mode;
  if (scene != kOnlineInGameScene) {
    result.deliver = true;
    result.physical_port = uint8_t(logical_port);
    return result;
  }

  if (r13 < kOdbPointerR13Offset) return result;
  uint32_t odb = 0;
  if (!read_be32(ram, ram_size, r13 - kOdbPointerR13Offset, odb) || odb == 0) {
    return result;
  }

  uint8_t local_player = 0;
  uint8_t input_source = 0;
  if (!read_u8(ram, ram_size, odb, local_player) ||
      !read_u8(ram, ram_size, odb + 2, input_source) ||
      local_player >= kPortCount || input_source >= kPortCount ||
      logical_port != local_player) {
    return result;
  }

  result.deliver = true;
  result.physical_port = input_source;
  return result;
}

}  // namespace hle::pad
