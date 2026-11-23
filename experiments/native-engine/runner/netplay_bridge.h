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
};
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
}
