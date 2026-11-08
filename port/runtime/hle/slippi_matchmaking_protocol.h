// Pure serialization/validation for Unlocked's existing matchmaking protocol.
// No profile, DNS, socket, thread or HTTP calls are made by these functions.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "slippi_net.h"
#include <nlohmann/json.hpp>

namespace slippi::protocol {
struct Assignment {
  bool is_host = false;
  int local_player_index = 0;
  std::vector<std::string> remote_addresses;
  Matchmaking::MatchmakeResult result;
};

bool create_ticket(const UserInfo& identity, const Matchmaking::MatchSearchSettings& search,
                   const std::string& compatibility_version, const std::string& lan_address,
                   nlohmann::json& output);
bool ticket_created(const nlohmann::json& input, std::string& error);
bool assigned_match(const nlohmann::json& input, Assignment& output, std::string& error,
                    std::string& latest_version);
}  // namespace slippi::protocol
