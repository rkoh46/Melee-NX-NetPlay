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
//   dispatch_reply(...)           after the netplay host answered a slippi_command (play mode with netplay), read only
//   dispatch_exchange(...)        the same, with the payload and the result, for every command the netplay host answered
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
  // After `change` or `press`: the settings.ini lines that hold the row's value now. `write(key, value)` once per key;
  // play mode writes them into settings.ini, keeping its other lines and comments. Null: nothing is saved.
  using Write = void (*)(const char* key, const std::string& value);
  void (*persist)(Write write) = nullptr;
};
void add_menu_entry(const MenuEntry& entry);

// Overlay text: each call appends the lines the module wants on screen now (empty when none).
// Called from whichever thread draws the overlay; a module copies what it shows under its own lock.
struct HudLine { std::string text; uint32_t rgba = 0xFFFFFFFFu; };
using HudFn = void (*)(std::vector<HudLine>& out);
void add_hud(HudFn fn);

// The netplay host's answer to a slippi_command the modules did not take (0xB3, the online match state, carries the
// players' names), on the simulation thread. Read only: the game gets the answer unchanged.
using ReplyFn = void (*)(uint8_t command, const uint8_t* response, uint32_t size);
void add_reply(ReplyFn fn);

// Every slippi_command the netplay host answered (the modules did not take it), with its payload and answer, on the simulation
// thread; `resim`: the game is re-simulating frames for a rollback. Read only (the session trace and the network overlay read
// 0xB0, the online inputs, and 0xB2, a rollback's state load).
using ExchangeFn = void (*)(uint8_t command, const uint8_t* payload, uint32_t payload_size, const uint8_t* response, uint32_t response_size,
                            int32_t result, bool resim);
void add_exchange(ExchangeFn fn);

// ---- what play mode calls ----
void dispatch_tables(MuHostApi* host, MuGameApi* game);
int32_t dispatch_command(uint8_t command, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity,
                         uint32_t* response_size);   // INT32_MIN when no module took it
void dispatch_retrace(const Retrace& info);
bool dispatch_setting(const std::string& key, const std::string& value);
void dispatch_run_end();
void dispatch_reply(uint8_t command, const uint8_t* response, uint32_t size);
void dispatch_exchange(uint8_t command, const uint8_t* payload, uint32_t payload_size, const uint8_t* response, uint32_t response_size,
                       int32_t result, bool resim);
const std::vector<MenuEntry>& menu_entries();
std::vector<HudLine> hud_lines();
// A module asks play mode to start the game again with these arguments added (the replay viewer's "Watch
// it": the game takes a replay only at boot). Play mode polls this after each retrace or when its menu
// closes; true hands the arguments over once.
bool take_restart_request(std::vector<std::string>* extra_args);
// gecko.cpp: the player's Gecko codes (GeckoCodes.ini) as the panel's GECKO CODES page shows them, and a switch (simulation thread).
struct GeckoRow {
  std::string name; bool enabled = false, supported = false; std::string reason;
  std::string key;            // gecko_toggle's argument when not the name (GECKO CODES > Online: "@online", "@tick:<id>", "@answer", "@none")
  bool info = false, action = false;   // a line of text; a button rather than a switch
};
std::vector<GeckoRow> gecko_rows();
std::vector<GeckoRow> gecko_online_rows();   // GECKO CODES > Online (codes agreed with another Melee-NX-NetPlay build, patches/net-0014)
std::string gecko_file();
void gecko_toggle(const std::string& name);
// The replays on the SD card, newest first (file names in the replay folder), for a launcher's list.
std::vector<std::string> replays_list();
// Slippi's game report asked for the replay (the netplay object's reporter thread, any thread): the path of the online game the
// game reported last, once the replay writer has written it (waits up to 15 s); empty when there is none.
std::string online_replay_for_report();

}  // namespace fe::features
