// The replay coach as melee-unlocked has it, compiled from the fetched tree (GPL-2.0-or-later): port/runtime/host/
// replay_coach.cpp (the game's frame data events turned into readouts, advice and the moments worth practising) and
// replay_scan.cpp (the same events read from a replay file's recorded frames at load). replays.cpp feeds and reads it.
// replay_scan.cpp includes "slippilib/SlippiGame.h": CMakeLists.txt points that name at the fetched tree's copy, the
// one replays.cpp compiles (this repository's port/third_party has an older one).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "replay_coach.cpp"
#include "replay_scan.cpp"
