// A small Slippi spectator client (what slippi-js's DolphinConnection does): connects over ENet, asks for the stream
// from cursor 0, and writes each game's events (the base64 payloads of its game_event messages, in cursor order) to
// OUT_DIR/game-N.bin. The bytes must be the .slp's raw element (tools/spectator_check.py compares them).
//   spectator_client HOST PORT OUT_DIR SECONDS
// Ends at {"type":"end_game","dolphin_closed":true} (the server closing) or after SECONDS. Exit 0 when at least one
// game was received whole (start_game ... end_game, cursors without gaps).
// Build: g++ -std=gnu++17 -I port/third_party/enet/include -I port/third_party spectator_client.cpp port/third_party/enet/*.c
// (without win32.c), with ENet's HAS_* definitions (tools/spectator_test.sh does it in a container).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <enet/enet.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using json = nlohmann::json;

static std::string unbase64(const std::string& in) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  };
  std::string out;
  int bits = 0; uint32_t acc = 0;
  for (char c : in) {
    const int v = value(c);
    if (v < 0) continue;
    acc = acc << 6 | uint32_t(v); bits += 6;
    if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xFF); }
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: spectator_client HOST PORT OUT_DIR SECONDS\n"); return 2; }
  const std::string out_dir = argv[3];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(argv[4]));
  if (enet_initialize() != 0) return 2;
  ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
  ENetAddress address{};
  enet_address_set_host(&address, argv[1]);
  address.port = uint16_t(std::atoi(argv[2]));
  ENetPeer* peer = nullptr;
  bool connected = false, closed = false, in_game = false;
  int games = 0, whole = 0, connects = 0;
  int64_t expected_cursor = -1;
  bool gap = false;
  std::string game;
  uint64_t messages = 0;
  auto last_try = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    if (!connected && std::chrono::steady_clock::now() - last_try > std::chrono::seconds(2)) {   // the server may not be up yet
      if (peer) enet_peer_reset(peer);
      peer = enet_host_connect(client, &address, 2, 0);
      last_try = std::chrono::steady_clock::now();
    }
    ENetEvent event;
    while (enet_host_service(client, &event, 20) > 0) {
      if (event.type == ENET_EVENT_TYPE_CONNECT) {
        connected = true; ++connects;
        const std::string request = json{{"type", "connect_request"}, {"cursor", 0}}.dump();
        enet_peer_send(peer, 0, enet_packet_create(request.data(), request.size(), ENET_PACKET_FLAG_RELIABLE));
        std::printf("spectator client: connected, asked for cursor 0\n");
      } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        connected = false; peer = nullptr;
        std::printf("spectator client: disconnected\n");
      } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        ++messages;
        json m = json::parse(std::string(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength), nullptr, false);
        enet_packet_destroy(event.packet);
        if (m.is_discarded() || !m.is_object()) { std::printf("spectator client: a message that is not JSON\n"); continue; }
        const std::string type = m.value("type", "");
        const int64_t cursor = m.find("cursor") != m.end() && m["cursor"].is_number() ? m["cursor"].get<int64_t>() : -1;
        if (type == "connect_reply") { std::printf("spectator client: connect_reply nick %s version %s cursor %lld\n", m.value("nick", "").c_str(), m.value("version", "").c_str(), (long long)cursor); continue; }
        if (cursor >= 0 && type != "menu_event") {
          if (expected_cursor >= 0 && cursor != expected_cursor) gap = true;
          expected_cursor = m.find("next_cursor") != m.end() ? m["next_cursor"].get<int64_t>() : cursor + 1;
        }
        if (type == "start_game") { in_game = true; game.clear(); gap = false; std::printf("spectator client: start_game (cursor %lld)\n", (long long)cursor); }
        else if (type == "game_event") { if (in_game) game += unbase64(m.value("payload", "")); }
        else if (type == "end_game") {
          const bool dolphin_closed = m.value("dolphin_closed", false);
          if (in_game) {
            ++games;
            const std::string path = out_dir + "/game-" + std::to_string(games) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(game.data(), 1, game.size(), f); std::fclose(f); }
            std::printf("spectator client: end_game: game %d, %zu bytes of events%s -> %s\n", games, game.size(), gap ? ", CURSOR GAP" : "", path.c_str());
            if (!gap) ++whole;
          }
          in_game = false;
          if (dolphin_closed) { std::printf("spectator client: the server closed (dolphin_closed)\n"); closed = true; }
        }
      }
    }
  }
  std::printf("spectator client: %d connects, %llu messages, %d games received, %d whole\n", connects, (unsigned long long)messages, games, whole);
  enet_host_destroy(client);
  enet_deinitialize();
  return whole > 0 ? 0 : 1;
}
