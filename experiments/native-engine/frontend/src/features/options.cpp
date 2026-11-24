// Small gameplay options melee-unlocked's Windows host provides beside the option words, and the plumbing
// the other modules share:
//   - the session the modules see (online or not, the scene, the retrace), from play mode's retrace call;
//   - the option words as the game reads them: play mode's words (settings.cpp) plus the Training tools'
//     bits and Training Mode CE's bit offline, or the recorded words during replay playback
//     (upstream port/app/source_host.cpp h_game_options, h_game_options2, h_game_options3);
//   - one wrapper of the host's pad_read, where frame advance, replay controls, the offline input delay and
//     automatic L-cancel see (and the last two change) the controllers once per retrace;
//   - shield colours (MuHostApi.shield_color; upstream gx_core.cpp shield_color, keys shield_colorp1..4);
//   - offline input delay (upstream port/runtime/host/offline_input_delay.*: offline_delay, offline_delay_frames);
//   - automatic L-cancel (upstream port/runtime/host/lcancel.cpp, the automatic half; key autolcancel);
//   - Zelda starts as Sheik (command 0xDE; key zelda_as_sheik);
//   - offline Squad Strike (key squad_strike, off by default; patches/0006: the game asks test_getenv("MU_SQUAD_STRIKE"));
//   - Slippi 3.7.0 input suggestions online (key input_suggestions, default on as upstream; the netplay object asks
//     mu_net_suggest_setting at each online game's start: patches/net-0001, play_io.cpp mu_net_suggest_read).
// Upstream's online rules: the input delay, Zelda as Sheik and the Training bits are offline only. Automatic
// L-cancel is allowed offline and in Direct upstream (host/lcancel_policy.h); here it is offline only,
// because this host does not know which pad drives the local fighter in a netplay match. Shield colours
// online: this player's shield_color_online (settings.ini, play_main) and the opponent's when their build is a Melee-NX-NetPlay
// one (patches/net-0004; upstream exchanges them with other melee-unlocked players), else the port's own.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "host_services.h"       // melee-unlocked (fetched tree): linux_host::game_api
#include "features/snapshot_store.h"     // runner/features
#include "netplay_bridge.h"                // runner: online shield colours (patches/net-0004)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

extern const unsigned char fe_dsp_coef_free[];   // CMakeLists.txt: the fetched tree's free DSP coefficient table
extern const unsigned fe_dsp_coef_free_size;

namespace fe::features {
namespace {
// ---- session ----
std::atomic<bool> g_online{false};
uint32_t g_retrace = 0, g_scene_major = 0xFFFFFFFFu, g_scene_minor = 0;
MuHostApi* g_host = nullptr;
MuGameApi* g_game = nullptr;
MuPadStatus g_raw[4]{}, g_out[4]{};

// ---- settings ----
uint32_t shield[4] = {};                 // 0 the port's own, else 0x1000000 | RGB
bool offline_delay_on = false;
int offline_delay_frames = 4;            // 1..9
bool auto_lcancel = false;
bool zelda_sheik = false;
bool squad_strike = false;
bool dsp_lowpass = false;          // the console's DSP low-pass on voices (upstream MELEE_AUDIO_POLYPHASE, off by default as upstream)        // offline Squad Strike in ordinary VS (Z on the select adds fighters); patches/0006
std::atomic<bool> input_suggestions{true};

uint32_t colour_value(const std::string& value) {
  return value == "-" || value.empty() || value == "0" ? 0u : 0x1000000u | (uint32_t(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFFu);
}
bool setting(const std::string& key, const std::string& value) {
  const int n = std::atoi(value.c_str());
  if (key.size() == 14 && key.compare(0, 13, "shield_colorp") == 0 && key[13] >= '1' && key[13] <= '4') shield[key[13] - '1'] = colour_value(value);
  else if (key == "offline_delay") offline_delay_on = value == "1";
  else if (key == "offline_delay_frames") offline_delay_frames = std::clamp(n, 1, 9);
  else if (key == "autolcancel") auto_lcancel = value == "1";
  else if (key == "zelda_as_sheik") zelda_sheik = value == "1";
  else if (key == "squad_strike") squad_strike = value == "1";
  else if (key == "dsp_lowpass") dsp_lowpass = value == "1";
  else if (key == "input_suggestions") input_suggestions = value != "0";
  else return false;
  return true;
}

// ---- option words ----
uint32_t (*orig_options)() = nullptr, (*orig_options2)() = nullptr, (*orig_options3)() = nullptr;
constexpr uint32_t kTeWord1 = 0x00007FF0u;     // 20XX TE's bits of word 1 (MU_GAME_OPTION_TE ... _HANDICAP_STOCKS)
uint32_t w_options() {
  const uint32_t own = orig_options ? orig_options() : 0u;
  uint32_t recorded = 0;
  if (replay_word(0, &recorded)) return (own & ~(kTeWord1 | MU_GAME_OPTION_TMCE)) | recorded;
  if (online() || vanilla()) return own;
  return own | training_option_bits() | (mods_tmce() ? MU_GAME_OPTION_TMCE : 0u);
}
// The Practice menu's Tools rows "Projectile hitboxes" (upstream: the Arcade look's switch from the Practice source, joined with the
// settings' own, gx_core.h arcade sources): offline only, display only.
uint32_t (*orig_display)() = nullptr;
uint32_t w_display() {
  uint32_t v = orig_display ? orig_display() : 0u;
  if (!online() && !replay_playing() && !vanilla() && training_practice_display()) v |= MU_DISPLAY_OPTION_PROJECTILE_HITBOXES;
  return v;
}
// The game's only way to read a host variable (MuHostApi.test_getenv). Answered: MU_SQUAD_STRIKE (patches/0006), "1" when the player
// turned offline Squad Strike on and nothing online runs, and while a replay plays back (its recorded squads replay); nothing else.
const char* w_test_getenv(const char* name) {
  if (name && std::strcmp(name, "MU_SQUAD_STRIKE") == 0) return (squad_strike && !online() && !vanilla()) || replay_playing() ? "1" : "0";
  return nullptr;
}
uint32_t w_options2() { uint32_t v = 0; return replay_word(1, &v) ? v : orig_options2 ? orig_options2() : 0u; }
uint32_t w_options3() { uint32_t v = 0; return replay_word(2, &v) ? v : orig_options3 ? orig_options3() : 0u; }

// ---- shield colours ----
uint32_t shield_color(int32_t slot) {
  if (slot < 0 || slot > 3) return 0;
  // Online: this player's shield_color_online, the opponent's own choice when their build sent one (patches/net-0004),
  // otherwise the port's colour. Display only.
  if (online()) return netplay_bridge::shield_color(slot);
  return shield[slot];
}

// ---- offline input delay (as upstream's Queue, for MuPadStatus) ----
struct DelayQueue {
  std::array<std::array<MuPadStatus, 4>, 9> history{};
  std::array<MuPadStatus, 4> output{};
  uint32_t tick = 0, scene = 0; int delay = 0, position = 0; bool active = false;
  void apply(MuPadStatus pads[4], bool on, int frames, uint32_t now, uint32_t scene_id) {
    frames = std::clamp(frames, 1, 9);
    if (!on) { active = false; return; }
    const bool reset = !active || frames != delay || scene_id != scene || now < tick || now - tick > 1;
    if (reset) {
      history = {}; output = {}; position = 0;
      for (auto& frame : history) for (int p = 0; p < 4; ++p) frame[size_t(p)].err = pads[p].err;
    } else if (now == tick) { std::memcpy(pads, output.data(), sizeof output); return; }
    active = true; delay = frames; tick = now; scene = scene_id;
    output = history[size_t(position)];
    for (int p = 0; p < 4; ++p)
      if (output[size_t(p)].err != pads[p].err || pads[p].err != 0) {
        for (auto& frame : history) { frame[size_t(p)] = {}; frame[size_t(p)].err = pads[p].err; }
        output[size_t(p)] = {}; output[size_t(p)].err = pads[p].err;
      }
    std::memcpy(history[size_t(position)].data(), pads, sizeof output);
    position = (position + 1) % frames;
    std::memcpy(pads, output.data(), sizeof output);
  }
} delay_queue;

// ---- automatic L-cancel: upstream lcancel.cpp's apply(), the automatic part, on the game's lcancel_view ----
constexpr int32_t kAttackAirN = 65, kAttackAirLw = 69, kKneeBend = 24, kFallAerialB = 34;
constexpr uint16_t kPadZ = 0x0010, kPadR = 0x0020, kPadL = 0x0040, kPadA = 0x0100;
constexpr int kCStickPush = 40, kPredictFrames = 9;
struct PadCal { int min = 30, max = 180, scale = 150; bool shift = true; float deadzone = 0.30f; };
PadCal pad_cal_of(const MuLcancelView& v) {
  PadCal c;
  if (v.pad_scale > 0 && v.pad_max > v.pad_min && v.pad_min >= 0) { c.min = v.pad_min; c.max = v.pad_max; c.scale = v.pad_scale; c.shift = v.pad_shift == 1; }
  if (v.have_common && v.trigger_deadzone > 0.0f && v.trigger_deadzone < 1.0f) c.deadzone = v.trigger_deadzone;
  return c;
}
float nml_trigger(uint8_t raw, const PadCal& c) {
  int v = raw;
  if (v < c.min) return 0.0f;
  if (v > c.max) v = c.max;
  if (c.shift) v -= c.min;
  return float(v) / float(c.scale);
}
uint8_t inject_value(const PadCal& c) {
  const float target = std::min(0.95f, c.deadzone + 0.45f);
  int v = int(target * float(c.scale) + 0.5f);
  if (c.shift) v += c.min;
  v = std::min(std::min(v, c.max), 254);
  v = std::max(v, c.min + 1);
  if (nml_trigger(uint8_t(v), c) <= c.deadzone) v = std::min(254, c.max);
  return uint8_t(v);
}
bool effective_lr(const MuPadStatus& p, const PadCal& c) {
  if (p.button & (kPadL | kPadR | kPadZ)) return true;
  return std::max(nml_trigger(p.trigger_l, c), nml_trigger(p.trigger_r, c)) > c.deadzone;
}
struct PortState {
  bool last_effective_lr = false; uint16_t last_button = 0; bool last_c_out = false;
  uint16_t converted = 0; int since_edge = 255, predict = 0; bool was_active = false;
};
PortState lc_ports[4];
uint64_t lc_presses = 0;
bool lc_logged = false;

void lcancel_apply(MuPadStatus pads[4], const MuLcancelView& view) {
  const PadCal cal = pad_cal_of(view);
  const uint8_t press = inject_value(cal);
  const uint8_t full = uint8_t(std::clamp(cal.max, cal.min + 1, 254));
  const float z_level = view.z_analog > cal.deadzone && view.z_analog < 1.0f ? view.z_analog : 0.35f;
  const uint8_t z_value = uint8_t(std::clamp(int(z_level * float(cal.scale) + 0.5f) + (cal.shift ? cal.min : 0), cal.min + 1, 254));
  for (int p = 0; p < 4; ++p) {
    PortState& st = lc_ports[p];
    MuPadStatus& pad = pads[p];
    const MuLcancelFighter& fighter = view.port[p];
    const uint16_t raw_button = pad.button;
    const bool c_out = std::abs(int(pad.sub_x)) >= kCStickPush || std::abs(int(pad.sub_y)) >= kCStickPush;
    if (!fighter.present) {
      st = PortState{}; st.last_effective_lr = effective_lr(pad, cal); st.last_button = raw_button; st.last_c_out = c_out;
      continue;
    }
    const int32_t motion = fighter.motion_id;
    const uint16_t shoulders = raw_button & (kPadL | kPadR | kPadZ);
    const uint16_t fresh = shoulders & uint16_t(~st.last_button);
    const bool a_rising = (raw_button & kPadA) && !(st.last_button & kPadA);
    const bool aerial = fighter.ground_or_air == 1 && motion >= kAttackAirN && motion <= kAttackAirLw;
    const bool jumping = motion >= kKneeBend && motion <= kFallAerialB;
    if ((a_rising || (c_out && !st.last_c_out) || (fresh & kPadZ)) && jumping) st.predict = kPredictFrames;
    else if (st.predict > 0 && !(jumping || aerial)) st.predict = 0;
    else if (st.predict > 0) --st.predict;
    const bool active = aerial || (st.predict > 0 && st.predict < kPredictFrames);
    st.converted &= shoulders;
    if (active) st.converted |= shoulders & uint16_t(~fresh);
    const bool own_analog = std::max(nml_trigger(pad.trigger_l, cal), nml_trigger(pad.trigger_r, cal)) > cal.deadzone;
    const bool hold = st.converted != 0 || own_analog;
    bool on;
    if (!active) on = hold || fresh;
    else if (fresh) on = true;
    else if (a_rising) on = hold;
    else if (hold) on = !(st.last_effective_lr && st.since_edge >= 2);
    else on = !st.last_effective_lr && (st.since_edge >= 3 || !st.was_active);
    uint16_t button = raw_button & uint16_t(~st.converted);
    if (st.converted & kPadZ) button |= kPadA;
    uint8_t l = pad.trigger_l, r = pad.trigger_r;
    if (on) {
      if (st.converted & kPadL) l = std::max(l, full);
      if (st.converted & kPadR) r = std::max(r, full);
      if (st.converted & kPadZ) l = std::max(l, z_value);
      if (active && !hold && !fresh) { if (l < press) ++lc_presses; l = std::max(l, press); }
    } else {
      if (nml_trigger(l, cal) > cal.deadzone) l = 0;
      if (nml_trigger(r, cal) > cal.deadzone) r = 0;
    }
    pad.button = button; pad.trigger_l = l; pad.trigger_r = r;
    st.was_active = active;
    const bool emitted_on = effective_lr(pad, cal);
    st.since_edge = emitted_on && !st.last_effective_lr ? 0 : std::min(st.since_edge + 1, 255);
    st.last_effective_lr = emitted_on;
    st.last_button = raw_button; st.last_c_out = c_out;
  }
}

// ---- the pad wrapper ----
void (*orig_pad_read)(MuPadStatus out[4]) = nullptr;
uint32_t last_pad_retrace = 0xFFFFFFFFu;
MuPadStatus last_pads[4]{};
void pad_read(MuPadStatus out[4]) {
  if (orig_pad_read) orig_pad_read(out); else std::memset(out, 0, sizeof(MuPadStatus) * 4);
  // The game may read twice inside one retrace: the second read gets the first one's answer, so the queue
  // and the L-cancel presses advance once per retrace.
  if (g_retrace == last_pad_retrace) { std::memcpy(out, last_pads, sizeof last_pads); return; }
  last_pad_retrace = g_retrace;
  std::memcpy(g_raw, out, sizeof g_raw);
  const bool replay = replay_playing();
  replay_pad_hook(out);
  training_pad_hook(out);
  const bool offline = !online() && !replay && !vanilla();
  MuLcancelView view{};
  bool gameplay = false;
  const MuGameApi* game = fe::features::game();
  if (offline && (offline_delay_on || auto_lcancel) && game && game->lcancel_view) {
    game->lcancel_view(&view);
    for (const auto& f : view.port) gameplay |= f.present != 0;
  }
  delay_queue.apply(out, offline_delay_on && offline && gameplay, offline_delay_frames, g_retrace, (g_scene_major << 16) | g_scene_minor);
  if (auto_lcancel && offline && gameplay) {
    if (!lc_logged) { lc_logged = true; say("options: automatic L-cancel on (offline)"); }
    lcancel_apply(out, view);
  } else {
    for (auto& st : lc_ports) st = PortState{};
  }
  std::memcpy(last_pads, out, sizeof last_pads);
  std::memcpy(g_out, out, sizeof g_out);
}

// ---- the DSP coefficient table (upstream ax_ucode.cpp polyphase_coefs, tools/package_release.py) ----
// With dsp_lowpass 1 the voices go through the console's polyphase low-pass, which needs a coefficient table: a player's own
// console dump in <app folder>/User/GC/dsp_coef.bin first, else the free table this program carries (Dolphin's free DSP ROM
// table v0.2.1, GPL-2.0-or-later), written once to <app folder>/Sys/GC/dsp_coef.bin. The fetched ax_ucode.cpp reads the file
// named by MELEE_DSP_COEF and logs its Adler-32. Off (the default, as upstream): Slippi-compatible conversion, no table read.
void dsp_table() {
  if (!dsp_lowpass) return;
  std::string base = options.app && !options.base_dir.empty() ? options.base_dir : options.out_dir;
  if (!base.empty() && base.back() != '/') base += '/';
  std::string path = base + "User/GC/dsp_coef.bin";
  const char* which = "the player's own table";
  if (FILE* f = std::fopen(path.c_str(), "rb")) std::fclose(f);
  else {
    path = base + "Sys/GC/dsp_coef.bin";
    which = "the free table (Dolphin's free DSP ROM v0.2.1)";
    FILE* sys = std::fopen(path.c_str(), "rb");
    if (sys) std::fclose(sys);
    else if (fe_dsp_coef_free_size == 4096) {
      for (const char* d : {"Sys", "Sys/GC"}) mkdir((base + d).c_str(), 0777);
      if (FILE* w = std::fopen(path.c_str(), "wb")) { std::fwrite(fe_dsp_coef_free, 1, 4096, w); std::fclose(w); }
    } else { say("options: dsp_lowpass: this build carries no free coefficient table; the low-pass stays off"); return; }
  }
  setenv("MELEE_DSP_COEF", path.c_str(), 1);
  setenv("MELEE_AUDIO_POLYPHASE", "1", 1);
  say("options: console low-pass on voices with %s (%s)", which, path.c_str());
}

// ---- tables, retrace, commands ----
void tables(MuHostApi* host, MuGameApi* game) {
  if (game) g_game = game;                   // called twice: the host's table first, then the game's
  if (!host) return;
  g_host = host;
  orig_options = host->game_options; host->game_options = w_options;
  orig_options2 = host->game_options2; host->game_options2 = w_options2;
  orig_options3 = host->game_options3; host->game_options3 = w_options3;
  if (host->display_options) { orig_display = host->display_options; host->display_options = w_display; }
  if (!host->test_getenv) host->test_getenv = w_test_getenv;
  dsp_table();
  orig_pad_read = host->pad_read; host->pad_read = pad_read;
  host->shield_color = shield_color;
  say("options: shield colours %06X %06X %06X %06X%s, offline input delay %s (%d frames), automatic L-cancel %s, Zelda as Sheik %s",
      shield[0] & 0xFFFFFFu, shield[1] & 0xFFFFFFu, shield[2] & 0xFFFFFFu, shield[3] & 0xFFFFFFu,
      (shield[0] | shield[1] | shield[2] | shield[3]) ? "" : " (the ports' own)", offline_delay_on ? "on" : "off", offline_delay_frames,
      auto_lcancel ? "on" : "off", zelda_sheik ? "on" : "off");
  say("options: offline Squad Strike %s", squad_strike ? "on (Z on the VS character select adds fighters)" : "off");
}
void on_retrace(const Retrace& info) {
  g_retrace = info.retrace;
  g_online.store(info.online || info.scene_major == 8, std::memory_order_relaxed);
  if (info.scene_major != g_scene_major || info.scene_minor != g_scene_minor) {
    g_scene_major = info.scene_major; g_scene_minor = info.scene_minor;
    training_scene_changed();
  }
}
int zelda_as_sheik(uint8_t, const uint8_t*, uint32_t size, std::vector<uint8_t>& reply) {
  if (size != 0) return 0;
  const bool on = zelda_sheik && !online() && !replay_playing() && !vanilla();
  if (on) say("options: Zelda starts as Sheik (setting)");
  reply.push_back(on ? 1 : 0);
  return 1;
}
void run_end() {
  if (auto_lcancel) say("options: automatic L-cancel pressed %llu times", static_cast<unsigned long long>(lc_presses));
}

std::string on_off(bool v) { return v ? "On" : "Off"; }
}  // namespace

bool online() { return g_online.load(std::memory_order_relaxed); }
bool vanilla() { return settings_vanilla() || settings_vanilla_only(); }
uint32_t retrace() { return g_retrace; }
uint32_t scene_major() { return g_scene_major; }
uint32_t scene_minor() { return g_scene_minor; }
bool feature_test(const char* name, std::string* value) {
  const std::string want = name;
  for (size_t i = 0; i + 1 < options.rest.size(); ++i) {
    if (options.rest[i] != "--feature-test") continue;
    const std::string& v = options.rest[i + 1];
    if (v == want || v.compare(0, want.size() + 1, want + ":") == 0) { if (value) *value = v.size() > want.size() ? v.substr(want.size() + 1) : std::string(); return true; }
  }
  return false;
}
const MuHostApi* host() { return g_host; }
// The game's table: from the tables hook's second call when play mode forwards it, else the host's own copy.
MuGameApi* game() { return g_game ? g_game : const_cast<MuGameApi*>(&linux_host::game_api()); }
const MuPadStatus* pads_raw() { return g_raw; }
const MuPadStatus* pads_out() { return g_out; }
void read_controllers(MuPadStatus out[4]) { if (orig_pad_read) orig_pad_read(out); else std::memset(out, 0, sizeof(MuPadStatus) * 4); }

void register_options() {
  snapshot_store::configure(say, decoder_sync);
  add_setting(setting);
  add_tables(tables);
  add_retrace(on_retrace);
  add_command(0xDE, zelda_as_sheik);
  add_run_end(run_end);
  add_menu_entry({"Game", "Automatic L-cancel (offline)", [] { return on_off(auto_lcancel); }, [](int) { auto_lcancel = !auto_lcancel; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("autolcancel", auto_lcancel ? "1" : "0"); }});
  add_menu_entry({"Game", "Offline input delay", [] { return offline_delay_on ? std::to_string(offline_delay_frames) + " frames" : std::string("Off"); },
                  [](int d) {
                    if (!offline_delay_on) { if (d > 0) { offline_delay_on = true; offline_delay_frames = 1; } return; }
                    const int next = offline_delay_frames + d;
                    if (next < 1) offline_delay_on = false; else offline_delay_frames = std::min(next, 9);
                  }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("offline_delay", offline_delay_on ? "1" : "0"); w("offline_delay_frames", std::to_string(offline_delay_frames)); }});
  add_menu_entry({"Game", "Zelda starts as Sheik", [] { return on_off(zelda_sheik); }, [](int) { zelda_sheik = !zelda_sheik; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("zelda_as_sheik", zelda_sheik ? "1" : "0"); }});
  add_menu_entry({"Game", "Console low-pass on voices (next start)", [] { return on_off(dsp_lowpass); }, [](int) { dsp_lowpass = !dsp_lowpass; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("dsp_lowpass", dsp_lowpass ? "1" : "0"); }});
  add_menu_entry({"Game", "Squad Strike in VS (Z adds fighters, offline)", [] { return on_off(squad_strike); }, [](int) { squad_strike = !squad_strike; }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("squad_strike", squad_strike ? "1" : "0"); }});
  add_menu_entry({"Game", "Input suggestions (online, next game)", [] { return on_off(input_suggestions.load()); },
                  [](int) { input_suggestions = !input_suggestions.load(); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("input_suggestions", input_suggestions.load() ? "1" : "0"); }});
}
}  // namespace fe::features

// The netplay object (patches/net-0001) asks at the start of each online game whether to send and use input suggestions.
extern "C" int mu_net_suggest_setting(void) { return fe::features::input_suggestions.load() ? 1 : 0; }
