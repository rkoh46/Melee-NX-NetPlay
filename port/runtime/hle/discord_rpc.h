// Discord Rich Presence through the Discord desktop app's local IPC socket (macOS).
// The same presence proposed for Slippi's Rust extensions (slippi-rust-extensions PR 36, not merged), done natively and with that PR's Discord application and artwork: the application id is
// built in, so there is nothing to set up and no login inside Dashdance. Menus, queue and opponent come
// from matchmaking; stage, characters, live stocks and set score come from the replay event stream.
// Nothing else is affected when Discord is not running.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace slippi::discord {
constexpr const char* kApplicationId = "1096595344600604772";   // Dashdance's Discord application (from slippi-rust-extensions PR 36)

// `application_id_override` is for testing against another application; empty uses kApplicationId.
void start(bool show_rank, const std::string& application_id_override = "");
void stop();
bool connected();
// The signed-in player: `rank` as the dashboard names it ("Platinum II", "Pending"), `rating` its ordinal.
void set_player(const std::string& name, const std::string& code, const std::string& rank, float rating);
void set_menus();                                                                  // "Slippi Online · In menus"
void set_searching(const std::string& mode);                                       // "In queue - Ranked · Searching for an opponent"
void set_opponent(const std::string& mode, const std::string& opponent, int opponent_rank);   // rank index 1..19, 0 unknown
void set_local_port(int port_index);                                               // 0..3, the local player's port in the match
// Game Start (0x36), Post-Frame Update (0x38) and Game End (0x39) events, command byte first.
void on_replay_event(const uint8_t* data, size_t size);
}  // namespace slippi::discord
