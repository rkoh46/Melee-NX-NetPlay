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
// the viewer's (upstream's controller layer, gx/replay_hud.cpp, with L and R for its bar's speed buttons):
//   Start or Z        pause / play
//   D-pad Left/Right  5 seconds back / forward (paused: Right steps one frame; held, it keeps stepping)
//   L / R             speed 1x, 1/2, 1/4 / fast forward on, off
//   A                 take over the player named top left (D-pad Up/Down picks another)
//   X / Y             the last / next coach tip of that player (a second before it)
//   B                 hide or show the viewer's overlay
// Jumping back restores a state kept at the top of a frame (the hot-page snapshot store, runner/features/
// snapshot_store.*; one every 4 seconds, further apart for long matches so that 20 cover the replay), then plays
// forward unseen to the target, as upstream's viewer does. The overlay (overlay_draw.cpp, replay_view.h) shows where
// playback is, the speed, the tips on a timeline, the newest frame data reading and the takeover.
//
// Takeover (upstream: MuReplayFrame.live_ports, shim/mu_replay_defend.c, command 0xE9, host/replay_control.cpp): from
// the frame A is pressed, that port is played from the player's controller while the others keep the recording until
// the live game leaves it; from then on the game's Practice CPU model defends them (replay_opponent: smart defence, no
// DI or techs, the Practice CPU's settings, or follow the recording). The state at that frame is kept: Start retries
// from it, Start held goes back to watching from it. replay_try_length ends a try when the combo drops or after 10 or 20
// seconds (the result shows for two seconds, then the next try starts). Offline only: replays are offline.
//
// Coach (upstream host/replay_coach.*, host/replay_scan.*, shim/mu_framedata.c, command 0xE8): every hit, shield hit,
// aerial landing, tech and grab is read from the file at load, and measured again by the game as the frames play;
// the moments worth practising (unsafe on shield, dropped combos, missed L-cancels and techs) are the tips: a banner
// at the moment, markers on the timeline, X / Y to jump between them, A to take over there (practice from replays).
// Analyze (the Replays page while a replay plays) runs the rest of the replay unseen so the game measures every
// moment, then comes back.
//
// Fidelity check: the game's own post-frame records during playback are compared with the file's; the
// end of the run says how many differ (0 is the bar). Takeover frames are not compared.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "host_services.h"                      // melee-unlocked (fetched tree): linux_host
#include "mu_elf_bridge.h"                      // runner
#include "features/snapshot_store.h"            // runner/features
#include "features/present/present.h"
#include "replay_view.h"
#include "replay_coach.h"                     // melee-unlocked (fetched tree), port/runtime/host (replay_coach_glue.cpp)
#include "replay_scan.h"

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
#include <array>
#include <atomic>
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
    if (ok) net_trace_replay_saved(path);           // an online match's session trace, beside it (netoverlay.cpp)
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
constexpr int kKeepSlots = 26;                  // kept states: the jump-back grid (at most kGridStates) and a takeover's start
constexpr int32_t kGridStates = 20;
constexpr int32_t kJump = 300;                  // D-pad Left / Right: 5 seconds
constexpr int32_t kLead = 60;                   // a tip is jumped to a second before it happens
constexpr int32_t kDropGrace = 30, kNoHitLimit = 600;   // a try "until the combo drops": half a second after; nothing landed: 10 s
constexpr double kHoldBack = 0.6, kResultSeconds = 2.0;
constexpr uint16_t kStart = 0x1000, kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kZ = 0x0010, kL = 0x0040, kR = 0x0020,
                   kDL = 0x0001, kDR = 0x0002, kDD = 0x0004, kDU = 0x0008;

// Takeover and coach settings (settings.ini; the Replays page). Upstream's ReplaySettings (gx/practice_menu.h), same defaults.
int try_length = 3;                             // replay_try_length: 0 until the combo drops, 1 ten seconds, 2 twenty, 3 no limit
int opponent = 0;                               // replay_opponent: 0 smart defence, 1 no DI or techs, 2 the Practice CPU's settings, 3 follow the recording
bool tips_on = true;                            // replay_tips: tip banners and timeline markers

const char* const kCharacters[26] = {"Captain Falcon", "Donkey Kong", "Fox", "Mr. Game & Watch", "Kirby", "Bowser", "Link", "Luigi",
    "Mario", "Marth", "Mewtwo", "Ness", "Peach", "Pikachu", "Ice Climbers", "Jigglypuff", "Samus", "Yoshi", "Zelda", "Sheik",
    "Falco", "Young Link", "Dr. Mario", "Roy", "Pichu", "Ganondorf"};

enum Back { kBackJump, kBackRetry, kBackStop, kBackAnalyze };

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
  uint64_t compared = 0, differ = 0, live_records = 0; int32_t first_differ = INT32_MIN;
  std::map<std::tuple<int32_t, uint8_t, uint8_t>, bool> seen_differ;
  bool finished = false;
  int32_t first = Slippi::GAME_FIRST_FRAME, last = 0, keep_every = 240;
  uint32_t ports = 0;
  std::string names[4];                           // "P1 Fox"
  // Viewer controls.
  bool paused = false, fast = false, hidden = false;
  int slow = 1;                                   // 1, 2, 4
  int32_t seek_target = INT32_MIN;               // playing forward unseen to this frame
  bool go_back = false;
  int32_t back_target = 0;
  Back back_reason = kBackJump;
  int32_t frame = -123;                           // the next frame the game asked to play
  uint16_t last_buttons = 0;
  uint16_t shoulders = 0; bool shoulder_chord = false;   // L / R act when let go, unless they were part of a chord
  uint32_t step_held = 0;
  double last_input = 0;                          // when the viewer last pressed something
  int sel = 0;                                    // the player A takes over
  // Takeover (simulation thread).
  int want_take = -1;
  int take_port = -1;
  int32_t take_frame = INT32_MIN;
  int tries = 0;
  bool start_was = false, start_used = false; double start_at = 0;
  double ended_at = 0;                            // the try ended (its result shows until kResultSeconds later)
  std::string result_head, result_value, result_detail; int result_sign = 0;
  // Analyze: 1 playing forward unseen to the end, then back to where it was.
  int analyze = 0; int32_t analyze_origin = 0; bool analyzed = false;
} pb;
bool playing = false;
std::atomic<bool> repaint{false};
std::atomic<bool> unseen{false};               // frames on the way to a jump's target: not shown
std::atomic<bool> analyze_request{false};
std::atomic<int> analyze_state{0};             // the menu row: 0 not yet, 1 running, 2 done
std::mutex view_mutex;
replay::View view_base;                           // what replay_view() hands the drawer, published by the simulation thread

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

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

// ---- the coach (upstream host/replay_coach.*, host/replay_scan.*, compiled by replay_coach_glue.cpp) ----
bool coach_log() {
  static const bool on = feature_test("replay-tips") || feature_test("replay-takeover") || feature_test("replay-practice") ||
                         feature_test("replay-analyze") || feature_test("replay-coach-log");
  return on;
}
std::vector<replay_coach::Moment> tips_for(int port) {
  return replay_coach::moments(replay_coach::events(), port);
}
void scan_coach() {
  replay_coach::clear();
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<replay_coach::Event> found = replay_scan::scan(*pb.game);
  replay_coach::add_scanned(found);
  say("replay: coach: %zu frame data events read from the file (%.0f ms)", found.size(),
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  for (int port = 0; port < 4; ++port) {
    if (!(pb.ports >> port & 1)) continue;
    const auto list = tips_for(port);
    say("replay: coach: %s has %zu tips", pb.names[port].c_str(), list.size());
    for (size_t i = 0; i < list.size() && i < 6; ++i)
      say("replay: coach:   %s %s %s | %s", replay_coach::clock(list[i].frame).c_str(), list[i].what.c_str(), list[i].value.c_str(), list[i].advice.c_str());
  }
}
// 0xE8: one frame data event the game measured during playback (shim/mu_framedata.c).
int frame_event(uint8_t, const uint8_t* p, uint32_t n, std::vector<uint8_t>&) {
  replay_coach::Event e;
  if (!playing || !replay_coach::parse(p, n, e)) return 1;
  replay_coach::add(e);
  static int logged = 0;
  if (coach_log() && logged < 200 && (e.kind == replay_coach::Kind::Hit || e.kind == replay_coach::Kind::Shield)) {
    ++logged;
    const replay_coach::Readout r = replay_coach::readout(e);
    say("coach: frame %d P%d %s %s %s | %s", e.frame, e.actor + 1, r.head.c_str(), r.value.c_str(), r.detail.c_str(), replay_coach::advice(e).c_str());
  }
  return 1;
}
// 0xE9: a takeover's opponents once the live game left the recording (shim/mu_replay_defend.c): the Practice CPU's
// settings to defend with, or nothing to follow the recording.
int replay_defense(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>& reply) {
  if (!playing) return 0;
  reply = training_replay_defense(opponent);
  static int told = -1;
  if (told != opponent) { told = opponent; say("replay: the opponents %s (setting %d)", reply.empty() ? "follow the recording" : "defend themselves", opponent); }
  return 1;
}

// ---- what the drawer sees (replay_view.h) ----
void publish() {
  std::lock_guard<std::mutex> lock(view_mutex);
  replay::View& v = view_base;
  v.active = playing;
  v.hidden = pb.hidden;
  v.paused = pb.paused; v.fast = pb.fast; v.seeking = pb.seek_target != INT32_MIN; v.slow = pb.slow;
  v.frame = pb.frame; v.first = pb.first; v.last = pb.last;
  v.idle = now_s() - pb.last_input;
  v.live = pb.take_port >= 0;
  v.live_frame = pb.take_frame;
  v.port = v.live ? pb.take_port : pb.sel;
  v.name = pb.names[v.port & 3];
  v.ports = pb.ports;
  v.tips_on = tips_on;
  v.tries = pb.tries;
  const int32_t limit = try_length == 1 ? 600 : try_length == 2 ? 1200 : 0;
  v.seconds_left = v.live && limit ? std::max(0, (limit - (pb.frame - pb.take_frame) + 59) / 60) : -1;
  v.result = v.live && pb.ended_at > 0;
  v.result_head = pb.result_head; v.result_value = pb.result_value; v.result_detail = pb.result_detail; v.result_sign = pb.result_sign;
  v.next_try_in = v.result ? std::max(1, int(kResultSeconds - (now_s() - pb.ended_at) + 0.99)) : 0;
  v.analyzing = pb.analyze != 0;
  v.analyze_progress = pb.analyze ? std::clamp(float(pb.frame - pb.analyze_origin) / float(std::max(1, pb.last - 2 - pb.analyze_origin)), 0.0f, 1.0f) : 0.0f;
  repaint.store(playing && (pb.paused || pb.ended_at > 0), std::memory_order_relaxed);
}

// ---- the fidelity check and the replay's frames ----
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
    pb.ports |= 1u << i;
    pb.names[i] = "P" + std::to_string(i + 1) + " " + (ext < 26 ? kCharacters[ext] : "Fighter");
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
  pb.first = Slippi::GAME_FIRST_FRAME;
  pb.last = pb.game->GetLatestIndex();
  // Jump-back points cover the whole replay: one every 4 seconds, further apart for a long match, so that at most
  // kGridStates are kept and the store (kKeepSlots) never has to drop one (upstream keeps up to 50 for the same reason).
  pb.keep_every = std::max<int32_t>(240, (pb.last - pb.first + kGridStates) / kGridStates);
  for (int i = 0; i < 4; ++i) if (pb.ports >> i & 1) { pb.sel = i; break; }
  say("replay: %s, Slippi %s, frames %d..%d, stage %u, seed %08X, codes %08X (%zu others not native), options %08X %08X %08X, %zu post-frame records",
      path.c_str(), pb.game->GetVersionString().c_str(), pb.first, pb.last, unsigned(s->stage), s->randomSeed,
      pb.start.codes, unknown, pb.words[0], pb.words[1], pb.words[2], pb.post.size());
  if (s->stage > 85) say("replay: stage %u is not in the standard game: this replay needs the mod it was recorded with", unsigned(s->stage));
  scan_coach();
  return true;
}

const MuReplayStart* h_replay_start() { return playing ? &pb.start : nullptr; }
void h_replay_frame(int32_t frame, MuReplayFrame* out) {
  *out = MuReplayFrame{};
  if (!playing || !pb.game->DoesFrameExist(frame)) { out->result = MU_REPLAY_TERMINATE; return; }
  Slippi::FrameData* f = pb.game->GetFrame(frame);
  out->result = MU_REPLAY_CONTINUE;
  // Takeover: this port is played from the controller (shim/mu_replay.c mu_replay_input); the others keep the recording
  // until the live game leaves it (shim/mu_replay_defend.c).
  out->live_ports = pb.take_port >= 0 && pb.take_port < 4 ? 1u << pb.take_port : 0u;
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
  // During a takeover the game is not the recording: its frames are not compared (they come again, compared, once the
  // viewer goes back to watching from the takeover's frame).
  if (pb.take_port >= 0) { ++pb.live_records; return; }
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

void report_check(const char* how) {
  say("replay: playback %s at frame %d: %llu post-frame records compared with the file, %llu differ%s%s", how, pb.frame,
      static_cast<unsigned long long>(pb.compared), static_cast<unsigned long long>(pb.differ),
      pb.differ ? (" (first at frame " + std::to_string(pb.first_differ) + ")").c_str() : "",
      pb.live_records ? (" (" + std::to_string(pb.live_records) + " records of takeover frames not compared)").c_str() : "");
}
void h_replay_finished() {
  if (!playing) { write_recording(); return; }
  if (pb.finished) return;
  pb.finished = true;
  report_check(pb.take_port >= 0 ? "finished (during a takeover)" : "finished");
  snapshot_store::release("the replay ended");
  linux_host::request_stop(0);
}

// ---- the viewer: command 0xFA with 5 bytes, at the top of every frame of the replay's match ----
// Watching:  Start or Z pause / play   D-pad Left / Right 5 s back / forward (paused: Right steps one frame; held, repeats)
//            A take over the player shown top left (D-pad Up / Down picks another)   X / Y last / next tip
//            L speed 1, 1/2, 1/4   R fast forward   B hide or show the viewer's overlay
// Takeover:  everything is the fighter's, except Start: a tap retries from the takeover's frame, held: back to watching.
void apply_speed() {
  options.pace = !(pb.fast || pb.seek_target != INT32_MIN || pb.analyze);
}
void sleep_ms(int ms) { usleep(useconds_t(ms) * 1000); }

// --feature-test replay-controls|replay-tips|replay-takeover|replay-practice|replay-analyze: the viewer's buttons pressed
// by a timeline of its own (a controller script stops with the game while it is paused).
struct TestRun {
  std::string name;
  int stage = 0, release = 0, hold = 0;
  double t0 = 0;
  int32_t mark = 0;
  uint16_t holding = 0;
};
TestRun& test_run() {
  static TestRun t = [] {
    TestRun r;
    for (const char* n : {"replay-controls", "replay-tips", "replay-takeover", "replay-practice", "replay-analyze"}) if (feature_test(n)) r.name = n;
    return r;
  }();
  return t;
}
uint16_t test_buttons() {
  TestRun& t = test_run();
  if (t.name.empty()) return 0;
  if (t.hold > 0) { --t.hold; return t.holding; }
  if (t.release) { --t.release; return 0; }
  const double now = now_s();
  const bool seeking = pb.seek_target != INT32_MIN || pb.go_back;
  uint16_t b = 0;
  auto at = [&](bool when, uint16_t buttons) { if (when) { b = buttons; ++t.stage; t.t0 = now; t.mark = pb.frame; } };
  if (t.name == "replay-controls") {
    switch (t.stage) {
      case 0: at(pb.frame >= 60, kStart); break;                         // pause
      case 1: at(now - t.t0 > 0.4, kDR); break;                          // one frame
      case 2: at(now - t.t0 > 0.4, kDR); break;                          // one frame
      case 3: at(now - t.t0 > 0.4, kStart); break;                       // play
      case 4: at(pb.frame >= 240, kDL); break;                           // 5 s back
      case 5: at(!seeking && pb.frame >= 0 && pb.frame < t.mark - 100, kDR); break;   // 5 s forward
      case 6: at(!seeking && pb.frame >= 300, kL); break;                // 1/2
      case 7: at(pb.frame >= t.mark + 20, kL); break;                    // 1/4
      case 8: at(pb.frame >= t.mark + 10, kL); break;                    // 1
      case 9: at(pb.frame >= t.mark + 10, kR); break;                    // fast forward on
      case 10: at(pb.frame >= t.mark + 240, kR); break;                  // and off
      case 11: at(pb.frame >= t.mark + 60, kB); break;                   // overlay hidden
      case 12: at(pb.frame >= t.mark + 60, kB); break;                   // and shown
      default: break;
    }
  } else if (t.name == "replay-tips") {
    switch (t.stage) {
      case 0: at(pb.frame >= 30, kY); break;
      case 1: at(!seeking && pb.frame >= t.mark + 90 && !pb.paused, kY); break;
      case 2: at(!seeking && pb.frame >= t.mark + 90 && !pb.paused, kY); break;
      case 3: at(!seeking && pb.frame >= t.mark + 90 && !pb.paused, kX); break;
      default: break;
    }
  } else if (t.name == "replay-takeover") {
    switch (t.stage) {
      case 0: at(pb.frame >= 400, kA); break;                                              // take over P1
      case 1: at(pb.take_port >= 0 && pb.frame >= pb.take_frame + 240, kStart); break;       // tap: retry
      case 2: if (pb.take_port >= 0 && pb.tries >= 2 && pb.frame >= pb.take_frame + 180) { at(true, kStart); t.holding = kStart; t.hold = 60; } break;   // held: back
      default: break;
    }
  } else if (t.name == "replay-practice") {
    switch (t.stage) {
      case 0: at(pb.frame >= 30, kY); break;                                               // the first tip
      case 1: at(!seeking && pb.frame >= t.mark + 30, kA); break;                          // take over there
      case 2: if (pb.take_port >= 0 && (pb.tries >= 3 || pb.frame >= pb.take_frame + 1500)) { at(true, kStart); t.holding = kStart; t.hold = 60; } break;
      default: break;
    }
  } else if (t.name == "replay-analyze") {
    if (t.stage == 0 && pb.frame >= 300) { analyze_request.store(true); ++t.stage; say("replay: test asks for Analyze at frame %d", pb.frame); }
  }
  if (b) { t.release = 1; say("replay: test presses %04X at frame %d", b, pb.frame); }
  return b;
}
// The live player in the takeover tests: jabs (A for 4 frames every 20) and walks toward the middle now and then.
uint16_t test_live_buttons(int8_t* stick_x) {
  const std::string& n = test_run().name;
  if (n != "replay-takeover" && n != "replay-practice") return 0;
  static uint32_t tick = 0;
  ++tick;
  *stick_x = (tick / 60) % 4 == 3 ? int8_t(80) : 0;
  return tick % 20 < 4 ? kA : 0;
}

uint16_t read_buttons() {
  // The controllers as they were read this retrace, before this module neutralised them for the game.
  uint16_t b = test_buttons();
  for (int i = 0; i < 4; ++i) b |= pads_raw()[i].button;
  return b;
}
uint16_t poll_buttons() {
  MuPadStatus pads[4]{};
  read_controllers(pads);
  uint16_t b = test_buttons();
  for (const auto& p : pads) b |= p.button;
  return b;
}

int next_port(int from, int dir) {
  for (int i = 1; i <= 4; ++i) {
    const int p = (from + dir * i + 8) % 4;
    if (pb.ports >> p & 1) return p;
  }
  return from;
}

void request_back(Back reason, int32_t target) {
  pb.go_back = true;
  pb.back_target = std::max(target, pb.first);
  pb.back_reason = reason;
  pb.paused = false;
}
void jump_to(int32_t target, const char* why) {
  target = std::clamp(target, pb.first, pb.last - 2);
  if (target > pb.frame) { pb.seek_target = target; say("replay: forward to frame %d (%s)", target, why); }
  else if (target < pb.frame) { request_back(kBackJump, target); say("replay: back to frame %d (%s)", target, why); }
}
void tip_jump(int dir) {
  const auto list = tips_for(pb.sel);
  const replay_coach::Moment* pick = nullptr;
  int index = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    const auto& m = list[i];
    if (dir > 0 && m.frame - kLead > pb.frame + 5) { pick = &m; index = int(i); break; }
    if (dir < 0 && m.frame - kLead < pb.frame - 5) { pick = &m; index = int(i); }
  }
  if (!pick) { say("replay: no %s tip for %s", dir > 0 ? "later" : "earlier", pb.names[pb.sel].c_str()); return; }
  say("replay: tip %d of %zu for %s: %s %s at %s (frame %d)", index + 1, list.size(), pb.names[pb.sel].c_str(), pick->what.c_str(),
      pick->value.c_str(), replay_coach::clock(pick->frame).c_str(), pick->frame);
  jump_to(pick->frame - kLead, "tip");
}

// Watching. Returns true when, paused, one frame is let through.
bool watch_controls(uint16_t b, bool in_pause) {
  const uint16_t pressed = b & uint16_t(~pb.last_buttons);
  const uint16_t released = pb.last_buttons & uint16_t(~b);
  pb.last_buttons = b;
  if (pressed) pb.last_input = now_s();
  if ((b & (kL | kR | kStart)) == (kL | kR | kStart)) {      // the in-game menu's chord (or play mode's way out): not the viewer's
    pb.paused = false; pb.shoulder_chord = true; return false;
  }
  // L and R act when let go, unless something else was pressed while they were held (the menu's chord starts that way).
  if (pressed & (kL | kR)) { if (pb.shoulders) pb.shoulder_chord = true; pb.shoulders |= pressed & (kL | kR); }
  if (pb.shoulders && (pressed & ~(kL | kR))) pb.shoulder_chord = true;
  if (released & pb.shoulders) {
    const uint16_t let_go = released & pb.shoulders;
    if (!pb.shoulder_chord) {
      if (let_go & kL) { pb.slow = pb.slow == 1 ? 2 : pb.slow == 2 ? 4 : 1; say("replay: speed 1/%d", pb.slow); }
      if (let_go & kR) { pb.fast = !pb.fast; say("replay: fast forward %s", pb.fast ? "on" : "off"); }
    }
    pb.shoulders &= uint16_t(~let_go);
    if (!pb.shoulders) pb.shoulder_chord = false;
  }
  if (pressed & (kStart | kZ)) { pb.paused = !pb.paused; say("replay: %s at frame %d", pb.paused ? "paused" : "playing", pb.frame); }
  if (pressed & kB) { pb.hidden = !pb.hidden; say("replay: the viewer's overlay %s", pb.hidden ? "hidden" : "shown"); }
  if (pressed & kDU) pb.sel = next_port(pb.sel, -1);
  if (pressed & kDD) pb.sel = next_port(pb.sel, 1);
  if (pressed & (kDU | kDD)) say("replay: A takes over %s", pb.names[pb.sel].c_str());
  if (pressed & kA) pb.want_take = pb.sel;
  if (pressed & kY) tip_jump(1);
  if (pressed & kX) tip_jump(-1);
  if (pressed & kDL) jump_to(pb.frame - kJump, "5 s back");
  bool step = false;
  if (in_pause) {
    pb.step_held = (b & kDR) ? pb.step_held + 1 : 0;
    if (pb.step_held == 1 || (pb.step_held > 20 && pb.step_held % 4 == 0)) { step = true; say("replay: one frame forward from frame %d", pb.frame); }
  } else if (pressed & kDR) {
    jump_to(pb.frame + kJump, "5 s forward");
  }
  return step;
}

// Takeover: Start tapped retries, held goes back to watching (both from the takeover's frame).
void live_controls(uint16_t b) {
  const double now = now_s();
  bool start = (b & kStart) != 0;
  if (start && (b & (kL | kR)) == (kL | kR)) { pb.start_used = true; }   // the in-game menu's chord: not a retry
  if (start && !pb.start_was) { pb.start_at = now; pb.start_used = (b & (kL | kR)) == (kL | kR); }
  if (start && !pb.start_used && now - pb.start_at >= kHoldBack) {
    pb.start_used = true;
    say("replay: back to watching from frame %d", pb.take_frame);
    request_back(kBackStop, pb.take_frame);
  }
  if (!start && pb.start_was && !pb.start_used) {
    say("replay: takeover retried from frame %d (try %d)", pb.take_frame, pb.tries + 1);
    request_back(kBackRetry, pb.take_frame);
  }
  pb.start_was = start;
}

// The live player's combo in this try, as the game measured it (not read from the file): hits, and the frame the
// opponent could act again after the last one.
struct Combo { int hits = 0; int32_t free_at = 0; };
Combo combo_of(int port, int32_t from, int32_t to) {
  Combo c;
  int32_t last = INT32_MIN;
  for (const replay_coach::Event& e : replay_coach::events()) {
    if (e.from_file || e.actor != port || e.kind != replay_coach::Kind::Hit || e.frame < from || e.frame > to) continue;
    ++c.hits;
    if (e.frame >= last) { last = e.frame; c.free_at = e.frame + (e.free_at > 0 ? e.free_at : 30); }
  }
  return c;
}
void capture_result(int port, int32_t from, int32_t to) {
  pb.result_head = "Try " + std::to_string(pb.tries);
  pb.result_value.clear(); pb.result_detail.clear(); pb.result_sign = 0;
  replay_coach::Event last{}; bool have = false;
  for (const replay_coach::Event& e : replay_coach::events()) {
    if (e.from_file || e.frame < from || e.frame > to || e.actor != port) continue;
    if (e.kind == replay_coach::Kind::Hit || e.kind == replay_coach::Kind::Shield) { last = e; have = true; }
  }
  if (have) {
    const replay_coach::Readout r = replay_coach::readout(last);
    pb.result_value = r.head + "  " + r.value;
    pb.result_sign = r.sign;
    pb.result_detail = r.detail;
  }
}
// After each live frame's gate: has the try ended (the combo dropped, or its time is up)?
void try_check(int32_t frame) {
  if (pb.take_port < 0 || pb.ended_at > 0 || pb.go_back) return;
  if (frame >= pb.last - 2) { say("replay: the replay ends at frame %d: the takeover starts again", pb.last); request_back(kBackRetry, pb.take_frame); return; }
  const int32_t into = frame - pb.take_frame;
  const Combo combo = combo_of(pb.take_port, pb.take_frame, frame);
  const int32_t limit = try_length == 1 ? 600 : try_length == 2 ? 1200 : 0;
  const bool over = try_length == 0 ? (combo.hits ? frame >= combo.free_at + kDropGrace : into >= kNoHitLimit) : (limit && into >= limit);
  if (!over) return;
  pb.ended_at = now_s();
  capture_result(pb.take_port, pb.take_frame, frame);
  pb.paused = true;
  say("replay: try %d ends at frame %d (%d hits%s%s), back to frame %d", pb.tries, frame, combo.hits, pb.result_value.empty() ? "" : ": ",
      pb.result_value.c_str(), pb.take_frame);
}

// Top of frame `frame`: a takeover asked for starts here, from a state kept now (Retry and Back return to it).
void takeover_begin(int32_t frame) {
  const int port = pb.want_take;
  pb.want_take = -1;
  if (port < 0 || pb.analyze) return;
  if (!snapshot_store::has(frame)) {
    if (!snapshot_store::keep(frame, kKeepSlots, "replay")) { say("replay: cannot keep the state at frame %d: no takeover", frame); return; }
    pb.marks[frame] = pb.check.mark();
  }
  pb.take_port = port; pb.take_frame = frame; pb.tries = 1;
  pb.ended_at = 0; pb.start_was = true; pb.start_used = true;   // a Start already held is not a retry
  pb.paused = false; pb.fast = false; pb.slow = 1;
  pb.last_buttons = 0xFFFF;
  say("replay: takeover of %s from frame %d (Start: retry, hold Start: back to watching; opponents: %s)", pb.names[port].c_str(), frame,
      opponent == 0 ? "smart defence" : opponent == 1 ? "no DI or techs" : opponent == 2 ? "the Practice CPU's settings" : "follow the recording");
}

void keep_if_due(int32_t frame) {
  // A state now and then, where a frame starts (the jump-back points): the first frame the game asks about and then
  // every keep_every frames.
  if ((!snapshot_store::running() || (frame - pb.first) % pb.keep_every == 0) && !snapshot_store::has(frame)) {
    if (snapshot_store::keep(frame, kKeepSlots, "replay")) pb.marks[frame] = pb.check.mark();
  }
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
      present::note_discontinuity();
      answer = 2;
      switch (pb.back_reason) {
        case kBackRetry:
          replay_coach::forget_from(pb.take_frame);
          ++pb.tries; pb.ended_at = 0; pb.start_was = false; pb.start_used = false;
          say("replay: try %d of %s from frame %d", pb.tries, pb.names[pb.take_port].c_str(), pb.take_frame);
          break;
        case kBackStop:
          replay_coach::forget_from(pb.take_frame);
          pb.last_buttons = 0xFFFF;               // what is still held (the Start that went back) is not a press for the viewer
          say("replay: watching again from frame %d after %d tr%s", pb.take_frame, pb.tries, pb.tries == 1 ? "y" : "ies");
          pb.take_port = -1; pb.take_frame = INT32_MIN; pb.tries = 0; pb.ended_at = 0;
          break;
        case kBackAnalyze:
          pb.analyze = 0; pb.analyzed = true; analyze_state.store(2);
          say("replay: analyzed: %zu frame data events; back at frame %d", replay_coach::events().size(), pb.back_target);
          break;
        default: say("replay: back to frame %d (kept state %d)", pb.back_target, key); break;
      }
    }
    if (!answer) {
      say("replay: no kept state at or before frame %d", pb.back_target);
      if (pb.back_reason == kBackAnalyze) { pb.analyze = 0; analyze_state.store(0); }
    }
    pb.go_back = false;
    apply_speed();
    publish();
    reply.push_back(answer);
    return 1;
  }
  pb.frame = frame;
  if (pb.seek_target != INT32_MIN && frame >= pb.seek_target) { pb.seek_target = INT32_MIN; say("replay: at frame %d", frame); }
  keep_if_due(frame);
  // Analyze (the Replays page, or the test): run to the end unseen so the game measures every moment, then come back.
  if (analyze_request.exchange(false) && !pb.analyze && pb.take_port < 0) {
    pb.analyze = 1; pb.analyze_origin = frame; pb.paused = false;
    pb.seek_target = std::max(frame + 1, pb.last - 2);
    analyze_state.store(1);
    say("replay: analyzing from frame %d to %d", frame, pb.seek_target);
  }
  if (pb.analyze == 1 && pb.seek_target == INT32_MIN) { pb.analyze = 2; request_back(kBackAnalyze, pb.analyze_origin); }
  if (!pb.analyze) {
    const uint16_t b = read_buttons();
    if (pb.take_port >= 0) live_controls(b); else watch_controls(b, false);
    if (pb.want_take >= 0) takeover_begin(frame);
    try_check(frame);
  }
  // Paused (or a try's result showing): hold this frame, reading the controls, until play, one step, a jump or a takeover.
  if (pb.paused && !pb.go_back && pb.seek_target == INT32_MIN) {
    pb.step_held = 0;
    const double paused_at = now_s();
    while (pb.paused && !pb.go_back && pb.seek_target == INT32_MIN && pb.want_take < 0) {
      publish();
      sleep_ms(16);
      // A test run never waits for a controller that is not there: after 20 s paused it plays on.
      if (!test_run().name.empty() && now_s() - paused_at > 20) { say("replay: test: paused for 20 s at frame %d, playing on", frame); pb.paused = false; break; }
      const uint16_t b = poll_buttons();
      if (pb.take_port >= 0) {
        live_controls(b);
        if (pb.ended_at > 0 && now_s() - pb.ended_at >= kResultSeconds && !pb.go_back) request_back(kBackRetry, pb.take_frame);
      } else if (watch_controls(b, true)) {
        break;
      }
    }
    if (pb.want_take >= 0) takeover_begin(frame);
  }
  if (pb.slow > 1 && !pb.fast && pb.seek_target == INT32_MIN && !pb.go_back) sleep_ms(17 * (pb.slow - 1));
  // Frames on the way to a jump's target are simulated with their draw pass, as on any other frame: an undrawn frame
  // is not exact with items on (README "Undrawn repeated frames"; an Analyze pass over a whole match differed from frame
  // 1866 on). The picture just is not shown (replay_frames_unseen, the render loop).
  unseen.store(pb.seek_target != INT32_MIN || pb.go_back, std::memory_order_relaxed);
  apply_speed();
  publish();
  reply.push_back(pb.go_back ? 1 : 0);
  return 1;
}

// The pads the game reads during playback: none of the controllers' presses reach it (the fighters play the
// recording; Start or L + R + A + Start would pause or end the match), except the taken-over port's, which gets the
// player's controller (the port's own, else the first connected one), without Start (the viewer's: retry / back).
bool pad_hook(MuPadStatus pads[4]) {
  if (!playing) return false;
  MuPadStatus source{}; bool have = false;
  const int port = pb.take_port;
  if (port >= 0 && port < 4) {
    if (pads[port].err == 0) { source = pads[port]; have = true; }
    for (int i = 0; i < 4 && !have; ++i) if (pads[i].err == 0) { source = pads[i]; have = true; }
  }
  for (int i = 0; i < 4; ++i) { const int8_t err = pads[i].err; pads[i] = MuPadStatus{}; pads[i].err = err; }
  if (have) {
    int8_t sx = 0;
    const uint16_t extra = test_live_buttons(&sx);
    source.button = uint16_t((source.button | extra) & ~kStart);
    if (sx) source.stick_x = sx;
    source.err = 0;
    pads[port] = source;
  }
  return true;
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
      pb.last_input = now_s();
      publish();
      say("replay: playing back %s: Start/Z pause, D-pad Left/Right 5 s (paused: Right steps), L speed, R fast forward, A take over "
          "(D-pad Up/Down: who), X/Y last/next tip, B hides the overlay", path.c_str());
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

std::vector<std::string> pending_restart;
void run_end() {
  if (!playing) write_recording();
  else if (!pb.finished) report_check("stopped");
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

bool setting_takeover(const std::string& key, const std::string& value) {
  const int n = std::atoi(value.c_str());
  if (key == "replay_try_length") try_length = std::clamp(n, 0, 3);
  else if (key == "replay_opponent") opponent = std::clamp(n, 0, 3);
  else if (key == "replay_tips") tips_on = value != "0";
  else return false;
  return true;
}
const char* const kLengths[4] = {"Until the combo drops", "10 seconds", "20 seconds", "No limit"};
const char* const kOpponents[4] = {"Smart defence", "No DI or techs", "Practice CPU settings", "Follow the recording"};
}  // namespace

bool replay_playing() { return playing; }
bool replay_word(int which, uint32_t* value) {
  if (!playing || which < 0 || which > 2) return false;
  *value = pb.words[which];
  return true;
}
bool replay_pad_hook(MuPadStatus* pads) { return pad_hook(pads); }
std::vector<std::string> replays_list() { return list_replays(); }
std::string replay_folder() { return dir(); }
void replay_watch(const std::string& path) {
  pending_restart = {"--replay", path};
  say("replay: %s asked for; play mode starts the game again with it", path.c_str());
}
bool take_restart_request(std::vector<std::string>* extra_args) {
  if (pending_restart.empty()) return false;
  *extra_args = std::move(pending_restart);
  pending_restart.clear();
  return true;
}
void replay_view(replay::View& out) {
  {
    std::lock_guard<std::mutex> lock(view_mutex);
    out = view_base;
  }
  if (!out.active) return;
  // The shown player's tips (cached until the coach has more events or another player is shown) and the newest reading.
  static std::mutex cache_mutex;
  static std::vector<replay::Tip> cache;
  static size_t cache_events = size_t(-1);
  static int cache_port = -1;
  const std::vector<replay_coach::Event> all = replay_coach::events();
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (cache_port != out.port || cache_events != all.size()) {
      cache.clear();
      for (const auto& m : replay_coach::moments(all, out.port))
        cache.push_back({m.frame, m.what, m.value, m.advice, m.sign, int(m.type)});
      cache_port = out.port; cache_events = all.size();
    }
    out.tips = cache;
  }
  replay_coach::Event e; double age = 0;
  // Live: only what happened in this try, never a reading left over from the recording.
  if (replay_coach::latest(e, age) && age < 5.0 && (!out.live || (e.frame >= out.live_frame && !e.from_file))) {
    const replay_coach::Readout r = replay_coach::readout(e);
    out.readout = true; out.readout_age = age; out.head = r.head; out.value = r.value; out.detail = r.detail; out.sign = r.sign;
  }
}
bool replay_repaint_wanted() { return repaint.load(std::memory_order_relaxed); }
bool replay_frames_unseen() { return playing && unseen.load(std::memory_order_relaxed); }

void register_replays() {
  register_replay_library();   // the list, Watch it, stats and delete first on the Replays page
  add_setting(setting);
  add_setting(setting_takeover);
  add_tables(tables);
  add_command(0xFA, replay_gate);
  add_command(0xE8, frame_event);
  add_command(0xE9, replay_defense);
  add_command(0xB4, observe_search);
  add_command(0xB2, observe_rollback);
  add_run_end(run_end);
  add_menu_entry({"Replays", "Takeover: a try lasts", [] { return std::string(kLengths[try_length]); },
                  [](int d) { try_length = (try_length + 4 + (d < 0 ? -1 : 1)) % 4; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("replay_try_length", std::to_string(try_length)); }});
  add_menu_entry({"Replays", "Takeover: the other players", [] { return std::string(kOpponents[opponent]); },
                  [](int d) { opponent = (opponent + 4 + (d < 0 ? -1 : 1)) % 4; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("replay_opponent", std::to_string(opponent)); }});
  add_menu_entry({"Replays", "Coach tips", [] { return std::string(tips_on ? "On" : "Off"); }, [](int) { tips_on = !tips_on; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("replay_tips", tips_on ? "1" : "0"); }});
  add_menu_entry({"Replays", "Analyze this replay (while one plays)", [] { const int a = analyze_state.load(); return std::string(!playing ? "-" : a == 2 ? "Done" : a == 1 ? "Running" : ""); },
                  nullptr, [] { if (playing) analyze_request.store(true); }});
  add_menu_entry({"Replays", "Compress replays (.slpz)", [] { return std::string(compress_replays ? "On" : "Off"); }, [](int) { compress_replays = !compress_replays; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("compressreplays", compress_replays ? "1" : "0"); }});
  add_menu_entry({"Replays", "Save every match", [] { return std::string(save_replays ? "On" : "Off"); }, [](int) { save_replays = !save_replays; setenv("MELEE_SOURCE_RECORD", (save_replays || spectator_wanted()) && !playing ? "1" : "0", 1); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("savereplays", save_replays ? "1" : "0"); }});
}
}  // namespace fe::features
