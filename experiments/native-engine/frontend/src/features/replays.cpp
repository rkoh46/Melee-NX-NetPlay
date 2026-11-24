// Slippi replays: every match recorded as a .slp, and playback of a .slp with pause, step, slow motion,
// fast forward and jumps back and forward. As melee-unlocked does it (its host: port/app/source_host.cpp,
// h_replay_event / h_replay_finished / write_replay_recording for recording, set_replay / h_replay_start /
// h_replay_frame / replay_gate for playback; port/runtime/hle/native_replay_stream.* writes the file;
// port/third_party/slippilib reads one. Its game: shim/mu_replay.c, shim/mu_replay_abi.c).
//
// Recording. The game sends Slippi's events itself (replay_event): Game Start, the Gecko list is ours to
// add, then the pre-frame and post-frame events of every frame, Game End. The file is Slippi's format with
// Dolphin's metadata, so the Slippi Launcher and slippi-js read it, plus upstream's own keys muOptions,
// muOptions2, muOptions3 (the offline option words the match ran with, for playback) and modProfile.
// Offline and online matches alike; online matches carry no offline options. Written when the match ends,
// on a writer thread, to sdmc:/switch/melee-nx-netplay/Replays (Linux: <out>/Replays) as
// Game_YYYYMMDDTHHMMSS.slp. settings.ini: savereplays 0 turns it off, replay_dir DIR moves it, compressreplays 1
// writes .slpz instead (the slpz tool's format, 8 to 12 times smaller; playback and the Replays page read both).
//
// Playback (--replay FILE|latest|N on the command line, or the Replays page of the in-game menu, which asks
// play mode to start the game again with it). The game boots straight into the replay's match and asks for
// each frame's recorded inputs; the option words are the recorded ones. While it plays, the controllers are
// the viewer's controls:
//   Start             pause / play
//   A (paused)        one frame forward (held: keeps stepping)
//   D-pad Left/Right  5 seconds back / forward (back: from a kept state; forward: fast, not drawn)
//   D-pad Up          speed 1x, 1/2, 1/4
//   D-pad Down        fast forward (4x) on / off
// Jumping back restores a state kept every 4 seconds at the top of a frame (the hot-page snapshot store,
// runner/features/snapshot_store.*), then plays forward unseen to the target, as upstream's viewer does.
// Fidelity check: the game's own post-frame records during playback are compared with the file's; the
// end of the run says how many differ (0 is the bar).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "host_services.h"                      // melee-unlocked (fetched tree): linux_host
#include "mu_elf_bridge.h"                      // runner: linux_host::skip_draws
#include "features/snapshot_store.h"            // runner/features
#include "features/present/present.h"

#include <cerrno>
#include <ctime>
// native_replay_stream.cpp uses the Windows name (upstream's Linux build gets it from its port/linux/compat).
static inline int gmtime_s(struct tm* out, const time_t* t) { return gmtime_r(t, out) ? 0 : errno; }
#include "native_replay_stream.h"               // melee-unlocked (fetched tree), port/runtime/hle
#include "native_replay_stream.cpp"             // compiled here: header-only use, no other dependencies
// melee-unlocked (fetched tree), port/third_party/slippilib, named from its port/runtime/abi folder (on the include path)
#include "../../third_party/slippilib/SlippiGame.h"
#include "../../third_party/slippilib/SlippiGame.cpp"
// melee-unlocked (fetched tree), port/app: .slpz, the slpz tool's compressed replays (Alexander Harrison, MIT or
// Apache-2.0; upstream's port is GPL-2.0-or-later). Its "zstd/zstd.h" is a shim to the system's zstd (CMakeLists.txt).
#include "../../app/replay_slpz.h"
#include "../../app/replay_slpz.cpp"
#if __has_include("native_recording_codes.h")
#include "native_recording_codes.h"             // generated from the fetched tree (tools/native_recording_codes.py)
#define MU_HAVE_RECORDING_CODES 1
#else
#define MU_HAVE_RECORDING_CODES 0
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>

namespace fe::features {
namespace {
// ---- settings ----
bool save_replays = true;
bool compress_replays = false;                  // settings.ini compressreplays 1: write .slpz (upstream's launcher.ini key)
std::string replay_dir;                         // empty: the default folder
std::string requested;                          // --replay FILE|latest|N

std::string default_dir() {
#ifdef __SWITCH__
  return "sdmc:/switch/melee-nx-netplay/Replays";
#else
  return options.out_dir + "/Replays";
#endif
}
std::string dir() { return replay_dir.empty() ? default_dir() : replay_dir; }
void make_dirs(const std::string& path) {
  for (size_t at = path.find('/', 1); ; at = path.find('/', at + 1)) {
    mkdir(path.substr(0, at).c_str(), 0777);
    if (at == std::string::npos) break;
  }
}

bool setting(const std::string& key, const std::string& value) {
  if (key == "savereplays") save_replays = value != "0";
  else if (key == "replay_dir") replay_dir = value;
  else if (key == "compressreplays") compress_replays = value != "0";
  else return false;
  return true;
}

// ---- the replay list (newest first) ----
std::vector<std::string> list_replays() {
  std::vector<std::pair<time_t, std::string>> found;
  if (DIR* d = opendir(dir().c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      const bool slp = name.size() > 4 && name.compare(name.size() - 4, 4, ".slp") == 0;
      const bool slpz = name.size() > 5 && name.compare(name.size() - 5, 5, ".slpz") == 0;
      if (name[0] == '.' || (!slp && !slpz)) continue;     // .slpz-playback.slp: the unpacked copy a .slpz plays from
      struct stat st{};
      if (stat((dir() + "/" + name).c_str(), &st) == 0) found.emplace_back(st.st_mtime, name);
    }
    closedir(d);
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
  std::vector<std::string> out;
  for (auto& f : found) out.push_back(f.second);
  return out;
}

// ---- writer thread ----
std::mutex write_mutex;
std::condition_variable write_cv;
std::deque<std::pair<std::string, std::vector<uint8_t>>> write_queue;
std::thread writer;
bool writer_stop = false;
int writes_done = 0;
void writer_main() {
  std::unique_lock<std::mutex> lock(write_mutex);
  while (true) {
    write_cv.wait(lock, [] { return writer_stop || !write_queue.empty(); });
    if (write_queue.empty()) { if (writer_stop) return; continue; }
    auto job = std::move(write_queue.front());
    write_queue.pop_front();
    lock.unlock();
    // compressreplays 1: the .slpz, written only when it unpacks to exactly the recording (otherwise the .slp).
    const char* ext = ".slp";
    size_t raw = job.second.size();
    if (compress_replays) {
      std::vector<uint8_t> packed, back;
      if (slpz::compress(job.second, packed) && slpz::decompress(packed, back) && back == job.second) { job.second.swap(packed); ext = ".slpz"; }
      else say("replay: could not compress the recording (it is written as .slp)");
    }
    // Exclusive names: two matches in one second get _1, _2.
    std::string path;
    FILE* out = nullptr;
    for (unsigned suffix = 0; suffix < 1000 && !out; ++suffix) {
      path = dir() + "/" + job.first + (suffix ? "_" + std::to_string(suffix) : std::string()) + ext;
      if (access(path.c_str(), F_OK) == 0) continue;
      out = std::fopen(path.c_str(), "wb");
    }
    bool ok = false;
    if (out) { ok = std::fwrite(job.second.data(), 1, job.second.size(), out) == job.second.size(); ok = std::fclose(out) == 0 && ok; }
    if (raw != job.second.size()) say("replay: %s %s (%zu bytes, %zu as .slp)", ok ? "recording written to" : "WRITE FAILED for", path.c_str(), job.second.size(), raw);
    else say("replay: %s %s (%zu bytes)", ok ? "recording written to" : "WRITE FAILED for", path.c_str(), job.second.size());
    lock.lock();
    ++writes_done;
    write_cv.notify_all();
  }
}
void queue_write(std::string name, std::vector<uint8_t> bytes) {
  make_dirs(dir());
  std::lock_guard<std::mutex> lock(write_mutex);
  if (!writer.joinable()) writer = std::thread(writer_main);
  write_queue.emplace_back(std::move(name), std::move(bytes));
  write_cv.notify_all();
}

// ---- recording ----
slippi::NativeReplayStream stream;
bool written = true;
uint32_t online_games = 0;
uint64_t recorded_matches = 0;

void write_recording() {
  if (written || !stream.active()) return;
  written = true;
  if (!save_replays) return;                      // recorded only for the spectator server
  std::vector<uint8_t> file = stream.encode();
  if (file.empty()) { say("replay: recording failed: %s", stream.error().c_str()); return; }
  const std::time_t start = stream.start_time() ? std::time_t(stream.start_time()) : std::time(nullptr);
  std::tm tm{};
  localtime_r(&start, &tm);
  char name[64];
  std::strftime(name, sizeof name, "Game_%Y%m%dT%H%M%S", &tm);
  ++recorded_matches;
  queue_write(name, std::move(file));
}

// ---- playback ----
struct Playback {
  std::string path;
  std::unique_ptr<Slippi::SlippiGame> game;
  MuReplayStart start{};
  uint32_t words[3] = {};
  std::vector<uint8_t> game_start, game_end;
  // The file's own post-frame records, (frame, port, follower) -> payload, for the fidelity check.
  std::map<std::tuple<int32_t, uint8_t, uint8_t>, std::vector<uint8_t>> post;
  slippi::NativeReplayStream check;               // the game's re-recording (kept only to rewind the comparison)
  std::map<int32_t, slippi::NativeReplayStream::Mark> marks;
  uint64_t compared = 0, differ = 0; int32_t first_differ = INT32_MIN;
  std::map<std::tuple<int32_t, uint8_t, uint8_t>, bool> seen_differ;
  bool finished = false;
  // Viewer controls.
  bool paused = false, fast = false;
  int slow = 1;                                   // 1, 2, 4
  int32_t seek_target = INT32_MIN;               // playing forward unseen to this frame
  bool go_back = false;
  int32_t back_target = 0;
  int32_t frame = -123;                           // the next frame the game asked to play
  uint16_t last_buttons = 0;
  uint32_t a_held = 0;
} pb;
bool playing = false;

std::vector<uint8_t> read_file(const std::string& path) {
  std::vector<uint8_t> d;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t block[65536]; size_t n;
    while ((n = std::fread(block, 1, sizeof block, f)) > 0) d.insert(d.end(), block, block + n);
    std::fclose(f);
  }
  return d;
}
// Every event of the file's raw element (as upstream's read_replay_event walks it).
template <typename Fn> bool each_event(const std::vector<uint8_t>& d, Fn fn) {
  static const char tag[] = "raw[$U#l";
  auto it = std::search(d.begin(), d.end(), tag, tag + 8);
  if (it == d.end()) return false;
  size_t i = size_t(it - d.begin()) + 8 + 4;
  if (i + 2 > d.size() || d[i] != 0x35 || d[i + 1] < 1 || (d[i + 1] - 1) % 3) return false;
  int32_t sizes[256]; std::fill(std::begin(sizes), std::end(sizes), -1);
  for (size_t k = 0; k < size_t((d[i + 1] - 1) / 3); ++k) {
    const size_t e = i + 2 + 3 * k;
    if (e + 3 > d.size()) return false;
    sizes[d[e]] = int32_t(uint32_t(d[e + 1]) << 8 | d[e + 2]);
  }
  const uint32_t raw_size = be32(d.data() + i - 4);
  const size_t end = raw_size && raw_size <= d.size() - i ? i + raw_size : d.size();
  for (size_t pos = i + 1 + d[i + 1]; pos < end;) {
    const uint8_t command = d[pos++];
    if (sizes[command] < 0 || size_t(sizes[command]) > end - pos) return false;
    fn(command, d.data() + pos, size_t(sizes[command]));
    pos += size_t(sizes[command]);
  }
  return true;
}
uint32_t read_word(const std::vector<uint8_t>& bytes, int word) {   // muOptions, muOptions2, muOptions3
  static const char k1[] = "U\x09muOptionsl", k2[] = "U\x0AmuOptions2l", k3[] = "U\x0AmuOptions3l";
  const char* key = word == 2 ? k3 : word == 1 ? k2 : k1;
  const size_t len = word == 0 ? sizeof k1 - 1 : sizeof k2 - 1;
  const auto at = std::search(bytes.begin(), bytes.end(), key, key + len);
  if (at == bytes.end() || size_t(bytes.end() - at) < len + 4) return 0;
  return be32(&*(at + std::ptrdiff_t(len)));
}
std::string read_mod_profile(const std::vector<uint8_t>& bytes) {
  static const char key[] = "U\x0amodProfileSU\x40";
  const auto at = std::search(bytes.begin(), bytes.end(), key, key + sizeof key - 1);
  if (at == bytes.end() || size_t(bytes.end() - at) < sizeof key - 1 + 64) return {};
  return std::string(at + std::ptrdiff_t(sizeof key - 1), at + std::ptrdiff_t(sizeof key - 1 + 64));
}

// The replay's gameplay codes the native game knows, by injection address (upstream's replay_codes, the
// same table; codes not in it are counted, not refused).
uint32_t replay_codes(const std::vector<uint8_t>& list, uint8_t* ps_frozen_toggle, size_t* unknown_out) {
  constexpr uint32_t kUcfShared = 0x80000000u;
  struct Known { uint32_t address, code; };
  static const Known known[] = {
      {0x800D65EC, MU_REPLAY_CODE_UCF084}, {0x800C9A44, kUcfShared}, {0x8006B460, MU_REPLAY_CODE_UCF084}, {0x8008E54C, MU_REPLAY_CODE_UCF084},
      {0x8009A0B8, MU_REPLAY_CODE_UCF084}, {0x800998A4, kUcfShared}, {0x80093294, MU_REPLAY_CODE_UCF084}, {0x800908F4, kUcfShared},
      {0x8016E510, MU_REPLAY_CODE_NEUTRAL_SPAWN}, {0x801239A8, MU_REPLAY_CODE_FREEZE_GLITCH}, {0x801C154C, MU_REPLAY_CODE_INIT_STAGE_DATA},
      {0x80068EEC, MU_REPLAY_CODE_INIT_PLAYER_DATA}, {0x8006A880, MU_REPLAY_CODE_OFFSCREEN_DAMAGE}, {0x800D4C1C, MU_REPLAY_CODE_DEAD_UP_FALL},
      {0x800D4D68, MU_REPLAY_CODE_DEAD_UP_FALL}, {0x80080E80, MU_REPLAY_CODE_DEAD_UP_FALL}, {0x8021AAE4, MU_REPLAY_CODE_FD_BG_SEED},
      {0x801C65C8, MU_REPLAY_CODE_PS_ZERO_BUFFER}, {0x801D4760, MU_REPLAY_CODE_PS_IS_VALID}, {0x801D457C, MU_REPLAY_CODE_PS_FROZEN_CHECK},
      {0x801D4578, MU_REPLAY_CODE_PS_FROZEN_CHECK}, {0x801D14C8, MU_REPLAY_CODE_PS_PRELOAD}, {0x801D45EC, MU_REPLAY_CODE_PS_PRELOAD},
      {0x801D460C, MU_REPLAY_CODE_PS_PRELOAD}, {0x801D4610, MU_REPLAY_CODE_PS_PRELOAD}, {0x801D4724, MU_REPLAY_CODE_PS_PRELOAD},
      {0x801D4F14, MU_REPLAY_CODE_PS_PRELOAD}, {0x800165AC, MU_REPLAY_CODE_PS_FILE_LOAD}, {0x801CD37C, MU_REPLAY_CODE_CASTLE_WIND_RESET},
      {0x801D69B0, MU_REPLAY_CODE_KONGO_LOG_RESET}, {0x8008653C, MU_REPLAY_CODE_WHISPY_FIX}, {0x800AC5B8, MU_REPLAY_CODE_NANA_DETERMINISM},
      {0x801D24FC, MU_REPLAY_CODE_PS_MONITOR}, {0x800DB880, MU_REPLAY_CODE_PREVENT_WOBBLING}, {0x800DBBD4, MU_REPLAY_CODE_PREVENT_WOBBLING},
      {0x8008F090, MU_REPLAY_CODE_PREVENT_WOBBLING},
      {0x80179378, 0}, {0x8017900C, 0}, {0x8017A890, 0}, {0x8000568C, 0}, {0x802F84C8, 0},
  };
  uint32_t codes = 0; size_t unknown = 0, dashback = 0, shield_drop = 0;
  for (size_t idx = 0; idx + 8 <= list.size();) {
    const uint8_t type = list[idx] & 0xFE;
    const uint32_t address = (be32(&list[idx]) & 0x01FFFFFFu) | 0x80000000u;
    const uint32_t word = be32(&list[idx + 4]);
    size_t len = 8;
    if (type == 0xC0 || type == 0xC2) len = 8 + size_t(word) * 8;
    else if (type == 0x08) len = 16;
    else if (type == 0x06) len = 8 + ((word + 7) & ~7u);
    if (list[idx] == 0xFF || idx + len > list.size()) break;
    const Known* k = nullptr;
    for (const auto& e : known) if (e.address == address) { k = &e; break; }
    if (!k) ++unknown;
    else {
      codes |= k->code;
      if (address == 0x800C9A44) dashback = len;
      if (address == 0x800998A4) shield_drop = len;
      if (address == 0x801D457C && len >= 8 + 12) *ps_frozen_toggle = list[idx + 8 + 8];
      if (address == 0x801D4578 && len == 32 && list[idx + 0x18] == 0x4E && list[idx + 0x1B] == 0x20) *ps_frozen_toggle = 1;
    }
    idx += len;
  }
  if (codes & kUcfShared) {
    codes &= ~kUcfShared;
    if (!(codes & MU_REPLAY_CODE_UCF084)) codes |= dashback == 352 && shield_drop == 312 ? MU_REPLAY_CODE_UCF08 : MU_REPLAY_CODE_UCF_OTHER;
  }
  *unknown_out = unknown;
  return codes;
}

bool open_replay(const std::string& given) {
  std::vector<uint8_t> bytes = read_file(given);
  if (bytes.empty()) { say("replay: cannot read %s", given.c_str()); return false; }
  std::string path = given;
  if (slpz::is_slpz(bytes.data(), bytes.size())) {
    // A .slpz: unpacked once into the replay folder (the replay reader takes a file), then played from there.
    std::vector<uint8_t> slp;
    if (!slpz::decompress(bytes, slp)) { say("replay: %s is a damaged or newer .slpz", given.c_str()); return false; }
    path = dir() + "/.slpz-playback.slp";
    make_dirs(dir());
    FILE* out = std::fopen(path.c_str(), "wb");
    const bool ok = out && std::fwrite(slp.data(), 1, slp.size(), out) == slp.size();
    if (out && std::fclose(out) != 0) { say("replay: cannot write %s", path.c_str()); return false; }
    if (!ok) { say("replay: cannot write %s", path.c_str()); return false; }
    say("replay: %s unpacked (%zu bytes -> %zu)", given.c_str(), bytes.size(), slp.size());
    bytes.swap(slp);
  }
  pb = Playback{};
  pb.path = path;
  for (int w = 0; w < 3; ++w) pb.words[w] = read_word(bytes, w);
  const std::string profile = read_mod_profile(bytes);
  // As upstream's set_replay: a vanilla-only session does not play a replay recorded with gameplay mods.
  if (settings_vanilla_only() && (!profile.empty() || pb.words[0] || pb.words[1] || (pb.words[2] & MU_GAME_OPTION3_CPU_ALL))) {
    say("replay: %s was recorded with gameplay mods; a vanilla-only session does not play it (open it in a normal session)", path.c_str());
    pb = Playback{};
    return false;
  }
  if (!profile.empty() && profile != mods_fingerprint())
    say("replay: %s was recorded with mod content %.16s; this session has %s: it may not play as recorded", path.c_str(), profile.c_str(),
        mods_fingerprint().empty() ? "the retail game" : mods_fingerprint().substr(0, 16).c_str());
  each_event(bytes, [&](uint8_t command, const uint8_t* p, size_t n) {
    if (command == 0x36 && pb.game_start.empty()) pb.game_start.assign(p, p + n);
    else if (command == 0x39) pb.game_end.assign(p, p + n);
    else if (command == 0x38 && n >= 6) pb.post[{int32_t(be32(p)), p[4], p[5]}] = std::vector<uint8_t>(p, p + n);
  });
  pb.game = Slippi::SlippiGame::FromFile(path);
  if (!pb.game || !pb.game->AreSettingsLoaded() || pb.game_start.empty()) { say("replay: %s is not a Slippi replay this build reads", path.c_str()); pb = Playback{}; return false; }
  Slippi::GameSettings* s = pb.game->GetSettings();
  std::array<uint32_t, Slippi::GAME_INFO_HEADER_SIZE> header = s->header;
  for (int i = 0; i < 4; ++i) {          // Dolphin's prepareGameInfo: Sheik or Zelda in the header
    if (!pb.game->DoesPlayerExist(int8_t(i))) continue;
    const uint8_t ext = s->players[uint8_t(i)].characterId;
    if (ext == 0x12 || ext == 0x13) header[size_t(24 + 9 * i)] = (header[size_t(24 + 9 * i)] & 0x00FFFFFFu) | uint32_t(ext) << 24;
  }
  for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; ++i)
    for (int b = 0; b < 4; ++b) pb.start.game_info[4 * i + b] = uint8_t(header[size_t(i)] >> (24 - 8 * b));
  pb.start.random_seed = s->randomSeed;
  pb.start.is_pal = s->isPAL;
  const auto version = pb.game->GetVersion();
  pb.start.preload_ps = version[0] > 1 || (version[0] == 1 && version[1] > 2);
  pb.start.frozen_ps = s->isFrozenPS;
  size_t unknown = 0;
  pb.start.codes = replay_codes(s->geckoCodes, &pb.start.ps_frozen_toggle, &unknown);
  if (!pb.check.begin(pb.game_start, s->geckoCodes)) say("replay: the fidelity check cannot start: %s", pb.check.error().c_str());
  say("replay: %s, Slippi %s, frames %d..%d, stage %u, seed %08X, codes %08X (%zu others not native), options %08X %08X %08X, %zu post-frame records",
      path.c_str(), pb.game->GetVersionString().c_str(), Slippi::GAME_FIRST_FRAME, pb.game->GetLatestIndex(), unsigned(s->stage), s->randomSeed,
      pb.start.codes, unknown, pb.words[0], pb.words[1], pb.words[2], pb.post.size());
  if (s->stage > 85) say("replay: stage %u is not in the standard game: this replay needs the mod it was recorded with", unsigned(s->stage));
  return true;
}

const MuReplayStart* h_replay_start() { return playing ? &pb.start : nullptr; }
void h_replay_frame(int32_t frame, MuReplayFrame* out) {
  *out = MuReplayFrame{};
  if (!playing || !pb.game->DoesFrameExist(frame)) { out->result = MU_REPLAY_TERMINATE; return; }
  Slippi::FrameData* f = pb.game->GetFrame(frame);
  out->result = MU_REPLAY_CONTINUE;
  out->seed_exists = f->randomSeedExists ? 1 : 0;
  out->seed = f->randomSeed;
  for (uint8_t port = 0; port < 4; ++port)
    for (int follower = 0; follower < 2; ++follower) {
      const auto& source = follower ? f->followers : f->players;
      auto it = source.find(port);
      if (it == source.end()) continue;
      const Slippi::PlayerFrameData& d = it->second;
      MuReplayCharacter& c = out->character[port][follower];
      c.present = 1; c.random_seed = d.randomSeed;
      c.joystick_x = d.joystickX; c.joystick_y = d.joystickY; c.cstick_x = d.cstickX; c.cstick_y = d.cstickY;
      c.trigger = d.trigger; c.buttons = d.buttons;
      c.x = d.locationX; c.y = d.locationY; c.facing = d.facingDirection;
      c.action_state = d.animation; c.percent = d.percent;
      c.joystick_x_raw = d.joystickXRaw; c.joystick_y_raw = d.joystickYRaw; c.cstick_x_raw = d.cstickXRaw; c.cstick_y_raw = d.cstickYRaw;
    }
}
int32_t h_replay_stock_steal(int32_t frame, int32_t port) {
  if (!playing || !pb.game->DoesFrameExist(frame)) return 0;
  return pb.game->GetFrame(frame)->players.count(uint8_t(port)) ? 1 : 0;
}

void playback_event(uint8_t command, const uint8_t* payload, uint32_t size) {
  if (pb.check.active() && !pb.check.ended()) pb.check.append(command, payload, size);
  if (command != 0x38 || size < 6) return;
  const auto key = std::make_tuple(int32_t(be32(payload)), payload[4], payload[5]);
  const auto it = pb.post.find(key);
  if (it == pb.post.end()) return;
  ++pb.compared;
  const size_t n = std::min<size_t>(it->second.size(), size);
  if (std::memcmp(it->second.data(), payload, n) != 0 && !pb.seen_differ[key]) {
    pb.seen_differ[key] = true;
    if (pb.differ < 4) {
      size_t at = 0; while (at < n && it->second[at] == payload[at]) ++at;
      say("replay: post-frame record of frame %d port %u%s differs from the file at byte %zu (%02X, file %02X; %u and %zu bytes)", std::get<0>(key),
          unsigned(std::get<1>(key)), std::get<2>(key) ? " (follower)" : "", at, payload[at], it->second[at], size, it->second.size());
    }
    if (!pb.differ++) pb.first_differ = std::get<0>(key);
  }
}

void h_replay_event(uint8_t command, const uint8_t* payload, uint32_t size) {
  if (playing) { playback_event(command, payload, size); return; }
  if (command == 0x36) {
    write_recording();                            // a recording never finished (a reset): kept as it is
    if (!save_replays && !spectator_on()) { stream = slippi::NativeReplayStream{}; return; }
    std::vector<uint8_t> start(payload, payload + size);
    const bool net = size >= 0x2F8 && (start[0x1A3] == 0x08 || start[0x2BD] != 0);
    std::vector<uint8_t> codes;
#if MU_HAVE_RECORDING_CODES
    if (net) {
      codes.assign(std::begin(native_online_recording_codes), std::end(native_online_recording_codes));
      const uint32_t game = ++online_games;
      for (int b = 0; b < 4; ++b) start[size_t(0x2F0 + b)] = uint8_t(game >> (24 - 8 * b));
    } else if (!vanilla()) {
      codes.assign(std::begin(native_recording_codes), std::end(native_recording_codes));
    } else
#endif
      codes = {0xFF, 0, 0, 0, 0, 0, 0, 0};
    written = false;
    stream = slippi::NativeReplayStream{};
    stream.set_mod_profile(net ? std::string() : mods_fingerprint());
    const MuHostApi* h = host();
    stream.set_feature_options(net || vanilla() || !h ? 0u : h->game_options() & 0x00007FF0u);
    stream.set_feature_options2(net || !h ? 0u : h->game_options2());
    stream.set_feature_options3(net || !h ? 0u : h->game_options3());
    // The spectator server (spectator.cpp) gets the live copy: start_game, the command table and every event as the
    // file gets it, end_game after Game End (the order Slippi Dolphin's EXI device sends them).
    if (spectator_on()) { spectator_start_game(); stream.set_event_sink(spectator_write); }
    if (!stream.begin_slippi(start, codes, int64_t(std::time(nullptr)))) { say("replay: cannot start the recording: %s", stream.error().c_str()); spectator_end_game(false); }
    else say("replay: recording %s match%s", net ? "an online" : "an offline", MU_HAVE_RECORDING_CODES ? "" : " (no code list in this build)");
    return;
  }
  if (!stream.active() || stream.ended()) return;
  if (!stream.append(command, payload, size)) say("replay: cannot record event %02X: %s", command, stream.error().c_str());
  if (command == 0x39) { spectator_end_game(false); write_recording(); }   // at GAME!, on the writer thread
}

void h_replay_finished() {
  if (!playing) { write_recording(); return; }
  if (pb.finished) return;
  pb.finished = true;
  say("replay: playback finished at frame %d: %llu post-frame records compared with the file, %llu differ%s", pb.frame,
      static_cast<unsigned long long>(pb.compared), static_cast<unsigned long long>(pb.differ),
      pb.differ ? (" (first at frame " + std::to_string(pb.first_differ) + ")").c_str() : "");
  snapshot_store::release("the replay ended");
  linux_host::request_stop(0);
}

// ---- viewer controls: command 0xFA with 5 bytes, at the top of every frame of the replay's match ----
void apply_speed() {
  options.pace = !(pb.fast || pb.seek_target != INT32_MIN);
}
void sleep_ms(int ms) { usleep(useconds_t(ms) * 1000); }
constexpr uint16_t kStart = 0x1000, kA = 0x0100, kL = 0x0040, kR = 0x0020, kDL = 0x0001, kDR = 0x0002, kDD = 0x0004, kDU = 0x0008;
constexpr int32_t kKeepEvery = 240, kJump = 300;
// --feature-test replay-controls: the viewer's buttons pressed by a timeline of its own (a controller script stops
// with the game while it is paused): pause, two steps, play, back, forward, the three speeds, fast forward.
uint16_t test_buttons() {
  static const bool on = feature_test("replay-controls");
  static int stage = 0, release = 0; static double t0 = 0; static int32_t back_from = 0;
  if (!on) return 0;
  if (release) { --release; return 0; }
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  uint16_t b = 0;
  switch (stage) {
    case 0: if (pb.frame >= 60) { b = kStart; t0 = now; stage = 1; } break;
    case 1: if (now - t0 > 0.4) { b = kA; stage = 2; } break;
    case 2: if (now - t0 > 0.8) { b = kA; stage = 3; } break;
    case 3: if (now - t0 > 1.2) { b = kStart; stage = 4; } break;
    case 4: if (pb.frame >= 240) { b = kDL; back_from = pb.frame; stage = 5; } break;
    case 5: if (pb.frame < back_from - 100 && pb.frame >= 0) { b = kDR; stage = 6; } break;
    case 6: if (pb.seek_target == INT32_MIN && pb.frame >= 300) { b = kDU; stage = 7; } break;
    case 7: if (pb.frame >= 320) { b = kDU; stage = 8; } break;
    case 8: if (pb.frame >= 330) { b = kDU; stage = 9; } break;
    case 9: if (pb.frame >= 340) { b = kDD; stage = 10; } break;
    default: break;
  }
  if (b) { release = 1; say("replay: test presses %04X at frame %d", b, pb.frame); }
  return b;
}
uint16_t read_buttons() {
  MuPadStatus pads[4]{};
  // The pads the game was given this retrace (the viewer's controllers; the game plays the recording).
  std::memcpy(pads, pads_raw(), sizeof pads);
  uint16_t b = test_buttons();
  for (const auto& p : pads) b |= p.button;
  return b;
}
uint16_t poll_buttons() {
  MuPadStatus pads[4]{};
  read_controllers(pads);
  uint16_t b = test_buttons();
  for (const auto& p : pads) b |= p.button;
  return b;
}

void control_edges(uint16_t b) {
  const uint16_t pressed = b & uint16_t(~pb.last_buttons);
  pb.last_buttons = b;
  if ((b & (kL | kR | kStart)) == (kL | kR | kStart)) { pb.paused = false; return; }   // the way out: never held here
  if (pressed & kStart) { pb.paused = !pb.paused; say("replay: %s at frame %d", pb.paused ? "paused" : "playing", pb.frame); }
  if (pressed & kDU) { pb.slow = pb.slow == 1 ? 2 : pb.slow == 2 ? 4 : 1; say("replay: speed 1/%d", pb.slow); }
  if (pressed & kDD) { pb.fast = !pb.fast; say("replay: fast forward %s", pb.fast ? "on" : "off"); }
  if (pressed & kDR) { pb.seek_target = pb.frame + kJump; say("replay: forward to frame %d", pb.seek_target); }
  if (pressed & kDL) { pb.go_back = true; pb.back_target = std::max(pb.frame - kJump, int32_t(Slippi::GAME_FIRST_FRAME)); }
}

int replay_gate(uint8_t, const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  if (n != 5 || !playing) return 0;               // the Hack Pack's 0xFA (mods.cpp)
  const int32_t frame = int32_t(be32(p));
  if (p[4] == 1) {                                // phase 1: restore the state now
    int32_t key = 0;
    uint8_t answer = 0;
    if (snapshot_store::newest_at_or_before(pb.back_target, &key) && snapshot_store::load(key, "replay")) {
      const auto mark = pb.marks.find(key);
      if (mark != pb.marks.end()) { pb.check.rewind(mark->second); pb.marks.erase(std::next(mark), pb.marks.end()); }
      pb.seek_target = pb.back_target > key ? pb.back_target : INT32_MIN;
      say("replay: back to frame %d (kept state %d)", pb.back_target, key);
      present::note_discontinuity();
      answer = 2;
    }
    if (!answer) say("replay: no kept state at or before frame %d", pb.back_target);
    pb.go_back = false;
    apply_speed();
    reply.push_back(answer);
    return 1;
  }
  pb.frame = frame;
  if (pb.seek_target != INT32_MIN && frame >= pb.seek_target) { pb.seek_target = INT32_MIN; say("replay: at frame %d", frame); }
  // A state now and then, where a frame starts (the jump-back points).
  // The first frame the game asks about (the match's first is not always one) and then every kKeepEvery frames.
  if ((!snapshot_store::running() || (frame - Slippi::GAME_FIRST_FRAME) % kKeepEvery == 0) && !snapshot_store::has(frame)) {
    if (snapshot_store::keep(frame, 24, "replay")) pb.marks[frame] = pb.check.mark();
  }
  control_edges(read_buttons());
  // Paused: hold this frame, reading the controls, until play or one step (A; held, every fourth frame).
  if (pb.paused && !pb.go_back && pb.seek_target == INT32_MIN) {
    pb.a_held = 0;
    while (pb.paused && !pb.go_back && pb.seek_target == INT32_MIN) {
      sleep_ms(16);
      const uint16_t b = poll_buttons();
      const bool a = (b & kA) != 0;
      pb.a_held = a ? pb.a_held + 1 : 0;
      control_edges(b);
      if (a && (pb.a_held == 1 || (pb.a_held > 20 && pb.a_held % 4 == 0))) { say("replay: one frame forward from frame %d", frame); break; }
    }
  }
  if (pb.slow > 1 && !pb.fast && pb.seek_target == INT32_MIN) sleep_ms(17 * (pb.slow - 1));
  if (pb.seek_target != INT32_MIN || pb.go_back) linux_host::skip_draws(1);
  apply_speed();
  reply.push_back(pb.go_back ? 1 : 0);
  return 1;
}

int observe_search(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>&) { online_games = 0; return 0; }   // 0xB4: game numbers restart
int observe_rollback(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>&) { present::note_discontinuity(); return 0; }   // 0xB2: a rollback load

// ---- tables, menu, run end ----
void tables(MuHostApi* host, MuGameApi*) {
  if (!host) return;
  // --replay FILE|latest|N among the arguments the frontend did not take.
  for (size_t i = 0; i + 1 < options.rest.size(); ++i) if (options.rest[i] == "--replay") requested = options.rest[i + 1];
  if (!requested.empty()) {
    std::string path = requested;
    const auto list = list_replays();
    if (requested == "latest" || (requested.find_first_not_of("0123456789") == std::string::npos)) {
      const size_t index = requested == "latest" ? 0 : size_t(std::strtoul(requested.c_str(), nullptr, 10));
      path = index < list.size() ? dir() + "/" + list[index] : std::string();
    }
    if (!path.empty() && open_replay(path)) {
      playing = true;
      say("replay: playing back %s: Start pauses, A steps, D-pad Left/Right 5 s back/forward, Up speed, Down fast forward", path.c_str());
    } else say("replay: --replay %s: nothing to play", requested.c_str());
  }
  // The game records an offline VS match only when MELEE_SOURCE_RECORD is "1" (shim/mu_replay.c reads it with the
  // C library's getenv, which the loader binds to this program's); online matches are always recorded.
  setenv("MELEE_SOURCE_RECORD", (save_replays || spectator_wanted()) && !playing ? "1" : "0", 1);
  host->replay_start = h_replay_start;
  host->replay_frame = h_replay_frame;
  host->replay_stock_steal = h_replay_stock_steal;
  host->replay_event = h_replay_event;
  host->replay_finished = h_replay_finished;
  say("replay: recording %s to %s%s%s", save_replays ? "every match" : "off (savereplays 0)", dir().c_str(), compress_replays ? " as .slpz" : "",
      MU_HAVE_RECORDING_CODES ? "" : "; this build has no Slippi code list, recordings carry an empty one");
}

std::vector<std::string> menu_list;
size_t menu_pick = 0;
std::vector<std::string> pending_restart;
void run_end() {
  if (!playing) write_recording();
  else if (!pb.finished) say("replay: playback stopped at frame %d: %llu post-frame records compared, %llu differ", pb.frame,
                             static_cast<unsigned long long>(pb.compared), static_cast<unsigned long long>(pb.differ));
  snapshot_store::release("the run ended");
  options.pace = true;
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    writer_stop = true;
    write_cv.notify_all();
  }
  if (writer.joinable()) writer.join();
  writer_stop = false;
}
}  // namespace

bool replay_playing() { return playing; }
bool replay_word(int which, uint32_t* value) {
  if (!playing || which < 0 || which > 2) return false;
  *value = pb.words[which];
  return true;
}
bool replay_pad_hook(MuPadStatus*) { return false; }
std::vector<std::string> replays_list() { return list_replays(); }
bool take_restart_request(std::vector<std::string>* extra_args) {
  if (pending_restart.empty()) return false;
  *extra_args = std::move(pending_restart);
  pending_restart.clear();
  return true;
}

void register_replays() {
  add_setting(setting);
  add_tables(tables);
  add_command(0xFA, replay_gate);
  add_command(0xB4, observe_search);
  add_command(0xB2, observe_rollback);
  add_run_end(run_end);
  add_menu_entry({"Replays", "Replay",
                  [] {
                    if (menu_list.empty()) menu_list = list_replays();
                    return menu_list.empty() ? std::string("(none in ") + dir() + ")" : menu_list[std::min(menu_pick, menu_list.size() - 1)];
                  },
                  [](int d) {
                    menu_list = list_replays();
                    if (menu_list.empty()) return;
                    menu_pick = (menu_pick + menu_list.size() + size_t(d < 0 ? -1 : 1)) % menu_list.size();
                  }, nullptr});
  add_menu_entry({"Replays", "Watch it", nullptr, nullptr,
                  [] {
                    if (menu_list.empty()) menu_list = list_replays();
                    if (menu_list.empty()) return;
                    pending_restart = {"--replay", dir() + "/" + menu_list[std::min(menu_pick, menu_list.size() - 1)]};
                    say("replay: %s asked for; play mode starts the game again with it", pending_restart[1].c_str());
                  }});
  add_menu_entry({"Replays", "Compress replays (.slpz)", [] { return std::string(compress_replays ? "On" : "Off"); }, [](int) { compress_replays = !compress_replays; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("compressreplays", compress_replays ? "1" : "0"); }});
  add_menu_entry({"Replays", "Save every match", [] { return std::string(save_replays ? "On" : "Off"); }, [](int) { save_replays = !save_replays; setenv("MELEE_SOURCE_RECORD", (save_replays || spectator_wanted()) && !playing ? "1" : "0", 1); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("savereplays", save_replays ? "1" : "0"); }});
}
}  // namespace fe::features
