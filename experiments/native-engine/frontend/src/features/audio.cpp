// melee-unlocked's AUDIO page beyond the volume sliders, on the console's sound output:
//   - Audio buffer (settings.ini `audio_mode`, `audio_buffer_ms`, upstream's keys): Auto (0, the default) runs upstream's
//     AudioBufferPolicy (port/runtime/host/audio_buffer_policy.h, compiled from the fetched tree): the ring's slack grows by
//     4 ms after each real gap during a match (gaps longer than 60 ms are stalls, not a short buffer), settles back 1 ms per
//     10 s without one, never under 10 ms, and is remembered per output in <app folder>/audio-auto.txt (upstream:
//     %USERPROFILE%\.melee-unlocked\audio-auto-v3.txt, per endpoint), so the next session starts where this one settled.
//     The console's outputs are the handheld speaker and the TV (the dock); each keeps its own number. Low latency (1) keeps
//     `audio_buffer_ms` (5..120) fixed. The output side is patches/0117 (audio_set_buffer_ms in the build-time copy of
//     port/runtime/host/audio_sdl.cpp). Replaces the fixed 40 ms. Output buffering only: the game's mixer is not touched.
//   - Use custom music (`musicpacks`, off by default here): the game hands its songs to melee-unlocked's jukebox
//     (port/runtime/hle/jukebox.cpp from the fetched tree with patches/0118; commands 0xD6 play, 0xD7 stop, 0xD8 music
//     volume, 0xEB a stage's songs), as upstream's hosts play them, and a song with a file of the same name in
//     <app folder>/MusicPacks/audio/ (or a folder of them: a random pick each time) plays that file instead (MP3, WAV,
//     FLAC, OGG through src/media_decode.cpp; HPS). Tracks.txt and README.txt are written into the folder at start.
//     Songs are read where the game reads them (the retail disc on the jukebox's worker, a mod disc's file on the
//     simulation thread). Off: the game's own stream through AX, exactly as before (patches/0119 asks the host for it).
//     Music only: nothing here reaches the simulation, so it is allowed online (upstream's packs play locally too).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "audio_buffer_policy.h"     // melee-unlocked port/runtime/host (the fetched tree)

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

// The output (patches/0117, 0110) and melee-unlocked's jukebox (gen/jukebox/jukebox.cpp, patches/0118): weak, so the
// build without a picture (no sound output, no jukebox object) links too.
namespace host {
__attribute__((weak)) void audio_set_buffer_ms(int ms);
__attribute__((weak)) int audio_buffer_ms();
__attribute__((weak)) uint64_t audio_underruns(uint64_t* silent_ms);
}
namespace slippi::jukebox {
__attribute__((weak)) void start_song(uint32_t disc_offset, uint32_t size);
__attribute__((weak)) void stop();
__attribute__((weak)) void set_melee_volume(uint8_t volume);
__attribute__((weak)) void set_user_volume(int percent);
__attribute__((weak)) void set_next_song_gain(float gain);
__attribute__((weak)) void note_stage_songs(int main_song, int alternate_song);
__attribute__((weak)) void open_music_packs_folder();
__attribute__((weak)) void set_music_packs_enabled(bool enabled);
using DiscReader = bool (*)(uint32_t offset, void* dst, uint32_t size);
__attribute__((weak)) void set_disc_reader(DiscReader reader);
__attribute__((weak)) void mix(int16_t* out, size_t frames, double master, double output_rate, uint32_t device_rate);
__attribute__((weak)) void set_paused(bool paused);
}
namespace linux_host {   // melee-unlocked's host services (patches/0101): the disc table and the retail disc
int32_t fe_disc_find(const char* path, uint32_t* start, uint32_t* length);
bool fe_disc_read_base(uint32_t offset, void* out, uint32_t bytes);
}
extern "C" int mu_frontend_disc_read(uint64_t offset, void* out, size_t bytes);   // mods.cpp: mod and Slippi files

namespace fe::features {
namespace {
// ---- settings ----
std::atomic<int> g_mode{0};              // 0 Auto, 1 Low latency (upstream's 2 Exclusive and 3 ASIO: not on the console)
std::atomic<int> g_fixed_ms{30};         // Low latency's buffer (upstream sets 30 when Low latency is picked)
std::atomic<bool> g_music_packs{false};  // Use custom music (the host jukebox plays the songs)
std::atomic<bool> g_jukebox_on{false};   // this run's music goes through the jukebox (decided at the host's start)

void index_songs();

// ---- Auto ----
host::AudioBufferPolicy g_policy;
std::string g_output;                    // the output Auto's number belongs to
uint64_t g_seen_underruns = 0, g_seen_silent_ms = 0;
bool g_auto_started = false;
int g_applied = -1;
constexpr int kAutoFloor = 10, kAutoStart = 40;   // upstream AUTO_FLOOR_MS; 40: the console's earlier fixed cushion

std::string memory_path() { return options.out_dir + "/audio-auto.txt"; }
std::map<std::string, int> memory_load() {
  std::map<std::string, int> out;
  std::ifstream in(memory_path());
  std::string line;
  while (std::getline(in, line)) {
    const size_t tab = line.rfind('\t');
    if (tab == std::string::npos) continue;
    const int ms = std::atoi(line.c_str() + tab + 1);
    if (ms >= kAutoFloor && ms <= 120) out[line.substr(0, tab)] = ms;
  }
  return out;
}
void memory_save(const std::string& output, int ms) {
  if (output.empty() || !options.play) return;
  std::map<std::string, int> all = memory_load();
  if (all.count(output) && all[output] == ms) return;
  all[output] = ms;
  const std::string tmp = memory_path() + ".tmp";
  if (FILE* f = std::fopen(tmp.c_str(), "w")) {
    for (const auto& [k, v] : all) std::fprintf(f, "%s\t%d\n", k.c_str(), v);
    std::fclose(f);
    std::rename(tmp.c_str(), memory_path().c_str());
  }
}
// The output the sound goes to: on the console the handheld speaker or the TV through the dock.
std::string current_output() {
#ifdef __SWITCH__
  return appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld speaker" : "TV (dock)";
#else
  return "default output";
#endif
}
void apply(int ms) {
  if (ms == g_applied || !host::audio_set_buffer_ms) return;
  g_applied = ms;
  host::audio_set_buffer_ms(ms);
}
void auto_begin(const std::string& output) {
  g_output = output;
  g_policy.configure(0, kAutoFloor);
  const auto memory = memory_load();
  const auto it = memory.find(output);
  g_policy.target_ms = (uint32_t)std::clamp(it != memory.end() ? it->second : kAutoStart, kAutoFloor, 120);
  apply((int)g_policy.target_ms);
  char line[200];
  std::snprintf(line, sizeof line, "audio: buffer Auto for the %s: %u ms%s", output.c_str(), g_policy.target_ms,
                it != memory.end() ? " (what it settled on last time)" : " (nothing learned yet)");
  say("%s", line);
}

// Once per retrace (simulation thread): the gaps the output had since the last one.
void on_retrace(const Retrace& info) {
  static bool folder_written = false;   // the first retrace (the disc and a profile's layers are on): Tracks.txt, README.txt
  if (g_jukebox_on && !folder_written && slippi::jukebox::open_music_packs_folder) {
    folder_written = true; index_songs(); slippi::jukebox::open_music_packs_folder();
  }
  if (!host::audio_underruns || !host::audio_set_buffer_ms) return;
  if (g_mode.load() != 0) {
    if (!g_auto_started) { g_auto_started = true; apply(g_fixed_ms.load()); say("audio: buffer Low latency, %d ms", g_fixed_ms.load()); }
    return;
  }
  if (!g_auto_started) { g_auto_started = true; auto_begin(current_output()); }
  if (info.retrace % 300 == 0) {   // docked or undocked since: the other output's own number
    const std::string now = current_output();
    if (now != g_output) { memory_save(g_output, (int)g_policy.target_ms); auto_begin(now); }
  }
  uint64_t silent_ms = 0;
  const uint64_t underruns = host::audio_underruns(&silent_ms);
  const uint64_t gaps = underruns - g_seen_underruns, gap_ms = silent_ms - g_seen_silent_ms;
  g_seen_underruns = underruns; g_seen_silent_ms = silent_ms;
  bool grew = false;
  // Only gaps during a match grow it (upstream: in menus and while a scene loads the game itself stalls).
  if (gaps && info.fighting) {
    const uint64_t frames = gap_ms * 32 / gaps;   // each gap's length at the ring's 32 kHz
    for (uint64_t i = 0; i < std::min<uint64_t>(gaps, 4); ++i) grew |= g_policy.gap(std::max<uint64_t>(frames, 1), 32000);
  } else if (!gaps) {
    g_policy.clean(533, 32000);   // one 60 Hz frame of sound without a gap
  }
  if ((int)g_policy.target_ms != g_applied) {
    apply((int)g_policy.target_ms);
    if (grew) {
      char line[160];
      std::snprintf(line, sizeof line, "audio: Auto buffer %u ms after a gap in the match (retrace %u)", g_policy.target_ms, info.retrace);
      if (!log_queue_push(line)) say("%s", line);
    }
  }
}

void on_run_end() {
  if (g_mode.load() == 0 && g_auto_started) {
    memory_save(g_output, (int)g_policy.target_ms);
    say("audio: Auto buffer for the %s ends at %u ms (kept for the next session)", g_output.c_str(), g_policy.target_ms);
  }
  g_auto_started = false; g_applied = -1;
  g_seen_underruns = g_seen_silent_ms = 0;
}

// ---- the jukebox ----
// melee-unlocked's song files by id (jukebox.cpp kSongFiles; lbaudio_ax.c hps_files): a song's disc path by its offset.
const char* const kSongs[] = {
    "1p_qk", "akaneia", "baloon", "bigblue", "castle", "continue", "corneria", "docmari", "ending", "famidemo",
    "ff_1p01", "ff_1p02", "ff_bad", "ff_dk", "ff_emb", "ff_flat", "ff_fox", "ff_fzero", "ff_good", "ff_ice",
    "ff_kirby", "ff_link", "ff_mario", "ff_nes", "ff_poke", "ff_samus", "ff_step1", "ff_step2", "ff_step3", "ff_yoshi",
    "flatzone", "fourside", "gameover", "garden", "greatbay", "greens", "howto", "howto_s", "hyaku", "hyaku2",
    "icemt", "inis1_01", "inis1_02", "inis2_01", "inis2_02", "intro_es", "intro_nm", "item_h", "item_s", "izumi",
    "kongo", "kraid", "menu01", "menu02", "menu3", "mrider", "mutecity", "old_dk", "old_kb", "old_ys",
    "onetto", "onetto2", "opening", "pokesta", "pstadium", "pura", "rcruise", "s_info1", "s_info2", "s_info3",
    "s_new1", "s_new2", "s_newcom", "s_select", "saria", "shrine", "siren", "smari3", "sp_end", "sp_giga",
    "sp_metal", "sp_zako", "swm_15min", "target", "venom", "vl_battle", "vl_castle", "vl_corneria", "vl_cosmos", "vl_figure1",
    "vl_figure2", "vl_fzero", "vl_last_v2", "vs_hyou1", "vs_hyou2", "yorster", "ystory", "zebes", "testnz",
};
constexpr uint32_t kModBase = 0x70000000u;   // mods.cpp kBase: a mod disc's files are served from here up
std::mutex g_jmutex;
std::map<uint32_t, std::string> g_paths;      // disc offset -> "/audio/name.hps", refreshed on the simulation thread
std::map<uint32_t, std::vector<uint8_t>> g_preloaded;   // a mod disc's song, read on the simulation thread
std::atomic<uint32_t> g_songs{0};

void index_songs() {   // simulation thread: the disc table may change when a profile's layers go on
  std::map<uint32_t, std::string> paths;
  for (const char* name : kSongs) {
    const std::string path = std::string("/audio/") + name + ".hps";
    uint32_t start = 0, length = 0;
    if (linux_host::fe_disc_find(path.c_str(), &start, &length) >= 0 && length) paths[start] = path;
  }
  std::lock_guard<std::mutex> lock(g_jmutex);
  g_paths.swap(paths);
}

// The jukebox's worker reads a song through this.
bool jukebox_disc_read(uint32_t offset, void* dst, uint32_t size) {
  if (offset >= kModBase) {
    std::lock_guard<std::mutex> lock(g_jmutex);
    const auto it = g_preloaded.find(offset);
    if (it == g_preloaded.end() || it->second.size() < size) return false;
    std::memcpy(dst, it->second.data(), size);
    return true;
  }
  return linux_host::fe_disc_read_base(offset, dst, size);
}

int jukebox_command(uint8_t command, const uint8_t* p, uint32_t n, std::vector<uint8_t>&) {
  if (!g_jukebox_on || !slippi::jukebox::start_song) return 0;
  const auto be32 = [](const uint8_t* b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; };
  if (command == 0xD6 && n == 8) {
    const uint32_t offset = be32(p), size = be32(p + 4);
    index_songs();
    if (offset >= kModBase && size && size <= 64u * 1024 * 1024) {   // a mod disc's song: read here, where the game reads
      std::vector<uint8_t> bytes(size);
      if (mu_frontend_disc_read(offset, bytes.data(), size) == 1) { std::lock_guard<std::mutex> lock(g_jmutex); g_preloaded.clear(); g_preloaded[offset] = std::move(bytes); }
    }
    // 20XX TE "Reduce Dream Land volume" (offline, upstream source_host.cpp): its song 90/127 as loud.
    if (!online() && mods_te_owned() && (settings_te_word(2) & MU_GAME_OPTION2_TE_DL64_QUIET)) {
      std::lock_guard<std::mutex> lock(g_jmutex);
      const auto it = g_paths.find(offset);
      if (it != g_paths.end() && it->second == "/audio/old_kb.hps") slippi::jukebox::set_next_song_gain(90.0f / 127.0f);
    }
    if (g_songs.fetch_add(1) < 3) {
      std::string path;
      { std::lock_guard<std::mutex> lock(g_jmutex); const auto it = g_paths.find(offset); if (it != g_paths.end()) path = it->second; }
      say("audio: the game asks the jukebox for %s (disc offset %08X, %u bytes)", path.empty() ? "a song" : path.c_str(), offset, size);
    }
    slippi::jukebox::start_song(offset, size);
    return 1;
  }
  if (command == 0xD7) { slippi::jukebox::stop(); return 1; }
  if (command == 0xD8 && n == 1) { slippi::jukebox::set_melee_volume(p[0]); return 1; }
  if (command == 0xEB && n == 16) {   // a stage's main and alternate songs (shares 0xEB with upstream's tournament: 16 bytes only)
    slippi::jukebox::note_stage_songs((int32_t)be32(p), (int32_t)be32(p + 4));
    slippi::jukebox::note_stage_songs((int32_t)be32(p + 8), (int32_t)be32(p + 12));
    return 1;
  }
  return 0;
}

// ---- settings.ini and the panel ----
bool setting(const std::string& key, const std::string& value) {
  if (key == "audio_mode") { g_mode.store(std::clamp(std::atoi(value.c_str()), 0, 1)); return true; }
  if (key == "audio_buffer_ms") { g_fixed_ms.store(std::clamp(std::atoi(value.c_str()), 5, 120)); return true; }
  if (key == "musicpacks") { g_music_packs.store(value == "1"); return true; }
  if (key == "music" && slippi::jukebox::set_user_volume) { slippi::jukebox::set_user_volume(std::clamp(std::atoi(value.c_str()), 0, 100)); return false; }
  return false;
}
void persist_mode(MenuEntry::Write w) { w("audio_mode", std::to_string(g_mode.load())); w("audio_buffer_ms", std::to_string(g_fixed_ms.load())); }
void persist_music(MenuEntry::Write w) { w("musicpacks", g_music_packs.load() ? "1" : "0"); }

void register_rows() {
  add_menu_entry({"Audio", "Audio buffer",
    [] { return std::string(g_mode.load() == 0 ? "Auto" : "Low latency"); },
    [](int) {
      g_mode.store(g_mode.load() == 0 ? 1 : 0);
      g_auto_started = false;   // the next retrace applies the new mode
      if (g_mode.load() == 0 && !g_output.empty()) memory_save(g_output, (int)g_policy.target_ms);
    },
    nullptr, persist_mode});
  add_menu_entry({"Audio", "Audio buffer size",
    [] {
      char t[48];
      if (g_mode.load() == 0) std::snprintf(t, sizeof t, "Auto: %d ms", g_applied >= 0 ? g_applied : kAutoStart);
      else std::snprintf(t, sizeof t, "%d ms", g_fixed_ms.load());
      return std::string(t);
    },
    [](int d) {
      if (g_mode.load() == 0) return;   // Auto keeps its own number (upstream shows the slider in Low latency only)
      g_fixed_ms.store(std::clamp(g_fixed_ms.load() + d * 5, 5, 120));
      g_auto_started = false;
    },
    nullptr, persist_mode});
  add_menu_entry({"Audio", "Use custom music (next start)",
    [] { return std::string(g_music_packs.load() ? "On" : "Off"); },
    [](int) { g_music_packs.store(!g_music_packs.load()); },
    [] { g_music_packs.store(!g_music_packs.load()); },
    persist_music});
}
}  // namespace

// The replay viewer paused or jumping (replays.cpp): the jukebox's song stops where it is and goes on from there, as melee-unlocked's
// replay controls hold it (jukebox.h set_paused). The game's own music stops by itself then (its mixer runs with the game); the
// jukebox mixes in the sound output's callback, so it is held here.
void music_hold(bool hold) {
  static std::atomic<bool> held{false};
  if (!g_jukebox_on || !slippi::jukebox::set_paused || held.exchange(hold) == hold) return;
  slippi::jukebox::set_paused(hold);
  say("audio: the jukebox's music %s with the replay viewer", hold ? "stops" : "goes on");
}

void register_audio() {
  add_setting(setting);
  add_retrace(on_retrace);
  add_run_end(on_run_end);
  for (uint8_t c : {uint8_t(0xD6), uint8_t(0xD7), uint8_t(0xD8), uint8_t(0xEB)}) add_command(c, jukebox_command);
  register_rows();
}
}  // namespace fe::features

// ---- what the jukebox copy and the host ask for ----
namespace jukebox_switch {
std::filesystem::path music_packs_root() {
  if (fe::options.out_dir.empty()) return {};
  return std::filesystem::u8path(fe::options.out_dir + "/MusicPacks");
}
}  // namespace jukebox_switch

namespace host {
// host::log itself is the host services' (through the log thread while the game runs).
bool disc_read(uint32_t offset, void* dst, uint32_t size) { return fe::features::jukebox_disc_read(offset, dst, size); }
bool disc_find_path_by_offset(uint32_t offset, std::string* path) {
  std::lock_guard<std::mutex> lock(fe::features::g_jmutex);
  const auto it = fe::features::g_paths.find(offset);
  if (it == fe::features::g_paths.end()) return false;
  *path = it->second;
  return true;
}
bool disc_music_paths(std::vector<std::string>* paths) {
  std::lock_guard<std::mutex> lock(fe::features::g_jmutex);
  for (const auto& [offset, path] : fe::features::g_paths) paths->push_back(path);
  return !paths->empty();
}
}  // namespace host

// patches/0119: the host asks once at its start whether the game's music goes through the jukebox.
extern "C" int mu_frontend_jukebox(void) {
  using namespace fe::features;
  g_jukebox_on.store(fe::options.play && g_music_packs.load() && slippi::jukebox::start_song);
  if (!g_jukebox_on.load()) return 0;
  slippi::jukebox::set_disc_reader(jukebox_disc_read);
  slippi::jukebox::set_music_packs_enabled(true);
  fe::say("audio: Use custom music: the game's songs go through the jukebox; music packs in %s", jukebox_switch::music_packs_root().u8string().c_str());
  return 1;
}

// play_io.cpp: the output's device thread, after the game's sound (already at the player's volume) is in `out`.
extern "C" void fe_jukebox_mix(int16_t* out, size_t frames, int volume) {
  if (!fe::features::g_jukebox_on || !slippi::jukebox::mix) return;
  slippi::jukebox::mix(out, frames, volume / 100.0, 32000.0, 32000);
}

