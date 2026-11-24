// What the feature modules (this folder) share with each other. Not for play mode: play mode talks to
// them through ../feature_hooks.h only.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "feature_hooks.h"
#include "mu_host.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fe::features {
// registry.cpp: each module's registration, called once before the first dispatch.
void register_options();
void register_training();
void register_replays();
void register_overlays();
void register_spectator();
void register_netoverlay();
void register_replay_library();      // replay_library.cpp, from register_replays (its rows first on the Replays page)
void register_search();
void register_proai();               // proai.cpp: Fight a pro (command 0xEA) and the Practice menu's simulation-thread half

// options.cpp: the session as the modules see it, updated once per retrace and by the netplay commands.
bool online();                       // an online flow, search or match: offline-only features stay off
bool vanilla();                      // settings.ini `vanilla 1` or a vanilla-only session: no offline extras
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
// Into an offline Training match as `character` (Training's select pressed through, Final Destination); the CPU as `cpu_character`
// (-1: Training's own). from_online: allowed from the online screens (the search module: play while you search, Training after
// online). False when the game cannot be asked now.
bool training_go(int character, int costume, int cpu_character, int cpu_costume, bool from_online);
bool training_entering();            // Training's menus are being pressed through
bool training_lab_overlay();         // Training's "Inputs and frame advantage" is on (lab_overlay)
bool training_practice_display();    // the Practice menu's Tools: projectile hitboxes (MU_PC_TOOL_PROJECTILE_HITBOXES)
bool training_practice_combo();      // the Practice menu's Tools: combo counter (MU_PC_TOOL_COMBO_COUNTER)
bool training_advantage(int* frames, int* kind, uint32_t* retrace);   // the newest 0xF4 of this scene (false: none)
std::vector<uint8_t> training_replay_defense(int setting);   // a takeover's opponents (0xE9): MuPcConfig bytes, empty to follow the recording

// overlays.cpp
bool overlay_draws_training();       // a picture overlay draws the Training panel (frame advantage included)

// replays.cpp
bool replay_playing();               // a .slp is being played back
bool replay_word(int which, uint32_t* value);   // the recorded option words during playback (0..2)
bool replay_pad_hook(MuPadStatus pads[4]);      // playback controls; true when the pads must not reach the game as they are
std::string replay_folder();                    // where replays are written and listed
void replay_watch(const std::string& path);    // play mode starts the game again with this replay (the menu's "Watch it")
// replay_library.cpp: the replay list as the Replays page shows it (sorted, short games hidden), newest first by default.
std::vector<std::string> library_list();

// spectator.cpp: the Slippi spectator server (settings.ini slippi_spectator 1)
bool spectator_wanted();             // switched on in settings.ini (the server starts with the run)
bool spectator_on();                 // the server thread runs
void spectator_write(const uint8_t* data, uint32_t size);   // one recording event, command byte first (any thread)
void spectator_start_game();
void spectator_end_game(bool dolphin_closed);

// netoverlay.cpp: the replay writer wrote a match (its thread): that match's session trace goes beside it
void net_trace_replay_saved(const std::string& slp_path);

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
