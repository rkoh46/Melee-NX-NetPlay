// The stock icons the Practice menu's warm-up panel draws: there is no warm-up room in this app, so none.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace host::cosmetics {
inline bool standard_stock_icon(int, int, std::vector<uint8_t>*, int*, int*) { return false; }
}
