// Arena rooms are not part of this app (FEATURES.md): the Practice menu's small-window controls have nothing to move.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace slippi::arena {
inline void pip_adjust(bool) {}
inline bool pip_active() { return false; }
}
