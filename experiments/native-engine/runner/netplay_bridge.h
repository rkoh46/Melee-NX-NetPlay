// The app's Slippi 3.7.0 netplay host code (port/runtime/hle/slippi_online.cpp, slippi_net.cpp)
// behind the native engine's slippi_command interface. The engine's online code (mu_online.c) sends
// the same command bytes and payloads a Slippi game sends to its EXI device, so the app's device
// answers them unchanged. This header has no project types on purpose: the bridge is compiled
// against the app's headers, the rest of the runner against melee-unlocked's.
// Local testing only: two instances peer directly over loopback (the app's "local peer" mode);
// matchmaking, login, reporting and Discord are stubbed out and nothing leaves the machine.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The bridge and the app code behind it are linked as one object whose other symbols are made
// local (build.sh), because the app and melee-unlocked share namespace and function names.
#define NETPLAY_API __attribute__((visibility("default")))

namespace netplay_bridge {
struct Setup {
  int local_index = 0;            // 0 or 1: which of the two players this instance is
  uint16_t local_port = 0;        // UDP port this instance listens on
  std::string remote_ip;          // where the other instance (or a delay relay) is
  uint16_t remote_port = 0;
  std::string user_dir;           // a folder for a made-up user.json (never a real account)
  int delay_frames = 2;           // Slippi input delay
  uint32_t (*retrace_count)() = nullptr;
  void (*log_time)(uint64_t nanoseconds) = nullptr;   // told how long each "[net]" log line took to write
  // Real services (only in a build made with MU_NET_SERVICES=1; refused otherwise): the app's own
  // matchmaking, login file and reporting instead of a direct local peer. `user_dir` is then the
  // folder that already holds the player's user.json, and nothing is written to it.
  bool services = false;
  bool fake_user = false;         // tests against a local mock service only: write a made-up user.json
  std::string iso_path;           // for the game report's disc hash
  bool delay_auto = false;        // the app's automatic delay: 2..5 frames from the opponent's ping at each game's start
  // Slippi's time sync asks for a frame rate against real time (about 0.995..1.01, 1.0 outside a match); called on
  // the game thread when the value changes. The host paces its frames and its sound by it.
  void (*set_speed)(double speed) = nullptr;
  // The replay of the game just reported, for Slippi's upload request (patches/net-0008: asked on the reporter's thread when the
  // service's reply carries uploadUrl; may wait for the host's replay writer). Empty: none. Null: no uploads.
  std::string (*replay_path)() = nullptr;
};
// Choices exchanged with another Melee-NX-NetPlay build (patches/net-0004; against anyone else they change nothing but this
// player's own shield colour on this screen) and the Unranked skip list. From the next connection on.
struct OnlinePrefs {
  bool direct_pause = true;          // Start pauses Direct; false: off when every player's build reads the rule
  int port_colour[2] = {-1, -1};     // online 1v1 port colour, first and second choice (0 red, 1 blue, 2 yellow, 3 green; -1 none)
  uint32_t shield_online = 0;        // 0x1000000 | RGB, or 0
  std::string skip[3];               // Unranked: connect codes turned down
  bool as_slippi = false;            // tests: behave as Slippi Dolphin (no build message sent, the other's ignored)
  bool pick_previews = true;         // live pick previews on the online character select (patches/net-0005; our builds only)
};
NETPLAY_API void set_online_prefs(const OnlinePrefs& prefs);
// Display options bits 28-31 (online port colours) and a slot's online shield colour (0x1000000 | RGB, 0: the port's own);
// both 0 outside an online session.
NETPLAY_API uint32_t port_colour_bits();
NETPLAY_API uint32_t shield_color(int slot);
NETPLAY_API bool start(const Setup& setup, std::string* error);
NETPLAY_API void shutdown();
// One command from the game. False when the app's device does not own the command.
NETPLAY_API bool handle(uint8_t command, const uint8_t* payload, uint32_t size, std::vector<uint8_t>* reply);
// Asks for a match the way the game's online menus would (CMD 0xB5, then 0xB4). Modes are Slippi's:
// 1 Unranked, 2 Direct (with the opponent's connect code), 3 Teams, 4 Party. Ranked (0) is refused
// with the text returned by ranked_refusal(), as melee-unlocked refuses it. False when refused.
NETPLAY_API bool request_match(int mode, const std::string& connect_code, uint8_t character, uint8_t color);
NETPLAY_API const char* ranked_refusal();
NETPLAY_API bool has_services();
NETPLAY_API uint64_t rollbacks_seen_by_device();
// The round trip to the opponent as the app's device last measured it from the game's own pad acknowledgements
// (every acknowledged frame of an online game), in ms; -1 before the first one.
NETPLAY_API int ping_ms();
// The speed Slippi's time sync last asked for (1.0: normal).
NETPLAY_API double emulation_speed();
// Play while you search (patches/net-0005; the frontend's search module): the search as the netplay object sees it, and the kept
// search, which goes on through the game's own cleanup commands while the player plays offline. Simulation thread.
struct SearchInfo {
  int state = -1;                    // 0 idle, 1 initialising, 2 searching, 3 opponent connecting, 4 connected, 5 error; -1 offline
  int mode = -1;                     // 1 Unranked, 2 Direct, 3 Teams, 4 Party
  bool locked = false;               // this player's selections are locked in
  int character = -1, color = 0;     // the locked-in fighter (external id) and costume
  bool kept = false;
  bool remote_ready = false;         // connected and every other player locked in
  uint32_t elapsed_ms = 0;           // since the search started
  std::string error, opponent;
  std::string code;                  // the connect code searched for (Direct, Teams), patches/net-0010
  int remote_character = -1, remote_color = 0;   // the first other player's pick once locked in (external id), patches/net-0010
};
NETPLAY_API SearchInfo search_info();
NETPLAY_API void keep_search(bool kept);
NETPLAY_API bool search_kept();
// Quick chat (0 on, 1 Direct only, 2 off: Slippi's and upstream's quickchat) and live pick previews, from now on.
NETPLAY_API void set_chat(int mode, bool pick_previews);
// This console's player slot in the current online match (0..3).
NETPLAY_API int local_slot();
// Auto L-cancel in Direct (patches/net-0011): this player's offer, sent in the build message from the next connection on, and
// whether a connected Direct match has every player's build offering it (both sides decide from the same exchanged bytes).
NETPLAY_API void set_auto_lcancel(bool offered);
NETPLAY_API bool auto_lcancel_agreed();
NETPLAY_API void end_kept_search(const char* why);
// The last online frame (after its 0xB0): skip 0 none, 1 waiting for the other player's inputs, 2 halted by time sync; advanced
// by time sync; the time sync offset as last measured (us, above 0: this side is ahead).
struct FrameTrace { int skip = 0; bool advanced = false; int32_t offset_us = 0; };
NETPLAY_API FrameTrace frame_trace();
// The input delay from the next game on: `frames` 1..9, or automatic (2..5 from the round trip at each game's start).
NETPLAY_API void set_delay(int frames, bool automatic);
// melee-unlocked's own peer-to-peer match (P2P Direct, its mu_net session; p2p_bridge.cpp), the --p2p-* options of its game
// (melee-unlocked port/app/source_p2p.cpp). net_host answers the game's state saves and loads itself and gives the session
// command bytes to p2p_handle; p2p_exit_request() >= 0 asks for the run to end with that code (the session's game is over, or
// it failed before a match).
struct P2pHooks {
  void (*log)(const char* line) = nullptr;
  void (*set_speed)(double speed) = nullptr;   // the time sync's frame rate, as Setup::set_speed
};
NETPLAY_API bool p2p_option(const std::string& name, const char* value);
NETPLAY_API bool p2p_requested();
NETPLAY_API int p2p_local_slot();
NETPLAY_API int p2p_character();
NETPLAY_API bool p2p_start(const P2pHooks& hooks, std::string* error);
NETPLAY_API bool p2p_handle(uint8_t command, const uint8_t* payload, uint32_t size, std::vector<uint8_t>* reply);
NETPLAY_API int p2p_exit_request();
NETPLAY_API void p2p_report();
NETPLAY_API void p2p_shutdown();
NETPLAY_API void p2p_reset();
}
