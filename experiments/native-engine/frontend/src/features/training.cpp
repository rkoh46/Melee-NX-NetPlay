// Training tools, offline only, as melee-unlocked has them (its host: port/app/source_host.cpp lab_save /
// lab_load and the Lab bits of h_game_options; port/runtime/hle/native_practice.cpp frame_advance_tick;
// port/runtime/hle/practice_cpu.cpp; port/runtime/gx/training_overlay.cpp. Its game: shim/mu_lab.c,
// shim/mu_practice_cpu.c). The game does the work: it reads the D-pad in Training, records and plays back
// the dummy, draws the collision bubbles and measures frame advantage. The host provides:
//   - the option bits that switch those on (MU_GAME_OPTION_LAB, _BUBBLES, _BUBBLES_ONLY, _OVERLAY, _LOOP),
//     only offline and never during replay playback;
//   - the save state behind D-pad Right / Left (commands 0xF2 / 0xF3): the hot-page snapshot store of the
//     netplay host (runner/features/snapshot_store.*), released when the scene ends;
//   - the frame advantage the game measures (command 0xF4), as an overlay line for a few seconds;
//   - frame advance: L + R + Z freezes an offline Training match and lets it run again, Z steps one frame
//     (held: every fourth frame after a short wait); the D-pad moves the CPU while frozen. Through the
//     game's own debug pause (MuGameApi.practice, MU_PRACTICE_FRAME_PAUSE / MU_PRACTICE_CPU_MOVE);
//   - the Practice CPU's settings (command 0xF6, port/runtime/abi/mu_practice_cpu.h): the game asks for
//     them every Training match and reports what the CPU is doing.
//   - Training's random numbers (MuPcConfig.rng_mode / rng_seed, the game's mu_practice_cpu.c pc_rng): the game's
//     own, the same seed at each Training match start and after a state load, or frozen (the same seed every
//     frame). It changes the simulation, so it is off by default and never sent online, in a replay or vanilla;
//   - the Practice CPU's three slots (upstream practice_cpu.cpp g_slots): the menu saves the current settings
//     into a slot and loads one back; settings.ini keeps them in upstream's hex form.
// settings.ini keys: lab, lab_bubbles (1 over the model, 2 bubbles only), lab_loop, lab_overlay,
// frame_advance, practicecpu (upstream's hex form of MuPcConfig), practicecpu_preset (0..N), practicecpu_on,
// practicecpuslot1..3 (hex, as practicecpu), practice_rng (0 the game's, 1 same each start, 2 frozen),
// practice_rng_seed (0..255).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "features/snapshot_store.h"     // runner/features
#include "features/present/present.h"

#include "mu_practice_cpu.h"              // melee-unlocked (fetched tree)
#include "mu_practice_cpu_impl.inl"       // the presets and defaults (plain C, the same code the game runs)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace fe::features {
namespace {
constexpr int32_t kLabKey = 0x7FFF0000;       // the Lab's one state, outside any replay frame number
constexpr uint32_t kTrainingMajor = 0x1C;

MuPcConfig pc = [] { MuPcConfig c; mu_pc_defaults(&c); c.enabled = 0; return c; }();
int pc_preset = -1;                           // the preset applied last (-1 none, or the setting's own)
MuPcConfig slots[3]; bool slot_valid[3] = {};   // practicecpuslot1..3
// The Practice menu (proai.cpp, drawn on the render thread) reads and changes pc and the slots: this guards them there
// and where the simulation thread hands pc to the game (0xF6) and reads the CPU's reports.
std::mutex pc_mutex;
bool pc_driving = false; int pc_plan = 0, pc_kind = -1; uint32_t pc_status_frame = 0;

std::string to_text(const MuPcConfig& c) {     // upstream practice_cpu.cpp to_text: the whole struct as hex
  static const char* hex = "0123456789abcdef";
  const auto* p = reinterpret_cast<const uint8_t*>(&c);
  std::string s;
  for (size_t i = 0; i < sizeof c; ++i) { s += hex[p[i] >> 4]; s += hex[p[i] & 15]; }
  return s;
}
bool from_text(const std::string& value, MuPcConfig* out) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i + 1 < value.size(); i += 2) bytes.push_back(uint8_t(std::strtoul(value.substr(i, 2).c_str(), nullptr, 16)));
  return !bytes.empty() && mu_pc_load(bytes.data(), unsigned(bytes.size()), out);
}

// ---- overlay ----
std::mutex hud_mutex;
std::string advantage_text; uint32_t advantage_until = 0;
int advantage_frames = 0, advantage_kind = 0; uint32_t advantage_at = 0; bool advantage_seen = false;
std::string cpu_text;
bool frozen = false;

uint32_t tool_bits() { return pc.tools; }
bool tool(uint32_t bit) { return (pc.tools & bit) != 0; }
void set_tool(uint32_t bit, bool on) { pc.tools = uint8_t(on ? (pc.tools | bit) : (pc.tools & ~bit)); }

// The Lab dummy's own reactions (the game image's mu_lab.c dummy_behavior: DI, the action after hitstun, the tech), the option
// word's MU_GAME_OPTION_LAB_DI / _REACT / _TECH fields. Upstream never sends them (its Practice CPU replaces them); here they are
// settings.ini lab_di 0..3 (none, in, out, random), lab_react 0..3 (none, jump, shield, spot dodge), lab_tech 0..4 (its own, in
// place, toward, away, random), offline Training only, under the Practice CPU when that is on (the game's order).
std::atomic<int> lab_di{0}, lab_react{0}, lab_tech{0};

bool setting(const std::string& key, const std::string& value) {
  const bool on = value == "1";
  if (key == "lab_di") { lab_di = std::clamp(std::atoi(value.c_str()), 0, 3); return true; }
  if (key == "lab_react") { lab_react = std::clamp(std::atoi(value.c_str()), 0, 3); return true; }
  if (key == "lab_tech") { lab_tech = std::clamp(std::atoi(value.c_str()), 0, 4); return true; }
  if (key == "lab") set_tool(MU_PC_TOOL_DPAD, on);
  else if (key == "lab_bubbles") { set_tool(MU_PC_TOOL_HITBOXES, value == "1"); set_tool(MU_PC_TOOL_HITBOXES_ONLY, value == "2"); }
  else if (key == "lab_loop") set_tool(MU_PC_TOOL_LOOP, on);
  else if (key == "lab_overlay") set_tool(MU_PC_TOOL_OVERLAY, on);
  else if (key == "frame_advance") set_tool(MU_PC_TOOL_FRAME_ADVANCE, on);
  else if (key == "practicecpu") {
    // Upstream's form: the whole struct as hex, version byte first (practice_cpu.cpp to_text).
    MuPcConfig c;
    const uint8_t tools = pc.tools;
    if (from_text(value, &c)) { pc = c; pc.tools |= tools; }
    else say("training: practicecpu: not a setting this build reads");
  } else if (key.size() == 17 && key.compare(0, 16, "practicecpuslot") == 0 && key[16] >= '1' && key[16] <= '3') {
    const int n = key[16] - '1';
    slot_valid[n] = from_text(value, &slots[n]);
    if (!slot_valid[n]) say("training: %s: not a setting this build reads", key.c_str());
  } else if (key == "practice_rng") {
    const int n = std::atoi(value.c_str());
    pc.rng_mode = uint8_t(n >= 0 && n < MU_PC_RNG_COUNT ? n : MU_PC_RNG_GAME);
  } else if (key == "practice_rng_seed") pc.rng_seed = uint8_t(std::clamp(std::atoi(value.c_str()), 0, 255));
  else if (key == "practicecpu_preset") {
    const int n = std::atoi(value.c_str());
    if (n >= 0 && n < mu_pc_preset_count()) { const uint8_t tools = pc.tools, enabled = pc.enabled; mu_pc_preset(n, &pc); pc.tools = tools; pc.enabled = enabled; pc_preset = n; }
  } else if (key == "practicecpu_on") pc.enabled = on ? 1 : 0;
  else return false;
  return true;
}

bool offline_training() { return !online() && !replay_playing() && !vanilla() && scene_major() == kTrainingMajor; }

// ---- commands ----
// Where player 1 stands on the retrace after a save or a load, for the log (the proof that a load went back).
int position_note = 0;                      // 1 after a save, 2 after a load
float last_x = 0, last_y = 0; uint32_t last_frame = 0;
void note_position(const Retrace& info) {
  if (!info.state) return;
  float x, y; std::memcpy(&x, &info.state->player[0].pos_x, 4); std::memcpy(&y, &info.state->player[0].pos_y, 4);
  if (position_note == 1) say("lab: after the save, player 1 at x %.2f y %.2f (match frame %u)", x, y, info.match_frame);
  if (position_note == 2) say("lab: after the load, player 1 at x %.2f y %.2f (match frame %u); the frame before, x %.2f y %.2f (match frame %u)",
                              x, y, info.match_frame, last_x, last_y, last_frame);
  position_note = 0;
  last_x = x; last_y = y; last_frame = info.match_frame;
}
int lab_save(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>&) {
  if (online() || replay_playing()) return 1;
  if (snapshot_store::keep(kLabKey, 4, "lab")) { say("lab: state saved (%.2f ms)", snapshot_store::stats().keep_ms_max); position_note = 1; }
  return 1;
}
int lab_load(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>&) {
  if (online() || replay_playing()) return 1;
  if (!snapshot_store::load(kLabKey, "lab")) { say("lab: no saved state"); return 1; }
  say("lab: state loaded");
  present::note_discontinuity();
  position_note = 2;
  return 1;
}
int lab_advantage(uint8_t, const uint8_t* p, uint32_t n, std::vector<uint8_t>&) {
  if (n != 5) return 0;
  const int32_t frames = int32_t(be32(p));
  char line[96];
  std::snprintf(line, sizeof line, "Frame advantage %+d (on %s)", frames, p[4] == 2 ? "shield" : "hit");
  {
    std::lock_guard<std::mutex> lock(hud_mutex);
    advantage_text = line; advantage_until = retrace() + 180;
    advantage_frames = frames; advantage_kind = p[4]; advantage_at = retrace(); advantage_seen = true;
  }
  say("training: %s", line);
  return 1;
}
int practice_cpu(uint8_t, const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  if (n == 1) return 0;                     // 20XX TE's Frozen Mode switch (mods.cpp)
  if (n == 2 || n == 3) {                   // what the CPU is doing: driving, plan[, kind]
    {
      std::lock_guard<std::mutex> lock(pc_mutex);
      pc_driving = p[0] != 0; pc_plan = p[1]; pc_kind = n == 3 ? int(p[2]) : -1; pc_status_frame = retrace();
    }
    char line[96];
    std::snprintf(line, sizeof line, "Practice CPU: %s", p[0] ? mu_pc_plan_name(p[1]) : "the game's own");
    std::lock_guard<std::mutex> lock(hud_mutex);
    if (cpu_text != line) { static int told_status = 0; if (told_status++ < 8) say("training: %s", line); }
    cpu_text = line;
    return 1;
  }
  MuPcConfig c;
  { std::lock_guard<std::mutex> lock(pc_mutex); c = pc; }
  // The game also keeps both to offline Training; here nothing that changes the simulation leaves the host online,
  // in a replay or in a vanilla session.
  if (online() || replay_playing() || vanilla()) { c.enabled = 0; c.rng_mode = MU_PC_RNG_GAME; }
  static int told_rng = MU_PC_RNG_GAME, told_seed = -1;   // said on a change (and not for the game's own at the first match)
  if (told_rng != c.rng_mode || (c.rng_mode != MU_PC_RNG_GAME && told_seed != pc.rng_seed)) {
    told_rng = c.rng_mode; told_seed = pc.rng_seed;
    say("training: random numbers %s (seed number %d)", c.rng_mode == MU_PC_RNG_FROZEN ? "frozen" : c.rng_mode == MU_PC_RNG_SAME ? "the same at each start" : "the game's own", pc.rng_seed);
  }
  {
    static std::string told_rows;
    char rows[200];
    std::snprintf(rows, sizeof rows, "only react %d, moving target %d, CPU delay %d, respawn %d, floorhug %d, smash DI %d, throw wait %d, drift %d, double jump %d",
                  c.freeze, c.no_attacks, c.act_delay, c.respawn, c.floorhug, c.sdi_amount, c.throw_wait, c.jump_drift, c.double_jump);
    if (c.enabled && told_rows != rows) { told_rows = rows; say("training: practice CPU sent to the game: %s", rows); }
  }
  static int told = -1;
  if (told != c.enabled) { told = c.enabled; say("training: practice CPU %s (preset %s, tools %02X)", c.enabled ? "on" : "off", pc_preset >= 0 ? mu_pc_preset_name(pc_preset) : "own", pc.tools); }
  const auto* bytes = reinterpret_cast<const uint8_t*>(&c);
  reply.assign(bytes, bytes + sizeof c);
  return 1;
}

// The in-game menu's Practice CPU rows (below).
int pc_row_count();
const char* pc_row_label(int n);
std::string pc_row_value(int n);
void pc_row_change(int n, int d);

// ---- frame advance (native_practice.cpp frame_advance_tick, through the game's practice operations) ----
bool frame_pause(int pause, bool step) {
  MuGameApi* g = game();
  if (!g || !g->practice) return false;
  int32_t a[2] = {pause, step ? 1 : 0};
  if (g->practice(MU_PRACTICE_FRAME_PAUSE, a, 2) != 0) return false;
  return a[0] != 0;
}
void cpu_move(float dx, float dy) {
  MuGameApi* g = game();
  if (!g || !g->practice) return;
  int32_t a[2] = {int32_t(dx * 100.0f), int32_t(dy * 100.0f)};
  g->practice(MU_PRACTICE_CPU_MOVE, a, 2);
}
bool combo_was = false, z_was = false;
uint32_t z_held = 0, dpad_held = 0;
void frame_advance(const MuPadStatus pads[4]) {
  const bool allowed = tool(MU_PC_TOOL_FRAME_ADVANCE) && offline_training();
  if (!allowed) {
    if (frozen) { frame_pause(0, false); frozen = false; say("training: frame advance off, the game runs"); }
    combo_was = z_was = false;
    return;
  }
  bool combo = false, z = false;
  uint16_t dpad = 0;
  for (int i = 0; i < 4; ++i) {
    const uint16_t b = pads[i].button;
    if ((b & 0x0070u) == 0x0070u) combo = true;             // L + R + Z
    else if ((b & 0x0010u) && !(b & 0x0060u)) z = true;
    dpad |= b & 0x000Fu;
  }
  if (combo && !combo_was) {
    frozen = !frozen;
    frame_pause(frozen ? 1 : 0, false);
    say("training: frame advance %s at retrace %u", frozen ? "froze the game" : "let the game run", retrace());
  }
  combo_was = combo;
  if (frozen) {
    if (!frame_pause(-1, false)) frame_pause(1, false);    // keep it frozen
    z_held = z ? z_held + 1 : 0;
    if (z && (!z_was || (z_held > 20 && z_held % 4 == 0))) {
      frame_pause(1, true);
      static int steps_told = 0;
      if (steps_told++ < 5) say("training: frame advance: one frame at retrace %u", retrace());
    }
    dpad_held = dpad ? dpad_held + 1 : 0;
    if (dpad && (dpad_held == 1 || (dpad_held > 15 && dpad_held % 2 == 0))) {
      const float step = 2.0f;
      cpu_move((dpad & 0x2) ? step : (dpad & 0x1) ? -step : 0.0f, (dpad & 0x8) ? step : (dpad & 0x4) ? -step : 0.0f);
      frame_pause(1, true);
    }
  }
  z_was = z;
}

// ---- "Go to Training": from anywhere offline into an offline Training match, as upstream's Practice menu does it
// (native_practice.cpp, TestTraining and the press-through of Training's menus): the picks are set, the game is
// asked for Training's scene, then the character select is confirmed with Start and the stage is picked at once.
struct Enter { bool on = false; uint32_t ticks = 0; bool seen_select = false; int stage = 32; bool from_online = false; } enter;
bool go_training_from(int character, int costume, int cpu_character, int cpu_costume, bool from_online) {
  MuGameApi* g = game();
  if (!g || !g->practice || (online() && !from_online) || replay_playing()) return false;
  if (character >= 0 && character < 128) { int32_t a[3] = {0, character, std::max(costume, 0)}; g->practice(MU_PRACTICE_TRAINING_PICK, a, 3); }
  if (cpu_character >= 0 && cpu_character < 26) { int32_t a[3] = {1, cpu_character, std::max(cpu_costume, 0)}; g->practice(MU_PRACTICE_TRAINING_PICK, a, 3); }
  int32_t m[1] = {int32_t(kTrainingMajor)};
  g->practice(MU_PRACTICE_REQUEST_MAJOR, m, 1);
  enter = Enter{}; enter.on = true; enter.from_online = from_online;
  say("training: into Training as character %d (CPU %d) from scene %u/%u%s", character, cpu_character, scene_major(), scene_minor(),
      from_online ? " (from the online screens)" : "");
  return true;
}
void go_training(int character, int cpu_character) { go_training_from(character, 0, cpu_character, 0, false); }
void enter_tick(MuPadStatus pads[4]) {
  if (!enter.on) return;
  ++enter.ticks;
  const uint32_t major = scene_major(), minor = scene_minor();
  if (major == kTrainingMajor && minor < 2) enter.seen_select = true;
  const auto neutral = [&] { for (int i = 0; i < 4; ++i) { const int8_t err = pads[i].err; pads[i] = MuPadStatus{}; pads[i].err = err; } };
  if (major == kTrainingMajor && minor >= 2 && enter.seen_select) { enter.on = false; say("training: in the Training match at retrace %u", retrace()); }
  else if (enter.ticks > 60 * 15 || (online() && !enter.from_online) || (major != kTrainingMajor && enter.ticks > 180)) { enter.on = false; say("training: stopped pressing through Training's menus (scene %u/%u)", major, minor); }
  else if (major == kTrainingMajor && minor == 1 && enter.stage < 0) {
    enter.on = false; say("training: at the stage select; the controller picks");   // --feature-test training:N:-1
  } else if (major == kTrainingMajor && minor == 1) {
    neutral();
    if (MuGameApi* g = game(); g && g->practice) { int32_t a[1] = {enter.stage}; g->practice(MU_PRACTICE_TRAINING_STAGE, a, 1); }
  } else if (major == kTrainingMajor) {
    neutral();
    if (enter.ticks % 40 < 3) pads[0].button |= minor == 0 ? 0x1000 : 0x0100;    // Start on the select, A after
  } else neutral();
}
std::atomic<bool> go_requested{false};     // the menu's "Go to Training" (menu thread): acted on at the next retrace
uint32_t test_main_menu_ticks = 0;
bool test_done = false;
void test_tick() {
  if (go_requested.exchange(false)) go_training(-1, -1);
  // --feature-test major:N: ask the game for scene N (decimal) once a menu has been up for a second (verification
  // runs: Event Match 43, 1-P Classic 3, ...), as the Practice menu's own requests do.
  {
    static bool major_done = false; static uint32_t ticks = 0;
    std::string v;
    if (!major_done && feature_test("major", &v)) {
      ticks = scene_major() == 1 || scene_major() == 24 ? ticks + 1 : 0;
      if (ticks == 60) {
        major_done = true;
        if (MuGameApi* g = game(); g && g->practice) {
          int32_t m[1] = {int32_t(std::atoi(v.c_str()))};
          g->practice(MU_PRACTICE_REQUEST_MAJOR, m, 1);
          say("training: test: asked for scene %d from %u/%u", m[0], scene_major(), scene_minor());
        }
      }
    }
  }
  // --feature-test practice-rows: every Practice CPU row of the in-game menu stepped once to the right at the start, through
  // the rows' own code (verification runs; the game's next 0xF6 request gets the result, logged there).
  {
    static bool rows_done = false;
    if (!rows_done && feature_test("practice-rows")) {
      rows_done = true;
      std::string line;
      for (int i = 0; i < pc_row_count(); ++i) { pc_row_change(i, 1); line += std::string(i ? ", " : "") + (pc_row_label(i) + 14) + " " + pc_row_value(i); }
      say("training: test: every Practice CPU row stepped once: %s", line.c_str());
    }
  }
  // --feature-test training[:CHARACTER[:STAGE]]: Go to Training once a menu (the title screen or the main menu) has
  // been up for a second; STAGE is the stage id Training's stage select picks (default 32, Final Destination; up to
  // 127), or -1: the stage select is left to the controller (an m-ex disc's added stages have ids above 127).
  std::string value;
  if (test_done || !feature_test("training", &value)) return;
  test_main_menu_ticks = scene_major() == 1 || scene_major() == 24 ? test_main_menu_ticks + 1 : 0;
  if (test_main_menu_ticks == 60) {
    test_done = true;
    const size_t colon = value.find(':');
    go_training(value.empty() ? 2 : std::atoi(value.c_str()), -1);
    if (colon != std::string::npos) enter.stage = std::clamp(std::atoi(value.c_str() + colon + 1), -1, 0x7F);
  }
}

void hud(std::vector<HudLine>& out) {
  std::lock_guard<std::mutex> lock(hud_mutex);
  if (scene_major() != kTrainingMajor) return;
  if (frozen) out.push_back({"Frame advance: Z steps, L+R+Z runs", 0xFFE070FFu});
  // The frame advantage itself is drawn by the overlay drawer in its Training panel (overlays.cpp, overlay_draw.cpp);
  // the line stays for a build without a picture overlay (the null variant draws nothing anyway).
  if (tool(MU_PC_TOOL_OVERLAY) && !advantage_text.empty() && retrace() < advantage_until && !overlay_draws_training()) out.push_back({advantage_text, 0xFFFFFFFFu});
  if (pc.enabled && !cpu_text.empty()) out.push_back({cpu_text, 0xB0E0FFFFu});
}

void run_end() {
  snapshot_store::release("the run ended");
  if (frozen) frozen = false;
}

std::string on_off(bool v) { return v ? "On" : "Off"; }
void flip(uint32_t bit) { set_tool(bit, !tool(bit)); }

// The menu's rows for one Practice CPU slot (the menu takes plain function pointers).
template <int N> void add_slot_rows() {
  static const std::string save = "Practice CPU: save to slot " + std::to_string(N + 1), load = "Practice CPU: load slot " + std::to_string(N + 1);
  add_menu_entry({"Training", save.c_str(), [] { return std::string(slot_valid[N] ? "Saved" : "Empty"); }, nullptr,
                  [] { slots[N] = pc; slot_valid[N] = true; say("training: practice CPU settings saved to slot %d", N + 1); },
                  [](fe::features::MenuEntry::Write w) { if (slot_valid[N]) w(("practicecpuslot" + std::to_string(N + 1)).c_str(), to_text(slots[N])); }});
  add_menu_entry({"Training", load.c_str(), nullptr, nullptr,
                  [] {
                    if (!slot_valid[N]) { say("training: practice CPU slot %d is empty", N + 1); return; }
                    const uint8_t tools = pc.tools; pc = slots[N]; pc.tools = tools; pc_preset = -1;
                    say("training: practice CPU settings loaded from slot %d", N + 1);
                  },
                  [](fe::features::MenuEntry::Write w) {
                    w("practicecpu", to_text(pc)); w("practicecpu_on", pc.enabled ? "1" : "0");
                    if (pc_preset < 0) w("practicecpu_preset", "-1");   // the slot's own settings, not a preset, at the next start
                  }});
}
// The Practice CPU's single settings on the in-game menu's Training page (upstream's Practice menu pages: CPU, Throws, Jumps,
// Hit reactions, Ledge). Thin rows over MuPcConfig: a change counts as the player's own settings (preset 0), as upstream's
// change() does, and settings.ini keeps the whole struct (practicecpu, upstream's hex form).
struct PcRow { const char* label; uint8_t MuPcConfig::*field; int low, high, step; const char* format; const char* const* names; };
const char* const kRespawnNames[] = {"Wait", "Get off", "Mix"};
const char* const kSdiNames[] = {"None", "Light", "Medium", "Heavy"};
const char* const kDriftNames[] = {"Mix", "Toward you", "Away", "None"};
const char* const kTimingNames[] = {"Mix", "Rising", "Falling"};
const char* const kRecoveryNames[] = {"Game's own", "High", "Low", "Mix"};
const char* const kOnOff[] = {"Off", "On"};
const PcRow kPcRows[] = {
    {"Practice CPU: only react to hits", &MuPcConfig::freeze, 0, 1, 1, nullptr, kOnOff},
    {"Practice CPU: moving target (no attacks)", &MuPcConfig::no_attacks, 0, 1, 1, nullptr, kOnOff},
    {"Practice CPU: aggression", &MuPcConfig::aggression, 0, 100, 5, "%d%%", nullptr},
    {"Practice CPU: reaction", &MuPcConfig::reaction_frames, 0, 40, 1, "%d frames", nullptr},
    {"Practice CPU: CPU delay", &MuPcConfig::act_delay, 0, 30, 1, "%d frames", nullptr},
    {"Practice CPU: respawn platform", &MuPcConfig::respawn, 0, MU_PC_RESPAWN_COUNT - 1, 1, nullptr, kRespawnNames},
    {"Practice CPU: L-cancel", &MuPcConfig::l_cancel, 0, 100, 5, "%d%%", nullptr},
    {"Practice CPU: run shine (weight)", &MuPcConfig::run_shine, 0, 100, 5, "%d", nullptr},
    {"Practice CPU: throw wait (0: 4 to 12)", &MuPcConfig::throw_wait, 0, 60, 1, "%d frames", nullptr},
    {"Practice CPU: jump drift", &MuPcConfig::jump_drift, 0, MU_PC_DRIFT_COUNT - 1, 1, nullptr, kDriftNames},
    {"Practice CPU: double jump", &MuPcConfig::double_jump, 0, 100, 5, "%d%%", nullptr},
    {"Practice CPU: fast fall", &MuPcConfig::fast_fall, 0, 100, 5, "%d%%", nullptr},
    {"Practice CPU: fast fall delay", &MuPcConfig::fast_fall_delay, 0, 30, 1, "%d frames", nullptr},
    {"Practice CPU: aerial timing", &MuPcConfig::aerial_timing, 0, MU_PC_TIMING_COUNT - 1, 1, nullptr, kTimingNames},
    {"Practice CPU: full hop aerials", &MuPcConfig::full_hop_aerials, 0, 100, 5, "%d%%", nullptr},
    {"Practice CPU: smash DI", &MuPcConfig::sdi_amount, 0, 3, 1, nullptr, kSdiNames},
    {"Practice CPU: floorhug", &MuPcConfig::floorhug, 0, 100, 5, "%d%%", nullptr},
    {"Practice CPU: ledge wait (most)", &MuPcConfig::ledge_delay, 0, 120, 5, "%d frames", nullptr},
    {"Practice CPU: recovery", &MuPcConfig::recovery, 0, MU_PC_RECOVER_COUNT - 1, 1, nullptr, kRecoveryNames},
};
// The throw odds (upstream's Throws page): each throw's weight; the share of the four is what the CPU picks.
const char* const kThrowNames[MU_PC_THROW_COUNT] = {"forward", "back", "up", "down"};
int pc_row_count() { return int(sizeof kPcRows / sizeof kPcRows[0]); }
const char* pc_row_label(int n) { return kPcRows[n].label; }
std::string pc_row_value(int n) {
  const PcRow& r = kPcRows[n];
  std::lock_guard<std::mutex> lock(pc_mutex);
  const int v = pc.*r.field;
  if (r.names) return std::string(v >= r.low && v <= r.high ? r.names[v] : "?");
  char b[32]; std::snprintf(b, sizeof b, r.format, v); return std::string(b);
}
void pc_row_change(int n, int d) {
  const PcRow& r = kPcRows[n];
  std::lock_guard<std::mutex> lock(pc_mutex);
  int v = pc.*r.field + (d < 0 ? -r.step : r.step);
  if (r.names) v = v < r.low ? r.high : v > r.high ? r.low : v;   // choices wrap
  pc.*r.field = uint8_t(std::clamp(v, r.low, r.high));
  pc.preset = 0; pc_preset = -1;
}
template <int N> void add_pc_row() {
  add_menu_entry({"Training", kPcRows[N].label, [] { return pc_row_value(N); }, [](int d) { pc_row_change(N, d); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("practicecpu", to_text(pc)); w("practicecpu_preset", "-1"); }});
}
template <int N> void add_throw_row() {
  static const std::string label = std::string("Practice CPU: ") + kThrowNames[N] + " throw";
  add_menu_entry({"Training", label.c_str(),
                  [] {
                    std::lock_guard<std::mutex> lock(pc_mutex);
                    int total = 0; for (uint8_t v : pc.throw_odds) total += v;
                    char b[32]; std::snprintf(b, sizeof b, "%d%%", total ? (pc.throw_odds[N] * 100 + total / 2) / total : 0);
                    return std::string(pc.throw_odds[N] ? b : "Never");
                  },
                  [](int d) {
                    std::lock_guard<std::mutex> lock(pc_mutex);
                    pc.throw_odds[N] = uint8_t(std::clamp(pc.throw_odds[N] + (d < 0 ? -5 : 5), 0, 100));
                    pc.preset = 0; pc_preset = -1;
                  }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("practicecpu", to_text(pc)); w("practicecpu_preset", "-1"); }});
}
template <int... I> void add_pc_rows(std::integer_sequence<int, I...>) { (add_pc_row<I>(), ...); }
}  // namespace

// The replay takeover's opponents once the live game leaves the recording (command 0xE9, replays.cpp; upstream
// source_host.cpp CMD_REPLAY_DEFENSE): 0 smart defence (every defensive option, never an attack in neutral), 1 no DI
// and no techs (stands and only reacts to being hit), 2 the Practice CPU's own settings, 3 nothing: they follow the
// recording (an empty answer).
std::vector<uint8_t> training_replay_defense(int setting) {
  if (setting < 0 || setting > 2) return {};
  MuPcConfig cfg = pc;
  if (setting == 0) {
    mu_pc_defaults(&cfg);
    cfg.aggression = 0;
    for (auto& v : cfg.move_odds) v = 0;
    cfg.move_odds[MU_PC_MOVE_STAND] = 100;
    cfg.sdi_amount = 2;
  } else if (setting == 1) {
    mu_pc_preset(1, &cfg);
    for (auto& v : cfg.di_odds) v = 0;
    cfg.di_odds[MU_PC_DI_NONE] = 100;
    for (auto& v : cfg.tech_odds) v = 0;
    cfg.tech_odds[MU_PC_TECH_MISS] = 100;
  }
  cfg.enabled = 1;
  cfg.rng_mode = MU_PC_RNG_GAME;           // a replay's random numbers stay the game's own
  const auto* bytes = reinterpret_cast<const uint8_t*>(&cfg);
  return std::vector<uint8_t>(bytes, bytes + sizeof cfg);
}

uint32_t training_option_bits() {
  // As upstream's apply_practice_tools (pc_settings.cpp) builds them from the tools byte.
  uint32_t te = 0;
  const uint32_t t = tool_bits();
  if (t & MU_PC_TOOL_DPAD) te |= MU_GAME_OPTION_LAB;
  if (t & (MU_PC_TOOL_HITBOXES | MU_PC_TOOL_HITBOXES_ONLY)) te |= MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_BUBBLES;
  if (t & MU_PC_TOOL_HITBOXES_ONLY) te |= MU_GAME_OPTION_LAB_BUBBLES_ONLY;
  if (t & MU_PC_TOOL_OVERLAY) te |= MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_OVERLAY;
  if (t & MU_PC_TOOL_LOOP) te |= MU_GAME_OPTION_LAB_LOOP;
  te |= (uint32_t(lab_di.load()) << 19) & MU_GAME_OPTION_LAB_DI_MASK;
  te |= (uint32_t(lab_react.load()) << 21) & MU_GAME_OPTION_LAB_REACT_MASK;
  te |= (uint32_t(lab_tech.load()) << 23) & MU_GAME_OPTION_LAB_TECH_MASK;
  if (te & (MU_GAME_OPTION_LAB_DI_MASK | MU_GAME_OPTION_LAB_REACT_MASK | MU_GAME_OPTION_LAB_TECH_MASK)) te |= MU_GAME_OPTION_LAB;
  return te;
}
bool training_go(int character, int costume, int cpu_character, int cpu_costume, bool from_online) {
  return go_training_from(character, costume, cpu_character, cpu_costume, from_online);
}
bool training_entering() { return enter.on; }
// The Practice menu's view of the Practice CPU (proai.cpp: upstream's practice_cpu::config / set_config / slot / status).
MuPcConfig training_pc() { std::lock_guard<std::mutex> lock(pc_mutex); return pc; }
void training_pc_set(const MuPcConfig& c) {
  std::lock_guard<std::mutex> lock(pc_mutex);
  pc = c;
  pc_preset = c.preset >= 1 && c.preset <= mu_pc_preset_count() ? int(c.preset) - 1 : -1;
}
bool training_pc_slot(int index, MuPcConfig* out) {
  std::lock_guard<std::mutex> lock(pc_mutex);
  if (index < 0 || index > 2 || !slot_valid[index]) return false;
  *out = slots[index];
  return true;
}
void training_pc_set_slot(int index, const MuPcConfig& c) {
  std::lock_guard<std::mutex> lock(pc_mutex);
  if (index < 0 || index > 2) return;
  slots[index] = c; slot_valid[index] = true;
}
void training_pc_status(bool* driving, int* plan, uint32_t* frame, int* kind) {
  std::lock_guard<std::mutex> lock(pc_mutex);
  *driving = pc_driving; *plan = pc_plan; *frame = pc_status_frame; *kind = pc_kind;
}
// settings.ini lines for the current Practice CPU (written by proai.cpp when the Practice menu closes after a change).
void training_pc_persist(void (*write)(const std::string&, const std::string&)) {
  MuPcConfig c; int preset; MuPcConfig s[3]; bool valid[3];
  {
    std::lock_guard<std::mutex> lock(pc_mutex);
    c = pc; preset = pc_preset;
    for (int i = 0; i < 3; ++i) { s[i] = slots[i]; valid[i] = slot_valid[i]; }
  }
  write("practicecpu", to_text(c));
  write("practicecpu_on", c.enabled ? "1" : "0");
  write("practicecpu_preset", std::to_string(preset));
  write("lab", (c.tools & MU_PC_TOOL_DPAD) ? "1" : "0");
  write("lab_bubbles", (c.tools & MU_PC_TOOL_HITBOXES_ONLY) ? "2" : (c.tools & MU_PC_TOOL_HITBOXES) ? "1" : "0");
  write("lab_loop", (c.tools & MU_PC_TOOL_LOOP) ? "1" : "0");
  write("lab_overlay", (c.tools & MU_PC_TOOL_OVERLAY) ? "1" : "0");
  write("frame_advance", (c.tools & MU_PC_TOOL_FRAME_ADVANCE) ? "1" : "0");
  write("practice_rng", std::to_string(c.rng_mode));
  write("practice_rng_seed", std::to_string(c.rng_seed));
  for (int i = 0; i < 3; ++i) if (valid[i]) write("practicecpuslot" + std::to_string(i + 1), to_text(s[i]));
}
void training_pad_hook(MuPadStatus pads[4]) { test_tick(); enter_tick(pads); frame_advance(pads); }
void training_scene_changed() {
  if (snapshot_store::running() && !replay_playing()) snapshot_store::release("the scene ended");
  frozen = false; combo_was = z_was = false;
  std::lock_guard<std::mutex> lock(hud_mutex);
  advantage_text.clear(); cpu_text.clear(); advantage_seen = false;
}
bool training_lab_overlay() { return tool(MU_PC_TOOL_OVERLAY); }
bool training_practice_display() { return tool(MU_PC_TOOL_PROJECTILE_HITBOXES); }
bool training_practice_combo() { return tool(MU_PC_TOOL_COMBO_COUNTER); }
bool training_advantage(int* frames, int* kind, uint32_t* at) {
  std::lock_guard<std::mutex> lock(hud_mutex);
  if (!advantage_seen) return false;
  *frames = advantage_frames; *kind = advantage_kind; *at = advantage_at;
  return true;
}

void register_training() {
  add_setting(setting);
  add_command(0xF2, lab_save);
  add_command(0xF3, lab_load);
  add_command(0xF4, lab_advantage);
  add_command(0xF6, practice_cpu);
  add_hud(hud);
  add_retrace(note_position);
  add_run_end(run_end);
  add_menu_entry({"Training", "Go to Training (offline)", nullptr, nullptr, [] { go_requested = true; }});
  add_menu_entry({"Training", "Save states and recording on the D-pad", [] { return on_off(tool(MU_PC_TOOL_DPAD)); }, [](int) { flip(MU_PC_TOOL_DPAD); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab", tool(MU_PC_TOOL_DPAD) ? "1" : "0"); }});
  add_menu_entry({"Training", "Lab dummy: DI", [] { static const char* n[] = {"None", "In", "Out", "Random"}; return std::string(n[lab_di.load()]); },
                  [](int d) { lab_di = (lab_di + (d < 0 ? 3 : 1)) % 4; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab_di", std::to_string(lab_di.load())); }});
  add_menu_entry({"Training", "Lab dummy: after hitstun", [] { static const char* n[] = {"Nothing", "Jump", "Shield", "Spot dodge"}; return std::string(n[lab_react.load()]); },
                  [](int d) { lab_react = (lab_react + (d < 0 ? 3 : 1)) % 4; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab_react", std::to_string(lab_react.load())); }});
  add_menu_entry({"Training", "Lab dummy: tech", [] { static const char* n[] = {"Its own", "In place", "Toward", "Away", "Random"}; return std::string(n[lab_tech.load()]); },
                  [](int d) { lab_tech = (lab_tech + (d < 0 ? 4 : 1)) % 5; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab_tech", std::to_string(lab_tech.load())); }});
  add_menu_entry({"Training", "Loop the recording", [] { return on_off(tool(MU_PC_TOOL_LOOP)); }, [](int) { flip(MU_PC_TOOL_LOOP); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab_loop", tool(MU_PC_TOOL_LOOP) ? "1" : "0"); }});
  add_menu_entry({"Training", "Collision bubbles",
                  [] { return std::string(tool(MU_PC_TOOL_HITBOXES_ONLY) ? "Bubbles only" : tool(MU_PC_TOOL_HITBOXES) ? "Over the model" : "Off"); },
                  [](int d) {
                    const int now = tool(MU_PC_TOOL_HITBOXES_ONLY) ? 2 : tool(MU_PC_TOOL_HITBOXES) ? 1 : 0;
                    const int next = (now + (d < 0 ? 2 : 1)) % 3;
                    set_tool(MU_PC_TOOL_HITBOXES, next == 1); set_tool(MU_PC_TOOL_HITBOXES_ONLY, next == 2);
                  }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab_bubbles", tool(MU_PC_TOOL_HITBOXES_ONLY) ? "2" : tool(MU_PC_TOOL_HITBOXES) ? "1" : "0"); }});
  add_menu_entry({"Training", "Inputs and frame advantage", [] { return on_off(tool(MU_PC_TOOL_OVERLAY)); }, [](int) { flip(MU_PC_TOOL_OVERLAY); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("lab_overlay", tool(MU_PC_TOOL_OVERLAY) ? "1" : "0"); }});
  add_menu_entry({"Training", "Frame advance (L+R+Z, then Z)", [] { return on_off(tool(MU_PC_TOOL_FRAME_ADVANCE)); }, [](int) { flip(MU_PC_TOOL_FRAME_ADVANCE); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("frame_advance", tool(MU_PC_TOOL_FRAME_ADVANCE) ? "1" : "0"); }});
  add_menu_entry({"Training", "Practice CPU",
                  [] { return pc.enabled ? std::string(pc_preset >= 0 ? mu_pc_preset_name(pc_preset) : "Own settings") : std::string("Off"); },
                  [](int d) {
                    // Off, then each built-in preset in turn.
                    const int count = mu_pc_preset_count();
                    int at = pc.enabled ? (pc_preset >= 0 ? pc_preset + 1 : 1) : 0;
                    at = (at + (d < 0 ? count : 1)) % (count + 1);
                    const uint8_t tools = pc.tools;
                    if (at == 0) pc.enabled = 0;
                    else { mu_pc_preset(at - 1, &pc); pc.tools = tools; pc.enabled = 1; pc_preset = at - 1; }
                  }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("practicecpu_on", pc.enabled ? "1" : "0"); if (pc_preset >= 0) w("practicecpu_preset", std::to_string(pc_preset)); }});
  add_menu_entry({"Training", "Random numbers (items, turnips)",
                  [] { return std::string(pc.rng_mode == MU_PC_RNG_FROZEN ? "Frozen" : pc.rng_mode == MU_PC_RNG_SAME ? "Same each start" : "The game's"); },
                  [](int d) { pc.rng_mode = uint8_t((pc.rng_mode + (d < 0 ? MU_PC_RNG_COUNT - 1 : 1)) % MU_PC_RNG_COUNT); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("practice_rng", std::to_string(pc.rng_mode)); w("practice_rng_seed", std::to_string(pc.rng_seed)); }});
  add_menu_entry({"Training", "Random numbers: seed number",
                  [] { return std::to_string(pc.rng_seed) + (pc.rng_mode == MU_PC_RNG_GAME ? " (not used)" : ""); },
                  [](int d) { pc.rng_seed = uint8_t(pc.rng_seed + (d < 0 ? 255 : 1)); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("practice_rng_seed", std::to_string(pc.rng_seed)); }});
  add_slot_rows<0>(); add_slot_rows<1>(); add_slot_rows<2>();
  add_pc_rows(std::make_integer_sequence<int, int(sizeof kPcRows / sizeof kPcRows[0])>{});
  add_throw_row<0>(); add_throw_row<1>(); add_throw_row<2>(); add_throw_row<3>();
  add_menu_entry({"Training", "Practice CPU: every setting back to the defaults", nullptr, nullptr,
                  [] {
                    std::lock_guard<std::mutex> lock(pc_mutex);
                    const uint8_t tools = pc.tools, enabled = pc.enabled, rng = pc.rng_mode, seed = pc.rng_seed;
                    mu_pc_defaults(&pc); pc.tools = tools; pc.enabled = enabled; pc.rng_mode = rng; pc.rng_seed = seed; pc_preset = -1;
                    say("training: practice CPU settings back to the defaults");
                  },
                  [](fe::features::MenuEntry::Write w) { w("practicecpu", to_text(pc)); w("practicecpu_preset", "-1"); }});
}
}  // namespace fe::features
