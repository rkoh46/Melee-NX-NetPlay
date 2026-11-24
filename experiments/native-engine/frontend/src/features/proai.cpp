// Fight a pro and the Practice menu, as melee-unlocked has them (its host: port/app/source_host.cpp CMD_GHOST_AI,
// port/runtime/hle/ghost_ai.cpp and phillip_ai.cpp, hle/practice_cpu.cpp, gx/practice_menu.cpp and pc_settings.cpp's
// Practice menu frame; its game: shim/mu_practice_cpu.c ghost_drive, shim/mu_pro_tag_robot.c). The game does the
// fighting: in an offline Training (or any offline) match with the Practice CPU on, a CPU playing Falco, Marth, Fox or
// Yoshi asks the host each frame (command 0xEA, abi/mu_ghost_ai.h) for the controller Phillip would hold; it shows the
// pro's name and robot on the CPU's tag by itself. This module:
//   - answers 0xEA through upstream's phillip_ai.cpp (patches/0112) on the app's own inference engine (src/proai/), never
//     online, never during replay playback, never in a vanilla session (the game then plays its Practice CPU);
//   - the ProAI folder (sdmc:/switch/melee-nx-netplay/ProAI/, or <out>/ProAI off the console), its catalog.json (the
//     app's own, src/proai/catalog.json, written there when missing or older) and the downloads the player asks for
//     (the services program's libcurl, src/proai/proai_fetch_curl.cpp; each file's SHA-256 checked); a manual copy of
//     the .onnx files into that folder works too;
//   - upstream's practice_cpu, ghost_ai and native_practice calls the Practice menu makes (the Practice CPU is
//     training.cpp's; "Training again with the CPU as the pro" is training_go);
//   - the Practice menu's simulation-thread half: Start + D-pad Up + Z in offline Training opens and closes it (the
//     chord's other uses stay in controls.cpp), the controller that opened it drives it and the game gets neutral pads
//     meanwhile, what is held when it closes is kept from the game until let go, and its settings are written to
//     settings.ini when it closes.
// settings.ini keys (upstream's): practicestyle (0 Main Menu bars, 1 Side rails, 2 Character select slots, 3 HUD strip),
// practiceopens, matchmakinghint (the corner hint), proaistronger (0 ask, 1 always, 2 never); and proai_threads (1..3,
// the policy's Gemm threads; default 1).
// SPDX-License-Identifier: GPL-3.0-or-later
#define MU_PROAI_FRONTEND 1
#include "features_internal.h"
#include "frontend.h"
#include "proai/practice_state.h"

#include "mu_ghost_ai.h"          // melee-unlocked (fetched tree)
#include "mu_practice_cpu.h"
#include "practice_cpu.h"         // upstream's interface; implemented here
#include "practice_ghosts.h"      // upstream's habit tables (data)
#include "ghost_ai.h"             // upstream's interface; the part the menu calls is implemented here
#include "proai/phillip_ai.h"     // build-time copy with patches/0112 (build/native-engine-gfx/gen/proai)
#include "proai/include/slippi_online.h"     // the Practice menu's view of these (src/proai/include)
#include "proai/include/native_practice.h"

#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe::features {
MuPcConfig training_pc();
void training_pc_set(const MuPcConfig& c);
bool training_pc_slot(int index, MuPcConfig* out);
void training_pc_set_slot(int index, const MuPcConfig& c);
void training_pc_status(bool* driving, int* plan, uint32_t* frame, int* kind);
void training_pc_persist(void (*write)(const std::string&, const std::string&));
}

// The services program's download (src/proai/proai_fetch_curl.cpp); absent in the other programs.
__attribute__((weak)) bool fe_proai_fetch_verified(const std::string& url, const std::string& sha256, const std::string& dest,
                                                   std::atomic<uint64_t>* progress, std::string* why);
extern const char fe_proai_catalog_json[];   // src/proai/catalog.json, embedded (CMakeLists.txt)

namespace fe::features {
namespace {
constexpr uint32_t kTrainingMajor = 0x1C;

// ---- settings ----
std::atomic<int> style{0}, opens{0}, threads{1};
std::atomic<bool> hint{true};

bool setting(const std::string& key, const std::string& value) {
  const int n = std::atoi(value.c_str());
  if (key == "practicestyle") style = n >= 0 && n < 4 ? n : 0;   // upstream's 4 (the window, for the mouse) is not here
  else if (key == "practiceopens") opens = std::clamp(n, 0, 99);
  else if (key == "matchmakinghint") hint = value != "0";
  else if (key == "proaistronger") phillip_ai::set_stronger_choice(std::clamp(n, 0, 2));
  else if (key == "proai_threads") threads = std::clamp(n, 1, 3);
  else return false;
  return true;
}

// ---- the ProAI folder and its catalog ----
std::string dir() {
  std::string base = options.app && !options.base_dir.empty() ? options.base_dir : options.out_dir;
  if (const char* v = std::getenv("MELEE_PRO_AI_DIR")) base = v;   // tests
  if (!base.empty() && base.back() != '/') base += '/';
  return base + "ProAI/";
}
// The app's catalog in the folder when there is none, or when it is not the one this build carries (a newer app, or an
// older catalog left there): the policies already downloaded keep their names.
void ensure_catalog() {
  static std::once_flag once;
  std::call_once(once, [] {
    const std::string d = dir();
    mkdir(d.substr(0, d.size() - 1).c_str(), 0777);
    const std::string path = d + "catalog.json";
    std::ifstream in(path, std::ios::binary);
    std::stringstream have;
    if (in) have << in.rdbuf();
    if (have.str() == fe_proai_catalog_json) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << fe_proai_catalog_json;
    say("pro ai: catalog written to %s", path.c_str());
  });
}

// ---- commands ----
std::atomic<uint32_t> asked{0}, answered{0};
int ghost_ai(uint8_t, const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  if (n != sizeof(MuGhostObs)) return 0;
  ++asked;
  // Offline only (upstream: Training and offline matches); never in a replay or a vanilla session. No reply: the game's
  // Practice CPU plays the fighter.
  if (online() || replay_playing() || vanilla()) return 1;
  ensure_catalog();
  MuGhostObs obs;
  std::memcpy(&obs, p, sizeof obs);
  MuGhostAct act;
  if (!phillip_ai::act(obs, act)) return 1;
  ++answered;
  static const bool log_answers = std::getenv("MU_PROAI_LOG") != nullptr;   // tests: what Phillip holds, now and then
  if (log_answers && answered % 30 == 0)
    say("pro ai: frame %u P%u pro %u: stick %.2f %.2f c-stick %.2f %.2f L %.2f buttons %08X (self motion %u at %.1f,%.1f, opponent motion %u)",
        obs.frame, obs.slot + 1u, unsigned(obs.pro), act.lx, act.ly, act.cx, act.cy, act.l, act.buttons, obs.self.motion,
        obs.self.num[0], obs.self.num[1], obs.opp.motion);
  const auto* bytes = reinterpret_cast<const uint8_t*>(&act);
  reply.assign(bytes, bytes + sizeof act);
  return 1;
}

// ---- the Practice menu's simulation-thread half ----
std::mutex menu_mutex;
struct Menu {
  bool open = false, armed = false, just_opened = false;
  int port = 0;
  uint16_t buttons = 0;
  int8_t sx = 0, sy = 0;
  bool training = false;
  uint32_t unlocked = 0xFFFFFFFFu;
  bool close_asked = false, dirty = false;
  uint16_t swallow[4] = {};
} menu;

// "Training again with the CPU as the pro" (upstream native_practice TrainAgainst): acted on at the next retrace.
std::mutex train_mutex;
int train_character = -1, train_costume = 0;

void persist() {
  training_pc_persist([](const std::string& k, const std::string& v) { settings_write_key(k, v); });
  settings_write_key("practicestyle", std::to_string(style.load()));
  settings_write_key("practiceopens", std::to_string(opens.load()));
  settings_write_key("matchmakinghint", hint ? "1" : "0");
  settings_write_key("proaistronger", std::to_string(phillip_ai::stronger_choice()));
  for (int i = 0; i < 3; ++i) {
    const std::string key = "blockcode" + std::to_string(i + 1);
    const std::string& v = slippi::online::config().blocked_codes[i];
    if (settings_get(key) != v) settings_change(key, v);
  }
}

void retrace(const Retrace& info) {
  // The Training match itself (minor 2), not its character and stage selects.
  const bool training = !online() && !replay_playing() && !vanilla() && info.scene_major == kTrainingMajor && info.scene_minor == 2;
  uint32_t unlocked = 0xFFFFFFFFu;
  if (training) {
    static uint32_t asked_scene = ~0u, have = 0xFFFFFFFFu;
    if (asked_scene != info.scene_major * 256 + info.scene_minor) {
      asked_scene = info.scene_major * 256 + info.scene_minor;
      if (MuGameApi* g = game(); g && g->practice) { int32_t a[1] = {0}; if (g->practice(MU_PRACTICE_UNLOCKED, a, 1) == 0) have = uint32_t(a[0]); }
    }
    unlocked = have;
  }
  bool write = false;
  {
    std::lock_guard<std::mutex> lock(menu_mutex);
    menu.training = training;
    menu.unlocked = unlocked;
    if (menu.open && !training) { menu.open = false; say("practice menu: closed (left Training)"); }
    if (!menu.open && menu.dirty) { menu.dirty = false; write = true; }
  }
  if (write) persist();
  int character = -1, costume = 0;
  { std::lock_guard<std::mutex> lock(train_mutex); std::swap(character, train_character); costume = train_costume; }
  if (character >= 0 && training && info.scene_minor == 2) {
    // The player's own fighter and colours stay; the CPU takes the pro's character (the next colours when they match).
    int mine = -1, mine_costume = 0, cpu = -1;
    if (MuGameApi* g = game(); g && g->practice)
      for (int slot = 0; slot < 4; ++slot) {
        int32_t a[9] = {slot};
        if (g->practice(MU_PRACTICE_GET_PLAYER, a, 9) != 0 || a[1] == 3 || a[2] < 0 || a[2] >= 26) continue;
        if (a[3] == 0 && mine < 0) { mine = a[2]; mine_costume = a[4]; }
        else if (a[3] == 1 && cpu < 0) cpu = a[2];
      }
    const int costumes = character == 9 ? 5 : character == 17 ? 6 : 4;   // upstream: Marth 5, Yoshi 6, Fox 4 colours
    if (costume < 0 || costume >= costumes) costume = 0;
    if (mine == character && mine_costume == costume) costume = (costume + 1) % costumes;
    if (cpu == character) { say("practice menu: the CPU already plays character %d: the match goes on", character); return; }
    say("practice menu: Training again with the CPU as character %d, costume %d", character, costume);
    training_go(mine, mine_costume, character, costume, false);
  }
}

// Where the match stands every ten seconds while Phillip plays (the log's proof that the pro fights): percent, stocks
// and position of the first two players.
void pro_note(const Retrace& info) {
  static uint32_t last_answered = 0, last_retrace = 0;
  if (!info.state || !info.fighting || info.retrace - last_retrace < 600) return;
  last_retrace = info.retrace;
  const uint32_t now = answered.load();
  if (now == last_answered) return;
  const auto f = [](uint32_t bits) { float v; std::memcpy(&v, &bits, 4); return v; };
  const MuFighterState& a = info.state->player[0];
  const MuFighterState& b = info.state->player[1];
  say("pro ai: match frame %u: P1 %.0f%% %u stocks at %.0f,%.0f; P2 %.0f%% %u stocks at %.0f,%.0f (action %u); %u answers since the last line",
      info.match_frame, f(a.percent), a.stocks, f(a.pos_x), f(a.pos_y), f(b.percent), b.stocks, f(b.pos_x), f(b.pos_y), b.action, now - last_answered);
  last_answered = now;
}

void run_end() {
  bool write;
  { std::lock_guard<std::mutex> lock(menu_mutex); write = menu.dirty; menu.dirty = false; menu.open = false; }
  if (write) persist();
  if (asked) say("pro ai: the game asked %u times, Phillip answered %u", asked.load(), answered.load());
}

std::string on_off(bool v) { return v ? "On" : "Off"; }
const char* style_label(int s) {
  static const char* const names[] = {"Main Menu bars", "Side rails", "Character select slots", "HUD strip"};
  return names[std::clamp(s, 0, 3)];
}
}  // namespace

// controls.cpp: Start + D-pad Up + Z. In offline Training it opens or closes the Practice menu (true); elsewhere it is
// not this module's (false: controls.cpp opens the in-game menu's Training page as before).
bool practice_menu_chord(int port) {
  std::lock_guard<std::mutex> lock(menu_mutex);
  if (!menu.training) return false;
  if (menu.open) { menu.open = false; say("practice menu: closed (Start + D-pad Up + Z)"); }
  else {
    menu.open = true; menu.armed = false; menu.just_opened = true; menu.port = port;
    menu.buttons = 0; menu.sx = menu.sy = 0; menu.close_asked = false;
    if (opens < 99) { ++opens; menu.dirty = true; }
    say("practice menu: opened on port %d (%s)", port + 1, style_label(style));
  }
  return true;
}
bool practice_menu_open() { std::lock_guard<std::mutex> lock(menu_mutex); return menu.open; }
// controls.cpp, after the chords, every poll: while the menu is open the opener's controller is the menu's and every
// port gets neutral pads; what is held when it closes is kept from the game until let go.
void practice_menu_pads(MuPadStatus pads[4]) {
  std::lock_guard<std::mutex> lock(menu_mutex);
  if (menu.close_asked) {
    menu.close_asked = false;
    if (menu.open) { menu.open = false; say("practice menu: closed"); }
    for (int i = 0; i < 4; ++i) menu.swallow[i] = pads[i].err == 0 ? pads[i].button : 0;
  }
  if (menu.open) {
    const MuPadStatus& p = pads[menu.port];
    menu.buttons = p.err == 0 ? p.button : 0;
    menu.sx = int8_t(p.err == 0 ? p.stick_x : 0);
    menu.sy = int8_t(p.err == 0 ? p.stick_y : 0);
    if (!menu.armed && (menu.buttons & 0x1F7F) == 0 && std::abs(menu.sx) < 30 && std::abs(menu.sy) < 30) menu.armed = true;
    for (int i = 0; i < 4; ++i) {
      const int8_t err = pads[i].err;
      pads[i] = MuPadStatus{};
      pads[i].err = err;
      menu.swallow[i] = err == 0 ? 0x1F7F : 0;   // everything held at the close is kept from the game
    }
    return;
  }
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0) { menu.swallow[i] = 0; continue; }
    menu.swallow[i] &= pads[i].button;   // a released button is the game's again
    pads[i].button &= uint16_t(~menu.swallow[i]);
  }
}

void register_proai() {
  add_setting(setting);
  add_tables([](MuHostApi*, MuGameApi*) { ensure_catalog(); });   // run start: the catalog in the folder before anyone reads it
  add_command(0xEA, ghost_ai);
  add_retrace(retrace);
  add_retrace(pro_note);
  add_run_end(run_end);
  add_menu_entry({"Training", "Practice menu look (Start + D-pad Up + Z)", [] { return std::string(style_label(style)); },
                  [](int d) { style = (style + (d < 0 ? 3 : 1)) % 4; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("practicestyle", std::to_string(style.load())); }});
  add_menu_entry({"Training", "Practice menu hint in Training", [] { return on_off(hint); }, [](int) { hint = !hint; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("matchmakinghint", hint ? "1" : "0"); }});
}
}  // namespace fe::features

// ---- the render thread's view (practice_state.h) ----
namespace fe::proai {
MenuView menu_view() {
  using namespace fe::features;
  std::lock_guard<std::mutex> lock(menu_mutex);
  MenuView v;
  v.open = menu.open;
  v.armed = menu.armed;
  v.just_opened = menu.just_opened;
  menu.just_opened = false;
  v.buttons = menu.buttons;
  v.stick_x = menu.sx;
  v.stick_y = menu.sy;
  v.training = menu.training;
  v.hint = hint;
  v.unlocked = menu.unlocked;
  v.style = style;
  return v;
}
void menu_result(bool close, bool changed, bool stronger_changed) {
  using namespace fe::features;
  std::lock_guard<std::mutex> lock(menu_mutex);
  if (changed || stronger_changed) menu.dirty = true;
  if (close && menu.open) menu.close_asked = true;
}
}  // namespace fe::proai

// ---- what upstream's phillip_ai.cpp asks of its host (patches/0112) ----
namespace yougame { bool active() { return false; } }   // upstream's YouGame launcher: never here
std::string fe_proai_dir() { return fe::features::dir(); }
// The console: a policy's worker and the engine's Gemm workers below the game (core 2), the render thread (core 1, 0x28)
// and the decoder (core 0, 0x26): on cores 0 and 1 at 0x2C, so they use what those leave. Its answer is due 18 frames
// after its step, so it only has to keep up on average.
void fe_proai_place_thread() {
#ifdef __SWITCH__
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0x3);
#endif
}
void fe_proai_place_worker(int) { fe_proai_place_thread(); }
int fe_proai_threads() { return fe::features::threads.load(); }

namespace host::updater {
bool fetch_verified(const std::string& url, const std::string& sha256, const std::string& dest_utf8, std::string* why) {
  if (!&fe_proai_fetch_verified) { if (why) *why = "this program has no network (the app downloads)"; return false; }
  std::string from = url;
  // MELEE_PRO_AI_TEST_MIRROR=http://127.0.0.1:PORT/ (tests in a container without network): the same file names there.
  if (const char* mirror = std::getenv("MELEE_PRO_AI_TEST_MIRROR")) from = std::string(mirror) + url.substr(url.find_last_of('/') + 1);
  static std::atomic<uint64_t> progress{0};
  const auto host_now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  const double t0 = host_now();
  const bool ok = fe_proai_fetch_verified(from, sha256, dest_utf8, &progress, why);
  fe::say("pro ai: %s %s (%.1f MB in %.1f s)", ok ? "downloaded and checked" : "download failed:", ok ? dest_utf8.c_str() : (why ? why->c_str() : ""),
          progress.load() / 1e6, host_now() - t0);
  if (ok) phillip_ai::catalog_changed();
  return ok;
}
}  // namespace host::updater

// ---- what upstream's practice_menu.cpp asks of its host ----
// The game's menu font (practice_menu.cpp font_load): bytes of the disc's main.dol at their console address, through the
// DOL's section table (the disc header names where main.dol starts).
namespace host {
bool dol_data_read(uint32_t address, void* dst, uint32_t size) {
  std::ifstream f(fe::options.iso, std::ios::binary);
  if (!f) return false;
  const auto be32 = [](const uint8_t* q) { return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]; };
  uint8_t word[4];
  f.seekg(0x420);
  if (!f.read(reinterpret_cast<char*>(word), 4)) return false;
  const uint32_t dol = be32(word);
  uint8_t h[0x100];
  f.seekg(dol);
  if (!f.read(reinterpret_cast<char*>(h), sizeof h)) return false;
  for (int i = 0; i < 18; ++i) {
    const uint32_t off = be32(h + i * 4), addr = be32(h + 0x48 + i * 4), len = be32(h + 0x90 + i * 4);
    if (!len || address < addr || uint64_t(address) + size > uint64_t(addr) + len) continue;
    f.seekg(std::streamoff(dol) + off + (address - addr));
    return bool(f.read(static_cast<char*>(dst), size));
  }
  return false;
}
}  // namespace host
namespace practice_cpu {
MuPcConfig config() { return fe::features::training_pc(); }
void set_config(const MuPcConfig& c) { fe::features::training_pc_set(c); }
Status status() {
  Status s;
  fe::features::training_pc_status(&s.driving, &s.plan, &s.frame, &s.kind);
  return s;
}
bool slot(int index, MuPcConfig& out) { return fe::features::training_pc_slot(index, &out); }
void set_slot(int index, const MuPcConfig& c) { fe::features::training_pc_set_slot(index, c); }
// A pro's habits (upstream practice_cpu.cpp ghost, the same rule): the defaults with what that player's games measured
// in place; a group with no data keeps the default. Phillip's policy plays when it is installed and fast enough here.
bool ghost(int index, MuPcConfig& c) {
  if (index < 0 || index >= int(sizeof practice_ghosts::kGhosts / sizeof practice_ghosts::kGhosts[0])) return false;
  const practice_ghosts::Ghost& gh = practice_ghosts::kGhosts[index];
  const uint8_t tools = c.tools;
  mu_pc_defaults(&c);
  const auto take = [](uint8_t* dst, const uint8_t* src, int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += src[i];
    if (sum) std::memcpy(dst, src, size_t(n));
  };
  take(c.move_odds, gh.move, MU_PC_MOVE_COUNT);
  take(c.attack_odds, gh.attack, MU_PC_ATK_COUNT);
  take(c.defend_odds, gh.defend, MU_PC_DEF_COUNT);
  take(c.oos_odds, gh.oos, MU_PC_OOS_COUNT);
  take(c.di_odds, gh.di, MU_PC_DI_COUNT);
  take(c.tech_odds, gh.tech, MU_PC_TECH_COUNT);
  take(c.getup_odds, gh.getup, MU_PC_GETUP_COUNT);
  take(c.ledge_odds, gh.ledge, MU_PC_LEDGE_COUNT);
  take(c.after_odds, gh.after, MU_PC_AFTER_COUNT);
  if (gh.l_cancel) c.l_cancel = gh.l_cancel;
  if (gh.aggression) c.aggression = gh.aggression;
  c.tools = tools;
  c.preset = ghost_ai::specialist_available(unsigned(index)) ? uint8_t(MU_PC_PRESET_PRO(index)) : 0;
  fe::say("practice menu: fight %s (%s): %s", gh.name, gh.character,
          c.preset ? "Phillip's AI" : gh.games > 0 ? "the pro's habit table on the Practice CPU" : "the Practice CPU's defaults");
  return true;
}
const char* special_name(int kind, int which) {
#include "proai/special_names.inc"   // upstream's table (build.sh sync, from the fetched practice_cpu.cpp)
  if (kind < 0 || kind >= 27 || which < 0 || which > 3) return nullptr;
  return names[kind][which];
}
}  // namespace practice_cpu

namespace ghost_ai {
// Upstream's own networks (ghost_ai.bin) ship no files: only Phillip's policies count as a pro's "specialist" here.
bool specialist_available(unsigned pro) { return phillip_ai::available(pro); }
bool available(unsigned pro) { return phillip_ai::available(pro); }
}  // namespace ghost_ai

namespace slippi::native_practice {
void submit_train_against(int character, int costume) {
  std::lock_guard<std::mutex> lock(fe::features::train_mutex);
  fe::features::train_character = character;
  fe::features::train_costume = costume;
}
void submit_warmup_return() {}
}  // namespace slippi::native_practice

namespace slippi::online {
Config& config() {
  static Config c = [] {
    Config x;
    for (int i = 0; i < 3; ++i) x.blocked_codes[i] = fe::settings_get("blockcode" + std::to_string(i + 1));
    return x;
  }();
  return c;
}
}  // namespace slippi::online
