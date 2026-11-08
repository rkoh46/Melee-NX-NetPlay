// RFC 3284 VCDIFF decoder (standard code table, no secondary compression), used for the
// Slippi GameFiles "*.diff" patches applied to files read from the ISO.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host {
// Returns false (with a message in `error`) on malformed input.
bool vcdiff_decode(const uint8_t* source, size_t source_size, const uint8_t* delta, size_t delta_size,
                   std::vector<uint8_t>& target, std::string* error = nullptr);
}  // namespace host
