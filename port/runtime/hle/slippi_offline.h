// Offline EXI responses shared by the full runtime and the diagnostic build.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "slippi_online.h"

namespace slippi::online::offline {
void reset(const Config& config);
bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len,
            std::vector<uint8_t>& read_queue);
}  // namespace slippi::online::offline
