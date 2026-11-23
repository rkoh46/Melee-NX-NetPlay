// Stand-in for port/runtime/host/game_menu.h: the one call the app's input code makes into its in-game
// menu. play_io.cpp implements it (this program has no menu, only the way out).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "host.h"

namespace host {
// Per controller read, with every input source merged.
void menu_frame(PadState pads[4]);
}  // namespace host
