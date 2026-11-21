// Slippi Online networking: SFML-compatible packets, the ENet netplay client, the matchmaking
// client and the user record. Ports of Dolphin's SlippiNetplay/SlippiMatchmaking/SlippiUser
// with the same wire formats, so the port can play against stock Slippi clients.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace slippi {

constexpr int ROLLBACK_MAX_FRAMES = 7;
constexpr int ONLINE_LOCKSTEP_INTERVAL = 30;
constexpr int REMOTE_PLAYER_MAX = 3;
constexpr int PLAYER_COUNT_MAX = REMOTE_PLAYER_MAX + 1;
constexpr int PAD_FULL_SIZE = 0xC;
constexpr int PAD_INPUT_SIZE = 0x8;                  // buttons, sticks and triggers (an input suggestion carries only these)
constexpr int PAD_WIRE_SIZE = PAD_INPUT_SIZE + 1;    // plus the RNG bits: what each frame's pad is on the network (Slippi 3.7)
// A dropped player's newest inputs are reported to the other players (SLIPPI_DROP_* in Dolphin's SlippiNetplay.h).
constexpr size_t DROP_REPORT_INPUT_COUNT = 64;
constexpr uint64_t DROP_REPORT_WAIT_MS = 300;
constexpr int32_t DROP_DESPAWN_FRAMES = 2 * ROLLBACK_MAX_FRAMES + 2;
constexpr size_t GAME_PREP_STEP_QUEUE_MAX = 32;
constexpr uint8_t CHAT_MSG_CHAT_DISABLED = 0x10;

enum NetMsg : uint8_t {
  NP_MSG_SLIPPI_PAD = 0x80, NP_MSG_SLIPPI_PAD_ACK = 0x81, NP_MSG_SLIPPI_MATCH_SELECTIONS = 0x82, NP_MSG_SLIPPI_CONN_SELECTED = 0x83,
  NP_MSG_SLIPPI_CHAT_MESSAGE = 0x84, NP_MSG_SLIPPI_COMPLETE_STEP = 0x85, NP_MSG_SLIPPI_SYNCED_STATE = 0x86,
  NP_MSG_SLIPPI_PAD_SUGGESTION = 0x87, NP_MSG_SLIPPI_PLAYER_DROP = 0x88,
};

// sf::Packet wire format: integers big-endian, bool as one byte, strings as u32 length + bytes.
class Packet {
 public:
  Packet() = default;
  Packet(const uint8_t* data, size_t size) : buf_(data, data + size) {}
  void append(const void* data, size_t size);
  const uint8_t* data() const { return buf_.data(); }
  size_t size() const { return buf_.size(); }
  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  Packet& operator<<(uint8_t v);
  Packet& operator<<(bool v) { return *this << (uint8_t)(v ? 1 : 0); }
  Packet& operator<<(uint16_t v);
  Packet& operator<<(uint32_t v);
  Packet& operator<<(int32_t v) { return *this << (uint32_t)v; }
  Packet& operator<<(const std::string& s);
  Packet& operator>>(uint8_t& v);
  Packet& operator>>(bool& v);
  Packet& operator>>(uint16_t& v);
  Packet& operator>>(uint32_t& v);
  Packet& operator>>(int32_t& v);
  Packet& operator>>(std::string& s);
 private:
  bool check(size_t n);
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool ok_ = true;
};

struct Pad {
  int32_t frame = 0, checksum_frame = 0;
  uint32_t checksum = 0;
  uint8_t buf[PAD_FULL_SIZE] = {};
  explicit Pad(int32_t f) : frame(f) {}
  Pad(int32_t f, const uint8_t* data) : frame(f) { for (int i = 0; i < PAD_WIRE_SIZE; ++i) buf[i] = data[i]; }
  Pad(int32_t f, int32_t cf, uint32_t c, const uint8_t* data) : Pad(f, data) { checksum_frame = cf; checksum = c; }
};

struct RemotePadOutput {
  int32_t latest_frame = 0, checksum_frame = 0;
  uint32_t checksum = 0;
  uint8_t player_idx = 0;
  bool is_disconnected = false;
  int32_t despawn_frame = 0;   // the frame a dropped player's fighter despawns on; 0 until their last input is settled
  std::vector<uint8_t> data;
};

struct GamePrepStepResults { uint8_t step_idx = 0, char_selection = 0, char_color_selection = 0, stage_selections[2] = {}; };
struct SyncedFighterState { uint8_t stocks_remaining = 4; uint16_t current_health = 0; };
struct SyncedGameState { std::string match_id; uint32_t game_index = 0, tiebreak_index = 0, seconds_remaining = 480; SyncedFighterState fighters[4]; };
struct DesyncRecoveryResp { bool is_recovering = false, is_waiting = false, is_error = false; SyncedGameState state; };

struct PlayerSelections {
  uint8_t player_idx = 0, character_id = 0, character_color = 0, team_id = 0;
  bool is_character_selected = false;
  uint16_t stage_id = 0;
  bool is_stage_selected = false;
  uint8_t alt_stage_mode = 0;
  uint32_t rng_offset = 0;
  int message_id = 0;
  bool error = false;
  void Merge(const PlayerSelections& s);
  void Reset();
};

struct MatchInfo {
  PlayerSelections local;
  PlayerSelections remote[REMOTE_PLAYER_MAX];
  void Reset() { local.Reset(); for (auto& r : remote) r.Reset(); }
};

struct UserInfo {
  std::string uid, play_key, display_name, connect_code, latest_version;
  int port = 0;
  std::vector<std::string> chat_messages;
  bool is_bot = false;
  float ranked_rating = 0; int ranked_update_count = 0, ranked_global_placement = 0, ranked_regional_placement = 0;
  float ranked_mmr = 0;
};

// The user record written by the Slippi Launcher (user.json in the Slippi user folder).
class User {
 public:
  explicit User(std::string user_dir);
  bool AttemptLogin();          // (re)reads user.json
  bool IsLoggedIn() const { return logged_in_; }
  UserInfo GetUserInfo() const { return info_; }
  void LogOut();
  void OverwriteLatestVersion(const std::string& v) { info_.latest_version = v; }
  std::vector<std::string> GetUserChatMessages() const;
  static std::vector<std::string> GetDefaultChatMessages();
  const std::string& dir() const { return dir_; }
 private:
  std::string dir_;
  UserInfo info_;
  bool logged_in_ = false;
};

// Connect-code history (direct-codes.json / teams-codes.json in the user folder).
class DirectCodes {
 public:
  DirectCodes(std::string path);
  std::string get(int index) const;
  int length() const { return (int)codes_.size(); }
  void AddOrUpdateCode(const std::string& code);
 private:
  void Load();
  void Save();
  std::string path_;
  std::vector<std::string> codes_;
};

class NetplayClient {
 public:
  enum class ConnectStatus { UNSET, INITIATED, CONNECTED, FAILED, DISCONNECTED };
  enum class DisconnectReason : uint32_t { UNSPECIFIED = 0, POOR_PERFORMANCE = 1 };

  NetplayClient(std::vector<std::string> addrs, std::vector<uint16_t> ports, uint8_t remote_player_count, uint16_t local_port,
                bool is_decider, uint8_t player_idx, const std::string& bind_address = {});
  ~NetplayClient();

  bool IsDecider() const { return is_decider_; }
  uint8_t LocalPlayerPort() const { return player_idx_; }
  // Round trip to the opponent in ms, safe from any thread: the game's pad acknowledgements once a game
  // has run, before that the connection's own acknowledgements (at least 2 of them), else 0.
  unsigned RoundTripMs() const {
    const unsigned game = rtt_ms_.load(std::memory_order_relaxed);
    return game ? game : connect_rtt_ms_.load(std::memory_order_relaxed);
  }
  bool RoundTripFromGame() const { return rtt_ms_.load(std::memory_order_relaxed) != 0; }
  ConnectStatus GetSlippiConnectStatus() const { return status_.load(std::memory_order_acquire); }
  std::vector<int> GetFailedConnections() const { return failed_connections_; }
  void StartSlippiGame();
  void SendSlippiPad(std::unique_ptr<Pad> pad);
  void SetMatchSelections(PlayerSelections& s);
  void SendGamePrepStep(const GamePrepStepResults& s);
  void SendSyncedGameState(const SyncedGameState& s);
  bool GetGamePrepResults(uint8_t step_idx, GamePrepStepResults& res);
  std::unique_ptr<RemotePadOutput> GetSlippiRemotePad(int index, int max_frame_count);
  void DropOldRemoteInputs(int32_t finalized_frame);
  std::unordered_map<uint8_t, bool> GetActivePlayerIndices() const;
  void ForceDisconnectPlayer(uint8_t player_idx);
  void ForceDisconnect(DisconnectReason reason = DisconnectReason::UNSPECIFIED);
  void FillDroppedPlayerInputs(int32_t frame);
  uint64_t GetPingUs(int index) const { return ping_us_[index].load(std::memory_order_relaxed); }
  DisconnectReason GetDisconnectReason() const { return (DisconnectReason)disconnect_reason_.load(std::memory_order_acquire); }
  MatchInfo* GetMatchInfo() { return &match_info_; }
  PlayerSelections GetSlippiRemoteChatMessage(bool chat_enabled);
  uint8_t GetSlippiRemoteSentChatMessage(bool chat_enabled);
  int32_t CalcTimeOffsetUs();
  bool IsWaitingForDesyncRecovery();
  DesyncRecoveryResp GetDesyncRecoveryState();
  void SendChatMessage(int message_id);
  void SendAsync(std::unique_ptr<Packet> packet);
  uint8_t remote_sent_chat_message_id = 0;

 private:
  struct FrameTiming { int32_t frame = 0; uint64_t time_us = 0; };
  struct FrameOffsetData { int idx = 0; std::vector<int32_t> buf; };
  struct ActiveConnectionInfo { uint8_t player_idx = 0; bool is_disconnected = false; };

  uint8_t PlayerIdxFromPort(uint8_t port) const { return port > player_idx_ ? port - 1 : port; }
  void OnData(Packet& packet, _ENetPeer* peer);
  void Send(Packet& packet);
  void Disconnect();
  void ThreadFunc();
  bool AreAllPeersDisconnectedForKey(const std::string& key);
  bool AreAllConnectionsDisconnected();
  void AddRemotePad(uint8_t pidx, int32_t frame, const uint8_t* pad_data);   // pad_mutex_ held
  bool ProcessPlayerDrops();
  void SendPlayerDropReport(uint8_t port);
  void AddPlayerDropReportInputs(uint8_t pidx, int32_t last_frame, uint8_t input_count, const uint8_t* inputs);
  void WriteSelections(Packet& p, const PlayerSelections& s);
  std::unique_ptr<PlayerSelections> ReadSelections(Packet& p);
  std::unique_ptr<PlayerSelections> ReadChatMessage(Packet& p);

  std::mutex pad_mutex_, ack_mutex_, async_mutex_;
  std::deque<std::unique_ptr<Packet>> async_queue_;
  _ENetHost* client_ = nullptr;
  std::vector<_ENetPeer*> server_;
  std::thread thread_;
  // Wakes the netplay thread's socket wait for the game thread. The wake-up is a loopback send, and a
  // Horizon socket call can take 40-230 ms when the system core is busy (SD-card writes): the game
  // thread only signals this thread, so a slow socket never stops the game (online freezes, v0.3.0).
  std::thread waker_;
  std::condition_variable waker_cv_;
  bool waker_pending_ = false, waker_stop_ = false;   // guarded by async_mutex_
  void WakerFunc();
  void WakeNetplayThread();
  uint8_t remote_player_count_ = 0;
  std::atomic<bool> do_loop_{true};
  std::atomic<unsigned> rtt_ms_{0};
  std::atomic<unsigned> connect_rtt_ms_{0};
  void UpdateConnectRoundTrip();
  bool is_decider_ = false, has_game_started_ = false;
  uint8_t player_idx_ = 0;
  std::unordered_map<std::string, std::map<_ENetPeer*, ActiveConnectionInfo>> active_connections_;
  std::atomic<bool> player_active_[PLAYER_COUNT_MAX] = {};
  std::deque<std::unique_ptr<Pad>> local_pad_queue_;
  std::deque<std::unique_ptr<Pad>> remote_pad_queue_[REMOTE_PLAYER_MAX];
  // Each remote player's newest inputs, most recent first: what another client may still be missing if the player drops.
  std::deque<Pad> remote_pad_history_[REMOTE_PLAYER_MAX];
  bool is_desync_recovery_ = false;
  struct ChecksumEntry { int32_t frame = 0; uint32_t value = 0; } remote_checksums_[REMOTE_PLAYER_MAX];
  SyncedGameState remote_sync_states_[REMOTE_PLAYER_MAX], local_sync_state_;
  std::mutex game_prep_mutex_;   // filled by the netplay thread, drained by the game thread
  std::deque<GamePrepStepResults> game_prep_step_queue_;
  std::atomic<uint64_t> ping_us_[REMOTE_PLAYER_MAX] = {};
  // Player drops, by port. The netplay thread publishes the despawn frame (0 until it settles on the last input).
  std::atomic<int32_t> despawn_frames_[PLAYER_COUNT_MAX] = {};
  uint64_t player_drop_times_ms_[PLAYER_COUNT_MAX] = {};
  int32_t last_frame_acked_[REMOTE_PLAYER_MAX] = {};
  FrameOffsetData frame_offset_data_[REMOTE_PLAYER_MAX];
  FrameTiming last_frame_timing_[REMOTE_PLAYER_MAX];
  std::deque<FrameTiming> ack_timers_[REMOTE_PLAYER_MAX];
  std::atomic<ConnectStatus> status_{ConnectStatus::UNSET};
  std::atomic<uint32_t> pending_disconnect_reason_{0}, disconnect_reason_{0};
  std::vector<int> failed_connections_;
  MatchInfo match_info_;
  std::mutex chat_mutex_;        // set by the netplay thread, taken by the game thread
  PlayerSelections remote_chat_message_;
};

class Matchmaking {
 public:
  enum OnlinePlayMode { RANKED = 0, UNRANKED = 1, DIRECT = 2, TEAMS = 3, PARTY = 4 };
  enum ProcessState { IDLE, INITIALIZING, MATCHMAKING, OPPONENT_CONNECTING, CONNECTION_SUCCESS, ERROR_ENCOUNTERED };
  struct MatchSearchSettings { OnlinePlayMode mode = RANKED; std::string connect_code; };
  struct MatchmakeResult { std::string id; std::vector<UserInfo> players; std::vector<uint16_t> stages; uint32_t items = 0; };
  // Local test peering (no matchmaking server): fixed player index, ports and peer address.
  struct LocalPeer { bool enabled = false; int local_index = 0; uint16_t local_port = 0; std::string remote_ip; uint16_t remote_port = 0; };

  explicit Matchmaking(User* user);
  ~Matchmaking();
  void FindMatch(MatchSearchSettings settings);
  ProcessState GetMatchmakeState() const { return state_; }
  std::string GetErrorMessage() const { return error_msg_; }
  bool IsSearching() const { return state_ == INITIALIZING || state_ == MATCHMAKING || state_ == OPPONENT_CONNECTING; }
  std::unique_ptr<NetplayClient> GetNetplayClient() { return std::move(netplay_client_); }
  int LocalPlayerIndex() const { return local_player_index_; }
  std::vector<UserInfo> GetPlayerInfo() const { return player_info_; }
  std::string GetPlayerName(uint8_t port) const { return port < player_info_.size() ? player_info_[port].display_name : ""; }
  int GetPlayerRank(uint8_t port) const;
  std::vector<uint16_t> GetStages() const { return allowed_stages_; }
  uint8_t RemotePlayerCount() const { return player_info_.empty() ? 0 : (uint8_t)(player_info_.size() - 1); }
  MatchmakeResult GetMatchmakeResult() const { return mm_result_; }
  static bool IsFixedRulesMode(OnlinePlayMode m) { return m == UNRANKED || m == RANKED || m == PARTY; }
  static LocalPeer local_peer;          // set from the command line for local two-instance tests
  static uint16_t forced_port;          // 0 = random 41000..50999

 private:
  void MatchmakeThread();
  void startMatchmaking();
  void handleMatchmaking();
  void handleConnecting();
  void terminateMmConnection();
  bool stunBindingRequest(const void* server, void* result);   // ENetAddress, mm::StunObservation
  void sendStunKeepalive();

  User* user_;
  _ENetHost* client_ = nullptr;   // bound to the netplay port while queued; only its socket is used (STUN)
  uint32_t stun_host_ = 0; uint16_t stun_port_ = 0; bool has_stun_addr_ = false;
  uint64_t last_keepalive_ms_ = 0;
  uint64_t session_id_ = 0;       // the matchmaking service session this object started
  std::atomic<bool> is_mm_terminated_{false};
  std::thread thread_;
  MatchSearchSettings search_settings_;
  std::atomic<ProcessState> state_{IDLE};
  std::string error_msg_;
  int host_port_ = 0, local_player_index_ = 0;
  std::vector<std::string> remote_ips_;
  MatchmakeResult mm_result_;
  std::vector<UserInfo> player_info_;
  std::vector<uint16_t> allowed_stages_;
  bool is_host_ = false;
  std::unique_ptr<NetplayClient> netplay_client_;
};

// Helpers shared with the EXI device.
uint64_t time_us();
uint64_t time_ms();
std::string utf8_to_shiftjis(const std::string& s);
std::string shiftjis_to_utf8(const std::string& s);
std::string convert_string_for_game(const std::string& input, int length);
std::string truncate_length_char(const std::string& input, int length);
std::string convert_connect_code_for_game(const std::string& input);
bool enet_ready();   // initializes ENet once

}  // namespace slippi
