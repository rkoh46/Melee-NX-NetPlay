// Extracted from SlippiMatchmaking's existing create-ticket/get-ticket flow.
// Valid records retain their wire fields; incomplete assignments cannot start a
// peer connection or substitute an invented IP address.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "json_safe.h"
#include "slippi_matchmaking_protocol.h"
#include <charconv>

namespace slippi::protocol {
namespace {
bool number(std::string_view text, unsigned maximum, unsigned& output) {
  if (text.empty()) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && output <= maximum;
}
bool endpoint(std::string_view text) {
  const auto colon = text.find(':');
  if (colon == text.npos) return false;
  unsigned port = 0;
  if (!number(text.substr(colon + 1), 65535, port) || !port) return false;
  auto ip = text.substr(0, colon);
  for (int i = 0; i < 4; ++i) {
    const auto dot = ip.find('.');
    if ((i < 3 && dot == ip.npos) || (i == 3 && dot != ip.npos)) return false;
    unsigned octet = 0;
    if (!number(ip.substr(0, dot), 255, octet)) return false;
    if (i < 3) ip.remove_prefix(dot + 1);
  }
  return true;
}
std::string host_part(const std::string& address) { return address.substr(0, address.find(':')); }
}

bool create_ticket(const UserInfo& identity, const Matchmaking::MatchSearchSettings& search,
                   const std::string& compatibility_version, const std::string& lan_address,
                   nlohmann::json& output) {
  output = nullptr;
  if (identity.uid.empty() || identity.play_key.empty() || compatibility_version.empty() ||
      (!lan_address.empty() && !endpoint(lan_address)) || search.mode < Matchmaking::RANKED || search.mode > Matchmaking::PARTY)
    return false;
  output = {{"type", "create-ticket"},
            {"user", {{"uid", identity.uid}, {"playKey", identity.play_key},
                      {"connectCode", identity.connect_code}, {"displayName", identity.display_name}}},
            {"search", {{"mode", static_cast<int>(search.mode)},
                        {"connectCode", std::vector<uint8_t>(search.connect_code.begin(), search.connect_code.end())}}},
            {"appVersion", compatibility_version}, {"ipAddressLan", lan_address}};
  return true;
}

bool ticket_created(const nlohmann::json& input, std::string& error) {
  error.clear();
  try {
    if (!input.is_object() || jget(input, "type", "") != "create-ticket-resp") {
      error = "Invalid response when joining mm queue"; return false;
    }
    error = jget(input, "error", "");
    return error.empty();
  } catch (const nlohmann::json::exception&) {
    error = "Invalid matchmaking ticket response";
    return false;
  }
}

bool assigned_match(const nlohmann::json& input, Assignment& output, std::string& error, std::string& latest_version) {
  error.clear();
  latest_version.clear();
  Assignment result;
  try {
    if (!input.is_object() || jget(input, "type", "") != "get-ticket-resp") {
      error = "Invalid response when getting mm status"; return false;
    }
    error = jget(input, "error", "");
    latest_version = jget(input, "latestVersion", "");
    if (!error.empty()) return false;
    result.result.id = jget(input, "matchId", "");
    const auto& players = input.at("players");
    if (result.result.id.empty() || !players.is_array() || players.size() < 2 || players.size() > PLAYER_COUNT_MAX)
      throw std::invalid_argument("invalid assignment");
    result.result.players.resize(players.size());
    std::vector<bool> seen(players.size(), false);
    std::string local_external;
    unsigned local_players = 0;
    for (const auto& player : players) {
      UserInfo info;
      info.port = jget(player, "port", 0);
      if (info.port < 1 || size_t(info.port) > players.size() || seen[info.port - 1])
        throw std::invalid_argument("invalid player port");
      seen[info.port - 1] = true;
      info.uid = jget(player, "uid", "");
      info.display_name = jget(player, "displayName", "");
      info.connect_code = jget(player, "connectCode", "");
      info.is_bot = jget(player, "isBot", false);
      if (player.count("chatMessages") && player["chatMessages"].is_array())
        for (const auto& message : player["chatMessages"]) if (message.is_string()) info.chat_messages.push_back(message.get<std::string>());
      if (player.count("rank") && player["rank"].is_object()) {
        const auto& rank = player["rank"];
        info.ranked_rating = jget(rank, "rating", 0.0f);
        info.ranked_update_count = jget(rank, "updateCount", 0);
        info.ranked_global_placement = jget(rank, "globalPlacement", 0);
        info.ranked_regional_placement = jget(rank, "regionalPlacement", 0);
      }
      if (jget(player, "isLocalPlayer", false)) {
        ++local_players;
        result.local_player_index = info.port - 1;
        local_external = jget(player, "ipAddress", "");
        if (!endpoint(local_external)) throw std::invalid_argument("invalid local address");
      }
      result.result.players[info.port - 1] = std::move(info);
    }
    if (local_players != 1) throw std::invalid_argument("missing local player");
    for (size_t port = 1; port <= players.size(); ++port) {
      if (int(port) - 1 == result.local_player_index) continue;
      for (const auto& player : players) {
        if (jget(player, "port", 0) != int(port)) continue;
        const auto external = jget(player, "ipAddress", "");
        const auto lan = jget(player, "ipAddressLan", "");
        const auto chosen = host_part(external) == host_part(local_external) && !lan.empty() ? lan : external;
        if (!endpoint(chosen)) throw std::invalid_argument("invalid peer address");
        result.remote_addresses.push_back(chosen);
      }
    }
    result.is_host = jget(input, "isHost", false);
    if (input.count("stages") && input["stages"].is_array()) {
      for (const auto& stage : input["stages"]) {
        const auto value = stage.get<int>();
        if (value < 0 || value > 65535) throw std::invalid_argument("invalid stage");
        result.result.stages.push_back(static_cast<uint16_t>(value));
      }
    }
    if (result.result.stages.empty()) {
      result.result.stages = {0x3, 0x8, 0x1C, 0x1F, 0x20};
      if (players.size() == 2) result.result.stages.push_back(0x2);
    }
    result.result.items = jget(input, "items", 0u);
    output = std::move(result);
    return true;
  } catch (const nlohmann::json::exception&) {
    error = "Invalid matchmaking assignment";
  } catch (const std::invalid_argument&) {
    error = "Invalid matchmaking assignment";
  }
  return false;
}
}  // namespace slippi::protocol
