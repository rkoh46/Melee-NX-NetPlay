// The host services melee-unlocked's user_gecko.cpp (build-time copy, patches/0122) names, on this program. Only the log and
// the session rule are used here: the console memory reads and writes and the function redirect belong to upstream's Static
// Recomp path (set_code_patches_allowed, never set here), and features/gecko.cpp defines them as refusals.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "gameplay_policy.h"     // melee-unlocked port/runtime/host (the fetched tree): host::gameplay::vanilla_only

namespace host {
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));   // the host services' (patches/0101)
uint8_t rd8(uint32_t addr);
void wr8(uint32_t addr, uint8_t value);
void wr16(uint32_t addr, uint16_t value);
void wr32(uint32_t addr, uint32_t value);
}  // namespace host
namespace ppc {
bool redirect_function_at(uint32_t addr);
}
