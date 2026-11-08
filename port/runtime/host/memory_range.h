// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace host {
constexpr bool valid_range(uint32_t offset, uint32_t length, uint32_t capacity) {
  return offset <= capacity && length <= capacity - offset;
}
}
