// Registration interface between play mode and the feature modules (src/features/, runner/features/).
//
// A feature module registers what it needs once, from a static initialiser, and play mode calls the
// dispatch functions at the points named below. Nothing here runs in the scripted runs: play mode is
// the only caller of the dispatch functions, so a module that registers handlers changes nothing
// unless play mode is running. Every dispatch runs on the simulation thread unless it says otherwise.
//
// Call sites play mode provides (play_main.cpp, settings.cpp, mods.cpp, the in-game menu):
//   dispatch_tables(host, game)   first thing in play mode's tables hook, before it wraps anything, so the
//                                 option words it logs include what the modules add
//   dispatch_command(...)         first thing in mu_frontend_command (mods.cpp); INT32_MIN: nobody took it
//   dispatch_retrace(info)        once per retrace after the frame (play mode's frame observer)
//   dispatch_setting(key, value)  for every settings.ini line settings.cpp does not know; true: taken
//   dispatch_run_end()            after linux_host::run returns, before the game is unmapped
//   menu_entries()                the rows the in-game menu shows on the modules' pages
//   hud_lines()                   short text lines for the overlay, read by the render or menu thread
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

struct MuHostApi;
struct MuGameApi;
struct MuStatePod;

namespace fe::features {

// ---- what a module registers ----

// slippi_command: one byte, any payload. Return 1 when taken (reply holds the response, possibly
// empty), 0 when not this module's (the next handler or the netplay host gets it), or a negative
// slippi_command error (-1 unknown, -2 bounds) to answer the game with that.
using CommandFn = int (*)(uint8_t command, const uint8_t* payload, uint32_t size, std::vector<uint8_t>& reply);
void add_command(uint8_t command, CommandFn fn);

// The host table before the game sees it: a module may replace entries (keeping the old pointer to
// call through). Called once per run, in registration order.
using TablesFn = void (*)(MuHostApi* host, MuGameApi* game);
void add_tables(TablesFn fn);

// Once per retrace, after the frame.
struct Retrace {
  uint32_t retrace = 0;
  uint32_t scene_major = 0, scene_minor = 0;
  uint32_t match_frame = 0;
  bool fighting = false;      // fighters were simulated this retrace
  bool online = false;        // Slippi's online flow, or a search or match of the netplay host
  const MuStatePod* state = nullptr;
};
using RetraceFn = void (*)(const Retrace& info);
void add_retrace(RetraceFn fn);

// settings.ini: one "key value" line. Return true when the key is the module's.
using SettingFn = bool (*)(const std::string& key, const std::string& value);
void add_setting(SettingFn fn);

// The run ended (the game stopped); the game's memory is still mapped.
using RunEndFn = void (*)();
void add_run_end(RunEndFn fn);

// A row of the in-game menu. `page` groups rows ("Training", "Replays", "Game"...). `value` is shown
// right of the label (may be null for an action row); `change` steps it left (-1) or right (+1);
// `press` is the A button (may be null). Called from the menu's thread with the game paused or between
// frames; a module keeps its own state consistent for that.
struct MenuEntry {
  const char* page;
  const char* label;
  std::string (*value)();
  void (*change)(int direction);
  void (*press)();
};
void add_menu_entry(const MenuEntry& entry);

// Overlay text: each call appends the lines the module wants on screen now (empty when none).
// Called from whichever thread draws the overlay; a module copies what it shows under its own lock.
struct HudLine { std::string text; uint32_t rgba = 0xFFFFFFFFu; };
using HudFn = void (*)(std::vector<HudLine>& out);
void add_hud(HudFn fn);

// ---- what play mode calls ----
void dispatch_tables(MuHostApi* host, MuGameApi* game);
int32_t dispatch_command(uint8_t command, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity,
                         uint32_t* response_size);   // INT32_MIN when no module took it
void dispatch_retrace(const Retrace& info);
bool dispatch_setting(const std::string& key, const std::string& value);
void dispatch_run_end();
const std::vector<MenuEntry>& menu_entries();
std::vector<HudLine> hud_lines();
// A module asks play mode to start the game again with these arguments added (the replay viewer's "Watch
// it": the game takes a replay only at boot). Play mode polls this after each retrace or when its menu
// closes; true hands the arguments over once.
bool take_restart_request(std::vector<std::string>* extra_args);
// The replays on the SD card, newest first (file names in the replay folder), for a launcher's list.
std::vector<std::string> replays_list();

}  // namespace fe::features
