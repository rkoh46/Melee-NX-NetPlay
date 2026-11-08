// No Discord desktop app on the Switch: the presence interface is inert.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "discord_rpc.h"
namespace slippi::discord {
void start(bool, const std::string&) {}
void stop() {}
bool connected() { return false; }
void set_player(const std::string&, const std::string&, const std::string&, float) {}
void set_menus() {}
void set_searching(const std::string&) {}
void set_opponent(const std::string&, const std::string&, int) {}
void set_local_port(int) {}
void on_replay_event(const uint8_t*, size_t) {}
}  // namespace slippi::discord
