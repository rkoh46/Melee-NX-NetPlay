// Slippi Online networking (see slippi_net.h). Wire-compatible port of Dolphin's
// SlippiNetplayClient, SlippiMatchmaking and the user record.
// SPDX-License-Identifier: GPL-2.0-or-later
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include "slippi_net.h"
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#include "host.h"
#include "slippi_net_diag.h"
#include "slippi_version.h"
#include "slippi_matchmaking_ws.h"
#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <enet/enet.h>
#include <nlohmann/json.hpp>
#include "json_safe.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

using json = nlohmann::json;

#ifdef __APPLE__
#include <sys/socket.h>
#endif
namespace {
// Apple Wi-Fi and cellular stacks map the socket's service type to a traffic class; voice
// (NET_SERVICE_TYPE_VO) gets the lowest-latency queue, which is what rollback input packets want.
void prioritize_socket(ENetSocket socket) {
#ifdef __APPLE__
  int service = NET_SERVICE_TYPE_VO;
  setsockopt(socket, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service, sizeof service);
#else
  (void)socket;
#endif
}
}  // namespace
namespace slippi {

uint64_t time_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t time_ms() { return time_us() / 1000; }

// ---------------------------------------------------------------- ENet
static bool g_enet_ready = false;
bool enet_ready() {
  if (!g_enet_ready) { if (enet_initialize() < 0) { host::log("slippi: enet_initialize failed"); return false; } g_enet_ready = true; }
  return true;
}
static void wakeup_thread(ENetHost* host) {
  if (!host) return;
  ENetAddress address;
  if (host->address.port != 0) address.port = host->address.port;
  else enet_socket_get_address(host->socket, &address);
  address.host = 0x0100007f;
  uint8_t byte = 0;
  ENetBuffer buf; buf.data = &byte; buf.dataLength = 1;
  enet_socket_send(host->socket, &address, &buf, 1);
}
// MELEE_NET_DROP_CONNECT=1: test aid for one-way NATs. Incoming ENet CONNECT commands are discarded, so the opponent's
// outgoing attempt never completes and only this side's outgoing connection forms (docs/MAC_FIXES.md issue 9).
static int ENET_CALLBACK drop_connect_callback(ENetHost* host, ENetEvent*) {
  const enet_uint8* d = host->receivedData; const size_t n = host->receivedDataLength;
  if (n < 6) return 0;
  const enet_uint16 peer_id = (enet_uint16)(d[0] << 8 | d[1]);
  const size_t header = (peer_id & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? 4 : 2;
  return (d[header] & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_CONNECT ? 1 : 0;   // 1 = swallow the datagram
}
// Logs each ENet CONNECT command that reaches this socket, before ENet decides whether to accept it (it drops refused
// connects silently). Says whether the opponent's own connection attempts arrive at all when a Direct game drops ~30 s
// in with only our outgoing connection formed (docs/MAC_FIXES.md issue 9).
static void note_incoming_connect(ENetHost* host) {
  const enet_uint8* d = host->receivedData; const size_t n = host->receivedDataLength;
  if (n < 6) return;
  const enet_uint16 peer_id = (enet_uint16)(d[0] << 8 | d[1]);
  const size_t header = (peer_id & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? 4 : 2;
  if (n <= header || (d[header] & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_CONNECT) return;
  static int seen = 0;
  if (++seen <= 20)
    host::log("slippi: incoming ENet connect from %x:%u on local port %u (%zu bytes, %d so far)", host->receivedAddress.host,
              host->receivedAddress.port, host->address.port, n, seen);
}
static int ENET_CALLBACK intercept_callback(ENetHost* host, ENetEvent* event) {
  if (host->receivedDataLength == 1 && host->receivedData[0] == 0) { event->type = (ENetEventType)42; return 1; }
  note_incoming_connect(host);
  static const bool drop_connects = std::getenv("MELEE_NET_DROP_CONNECT") != nullptr;
  return drop_connects ? drop_connect_callback(host, event) : 0;
}
// One line per ENet peer event. Every mid-game drop seen so far came 30 to 32 s after connecting, which is ENet's cap
// for a peer whose reliable commands were never acknowledged; these lines say which peer that was (the outgoing
// attempt or the opponent's incoming connection), whether it ever connected, and whether the opponent asked for it.
static void log_peer(const char* what, ENetHost* host, ENetPeer* p, bool in_use, unsigned data) {
  host::log("slippi: peer %s: slot %d %x:%u state %d ids in %u out %u connect %08x rtt %u ms lost %u last-recv %u ms ago data %u%s",
            what, (int)(p - host->peers), p->address.host, p->address.port, (int)p->state, p->incomingPeerID, p->outgoingPeerID, p->connectID,
            p->roundTripTime, p->packetsLost, p->lastReceiveTime ? host->serviceTime - p->lastReceiveTime : 0, data, in_use ? " (the connection in use)" : "");
}
static std::string peer_key(ENetPeer* p) { std::stringstream s; s << p->address.host << "-" << p->address.port; return s.str(); }

// ---------------------------------------------------------------- Packet
void Packet::append(const void* data, size_t size) { const uint8_t* p = (const uint8_t*)data; buf_.insert(buf_.end(), p, p + size); }
Packet& Packet::operator<<(uint8_t v) { buf_.push_back(v); return *this; }
Packet& Packet::operator<<(uint16_t v) { buf_.push_back((uint8_t)(v >> 8)); buf_.push_back((uint8_t)v); return *this; }
Packet& Packet::operator<<(uint32_t v) { for (int i = 3; i >= 0; --i) buf_.push_back((uint8_t)(v >> (8 * i))); return *this; }
Packet& Packet::operator<<(const std::string& s) { *this << (uint32_t)s.size(); append(s.data(), s.size()); return *this; }
bool Packet::check(size_t n) { if (!ok_ || pos_ + n > buf_.size()) { ok_ = false; return false; } return true; }
Packet& Packet::operator>>(uint8_t& v) { if (check(1)) v = buf_[pos_++]; return *this; }
Packet& Packet::operator>>(bool& v) { uint8_t b = 0; *this >> b; v = b != 0; return *this; }
Packet& Packet::operator>>(uint16_t& v) { if (check(2)) { v = (uint16_t)((buf_[pos_] << 8) | buf_[pos_ + 1]); pos_ += 2; } return *this; }
Packet& Packet::operator>>(uint32_t& v) {
  if (check(4)) { v = ((uint32_t)buf_[pos_] << 24) | ((uint32_t)buf_[pos_ + 1] << 16) | ((uint32_t)buf_[pos_ + 2] << 8) | buf_[pos_ + 3]; pos_ += 4; }
  return *this;
}
Packet& Packet::operator>>(int32_t& v) { uint32_t u = 0; *this >> u; v = (int32_t)u; return *this; }
Packet& Packet::operator>>(std::string& s) {
  uint32_t n = 0; *this >> n;
  if (check(n)) { s.assign((const char*)&buf_[pos_], n); pos_ += n; }
  return *this;
}

// ---------------------------------------------------------------- selections
void PlayerSelections::Merge(const PlayerSelections& s) {
  rng_offset = s.rng_offset;
  if (s.is_stage_selected) { stage_id = s.stage_id; is_stage_selected = true; alt_stage_mode = s.alt_stage_mode; }
  if (s.is_character_selected) { character_id = s.character_id; character_color = s.character_color; team_id = s.team_id; is_character_selected = true; }
}
void PlayerSelections::Reset() { character_id = character_color = team_id = 0; is_character_selected = false; stage_id = 0; is_stage_selected = false; rng_offset = 0; }

// ---------------------------------------------------------------- user
User::User(std::string user_dir) : dir_(std::move(user_dir)) { AttemptLogin(); }
bool User::AttemptLogin() {
  std::ifstream f(dir_ + "/user.json");
  if (!f) { logged_in_ = false; return false; }
  try {
    json j = json::parse(f);
    info_.uid = jget(j, "uid", "");
    info_.play_key = jget(j, "playKey", "");
    info_.display_name = jget(j, "displayName", "");
    info_.connect_code = jget(j, "connectCode", "");
    info_.latest_version = jget(j, "latestVersion", "");
    info_.chat_messages.clear();
    if (j.count("chatMessages") && j["chatMessages"].is_array()) for (auto& m : j["chatMessages"]) info_.chat_messages.push_back(m.get<std::string>());
    if (info_.chat_messages.size() != 16) info_.chat_messages = GetDefaultChatMessages();
    bool was = logged_in_;
    logged_in_ = !info_.uid.empty() && !info_.play_key.empty();
    if (logged_in_ && !was) host::log("slippi: logged in as %s (%s)", info_.display_name.c_str(), info_.connect_code.c_str());
  } catch (const std::exception& e) {
    host::log("slippi: cannot parse %s/user.json: %s", dir_.c_str(), e.what());
    logged_in_ = false;
  }
  return logged_in_;
}
void User::LogOut() { logged_in_ = false; info_ = UserInfo(); host::log("slippi: logged out (user.json left in place)"); }
std::vector<std::string> User::GetUserChatMessages() const { return info_.chat_messages.size() == 16 ? info_.chat_messages : GetDefaultChatMessages(); }
std::vector<std::string> User::GetDefaultChatMessages() {
  return {"ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
          "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection"};
}

DirectCodes::DirectCodes(std::string path) : path_(std::move(path)) { Load(); }
void DirectCodes::Load() {
  codes_.clear();
  std::ifstream f(path_);
  if (!f) return;
  try {
    json j = json::parse(f);
    if (j.is_array()) for (auto& e : j) { std::string c = e.is_object() ? jget(e, "connectCode", "") : e.is_string() ? e.get<std::string>() : ""; if (!c.empty()) codes_.push_back(c); }
  } catch (...) {}
}
void DirectCodes::Save() {
  json j = json::array();
  for (auto& c : codes_) j.push_back({{"connectCode", c}, {"lastPlayed", ""}});
  std::ofstream f(path_);
  if (f) f << j.dump(2);
}
std::string DirectCodes::get(int index) const { return index >= 0 && index < (int)codes_.size() ? codes_[index] : "1"; }
void DirectCodes::AddOrUpdateCode(const std::string& code) {
  codes_.erase(std::remove(codes_.begin(), codes_.end(), code), codes_.end());
  codes_.insert(codes_.begin(), code);
  Save();
}

// ---------------------------------------------------------------- netplay client
NetplayClient::NetplayClient(std::vector<std::string> addrs, std::vector<uint16_t> ports, uint8_t remote_player_count, uint16_t local_port,
                             bool is_decider, uint8_t player_idx, const std::string& bind_address) {
  host::log("slippi: netplay client: local port %u, decider %d, player index %u, %u remote players", local_port, is_decider, player_idx, remote_player_count);
  if (remote_player_count == 0 || remote_player_count > REMOTE_PLAYER_MAX ||
      player_idx >= PLAYER_COUNT_MAX || addrs.size() != remote_player_count || ports.size() != remote_player_count) {
    host::log("slippi: invalid netplay peer configuration");
    status_.store(ConnectStatus::FAILED);
    return;
  }
  is_decider_ = is_decider;
  remote_player_count_ = remote_player_count;
  player_idx_ = player_idx;
  for (int i = 0, j = 0; i < REMOTE_PLAYER_MAX; ++i, ++j) {
    if (j == player_idx) ++j;
    match_info_.remote[i] = PlayerSelections();
    match_info_.remote[i].player_idx = (uint8_t)j;
    last_frame_acked_[i] = 0;
  }
  ENetAddress local_addr{};
  ENetAddress* local = nullptr;
  if (local_port > 0 || !bind_address.empty()) {
    local_addr.host = ENET_HOST_ANY;
    local_addr.port = local_port;
    // A numeric local bind address supports confined loopback validation without
    // DNS, a launcher profile, a matchmaker or a production-facing listener.
    if (!bind_address.empty()) {
      in_addr bind_ip{};
      if (inet_pton(AF_INET, bind_address.c_str(), &bind_ip) != 1) {
        host::log("slippi: invalid local bind address");
        status_.store(ConnectStatus::FAILED);
        return;
      }
      local_addr.host = bind_ip.s_addr;
    }
    local = &local_addr;
  }
  client_ = enet_host_create(local, 10, 3, 0, 0);
  if (client_ && std::getenv("MELEE_NET_DROP_CONNECT")) { client_->intercept = drop_connect_callback; host::log("slippi: test aid: incoming ENet connects are dropped"); }
  else if (client_) client_->intercept = intercept_callback;   // from the start, so incoming connects are logged while connecting
  if (!client_) { host::log("slippi: cannot create ENet client"); status_.store(ConnectStatus::FAILED); return; }
  prioritize_socket(client_->socket);
  for (int i = 0; i < remote_player_count; ++i) {
    ENetAddress addr{};
    in_addr peer_ip{};
    const bool numeric = inet_pton(AF_INET, addrs[i].c_str(), &peer_ip) == 1;
    if (numeric) addr.host = peer_ip.s_addr;
    else if (enet_address_set_host(&addr, addrs[i].c_str()) < 0) {
      host::log("slippi: cannot resolve configured peer address");
      status_.store(ConnectStatus::FAILED);
      return;
    }
    addr.port = ports[i];
    ENetPeer* peer = enet_host_connect(client_, &addr, 3, 0);
    if (!peer) { host::log("slippi: cannot create configured peer"); status_.store(ConnectStatus::FAILED); return; }
    server_.push_back(peer);
    ActiveConnectionInfo info;
    info.player_idx = match_info_.remote[i].player_idx;
    active_connections_[peer_key(peer)][peer] = info;
    player_active_[info.player_idx].store(true, std::memory_order_release);
  }
  status_.store(ConnectStatus::INITIATED, std::memory_order_release);
  waker_ = std::thread(&NetplayClient::WakerFunc, this);
  thread_ = std::thread(&NetplayClient::ThreadFunc, this);
}

// The connection's round trip from ENet's raw acknowledgement samples of the first opponent (any peer
// at that address: the two sides' connects can both succeed), once there are at least 2. Netplay thread.
void NetplayClient::UpdateConnectRoundTrip() {
  if (!client_ || server_.empty() || rtt_ms_.load(std::memory_order_relaxed) != 0) return;
  uint64_t sum = 0, count = 0;
  for (size_t i = 0; i < client_->peerCount; ++i) {
    const ENetPeer* p = &client_->peers[i];
    if (p->state == ENET_PEER_STATE_DISCONNECTED || p->address.host != server_[0]->address.host) continue;
    sum += p->rawRoundTripSum;
    count += p->rawRoundTripCount;
  }
  if (count < 2) return;
  const unsigned ms = (unsigned)((sum + count / 2) / count);
  const unsigned before = connect_rtt_ms_.exchange(ms ? ms : 1, std::memory_order_relaxed);
  if (before == 0) host::log("slippi: connection round trip %u ms (%llu samples)", ms, (unsigned long long)count);
}

NetplayClient::~NetplayClient() {
  do_loop_.store(false);
  if (client_) wakeup_thread(client_);
  if (thread_.joinable()) thread_.join();
  { std::lock_guard<std::mutex> lk(async_mutex_); waker_stop_ = true; }
  waker_cv_.notify_one();
  if (waker_.joinable()) waker_.join();
  if (!server_.empty()) Disconnect();
  if (client_) { enet_host_destroy(client_); client_ = nullptr; }
  host::log("slippi: netplay client cleanup complete");
}

void NetplayClient::OnData(Packet& packet, ENetPeer* peer) {
  uint8_t mid = 0;
  if (!(packet >> mid)) { host::log("slippi: empty netplay packet"); return; }
  switch (mid) {
    case NP_MSG_SLIPPI_PAD: {
      uint64_t cur_time = time_us();
      int32_t frame, checksum_frame; uint32_t checksum; uint8_t packet_player_port;
      if (!(packet >> frame) || !(packet >> packet_player_port) || !(packet >> checksum_frame) || !(packet >> checksum)) { host::log("slippi: pad packet too small"); break; }
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) { host::log("slippi: pad packet with invalid player idx %u", pidx); break; }
      // We have told the other players which inputs we have for this player, so that cannot change any more.
      if (!player_active_[packet_player_port].load(std::memory_order_acquire)) break;
      const int pad_data_offset = 14;
      int conn_idx = 0;
      for (int i = 0; i < (int)server_.size(); ++i)
        if (peer->address.host == server_[i]->address.host && peer->address.port == server_[i]->address.port) { conn_idx = i; break; }
      std::string key = peer_key(peer);
      int live = 0; bool current_active = false;
      for (auto& c : active_connections_[key]) { if (c.second.is_disconnected) continue; if (c.first == peer) current_active = true; ++live; }
      if (current_active && live > 1 && player_idx_ < packet_player_port) {
        server_[conn_idx] = peer;
        for (auto& c : active_connections_[key]) {
          if (c.first == peer || c.second.is_disconnected) continue;
          log_peer("closing the duplicate of the connection in use", client_, c.first, false, 0);
          enet_peer_disconnect(c.first, 0);
          c.second.is_disconnected = true;
        }
      }
      FrameTiming timing = last_frame_timing_[pidx];
      if (!has_game_started_) { timing.frame = 0; timing.time_us = cur_time; }
      int64_t opponent_send_time_us = (int64_t)cur_time - (int64_t)(ping_us_[pidx].load(std::memory_order_relaxed) / 2);
      int64_t frame_diff_offset_us = 16683 * (int64_t)(timing.frame - frame);
      int64_t time_offset_us = opponent_send_time_us - (int64_t)timing.time_us + frame_diff_offset_us;
      auto& fod = frame_offset_data_[pidx];
      if ((int)fod.buf.size() < ONLINE_LOCKSTEP_INTERVAL) fod.buf.push_back((int32_t)time_offset_us);
      else fod.buf[fod.idx] = (int32_t)time_offset_us;
      fod.idx = (fod.idx + 1) % ONLINE_LOCKSTEP_INTERVAL;
      int64_t inputs_to_copy;
      {
        std::lock_guard<std::mutex> lk(pad_mutex_);
        const uint8_t* data = packet.data();
        int32_t head_frame = remote_pad_queue_[pidx].empty() ? 0 : remote_pad_queue_[pidx].front()->frame;
        inputs_to_copy = (int64_t)frame - head_frame;
        if (pad_data_offset + inputs_to_copy * PAD_WIRE_SIZE > (int64_t)packet.size()) { host::log("slippi: pad packet too small for %lld inputs", (long long)inputs_to_copy); break; }
        if (inputs_to_copy > 128) { host::log("slippi: pad packet with too many frames (%lld)", (long long)inputs_to_copy); break; }
        for (int64_t i = inputs_to_copy - 1; i >= 0; --i)
          AddRemotePad(pidx, (int32_t)(frame - i), &data[pad_data_offset + i * PAD_WIRE_SIZE]);
        remote_checksums_[pidx] = {checksum_frame, checksum};
      }
      if (inputs_to_copy > 0) {
        Packet ack;
        ack << (uint8_t)NP_MSG_SLIPPI_PAD_ACK << frame << player_idx_;
        ENetPacket* epac = enet_packet_create(ack.data(), ack.size(), ENET_PACKET_FLAG_UNSEQUENCED);
        enet_peer_send(peer, 2, epac);
        net_diag::on_pad_received(time_us(), (int)inputs_to_copy);
      }
      break;
    }
    case NP_MSG_SLIPPI_PAD_ACK: {
      std::lock_guard<std::mutex> lk(ack_mutex_);
      int32_t frame; uint8_t packet_player_port;
      if (!(packet >> frame) || !(packet >> packet_player_port)) { host::log("slippi: ack packet too small"); break; }
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) break;
      last_frame_acked_[pidx] = std::max(last_frame_acked_[pidx], frame);
      auto& timers = ack_timers_[pidx];
      while (!timers.empty() && timers.front().frame < frame) timers.pop_front();
      if (timers.empty() || timers.front().frame != frame) break;
      uint64_t send_time = timers.front().time_us;
      timers.pop_front();
      const uint64_t ping_us = time_us() - send_time;
      ping_us_[pidx].store(ping_us, std::memory_order_relaxed);
      net_diag::on_ack(ping_us);
      if (pidx == 0) host::set_online_ping_ms((int)(ping_us / 1000));
      if (pidx == 0) {
        // The automatic input delay's round trip: the game's own pad acknowledgements, averaged
        // (1/16 per sample). ENet's roundTripTime is useless for this: it starts at 500 ms and
        // each sample moves it an eighth of the way, so right after connecting it read 444 ms for
        // a 52 ms opponent and picked 5 frames of delay. Unknown (0) until a game has run.
        const unsigned sample = (unsigned)(ping_us / 1000);
        const unsigned cur = rtt_ms_.load(std::memory_order_relaxed);
        // An acknowledgement after a stall (the opponent paused or left) is not a round trip: one game
        // ended with 3187 ms and the next game against that opponent got 5 frames of delay.
        if (sample < 1000)
        rtt_ms_.store(cur == 0 ? (sample ? sample : 1) : (cur * 15 + sample + 8) / 16, std::memory_order_relaxed);
      }
      if (frame % 600 == 0 && pidx == 0) host::log("slippi: ping %llu ms", (unsigned long long)(ping_us / 1000));
      break;
    }
    case NP_MSG_SLIPPI_PAD_SUGGESTION: {
      // Slippi 3.7 input suggestions: the opponent's newest input between frames, used only to predict.
      // This port neither sends nor uses them (the game predicts with the latest real input, as before
      // 3.7); every prediction is checked against the real input either way, so ignoring them is safe.
      uint8_t packet_player_port; int32_t target_frame; uint32_t seq;
      if (!(packet >> packet_player_port) || !(packet >> target_frame) || !(packet >> seq) || packet.size() < 10 + (size_t)PAD_INPUT_SIZE) {
        static int warned = 0; if (++warned <= 3) host::log("slippi: input suggestion packet too small");
      }
      break;
    }
    case NP_MSG_SLIPPI_PLAYER_DROP: {
      uint8_t sender_port, dropped_port, input_count; int32_t last_frame;
      if (!(packet >> sender_port) || !(packet >> dropped_port) || !(packet >> last_frame) || !(packet >> input_count)) { host::log("slippi: player drop packet too small to read header"); break; }
      const size_t inputs_offset = 8;   // message id, ports, last frame and input count
      if (packet.size() < inputs_offset + (size_t)input_count * PAD_WIRE_SIZE) { host::log("slippi: player drop packet too small to read inputs"); break; }
      if (sender_port == player_idx_ || sender_port == dropped_port || last_frame < 0 || sender_port >= PLAYER_COUNT_MAX || dropped_port >= PLAYER_COUNT_MAX ||
          PlayerIdxFromPort(sender_port) >= remote_player_count_ || (dropped_port != player_idx_ && PlayerIdxFromPort(dropped_port) >= remote_player_count_)) {
        host::log("slippi: invalid player drop packet (sender %u, dropped %u, last frame %d)", sender_port, dropped_port, last_frame);
        break;
      }
      // The other players take care of removing us if this is about us. A player we already lost has no say.
      if (dropped_port == player_idx_ || !player_active_[sender_port].load(std::memory_order_acquire)) break;
      // Losing a player to anyone means losing them to everyone; this also reports what we have for them.
      ForceDisconnectPlayer(dropped_port);
      ProcessPlayerDrops();
      const int32_t despawn_frame = despawn_frames_[dropped_port].load(std::memory_order_acquire);
      if (despawn_frame != 0) {
        if (last_frame + DROP_DESPAWN_FRAMES > despawn_frame)
          host::log("slippi: report about player %u came too late (last frame %d, despawn frame %d)", dropped_port, last_frame, despawn_frame);
        break;
      }
      AddPlayerDropReportInputs(PlayerIdxFromPort(dropped_port), last_frame, input_count, packet.data() + inputs_offset);
      break;
    }
    case NP_MSG_SLIPPI_MATCH_SELECTIONS: {
      auto s = ReadSelections(packet);
      if (!s->error) {
        uint8_t idx = PlayerIdxFromPort(s->player_idx);
        if (idx >= remote_player_count_) break;
        host::log("slippi: received selections from player %u (char %u color %u stage %u)", s->player_idx, s->character_id, s->character_color, s->stage_id);
        match_info_.remote[idx].Merge(*s);
        has_game_started_ = false;
        std::lock_guard<std::mutex> lk(pad_mutex_);
        remote_pad_queue_[idx].clear();
        remote_pad_history_[idx].clear();
      }
      break;
    }
    case NP_MSG_SLIPPI_CHAT_MESSAGE: {
      auto s = ReadChatMessage(packet);
      if (!s->error) {
        std::lock_guard<std::mutex> lk(chat_mutex_);   // the game thread takes this in GetSlippiRemoteChatMessage
        remote_chat_message_.message_id = s->message_id;
        remote_chat_message_.player_idx = s->player_idx;
      }
      break;
    }
    case NP_MSG_SLIPPI_CONN_SELECTED: break;
    case NP_MSG_SLIPPI_COMPLETE_STEP: {
      GamePrepStepResults r;
      if (!(packet >> r.step_idx >> r.char_selection >> r.char_color_selection >> r.stage_selections[0] >> r.stage_selections[1])) {
        host::log("slippi: complete step packet too small");
        break;
      }
      std::lock_guard<std::mutex> lk(game_prep_mutex_);
      if (game_prep_step_queue_.size() >= GAME_PREP_STEP_QUEUE_MAX) game_prep_step_queue_.pop_front();
      game_prep_step_queue_.push_back(r);
      break;
    }
    case NP_MSG_SLIPPI_SYNCED_STATE: {
      uint8_t packet_player_port;
      if (!(packet >> packet_player_port)) break;
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) break;
      SyncedGameState r;
      packet >> r.match_id >> r.game_index >> r.tiebreak_index >> r.seconds_remaining;
      for (int i = 0; i < 4; ++i) packet >> r.fighters[i].stocks_remaining >> r.fighters[i].current_health;
      remote_sync_states_[pidx] = r;
      break;
    }
    default: host::log("slippi: unknown netplay message %u", mid); break;
  }
}

void NetplayClient::WriteSelections(Packet& p, const PlayerSelections& s) {
  p << (uint8_t)NP_MSG_SLIPPI_MATCH_SELECTIONS << s.character_id << s.character_color << s.is_character_selected << s.player_idx
    << s.stage_id << s.is_stage_selected << s.rng_offset << s.team_id << s.alt_stage_mode;
}
std::unique_ptr<PlayerSelections> NetplayClient::ReadSelections(Packet& p) {
  auto s = std::make_unique<PlayerSelections>();
  if (!(p >> s->character_id >> s->character_color >> s->is_character_selected >> s->player_idx >> s->stage_id >> s->is_stage_selected
        >> s->rng_offset >> s->team_id >> s->alt_stage_mode)) { host::log("slippi: invalid selection packet"); s->error = true; }
  return s;
}
std::unique_ptr<PlayerSelections> NetplayClient::ReadChatMessage(Packet& p) {
  auto s = std::make_unique<PlayerSelections>();
  if (!(p >> s->message_id) || !(p >> s->player_idx)) { s->error = true; return s; }
  // The game shows this port as the sender, so it has to be one of the remote players.
  if (s->player_idx == player_idx_ || PlayerIdxFromPort(s->player_idx) >= remote_player_count_) {
    host::log("slippi: chat message with invalid player port %u", s->player_idx); s->error = true; return s;
  }
  static const int allowed[] = {136, 129, 130, 132, 34, 40, 33, 36, 72, 66, 68, 65, 24, 18, 20, 17, CHAT_MSG_CHAT_DISABLED};
  bool ok = false;
  for (int a : allowed) if (a == s->message_id) ok = true;
  if (!ok) { host::log("slippi: invalid chat message %d", s->message_id); s->error = true; }
  return s;
}

void NetplayClient::Send(Packet& packet) {
  for (size_t i = 0; i < server_.size(); ++i) {
    auto conn = active_connections_.find(peer_key(server_[i]));
    if (conn != active_connections_.end()) {
      auto p = conn->second.find(server_[i]);
      if (p != conn->second.end() && p->second.is_disconnected) continue;
    }
    uint8_t mid = packet.data()[0];
    enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
    uint8_t channel = 0;
    if (mid == NP_MSG_SLIPPI_PAD || mid == NP_MSG_SLIPPI_PAD_ACK || mid == NP_MSG_SLIPPI_PAD_SUGGESTION) { flags = ENET_PACKET_FLAG_UNSEQUENCED; channel = 1; }
    ENetPacket* epac = enet_packet_create(packet.data(), packet.size(), flags);
    enet_peer_send(server_[i], channel, epac);
  }
}

// ENet drops unreliable packets on purpose while the round trip time rises, and pads and acks are all unreliable:
// a latency increase became gaps in the opponent's inputs. A deceleration of 0 keeps the throttle at full, and ENet
// sends the setting to the peer, so it stops throttling what it sends us too (Slippi 3.7).
static void disable_packet_throttle_drops(ENetPeer* peer) {
  enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
}

void NetplayClient::Disconnect() {
  status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  if (active_connections_.empty()) return;
  for (auto& conn : active_connections_)
    for (auto& peer : conn.second) enet_peer_disconnect(peer.first, pending_disconnect_reason_.load(std::memory_order_acquire));
  ENetEvent ev;
  while (enet_host_service(client_, &ev, 3000) > 0) {
    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) host::log("slippi: got disconnect from peer %u", ev.peer->address.port);
  }
  for (auto& conn : active_connections_) for (auto& peer : conn.second) enet_peer_reset(peer.first);
  active_connections_.clear();
  for (auto& a : player_active_) a.store(false, std::memory_order_release);
  server_.clear();
}

#if defined(__SWITCH__)
extern "C" __attribute__((weak)) void switch_set_current_thread_priority(int priority);   // switch_threads.c
extern "C" __attribute__((weak)) void switch_cpu_register(const char* name);
#endif
void NetplayClient::SendAsync(std::unique_ptr<Packet> packet) {
  if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) return;
  { std::lock_guard<std::mutex> lk(async_mutex_); async_queue_.push_back(std::move(packet)); }
  WakeNetplayThread();
}

void NetplayClient::WakeNetplayThread() {
  if (!waker_.joinable()) { wakeup_thread(client_); return; }
  { std::lock_guard<std::mutex> lk(async_mutex_); waker_pending_ = true; }
  waker_cv_.notify_one();
}

void NetplayClient::WakerFunc() {
#if defined(__SWITCH__)
  // Same priority as the netplay thread: our inputs leave as soon as the socket call returns.
  if (switch_set_current_thread_priority) switch_set_current_thread_priority(0x24);
  if (switch_cpu_register) switch_cpu_register("netwake");
#endif
  std::unique_lock<std::mutex> lk(async_mutex_);
  for (;;) {
    waker_cv_.wait(lk, [this] { return waker_pending_ || waker_stop_; });
    if (waker_stop_) return;
    waker_pending_ = false;
    lk.unlock();
    const uint64_t t0 = time_ms();
    wakeup_thread(client_);
    const uint64_t took = time_ms() - t0;
    if (took >= 20) host::log("slippi: socket wake-up took %llu ms (the game kept running)", (unsigned long long)took);
    lk.lock();
  }
}

void NetplayClient::ThreadFunc() {
#if defined(__SWITCH__)
  // Horizon never time-slices threads below priority 59: at the default priority this thread waited
  // behind the graphics threads on cores 0-1 whenever they were busy, so the opponent's inputs (and
  // ours, which this thread sends) sat in a socket for milliseconds. It works for microseconds per
  // packet, so it now runs ahead of them (the game thread, 0x1C, still comes first).
  if (switch_set_current_thread_priority) switch_set_current_thread_priority(0x24);
  if (switch_cpu_register) switch_cpu_register("netplay");
#endif
#if defined(__APPLE__)
  // Opponent inputs are handled the moment they arrive: keep this thread on performance cores (an efficiency core can
  // take milliseconds to wake on an iPhone), whatever thread happened to create it.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  uint64_t start_time = time_ms();
  const uint64_t timeout = 8000;
  std::vector<bool> connections(remote_player_count_, false);
  std::vector<ENetAddress> remote_addrs;
  for (int i = 0; i < remote_player_count_; ++i) remote_addrs.push_back(server_[i]->address);

  while (status_.load(std::memory_order_acquire) == ConnectStatus::INITIATED) {
    ENetEvent ev;
    int net = enet_host_service(client_, &ev, 500);
    UpdateConnectRoundTrip();
    if (net > 0) {
      switch (ev.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          if (!ev.peer) break;
          Packet rpac(ev.packet->data, ev.packet->dataLength);
          OnData(rpac, ev.peer);
          enet_packet_destroy(ev.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
          if (ev.peer) log_peer("dropped while connecting", client_, ev.peer, false, ev.data);
          break;
        case ENET_EVENT_TYPE_CONNECT: {
          if (!ev.peer) break;
          log_peer("connected", client_, ev.peer, false, ev.data);
          disable_packet_throttle_drops(ev.peer);
          int early_idx = 0;
          for (int i = 0; i < (int)remote_addrs.size(); ++i)
            if (remote_addrs[i].host == ev.peer->address.host && remote_addrs[i].port == ev.peer->address.port) { early_idx = i; break; }
          ActiveConnectionInfo info;
          info.player_idx = match_info_.remote[early_idx].player_idx;
          active_connections_[peer_key(ev.peer)][ev.peer] = info;
          player_active_[info.player_idx].store(true, std::memory_order_release);
          bool already = false;
          for (size_t i = 0; i < server_.size(); ++i)
            if (connections[i] && ev.peer->address.host == server_[i]->address.host && ev.peer->address.port == server_[i]->address.port) { server_[i] = ev.peer; already = true; break; }
          if (already) break;
          for (size_t i = 0; i < server_.size(); ++i) {
            if (remote_addrs[i].host == ev.peer->address.host && !connections[i]) {
              host::log("slippi: connected to %x:%u", ev.peer->address.host, ev.peer->address.port);
              server_[i] = ev.peer;
              connections[i] = true;
              break;
            }
          }
          break;
        }
        default: break;
      }
    }
    bool all = true;
    for (int i = 0; i < remote_player_count_; ++i) if (!connections[i]) all = false;
    if (all) {
      client_->intercept = intercept_callback;
      host::log("slippi: online connection successful");
      status_.store(ConnectStatus::CONNECTED, std::memory_order_release);
      break;
    }
    if (time_ms() - start_time >= timeout || !do_loop_.load()) {
      for (int i = 0; i < remote_player_count_; ++i) if (!connections[i]) failed_connections_.push_back(i);
      status_.store(ConnectStatus::FAILED, std::memory_order_release);
      host::log("slippi: online connection failed");
      return;
    }
  }

  while (do_loop_.load()) {
    if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) break;
    const bool waiting_on_player_drop = ProcessPlayerDrops();
    for (auto& conn : active_connections_) {
      for (auto& peer : conn.second) {
        if (peer.second.is_disconnected || player_active_[peer.second.player_idx].load(std::memory_order_acquire)) continue;
        enet_peer_disconnect(peer.first, pending_disconnect_reason_.load(std::memory_order_acquire));
        peer.second.is_disconnected = true;
      }
    }
    ENetEvent ev;
    const uint64_t service_t0 = time_us();
    // Every player can be halted waiting on a dropped player, in which case no packet arrives to wake this thread.
    int net = enet_host_service(client_, &ev, waiting_on_player_drop ? 10 : 250);
    net_diag::on_service(time_us() - service_t0);
    UpdateConnectRoundTrip();
    for (;;) {
      std::unique_ptr<Packet> p;
      { std::lock_guard<std::mutex> lk(async_mutex_); if (async_queue_.empty()) break; p = std::move(async_queue_.front()); async_queue_.pop_front(); }
      Send(*p);
      net_diag::on_queue_depth(async_queue_.size());
    }
    net_diag::maybe_sample(time_us());
    {   // every 5 s: is each peer's reliable channel being acknowledged? (issue 9: the connection in use times out at 30 s)
      static uint64_t last_health_ms = 0;
      const uint64_t now_ms = time_ms();
      if (now_ms - last_health_ms >= 5000) {
        last_health_ms = now_ms;
        for (size_t i = 0; i < client_->peerCount; ++i) {
          ENetPeer* p = &client_->peers[i];
          if (p->state == ENET_PEER_STATE_DISCONNECTED) continue;
          bool in_use = false; for (auto* sp : server_) if (sp == p) in_use = true;
          host::log("slippi: peer health: slot %zu state %d rtt %u ms lost %u unacked reliable %zu (oldest %u ms) queued reliable %zu last-recv %u ms ago%s",
                    i, (int)p->state, p->roundTripTime, p->packetsLost, enet_list_size(&p->sentReliableCommands),
                    p->earliestTimeout ? client_->serviceTime - p->earliestTimeout : 0, enet_list_size(&p->outgoingReliableCommands),
                    p->lastReceiveTime ? client_->serviceTime - p->lastReceiveTime : 0, in_use ? " (in use)" : "");
        }
      }
    }
    if (net <= 0) continue;
    switch (ev.type) {
      case ENET_EVENT_TYPE_RECEIVE: {
        Packet rpac(ev.packet->data, ev.packet->dataLength);
        OnData(rpac, ev.peer);
        enet_packet_destroy(ev.packet);
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT: {
        std::string key = peer_key(ev.peer);
        { bool in_use = false; for (auto* sp : server_) if (sp == ev.peer) in_use = true; log_peer(ev.data ? "disconnected by the opponent" : "timed out or closed", client_, ev.peer, in_use, ev.data); }
        // ENet has already reset the peer (rtt 500, ids 4095 above); the cause and timings it recorded first say which
        // side ended the connection: the opponent's disconnect command, or our reliable commands going unacknowledged.
        {
          const char* cause = ev.peer->disconnectCause == ENET_PEER_DISCONNECT_CAUSE_REMOTE  ? "the opponent sent a disconnect"
                            : ev.peer->disconnectCause == ENET_PEER_DISCONNECT_CAUSE_TIMEOUT ? "timed out: reliable data not acknowledged"
                            : ev.peer->disconnectCause == ENET_PEER_DISCONNECT_CAUSE_LOCAL   ? "closed by this side"
                                                                                              : "unknown";
          host::log("slippi: disconnect cause for %x:%u: %s (rtt %u ms, last packet from them %u ms before)", ev.peer->address.host,
                    ev.peer->address.port, cause, ev.peer->disconnectRoundTripTime, ev.peer->disconnectReceiveAge);
        }
        if (active_connections_.count(key) && active_connections_[key].count(ev.peer)) active_connections_[key][ev.peer].is_disconnected = true;
        // A player is gone when no connected peer of theirs is left under any address, not when the peers under this
        // one address are. Both sides dial each other, and the opponent's real port can differ from the one matchmaking
        // advertised (their router remaps it): our own attempt to the advertised port then sits alone under its own
        // address, never connects, and expires at ENet's 30 s cap. Judged by address, that expiry marked the player
        // inactive and the loop above closed the healthy connection 30 s into the game (docs/MAC_FIXES.md issue 9).
        bool all_peers_gone = AreAllPeersDisconnectedForKey(key);
        if (all_peers_gone && active_connections_.count(key) && active_connections_[key].count(ev.peer)) {
          const uint8_t player = active_connections_[key][ev.peer].player_idx;
          for (auto& conn : active_connections_)
            for (auto& other : conn.second)
              if (other.second.player_idx == player && !other.second.is_disconnected && other.first->state == ENET_PEER_STATE_CONNECTED) all_peers_gone = false;
          if (!all_peers_gone) host::log("slippi: a connection attempt to %x:%u expired; the player is still connected on another address", ev.peer->address.host, ev.peer->address.port);
        }
        bool connected_client = false;
        for (size_t i = 0; i < server_.size(); ++i)
          if (ev.peer->address.host == server_[i]->address.host && ev.peer->address.port == server_[i]->address.port) connected_client = true;
        // Only an address we actually play through counts: losing a player makes everyone else drop them too (3.7).
        if (connected_client && all_peers_gone && active_connections_.count(key) && active_connections_[key].count(ev.peer))
          player_active_[active_connections_[key][ev.peer].player_idx].store(false, std::memory_order_release);
        if (connected_client && ev.data != 0) disconnect_reason_.store(ev.data, std::memory_order_release);
        host::log("slippi: disconnect from %x:%u (all peers gone: %d, connected client: %d)", ev.peer->address.host, ev.peer->address.port, all_peers_gone, connected_client);
        if (connected_client && all_peers_gone && AreAllConnectionsDisconnected()) do_loop_.store(false);
        break;
      }
      case ENET_EVENT_TYPE_CONNECT: {
        log_peer("connected late", client_, ev.peer, false, ev.data);
        disable_packet_throttle_drops(ev.peer);
        int late_idx = 0;
        for (int i = 0; i < (int)server_.size(); ++i)
          if (server_[i]->address.host == ev.peer->address.host && server_[i]->address.port == ev.peer->address.port) { late_idx = i; break; }
        ActiveConnectionInfo info;
        info.player_idx = match_info_.remote[late_idx].player_idx;
        // A dropped player stays dropped: the other players have already moved on without them.
        if (player_drop_times_ms_[info.player_idx] != 0) {
          info.is_disconnected = true;
          active_connections_[peer_key(ev.peer)][ev.peer] = info;
          enet_peer_disconnect(ev.peer, 0);
          break;
        }
        active_connections_[peer_key(ev.peer)][ev.peer] = info;
        player_active_[info.player_idx].store(true, std::memory_order_release);
        break;
      }
      default: break;
    }
  }
  Disconnect();
}

void NetplayClient::StartSlippiGame() {
  has_game_started_ = false;
  local_pad_queue_.clear();
  for (int i = 0; i < remote_player_count_; ++i) {
    last_frame_timing_[i] = {0, time_us()};
    last_frame_acked_[i] = 0;
    ack_timers_[i].clear();
  }
  is_desync_recovery_ = false;
  { std::lock_guard<std::mutex> lk(game_prep_mutex_); game_prep_step_queue_.clear(); }
  match_info_.Reset();
}

void NetplayClient::SendSlippiPad(std::unique_ptr<Pad> pad) {
  ConnectStatus st = status_.load(std::memory_order_acquire);
  if (st == ConnectStatus::FAILED || st == ConnectStatus::DISCONNECTED) return;
  if (pad) {
    local_pad_queue_.push_front(std::move(pad));
    net_diag::on_pad_sent(time_us());
  }
  int min_ack = INT_MAX;
  for (int i = 0; i < remote_player_count_; ++i) {
    if (!player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) continue;
    min_ack = std::min(min_ack, last_frame_acked_[i]);
  }
  if (!local_pad_queue_.empty()) min_ack = std::max(min_ack, local_pad_queue_.front()->frame - 128);
  while (!local_pad_queue_.empty() && local_pad_queue_.back()->frame < min_ack) local_pad_queue_.pop_back();
  if (local_pad_queue_.empty()) return;
  int32_t frame = local_pad_queue_.front()->frame;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_PAD << frame << player_idx_ << local_pad_queue_.front()->checksum_frame << local_pad_queue_.front()->checksum;
  for (auto& p : local_pad_queue_) spac->append(p->buf, PAD_WIRE_SIZE);   // inputs plus the RNG bits
  SendAsync(std::move(spac));
  uint64_t t = time_us();
  has_game_started_ = true;
  for (int i = 0; i < remote_player_count_; ++i) {
    last_frame_timing_[i] = {frame, t};
    std::lock_guard<std::mutex> lk(ack_mutex_);
    ack_timers_[i].push_back({frame, t});
  }
}

void NetplayClient::SetMatchSelections(PlayerSelections& s) {
  match_info_.local.Merge(s);
  match_info_.local.player_idx = player_idx_;
  auto spac = std::make_unique<Packet>();
  WriteSelections(*spac, match_info_.local);
  SendAsync(std::move(spac));
}
void NetplayClient::SendGamePrepStep(const GamePrepStepResults& s) {
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_COMPLETE_STEP << s.step_idx << s.char_selection << s.char_color_selection << s.stage_selections[0] << s.stage_selections[1];
  SendAsync(std::move(spac));
}
void NetplayClient::SendSyncedGameState(const SyncedGameState& s) {
  is_desync_recovery_ = true;
  local_sync_state_ = s;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_SYNCED_STATE << player_idx_ << s.match_id << s.game_index << s.tiebreak_index << s.seconds_remaining;
  for (int i = 0; i < 4; ++i) *spac << s.fighters[i].stocks_remaining << s.fighters[i].current_health;
  SendAsync(std::move(spac));
}
bool NetplayClient::GetGamePrepResults(uint8_t step_idx, GamePrepStepResults& res) {
  std::lock_guard<std::mutex> lk(game_prep_mutex_);
  while (!game_prep_step_queue_.empty()) {
    if (game_prep_step_queue_.front().step_idx == step_idx) { res = game_prep_step_queue_.front(); return true; }
    game_prep_step_queue_.pop_front();
  }
  return false;
}
void NetplayClient::SendChatMessage(int message_id) {
  remote_sent_chat_message_id = (uint8_t)message_id;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_CHAT_MESSAGE << message_id << player_idx_;
  SendAsync(std::move(spac));
}
PlayerSelections NetplayClient::GetSlippiRemoteChatMessage(bool chat_enabled) {
  PlayerSelections copied;
  bool reply_chat_disabled = false;
  {
    std::lock_guard<std::mutex> lk(chat_mutex_);
    if (chat_enabled) {
      copied.message_id = remote_chat_message_.message_id;
      copied.player_idx = remote_chat_message_.player_idx;
      remote_chat_message_.message_id = 0; remote_chat_message_.player_idx = 0;
    } else if (remote_chat_message_.message_id > 0 && remote_chat_message_.message_id != CHAT_MSG_CHAT_DISABLED) {
      reply_chat_disabled = true;
      remote_chat_message_.message_id = 0; remote_chat_message_.player_idx = 0;
    }
  }
  if (reply_chat_disabled) {   // chat is off here: say so to whoever wrote
    auto spac = std::make_unique<Packet>();
    *spac << (uint8_t)NP_MSG_SLIPPI_CHAT_MESSAGE << (int)CHAT_MSG_CHAT_DISABLED << player_idx_;
    SendAsync(std::move(spac));
    remote_sent_chat_message_id = 0;
  }
  return copied;
}
uint8_t NetplayClient::GetSlippiRemoteSentChatMessage(bool chat_enabled) {
  if (!chat_enabled) return 0;
  uint8_t id = remote_sent_chat_message_id;
  remote_sent_chat_message_id = 0;
  return id;
}

std::unique_ptr<RemotePadOutput> NetplayClient::GetSlippiRemotePad(int index, int max_frame_count) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  auto out = std::make_unique<RemotePadOutput>();
  out->player_idx = (uint8_t)(index >= player_idx_ ? index + 1 : index);
  out->is_disconnected = !player_active_[out->player_idx].load(std::memory_order_acquire);
  out->despawn_frame = despawn_frames_[out->player_idx].load(std::memory_order_acquire);
  if (remote_pad_queue_[index].empty()) {
    Pad empty(0);
    out->latest_frame = 0;
    out->data.insert(out->data.end(), empty.buf, empty.buf + PAD_FULL_SIZE);
    return out;
  }
  out->latest_frame = 0;
  out->checksum_frame = remote_checksums_[index].frame;
  out->checksum = remote_checksums_[index].value;
  int count = 0;
  for (auto it = remote_pad_queue_[index].rbegin(); it != remote_pad_queue_[index].rend(); ++it) {
    out->latest_frame = std::max(out->latest_frame, (*it)->frame);
    out->data.insert(out->data.begin(), (*it)->buf, (*it)->buf + PAD_FULL_SIZE);
    if (++count >= max_frame_count) break;
  }
  return out;
}

void NetplayClient::DropOldRemoteInputs(int32_t finalized_frame) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  for (int i = 0; i < remote_player_count_; ++i)
    while (remote_pad_queue_[i].size() > 1 && remote_pad_queue_[i].back()->frame < finalized_frame) remote_pad_queue_[i].pop_back();
}
std::unordered_map<uint8_t, bool> NetplayClient::GetActivePlayerIndices() const {
  std::unordered_map<uint8_t, bool> r;
  for (uint8_t i = 0; i < PLAYER_COUNT_MAX; ++i) if (player_active_[i].load(std::memory_order_acquire)) r[i] = true;
  return r;
}
void NetplayClient::ForceDisconnectPlayer(uint8_t player_idx) {
  if (player_idx >= PLAYER_COUNT_MAX) return;
  player_active_[player_idx].store(false, std::memory_order_release);
  bool any = false;
  for (uint8_t i = 0; i < remote_player_count_; ++i) if (player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) any = true;
  if (!any && status_.load(std::memory_order_acquire) == ConnectStatus::CONNECTED) {
    host::log("slippi: all remote players force-disconnected");
    status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  }
  if (client_) WakeNetplayThread();
}
void NetplayClient::ForceDisconnect(DisconnectReason reason) {
  pending_disconnect_reason_.store((uint32_t)reason, std::memory_order_release);
  disconnect_reason_.store((uint32_t)reason, std::memory_order_release);
  // Before marking the players as gone: we are the one leaving, so the netplay thread must not take that for those
  // players dropping and tell the others about it (see ProcessPlayerDrops).
  status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  for (uint8_t i = 0; i < remote_player_count_; ++i) ForceDisconnectPlayer(match_info_.remote[i].player_idx);
}
bool NetplayClient::AreAllPeersDisconnectedForKey(const std::string& key) {
  if (!active_connections_.count(key)) return true;
  for (auto& p : active_connections_[key]) if (!p.second.is_disconnected) return false;
  return true;
}
bool NetplayClient::AreAllConnectionsDisconnected() {
  for (auto& c : active_connections_) for (auto& p : c.second) if (!p.second.is_disconnected) return false;
  return true;
}
// Adds an input from a remote player. pad_mutex_ must be held.
void NetplayClient::AddRemotePad(uint8_t pidx, int32_t frame, const uint8_t* pad_data) {
  remote_pad_queue_[pidx].push_front(std::make_unique<Pad>(frame, pad_data));
  remote_pad_history_[pidx].emplace_front(frame, pad_data);
  if (remote_pad_history_[pidx].size() > DROP_REPORT_INPUT_COUNT) remote_pad_history_[pidx].pop_back();
}

// Netplay thread. Finds the players we just lost and tells the other players which inputs we have for them. Those
// reports add to the player's inputs, so once they have had time to arrive the newest input is the one everyone
// should have (a later report can leave players disagreeing; the desync detection catches that if it matters).
// Returns whether a report is still awaited. Port of SlippiNetplayClient::ProcessPlayerDrops (3.7).
bool NetplayClient::ProcessPlayerDrops() {
  bool waiting = false;
  const uint64_t now = time_ms();
  for (int i = 0; i < remote_player_count_; ++i) {
    const uint8_t port = match_info_.remote[i].player_idx;
    if (player_active_[port].load(std::memory_order_acquire)) continue;
    // ForceDisconnect sets the status before marking the players: when we are the one leaving there is nothing to report.
    if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) return false;
    int32_t last_frame;
    { std::lock_guard<std::mutex> lk(pad_mutex_); last_frame = remote_pad_history_[i].empty() ? 0 : remote_pad_history_[i].front().frame; }
    if (player_drop_times_ms_[port] == 0) {
      host::log("slippi: lost player %u, last input frame %d", port, last_frame);
      player_drop_times_ms_[port] = now ? now : 1;
      SendPlayerDropReport(port);
    }
    if (despawn_frames_[port].load(std::memory_order_acquire) != 0) continue;
    if (now - player_drop_times_ms_[port] < DROP_REPORT_WAIT_MS) { waiting = true; continue; }
    host::log("slippi: settled on player %u: last input frame %d, despawn frame %d", port, last_frame, last_frame + DROP_DESPAWN_FRAMES);
    despawn_frames_[port].store(last_frame + DROP_DESPAWN_FRAMES, std::memory_order_release);
  }
  return waiting;
}

// Netplay thread.
void NetplayClient::SendPlayerDropReport(uint8_t port) {
  Packet spac;
  spac << (uint8_t)NP_MSG_SLIPPI_PLAYER_DROP << player_idx_ << port;
  {
    std::lock_guard<std::mutex> lk(pad_mutex_);
    const auto& history = remote_pad_history_[PlayerIdxFromPort(port)];
    spac << (history.empty() ? (int32_t)0 : history.front().frame);
    spac << (uint8_t)history.size();
    for (const auto& pad : history) spac.append(pad.buf, PAD_WIRE_SIZE);
  }
  Send(spac);
}

// Netplay thread. Adds the inputs we were missing from a report about a dropped player, most recent first. They are
// the real inputs, so they work as if the player had sent them; the game rolls back if they differ from its prediction.
void NetplayClient::AddPlayerDropReportInputs(uint8_t pidx, int32_t last_frame, uint8_t input_count, const uint8_t* inputs) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  const int32_t head_frame = remote_pad_queue_[pidx].empty() ? 0 : remote_pad_queue_[pidx].front()->frame;
  const int64_t missing = (int64_t)last_frame - (int64_t)head_frame;
  if (missing > 128) { host::log("slippi: player drop packet contained too many frames (%lld)", (long long)missing); return; }
  for (int64_t i = missing - 1; i >= 0; --i) {
    uint8_t pad[PAD_WIRE_SIZE] = {};
    if (i < input_count) std::memcpy(pad, &inputs[i * PAD_WIRE_SIZE], PAD_WIRE_SIZE);
    else {
      // The queue cannot have gaps, so repeat an input; if it differs from the real one the desync detection catches it.
      host::log("slippi: player drop packet is missing frame %lld", (long long)((int64_t)last_frame - i));
      if (!remote_pad_queue_[pidx].empty()) std::memcpy(pad, remote_pad_queue_[pidx].front()->buf, PAD_WIRE_SIZE);
    }
    AddRemotePad(pidx, (int32_t)((int64_t)last_frame - i), pad);
  }
}

// Once a dropped player's last input is settled it repeats. These go in the same queue as real inputs since they are
// just as final, so the game does not need to know the player is gone.
void NetplayClient::FillDroppedPlayerInputs(int32_t frame) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  for (int i = 0; i < remote_player_count_; ++i) {
    const uint8_t port = match_info_.remote[i].player_idx;
    if (despawn_frames_[port].load(std::memory_order_acquire) == 0) continue;
    auto& queue = remote_pad_queue_[i];
    if (queue.empty()) queue.push_front(std::make_unique<Pad>(0));
    while (queue.front()->frame < frame) queue.push_front(std::make_unique<Pad>(queue.front()->frame + 1, queue.front()->buf));
  }
}
int32_t NetplayClient::CalcTimeOffsetUs() {
  std::vector<int> offsets;
  for (int i = 0; i < remote_player_count_; ++i) {
    if (!player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) continue;
    if (frame_offset_data_[i].buf.empty()) continue;
    std::vector<int32_t> buf = frame_offset_data_[i].buf;
    std::sort(buf.begin(), buf.end());
    int n = (int)buf.size(), off = (int)((1.0f / 3.0f) * n), end = n - off;
    int sum = 0;
    for (int k = off; k < end; ++k) sum += buf[k];
    int count = end - off;
    if (count <= 0) return 0;
    offsets.push_back(sum / count);
  }
  if (offsets.empty()) return 0;
  return *std::min_element(offsets.begin(), offsets.end());
}
bool NetplayClient::IsWaitingForDesyncRecovery() {
  if (!is_desync_recovery_) return false;
  for (int i = 0; i < remote_player_count_; ++i)
    if (local_sync_state_.game_index != remote_sync_states_[i].game_index || local_sync_state_.tiebreak_index != remote_sync_states_[i].tiebreak_index) return true;
  return false;
}
DesyncRecoveryResp NetplayClient::GetDesyncRecoveryState() {
  DesyncRecoveryResp r;
  r.is_recovering = is_desync_recovery_;
  r.is_waiting = IsWaitingForDesyncRecovery();
  if (!r.is_recovering || r.is_waiting) return r;
  r.state = local_sync_state_;
  for (int i = 0; i < remote_player_count_; ++i) {
    auto& s = remote_sync_states_[i];
    if (std::abs((int)r.state.seconds_remaining - (int)s.seconds_remaining) > 1) { r.is_error = true; return r; }
    if (s.seconds_remaining > r.state.seconds_remaining) r.state.seconds_remaining = s.seconds_remaining;
    for (int j = 0; j < 4; ++j) {
      auto& f = r.state.fighters[j]; auto& g = s.fighters[j];   // our state for fighter j, reconciled with what player i reported
      if (f.stocks_remaining != g.stocks_remaining) { r.is_error = true; return r; }
      if (std::abs((int)f.current_health - (int)g.current_health) > 25) { r.is_error = true; return r; }
      if (g.current_health < f.current_health) f.current_health = g.current_health;
    }
  }
  return r;
}

// ---------------------------------------------------------------- matchmaking
Matchmaking::LocalPeer Matchmaking::local_peer;
uint16_t Matchmaking::forced_port = 0;

Matchmaking::Matchmaking(User* user) : user_(user) {}
Matchmaking::~Matchmaking() {
  is_mm_terminated_ = true;
  state_ = ERROR_ENCOUNTERED;
  error_msg_ = "Matchmaking shut down";
  if (session_id_) mm::cancel(session_id_);   // never blocks; the thread below sees the flag within a poll
  if (thread_.joinable()) thread_.join();
  terminateMmConnection();
}
void Matchmaking::FindMatch(MatchSearchSettings settings) {
  search_settings_ = settings;
  error_msg_.clear();
  state_ = INITIALIZING;
  host::log("slippi: matchmaking started (mode %d, code '%s')", settings.mode, settings.connect_code.c_str());
  thread_ = std::thread(&Matchmaking::MatchmakeThread, this);
}
void Matchmaking::MatchmakeThread() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);   // the handshake that starts a match should not wait behind background work
#endif
  while (IsSearching()) {
    if (is_mm_terminated_) break;
    switch (state_.load()) {
      case INITIALIZING: startMatchmaking(); break;
      case MATCHMAKING: handleMatchmaking(); break;
      case OPPONENT_CONNECTING: handleConnecting(); break;
      default: break;
    }
  }
  terminateMmConnection();   // leave the queue if still in it and free the port
}
void Matchmaking::terminateMmConnection() {
  if (session_id_ != 0) { mm::cancel(session_id_); session_id_ = 0; }
  if (client_) { enet_host_destroy(client_); client_ = nullptr; }
}

// The local address the system would use to reach `remote` (a UDP connect sends nothing).
static std::string local_address_string(ENetAddress* remote) {
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return "";
  ENetAddress a;
  std::string out;
  if (enet_socket_connect(s, remote) != -1 && enet_socket_get_address(s, &a) != -1) {
    struct in_addr in; in.s_addr = a.host;
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &in, buf, sizeof buf)) out = buf;
  }
  enet_socket_destroy(s);
  return out;
}

// Sends a STUN binding request to `server` from the netplay socket and reads back the public address the server saw
// it come from (SlippiMatchmaking::stunBindingRequest, 3.7). The addresses are never logged.
bool Matchmaking::stunBindingRequest(const void* server_ptr, void* result_ptr) {
  const ENetAddress& server = *static_cast<const ENetAddress*>(server_ptr);
  mm::StunObservation& result = *static_cast<mm::StunObservation*>(result_ptr);
  uint8_t request[mm::kStunRequestLen];
  mm::stun_build_request(request);
  ENetBuffer send_buf; send_buf.data = request; send_buf.dataLength = sizeof request;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const int sent = enet_socket_send(client_->socket, &server, &send_buf, 1);
    if (sent != (int)sizeof request) { host::log("slippi: STUN send failed with %d", sent); return false; }
    const uint64_t start = time_ms();
    while (time_ms() - start < 500) {
      if (is_mm_terminated_) return false;
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(client_->socket, &condition, 100) != 0) { host::log("slippi: STUN wait failed"); return false; }
      if (!(condition & ENET_SOCKET_WAIT_RECEIVE)) continue;
      uint8_t response[512];
      ENetBuffer recv_buf; recv_buf.data = response; recv_buf.dataLength = sizeof response;
      ENetAddress from;
      const int received = enet_socket_receive(client_->socket, &from, &recv_buf, 1);
      if (received <= 0) continue;
      if (mm::stun_parse_response(request, response, (size_t)received, &result)) return true;
    }
  }
  return false;
}

// Keeps the NAT mapping of the netplay socket alive while queued. Any reply is drained so the netplay host does
// not find it later.
void Matchmaking::sendStunKeepalive() {
  last_keepalive_ms_ = time_ms();
  if (!client_ || !has_stun_addr_) return;
  uint8_t request[mm::kStunRequestLen];
  mm::stun_build_request(request);
  ENetAddress server; server.host = stun_host_; server.port = stun_port_;
  ENetBuffer send_buf; send_buf.data = request; send_buf.dataLength = sizeof request;
  enet_socket_send(client_->socket, &server, &send_buf, 1);
  enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
  if (enet_socket_wait(client_->socket, &condition, 100) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE)) {
    uint8_t response[512];
    ENetBuffer recv_buf; recv_buf.data = response; recv_buf.dataLength = sizeof response;
    ENetAddress from;
    enet_socket_receive(client_->socket, &from, &recv_buf, 1);
  }
}

void Matchmaking::startMatchmaking() {
  client_ = nullptr;
  UserInfo me = user_->GetUserInfo();
  static std::mt19937 rng((uint32_t)time_ms());

  if (local_peer.enabled) {
    // Two local instances peer directly; the "match" is fabricated the way the server would report it.
    host_port_ = local_peer.local_port;
    local_player_index_ = local_peer.local_index;
    is_host_ = local_player_index_ == 0;
    player_info_.clear();
    remote_ips_.clear();
    for (int i = 0; i < 2; ++i) {
      UserInfo p;
      if (i == local_player_index_) p = me;
      else { p.uid = "local-peer"; p.display_name = "Peer"; p.connect_code = "PEER#001"; }
      p.port = i + 1;
      p.chat_messages = User::GetDefaultChatMessages();
      player_info_.push_back(p);
    }
    remote_ips_.push_back(local_peer.remote_ip + ":" + std::to_string(local_peer.remote_port));
    allowed_stages_ = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
    mm_result_.id = "mode." + std::string(search_settings_.mode == DIRECT ? "direct" : "unranked") + "-local-test";
    mm_result_.players = player_info_;
    mm_result_.stages = allowed_stages_;
    mm_result_.items = 0;
    state_ = OPPONENT_CONNECTING;
    host::log("slippi: local peer test: player %d on port %d, peer %s", local_player_index_, host_port_, remote_ips_[0].c_str());
    return;
  }

  // The host is bound to the port we will play on. Talking to the STUN servers through its socket creates the NAT
  // mapping opponents will use and tells us what it is.
  for (int retry = 0; !client_ && retry < 15; ++retry) {
    host_port_ = forced_port ? forced_port : 41000 + (int)(rng() % 10000);
    ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = (enet_uint16)host_port_;
    client_ = enet_host_create(&addr, 1, 3, 0, 0);
  }
  if (!client_) { state_ = ERROR_ENCOUNTERED; error_msg_ = "Failed to create mm client"; return; }
  prioritize_socket(client_->socket);

  std::vector<ENetAddress> stun_addrs;
  for (size_t i = 0; i < version::kStunServerCount; ++i) {
    ENetAddress addr;
    if (enet_address_set_host(&addr, version::kStunServers[i].host) != 0) { host::log("slippi: cannot resolve STUN server %s", version::kStunServers[i].host); continue; }
    addr.port = version::kStunServers[i].port;
    stun_addrs.push_back(addr);
  }
  has_stun_addr_ = !stun_addrs.empty();
  ENetAddress stun_addr{};
  if (has_stun_addr_) stun_addr = stun_addrs[0];

  // The LAN address lets two players behind the same public address connect directly.
  std::string lan = has_stun_addr_ ? local_address_string(&stun_addr) : "";
  std::string lan_addr = lan.empty() ? "" : lan + ":" + std::to_string(host_port_);

  // Ask two servers what public address they see, walking the list in order so a network that blocks the first
  // entries still gets an answer. The service compares the two answers to classify the NAT.
  mm::StunObservation stun[2] = {};
  for (auto& addr : stun_addrs) {
    if (!stun[0].answered) { if (stunBindingRequest(&addr, &stun[0])) stun_addr = addr; }   // keepalives go to a server known to answer
    else if (stunBindingRequest(&addr, &stun[1])) break;
  }
  stun_host_ = stun_addr.host; stun_port_ = stun_addr.port;
  last_keepalive_ms_ = time_ms();
  const int answers = (stun[0].answered ? 1 : 0) + (stun[1].answered ? 1 : 0);
  if (answers == 0) host::log("slippi: STUN lookup failed, the service will use the address it observes");
  else host::log("slippi: STUN servers answered: %d of 2%s", answers,
                 answers == 2 && stun[0].port != stun[1].port ? " (different public ports: this network remaps the port per destination)" : "");

  mm::Request request;
  request.mode = (uint8_t)search_settings_.mode;
  request.connect_code.assign(search_settings_.connect_code.begin(), search_settings_.connect_code.end());
  request.netplay_port = (uint16_t)host_port_;
  request.lan_addr = lan_addr;
  request.stun[0] = stun[0]; request.stun[1] = stun[1];
  request.uid = me.uid; request.play_key = me.play_key; request.user_connect_code = me.connect_code; request.display_name = me.display_name;
  request.app_version = version::kSemver;
  request.url = version::kMatchmakingUrl;
  // A cancel that arrived during STUN must not start a session nobody will poll.
  if (is_mm_terminated_) return;
  session_id_ = mm::start(request);
  state_ = MATCHMAKING;
  host::log("slippi: matchmaking search started (Slippi %s)", version::kSemver);
}

void Matchmaking::handleMatchmaking() {
  if (state_ != MATCHMAKING) return;
  switch (mm::state()) {
    case mm::State::Connecting:
    case mm::State::Queued:
      if (has_stun_addr_ && time_ms() - last_keepalive_ms_ >= version::kStunKeepaliveMs) sendStunKeepalive();
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      return;
    case mm::State::Failed: {
      std::string message = mm::error_message();
      const std::string latest = mm::latest_version();
      if (!latest.empty()) {
        // The service refused this version: Slippi has released `latest`, this build speaks kSemver.
        user_->OverwriteLatestVersion(latest);
        host::log("slippi: the matchmaking service says Slippi %s is required; this build implements %s", latest.c_str(), version::kSemver);
        version::note_service_requires(latest);
      }
      error_msg_ = message.empty() ? "Matchmaking failed" : message;
      session_id_ = 0;
      host::log("slippi: mm error: %s", error_msg_.c_str());
      state_ = ERROR_ENCOUNTERED;
      return;
    }
    case mm::State::Matched: break;
    default:
      // The session ended without a result: it was cancelled out from under us.
      session_id_ = 0;
      error_msg_ = "Matchmaking was cancelled";
      state_ = ERROR_ENCOUNTERED;
      return;
  }
  std::string result_json;
  const bool have = mm::take_result_json(&result_json);
  session_id_ = 0;
  json resp = have ? json::parse(result_json, nullptr, false) : json();
  if (!have || resp.is_discarded() || !resp.is_object()) {
    host::log("slippi: match result %s", have ? "could not be parsed" : "was missing");
    error_msg_ = "Invalid response when getting mm status";
    state_ = ERROR_ENCOUNTERED;
    return;
  }
  try {
  netplay_client_ = nullptr;
  remote_ips_.clear();
  player_info_.clear();
  std::string match_id = jget(resp, "matchId", "");
  host::log("slippi: match id %s", match_id.c_str());
  auto queue = resp["players"];
  if (queue.is_array()) {
    std::string local_external_ip;
    for (auto& el : queue) {
      UserInfo p;
      bool is_local = jget(el, "isLocalPlayer", false);
      p.uid = jget(el, "uid", "");
      p.display_name = jget(el, "displayName", "");
      p.connect_code = jget(el, "connectCode", "");
      p.port = jget(el, "port", 0);
      p.is_bot = jget(el, "isBot", false);
      if (el.count("chatMessages") && el["chatMessages"].is_array()) {
        for (auto& m : el["chatMessages"]) if (m.is_string()) p.chat_messages.push_back(m.get<std::string>());
      }
      if (p.chat_messages.size() != 16) p.chat_messages = User::GetDefaultChatMessages();
      if (el.count("rank") && el["rank"].is_object()) {
        auto& rk = el["rank"];
        p.ranked_rating = jget(rk, "rating", 0.0f);
        p.ranked_update_count = jget(rk, "updateCount", 0);
        p.ranked_global_placement = jget(rk, "globalPlacement", 0);
        p.ranked_regional_placement = jget(rk, "regionalPlacement", 0);
        p.ranked_mmr = jget(rk, "mmr", 0.0f);
      }
      player_info_.push_back(p);
      if (is_local) {
        std::string ip = jget(el, "ipAddress", "1.1.1.1:123");
        local_external_ip = ip.substr(0, ip.find(':'));
        local_player_index_ = p.port - 1;
        // The address the opponent will dial. Behind carrier-grade NAT the public port differs from the local one; if
        // matchmaking advertises a port nothing forwards, the opponent's own attempt dies and Slippi Dolphin closes the
        // healthy connection when it expires (docs/MAC_FIXES.md issue 9).
        { const size_t colon = ip.find(':'); host::log("slippi: matchmaking advertises this device on public port %s (local port %d)", colon == std::string::npos ? "?" : ip.c_str() + colon + 1, host_port_); }
      }
    }
    for (auto& el : queue) {
      if (jget(el, "port", 0) - 1 == local_player_index_) continue;
      std::string ext = jget(el, "ipAddress", "1.1.1.1:123");
      std::string lan = jget(el, "ipAddressLan", "1.1.1.1:123");
      
      if (ext.substr(0, ext.find(':')) != local_external_ip || lan.empty()) remote_ips_.push_back(ext);
      else remote_ips_.push_back(lan);
    }
  }
  is_host_ = jget(resp, "isHost", false);
  allowed_stages_.clear();
  if (resp.count("stages") && resp["stages"].is_array()) for (auto& s : resp["stages"]) if (s.is_number()) allowed_stages_.push_back((uint16_t)s.get<int>());
  if (allowed_stages_.empty()) {
    allowed_stages_ = {0x3, 0x8, 0x1C, 0x1F, 0x20};
    if (player_info_.size() == 2) allowed_stages_.push_back(0x2);
  }
  mm_result_.id = match_id;
  mm_result_.players = player_info_;
  mm_result_.stages = allowed_stages_;
  mm_result_.items = jget(resp, "items", 0u);
  } catch (const std::exception& e) {
    host::log("slippi: unexpected match result shape: %s", e.what());
    error_msg_ = "Invalid response when getting mm status";
    state_ = ERROR_ENCOUNTERED;
    return;
  }
  if (remote_ips_.empty() || remote_ips_.size() > REMOTE_PLAYER_MAX) {
    host::log("slippi: match result names %zu remote players", remote_ips_.size());
    error_msg_ = "Invalid response when getting mm status";
    state_ = ERROR_ENCOUNTERED;
    return;
  }
  terminateMmConnection();   // free the port so the netplay client can bind it
  state_ = OPPONENT_CONNECTING;
  host::log("slippi: opponent found (decider: %d)", is_host_);
}

void Matchmaking::handleConnecting() {
  netplay_client_ = nullptr;
  std::vector<std::string> addrs;
  std::vector<uint16_t> ports;
  for (auto& ip : remote_ips_) {
    size_t colon = ip.find(':');
    addrs.push_back(ip.substr(0, colon));
    ports.push_back((uint16_t)std::atoi(ip.substr(colon + 1).c_str()));
  }
  auto client = std::make_unique<NetplayClient>(addrs, ports, (uint8_t)remote_ips_.size(), (uint16_t)host_port_, is_host_, (uint8_t)local_player_index_);
  while (!netplay_client_) {
    auto st = client->GetSlippiConnectStatus();
    if (st == NetplayClient::ConnectStatus::INITIATED) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (state_ != OPPONENT_CONNECTING) return;
      continue;
    }
    if (st != NetplayClient::ConnectStatus::CONNECTED) {
      if (local_peer.enabled) { state_ = ERROR_ENCOUNTERED; error_msg_ = "Could not connect to the local peer"; return; }
      if (search_settings_.mode == TEAMS) { state_ = ERROR_ENCOUNTERED; error_msg_ = "Timed out waiting for other players to connect"; return; }
      host::log("slippi: connection attempt failed, searching again");
      state_ = INITIALIZING;
      return;
    }
    netplay_client_ = std::move(client);
  }
  state_ = CONNECTION_SUCCESS;
}

int Matchmaking::GetPlayerRank(uint8_t port) const {
  if (port >= player_info_.size()) return 0;
  const UserInfo& info = player_info_[port];
  const float r = info.ranked_rating;
  if (info.ranked_update_count < 5) return 0;
  // Each threshold is the minimum rating for that rank, checked from the top down (3.7; matches the launcher and website).
  if (r >= 2191.75f && (info.ranked_global_placement > 0 || info.ranked_regional_placement > 0)) return 19;   // Grandmaster
  static const float minimum[] = {765.43f, 913.72f, 1054.87f, 1188.88f, 1315.75f, 1435.48f, 1548.07f, 1653.52f, 1751.83f,
                                  1843.0f, 1927.03f, 2003.92f, 2073.67f, 2136.28f, 2191.75f, 2275.0f, 2350.0f};
  for (int i = 16; i >= 0; --i) if (r >= minimum[i]) return i + 2;   // Bronze 2 .. Master 3
  return 1;   // Bronze 1
}

}  // namespace slippi
#endif  // online transport is deliberately absent from the diagnostic target
