// What the feature modules (this folder) share with each other. Not for play mode: play mode talks to
// them through ../feature_hooks.h only.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "feature_hooks.h"
#include "mu_host.h"

#include <cstdint>
#include <string>

namespace fe::features {
// registry.cpp: each module's registration, called once before the first dispatch.
void register_options();
void register_training();
void register_replays();

// options.cpp: the session as the modules see it, updated once per retrace and by the netplay commands.
bool online();                       // an online flow, search or match: offline-only features stay off
bool vanilla();                      // settings.ini `vanilla 1`: the retail game, no offline extras
uint32_t retrace();
uint32_t scene_major();
uint32_t scene_minor();
bool feature_test(const char* name, std::string* value = nullptr);   // --feature-test NAME[:VALUE] (verification runs)
const MuHostApi* host();             // the host table the game was given (after every module's changes)
MuGameApi* game();
// The pads as the controllers gave them this retrace (before any module changed them), and as the game got them.
const MuPadStatus* pads_raw();
const MuPadStatus* pads_out();
void read_controllers(MuPadStatus out[4]);   // the host's own pad read, now (the replay viewer, while paused)

// training.cpp
uint32_t training_option_bits();     // MU_GAME_OPTION_LAB* for option word 1 (offline Training tools)
void training_pad_hook(MuPadStatus pads[4]);   // frame advance (reads the pads; changes nothing)
void training_scene_changed();

// replays.cpp
bool replay_playing();               // a .slp is being played back
bool replay_word(int which, uint32_t* value);   // the recorded option words during playback (0..2)
bool replay_pad_hook(MuPadStatus pads[4]);      // playback controls; true when the pads must not reach the game as they are

// mods.cpp (play mode's mod layer; the modules may ask it)
}  // namespace fe::features

namespace fe {
bool mods_tmce();                    // the profile holds Training Mode CE's event menu (/tm/eventmenu.dat)
std::string mods_fingerprint();      // the replay's "modProfile": empty for the retail files
}

// Small helpers.
namespace fe::features {
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void put_be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v));
}
}
