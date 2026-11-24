// Slippi spectator server: every match's recording goes out live over ENet (port 51441 by default) the way Slippi
// Dolphin sends it, so the Slippi Launcher's Broadcast tab and stream overlays that read Dolphin (slippi-js
// DolphinConnection) can follow a match played here. settings.ini: slippi_spectator 1 (off by default),
// slippi_spectator_port N.
//
// Adapted from melee-unlocked's port/runtime/hle/slippi_spectate.cpp (GPL-2.0-or-later), itself a port of Slippi
// Dolphin's SlippiSpectateServer (Source/Core/Core/Slippi/SlippiSpectate.cpp, GPL-2.0-or-later): the same messages,
// the same cursors, the same ENet host (4 clients, 2 channels). What differs here: no Windows sockets, no YouGame or
// agent lock, the setting comes from settings.ini, and on the console it listens on every interface (the Launcher
// runs on another computer); off the console on 127.0.0.1, as upstream does.
//
// Feeding: replays.cpp hands the recording stream's live copy (slippi::NativeReplayStream::set_event_sink: the
// command table, then every event as the .slp gets it) to spectator_write between spectator_start_game and
// spectator_end_game. Menu frames are not sent (this engine's host gets no menu stream).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"

#include <enet/enet.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef MU_SLIPPI_VERSION
#define MU_SLIPPI_VERSION "3.7.0"
#endif

namespace fe::features {
namespace {
using json = nlohmann::json;

bool enabled = false;
int port_setting = 51441;

constexpr size_t kMaxClients = 4;   // Dolphin's MAX_CLIENTS
constexpr size_t kChannels = 2;

struct Socket {
  uint64_t cursor = 0, menu_cursor = 0;
  bool shook_hands = false;
  ENetPeer* peer = nullptr;
};

// Simulation thread (and the replay writer) to server thread.
std::mutex queue_mutex;
std::deque<std::string> queue;
// Server thread only.
bool in_game = false;
std::map<uint16_t, std::shared_ptr<Socket>> sockets;
std::string event_concat;
std::vector<std::string> event_buffer;
std::string menu_event;
uint64_t cursor_offset = 0, menu_cursor = 0;

std::thread server;
std::atomic<bool> stop_flag{false}, running{false};
std::atomic<uint16_t> port_in_use{0};
std::atomic<uint64_t> events_in{0}, messages_out{0}, clients_seen{0};

void push(std::string event) {
  if (!running) return;
  std::lock_guard<std::mutex> lock(queue_mutex);
  queue.push_back(std::move(event));
}
bool pop(std::string& event) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queue.empty()) return false;
  event = std::move(queue.front());
  queue.pop_front();
  return true;
}

std::string base64(const std::string& in) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
    out += table[v >> 18 & 63]; out += table[v >> 12 & 63]; out += table[v >> 6 & 63]; out += table[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    out += table[v >> 18 & 63]; out += table[v >> 12 & 63]; out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8;
    out += table[v >> 18 & 63]; out += table[v >> 12 & 63]; out += table[v >> 6 & 63]; out += '=';
  }
  return out;
}

void send(ENetPeer* peer, const std::string& text) {
  ENetPacket* packet = enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE);
  enet_peer_send(peer, 0, packet);
  ++messages_out;
}

// SlippiSpectateServer::writeEvents
void write_events(uint16_t peer_id) {
  Socket& socket = *sockets[peer_id];
  if (!in_game && socket.menu_cursor != menu_cursor) { send(socket.peer, menu_event); socket.menu_cursor = menu_cursor; }
  if (socket.cursor > event_buffer.size()) socket.cursor = 0;   // left over from an old game: start over
  for (uint64_t i = socket.cursor; i < event_buffer.size(); i++) { send(socket.peer, event_buffer[i]); socket.cursor++; }
}

// SlippiSpectateServer::popEvents
void pop_events() {
  std::string event;
  while (pop(event)) {
    json message = json::parse(event, nullptr, false);
    if (!message.is_discarded() && message.is_object() && (message.find("type") != message.end())) {
      if (message["type"] == "end_game") {
        const uint32_t cursor = uint32_t(event_buffer.size() + cursor_offset);
        message["cursor"] = cursor; message["next_cursor"] = cursor + 1;
        menu_cursor = 0;
        event_buffer.push_back(message.dump());
        cursor_offset += event_buffer.size();
        menu_event.clear();
        in_game = false;
        continue;
      }
      if (message["type"] == "start_game") {
        event_buffer.clear();
        const uint32_t cursor = uint32_t(event_buffer.size() + cursor_offset);
        in_game = true;
        message["cursor"] = cursor; message["next_cursor"] = cursor + 1;
        event_buffer.push_back(message.dump());
        continue;
      }
    }
    json game_event;
    if (!in_game) {
      game_event["payload"] = base64(event);
      menu_cursor += 1;
      game_event["type"] = "menu_event";
      menu_event = game_event.dump();
      continue;
    }
    const uint8_t command = uint8_t(event[0]);
    event_concat += event;
    // GAME_INIT, FRAME_END, GAME_END and SPLIT_MESSAGE close one game_event.
    if (command == 0x36 || command == 0x3C || command == 0x39 || command == 0x10) {
      const uint32_t cursor = uint32_t(event_buffer.size() + cursor_offset);
      game_event["payload"] = base64(event_concat);
      game_event["type"] = "game_event";
      game_event["cursor"] = cursor; game_event["next_cursor"] = cursor + 1;
      event_buffer.push_back(game_event.dump());
      event_concat.clear();
    }
  }
}

// SlippiSpectateServer::handleMessage
void handle_message(const uint8_t* buffer, size_t length, uint16_t peer_id) {
  json message = json::parse(std::string(reinterpret_cast<const char*>(buffer), length), nullptr, false);
  if (message.is_discarded() || !message.is_object() || !(message.find("type") != message.end()) || !message["type"].is_string()) return;
  if (message["type"] != "connect_request") return;
  if (!(message.find("cursor") != message.end()) || !message["cursor"].is_number_integer()) return;
  Socket& socket = *sockets[peer_id];
  const uint32_t requested = message["cursor"];
  if (requested >= cursor_offset) socket.cursor = requested > event_buffer.size() + cursor_offset ? 0 : requested - cursor_offset;
  else socket.cursor = 0;   // too low: bring the client up to the present
  const uint32_t sent_cursor = uint32_t(socket.cursor) + uint32_t(cursor_offset);
  if (!in_game) socket.cursor = event_buffer.size();   // a client that joins at the menu is not caught up
  json reply;
  reply["type"] = "connect_reply";
  reply["nick"] = "Slippi Online";
  reply["version"] = MU_SLIPPI_VERSION;
  reply["cursor"] = sent_cursor;
  send(socket.peer, reply.dump());
  socket.shook_hands = true;
}

// SlippiSpectateServer::SlippicommSocketThread
void server_thread(uint16_t port) {
  if (enet_initialize() != 0) { say("spectator: ENet did not start; no spectator server"); running = false; return; }
  ENetAddress address{};
#ifdef __SWITCH__
  address.host = ENET_HOST_ANY;                      // the Launcher or overlay runs on another computer
#else
  address.host = ENET_HOST_TO_NET_32(0x7F000001u);   // 127.0.0.1, as upstream
#endif
  address.port = port;
  ENetHost* host = enet_host_create(&address, kMaxClients, kChannels, 0, 0);
  for (int tries = 0; host == nullptr && tries < 20 && !stop_flag; ++tries) {   // the port may still be held: retry as Dolphin does
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    host = enet_host_create(&address, kMaxClients, kChannels, 0, 0);
  }
  if (!host) { say("spectator: could not open UDP port %u; no spectator server", unsigned(port)); running = false; enet_deinitialize(); return; }
  port_in_use = port;
  say("spectator: serving every match on %s port %u (Slippi Launcher Broadcast, slippi-js)",
#ifdef __SWITCH__
      "every interface,",
#else
      "127.0.0.1",
#endif
      unsigned(port));
  while (!stop_flag) {
    pop_events();
    for (auto& entry : sockets) if (entry.second->shook_hands) write_events(entry.first);
    ENetEvent event;
    while (enet_host_service(host, &event, 1) > 0) {
      switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
          auto socket = std::make_shared<Socket>();
          socket->peer = event.peer;
          sockets[event.peer->incomingPeerID] = socket;
          ++clients_seen;
          break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
          handle_message(event.packet->data, event.packet->dataLength, event.peer->incomingPeerID);
          enet_packet_destroy(event.packet);
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          sockets.erase(event.peer->incomingPeerID);
          event.peer->data = nullptr;
          break;
        default: break;
      }
    }
  }
  // The closing end_game goes out before the host closes.
  pop_events();
  for (auto& entry : sockets) if (entry.second->shook_hands) write_events(entry.first);
  enet_host_flush(host);
  sockets.clear();
  enet_host_destroy(host);
  enet_deinitialize();
  port_in_use = 0;
}

bool setting(const std::string& key, const std::string& value) {
  if (key == "slippi_spectator") enabled = value != "0";
  else if (key == "slippi_spectator_port") port_setting = int(std::strtol(value.c_str(), nullptr, 10));
  else return false;
  return true;
}

void tables(MuHostApi*, MuGameApi*) {
  if (!enabled || running || server.joinable()) return;
  if (port_setting < 1 || port_setting > 65535) { say("spectator: port %d is not valid; no spectator server", port_setting); return; }
  in_game = false; menu_cursor = 0; menu_event.clear(); cursor_offset = 0; event_buffer.clear(); event_concat.clear();
  stop_flag = false; running = true;
  server = std::thread(server_thread, uint16_t(port_setting));
}

void run_end() {
  if (!server.joinable()) return;
  spectator_end_game(true);           // {"type":"end_game","dolphin_closed":true}, as Dolphin's EXI device sends when it closes
  stop_flag = true;
  server.join();
  running = false;
  say("spectator: %llu events streamed, %llu messages sent, %llu clients connected in all", (unsigned long long)events_in.load(),
      (unsigned long long)messages_out.load(), (unsigned long long)clients_seen.load());
}
}  // namespace

bool spectator_wanted() { return enabled; }
bool spectator_on() { return running.load(); }
void spectator_write(const uint8_t* data, uint32_t size) {
  if (!data || !size || !running) return;
  ++events_in;
  push(std::string(reinterpret_cast<const char*>(data), size));
}
void spectator_start_game() { if (running) push(json{{"type", "start_game"}}.dump()); }
void spectator_end_game(bool dolphin_closed) {
  if (!running) return;
  json message;
  message["type"] = "end_game";
  message["dolphin_closed"] = dolphin_closed;
  push(message.dump());
}

void register_spectator() {
  add_setting(setting);
  add_tables(tables);
  add_run_end(run_end);
  add_menu_entry({"Replays", "Spectator server (next start)", [] { return std::string(enabled ? "On" : "Off"); }, [](int) { enabled = !enabled; }, nullptr,
                  [](MenuEntry::Write w) { w("slippi_spectator", enabled ? "1" : "0"); }});
}
}  // namespace fe::features
