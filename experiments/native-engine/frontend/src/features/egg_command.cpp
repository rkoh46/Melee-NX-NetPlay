// The character select frame command (0xDA) for melee-unlocked's easter egg (src/egg.cpp, builds with a picture only: weak).
// A build without the egg leaves the command unanswered, as before. SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"

namespace fe::features {
__attribute__((weak)) int egg_css_command(const uint8_t* p, uint32_t n, bool offline, std::vector<uint8_t>& reply);
namespace {
int css_frame(uint8_t, const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  if (!&egg_css_command) return 0;
  return egg_css_command(p, n, !online(), reply);
}
}  // namespace
void register_egg() { add_command(0xDA, css_frame); }
}  // namespace fe::features
