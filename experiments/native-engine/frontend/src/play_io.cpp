// Controllers and sound for the native-engine frontend, through the app's own code: port/runtime/host/
// input_sdl.cpp (SDL gamepads through Aurora's PAD mapping, keyboard, script), gc_adapter_usbhs.cpp
// (the GameCube adapter on the console) and audio_sdl.cpp (ring buffer, resampling against the
// device clock, SDL audio callback thread). Those files are compiled unchanged; this one connects
// them to the native host's services through the weak hooks of patches/0101 and supplies the few
// functions they expect from the rest of the app.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host_services.h"       // melee-unlocked: linux_host, MuPadStatus
#include "host.h"                // shim
#if MU_FE_AURORA
#include "audio.h"
namespace host { void audio_set_speed(double speed); }   // patches/0110: the build-time copy of audio_sdl.cpp
#include "window.h"
#include "jukebox.h"
#include <dolphin/pad.h>
#include "app/game_menu.h"       // menu_is_open (the build-time copy with patches/0104)
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include "mu_host.h"
#if MU_FE_AURORA
#include "input_script.h"        // the app's script reader: the input-suggestion test's look ahead
#endif
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe {
namespace {
std::atomic<bool> running{false};        // between io_start and io_stop
std::atomic<uint64_t> last_pad_ns{0};
FILE* raw_out = nullptr;
bool sound_open = false;
uint8_t block[640]; size_t block_fill = 0;   // one 5 ms AI frame, as the app's audio_push takes it
uint32_t quit_since = 0; bool quit_counting = false, quit_sent = false;
uint64_t pad_ns_frame = 0;               // time inside the controller read since io_take_pad_ms
uint64_t refills = 0, refill_ms = 0;     // sound: silence put in after the game stalled (see mu_frontend_audio)
uint64_t stat_frames = 0, stat_ns = 0, stat_underruns = 0;
// The controllers are read by the game's poll (simulation thread) and, during an online game, by the netplay
// object's input-suggestion thread between frames (mu_net_suggest_read): never both at once.
std::mutex input_mutex;
thread_local bool suggestion_read = false;   // this thread is reading for a suggestion: the in-game menu is not run
std::atomic<uint32_t> last_poll_retrace{0};

// A line made while the game runs (the simulation, decoder or render thread): to the log thread.
void hold(const char* line) {
  if (!log_queue_push(line)) say("%s", line);
}

// ---- --auto-vs: from boot into a VS match (Ness against a computer player, Onett), by what the game reports ----
// The scene comes from the game's state snapshot, the character select state from the css-state lines the
// game prints. Inside a screen the steps are counted in retraces from the moment the screen was entered:
// the hand starts in the same place and moves the same distance per retrace every time. The real
// controllers are not read until it is over, so a second pad cannot join or move a hand.
struct AutoVs {
  enum Phase { Off, Running, Done, Failed } phase = Off;
  uint32_t major = 0xFFFFFFFF, minor = 0, entered = 0, menu_tries = 0;
  std::atomic<bool> p1_locked{false}, p2_cpu{false};
  void finish(Phase result, const char* why) {
    phase = result;
    char line[160];
    std::snprintf(line, sizeof line, "auto-vs: %s at retrace %u (scene %u/%u); the controllers are live", why, linux_host::counters().retraces, major, minor);
    hold(line);
  }
  // Fills port 1; returns false when it is over.
  bool step(MuPadStatus out[4]) {
    if (phase != Running) return false;
    std::memset(out, 0, sizeof(MuPadStatus) * 4);
    for (int i = 1; i < 4; ++i) out[i].err = -1;
    MuStatePod state{};
    linux_host::state_snapshot(&state);
    const uint32_t now = linux_host::counters().retraces;
    if (state.scene_major != major || state.scene != minor) { major = state.scene_major; minor = state.scene; entered = now; }
    const uint32_t t = now - entered;
    auto& pad = out[0];
    const auto during = [&](uint32_t from, uint32_t length) { return t >= from && t < from + length; };
    enum : uint16_t { A = 0x0100, START = 0x1000 };
    if (major == 2 && minor == 2) { finish(Done, "the match has started"); return false; }
    if (major == 2 && minor == 0) {                         // character select
      // The hand starts on the first player's HMN tag. Straight up for ten retraces is a fighter in the second
      // row (Falco with everything unlocked); the second player's N/A tag is twelve retraces to the right of
      // the start (ten to fourteen all land on it).
      // With `vanilla 1` the roster is the memory card's and sits differently: up and a little right for thirty
      // retraces is Ness, and ten straight down from there is the tag.
      const bool vanilla = settings_vanilla();
      if (vanilla ? during(240, 30) : during(240, 10)) { pad.stick_y = 127; if (vanilla) pad.stick_x = 61; }
      else if (during(252, 4) && std::getenv("MU_AUTO_VS_Z")) pad.button = 0x0010;   // tests: Z on the hovered fighter (Squad Strike adds it)
      else if (during(vanilla ? 280 : 265, 10)) pad.button = A;
      else if (during(284, 4) && std::getenv("MU_AUTO_VS_SKIN")) pad.button = 0x0020;   // tests: R on the picked costume (the next skin)
      else if (t == 300 && !p1_locked.load()) { finish(Failed, "the fighter was not picked"); return false; }
      else if (during(vanilla ? 338 : 305, 10)) pad.stick_y = -127;
      else if (!vanilla && during(335, 12)) pad.stick_x = 127;
      else if (during(vanilla ? 368 : 365, 10)) pad.button = A;                // a computer player
      else if (t == 430 && !p2_cpu.load()) { finish(Failed, "the second player did not become a computer player"); return false; }
      else if (during(438, 10)) pad.button = START;
      else if (t > 700) { finish(Failed, "the stage select did not open"); return false; }
    } else if (major == 2 && minor == 1) {                  // stage select
      if (during(200, 40)) pad.stick_y = 100;                                 // up onto Onett
      else if (during(260, 10)) pad.button = A;
      else if (t > 600) { finish(Failed, "the match did not start"); return false; }
    } else if (major == 1) {                                // main menu (only without the Boot to CSS code)
      if (during(100, 6)) pad.stick_y = -127;                                 // down to VS Mode
      else if (during(118, 10)) pad.button = START;                           // into it
      else if (during(160, 8)) pad.button = A;                                // Melee
      else if (t > 400) { if (++menu_tries >= 2) { finish(Failed, "the character select did not open"); return false; } entered = now; }
    } else {                                                // boot: memory card message, opening, title
      if (now > 3600) { finish(Failed, "the game did not reach a menu"); return false; }
      if (during(130, 10) && now < 300) pad.button = A;                       // the memory card message
      else if (now >= 400 && (now % 120) < 10) pad.button = START;            // through the opening and the title screen
    }
    return true;
  }
} auto_vs;
}  // namespace

uint64_t io_last_pad_ns() { return last_pad_ns.load(std::memory_order_relaxed); }

void io_start(bool picture) {
  if (!options.audio_raw.empty()) raw_out = std::fopen(options.audio_raw.c_str(), "wb");
#if MU_FE_AURORA
  const int volume = options.volume >= 0 ? options.volume : options.play ? 70 : 0;
  if (picture && (volume > 0 || !options.wav.empty())) {   // SDL is up once Aurora is
#ifdef __SWITCH__
    // SDL's audio thread inherits the caller's priority. The app opens the device from its 0x28 owner
    // thread; this is the simulation thread, at 0x1C.
    s32 mine = 0x2C;
    svcGetThreadPriority(&mine, CUR_THREAD_HANDLE);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28);
#endif
    sound_open = host::audio_open(volume, options.wav.empty() ? nullptr : options.wav.c_str(), volume > 0);
#ifdef __SWITCH__
    svcSetThreadPriority(CUR_THREAD_HANDLE, mine);
#endif
    say("picture: sound %s, volume %d%%%s%s", sound_open ? "on" : "COULD NOT BE OPENED", volume, options.wav.empty() ? "" : ", recorded to ", options.wav.c_str());
  }
  if (options.play && options.auto_vs) { auto_vs.phase = AutoVs::Running; say("picture: --auto-vs: the controllers are read once the match has started"); }
  if (options.play && !options.play_script.empty())
    say("picture: controller script %s: %s", options.play_script.c_str(), host::input_load_script(options.play_script.c_str()) ? "loaded" : "COULD NOT BE LOADED");
#endif
  block_fill = 0; quit_counting = false; quit_sent = false;
  refills = refill_ms = 0; stat_frames = 0; stat_ns = 0; stat_underruns = 0;
  running.store(true);
  if ((options.play || picture) && !options.log_direct) log_queue_start();   // from here to io_stop no thread of the game writes a log line itself
}

void io_stop() {
  running.store(false);
  log_queue_stop();
#if MU_FE_AURORA
  if (sound_open) {
    uint64_t silent_ms = 0; const uint64_t underruns = host::audio_underruns(&silent_ms);
    double low = 1, high = 1; host::audio_rate_range(&low, &high);
    say("picture: sound: %.1f s handed to the output, %llu blocks dropped (ring full), %llu underruns (%llu ms of held sample), playback rate %.4f..%.4f of nominal; "
        "%llu refills after a stall (%llu ms of silence put in)", double(host::audio_pushed_frames()) / 32000.0, (unsigned long long)host::audio_dropped_blocks(),
        (unsigned long long)underruns, (unsigned long long)silent_ms, low, high, (unsigned long long)refills, (unsigned long long)refill_ms);
    host::audio_close();
    sound_open = false;
  }
  if (options.play) host::gcadapter_shutdown();
#endif
  if (raw_out) { std::fclose(raw_out); raw_out = nullptr; }
  if (auto_vs.phase == AutoVs::Running) auto_vs.finish(AutoVs::Failed, "the run ended first");
}

// Slippi's time sync runs the game up to 1 % off real time online; the output resamples at that speed (patches/0110).
void io_game_speed(double speed) {
#if MU_FE_AURORA
  host::audio_set_speed(speed);
#else
  (void)speed;
#endif
}

double io_take_pad_ms() { const double ms = double(pad_ns_frame) * 1e-6; pad_ns_frame = 0; return ms; }

// Once per retrace on the simulation thread. Every 600 retraces: the sound path's line, as the app logs it
// every ten seconds (port/runtime/host/host.cpp), with the scene and what the last ten seconds added.
void io_retrace(uint32_t retrace, uint32_t scene_major, uint32_t scene_minor, bool fighting) {
#if MU_FE_AURORA
  if (!sound_open || retrace % 600) return;
  const uint64_t now = host::now_ns(), frames = host::audio_pushed_frames();
  uint64_t silent_ms = 0; const uint64_t underruns = host::audio_underruns(&silent_ms);
  double low = 1, high = 1; host::audio_rate_range(&low, &high);
  if (stat_ns) {
    char line[320];
    std::snprintf(line, sizeof line, "audio: game made %.0f samples/s (32000 expected), dropped blocks %llu, underruns %llu (%llu ms), output speed %.4f..%.4f"
                  " | retrace %u scene %u/%u%s: %llu new underruns, %llu refills so far, %u ms queued", double(frames - stat_frames) / (double(now - stat_ns) * 1e-9),
                  (unsigned long long)host::audio_dropped_blocks(), (unsigned long long)underruns, (unsigned long long)silent_ms, low, high, retrace, scene_major,
                  scene_minor, fighting ? " (match)" : "", (unsigned long long)(underruns - stat_underruns), (unsigned long long)refills, host::audio_buffered_ms());
    hold(line);
  }
  stat_frames = frames; stat_ns = now; stat_underruns = underruns;
#else
  (void)retrace; (void)scene_major; (void)scene_minor; (void)fighting;
#endif
}
}  // namespace fe

// ---- hooks called by the host services (patches/0101) ----

// Every line the game prints. The character select state, for --auto-vs:
//   css-state: {"major":2,"minor":0,"p1":{"kind":"HMN","fighter":11,"name":"Ness","locked":true},"p2":{"kind":"CPU",...
extern "C" void mu_frontend_game_log(const char* text) {
  if (fe::auto_vs.phase != fe::AutoVs::Running || std::strncmp(text, "css-state:", 10) != 0) return;
  const char* p1 = std::strstr(text, "\"p1\":{");
  const char* p2 = std::strstr(text, "\"p2\":{");
  if (!p1 || !p2) return;
  const std::string one(p1, p2), two(p2, std::strchr(p2, '}') ? std::strchr(p2, '}') : p2 + std::strlen(p2));
  fe::auto_vs.p1_locked.store(one.find("\"locked\":true") != std::string::npos);
  fe::auto_vs.p2_cpu.store(two.find("\"kind\":\"CPU\"") != std::string::npos);
}

// Every line of the host services (the game's own lines as "[game] ...", the mixer's) and host::log of the
// decoder and the app's input code. In play mode while the game runs it goes to the log thread; otherwise
// (the scripted runs: only host::log lines get here with `running`) it is kept as before or written by the host.
extern "C" int mu_frontend_log(const char* line) {
  if (!fe::running.load(std::memory_order_relaxed)) return 0;
  return fe::log_queue_push(line) ? 1 : 0;
}

// Each block the mixer finishes, on the simulation thread. The app's audio_push takes what the
// console's audio interface would: big-endian right/left pairs in 5 ms blocks. It only copies into a
// ring; the device is fed from SDL's audio thread.
extern "C" void mu_frontend_audio(const int16_t* rl, uint32_t frames) {
  if (!fe::running.load(std::memory_order_relaxed)) return;
  if (fe::raw_out) std::fwrite(rl, 4, frames, fe::raw_out);
#if MU_FE_AURORA
  if (!fe::sound_open) return;
  // After the game stalled (a scene loading from the SD card) the ring is empty and the output is holding
  // its last sample. The app's output code would then rebuild its cushion only by playing 1.5% slow, a few
  // seconds during which every late frame is another gap. Put the cushion back at once instead: the gap has
  // happened, 40 ms of silence makes it a little longer and ends it.
  if (fe::block_fill == 0 && host::audio_buffered_ms() < 6) {
    static const uint8_t silence[640] = {};
    ++fe::refills;
    for (int i = 0; i < 8; ++i) { host::audio_push(silence, sizeof silence); fe::refill_ms += 5; }
  }
  for (uint32_t i = 0; i < frames * 2; ++i) {
    fe::block[fe::block_fill++] = uint8_t(uint16_t(rl[i]) >> 8);
    fe::block[fe::block_fill++] = uint8_t(rl[i]);
    if (fe::block_fill == sizeof fe::block) { host::audio_push(fe::block, sizeof fe::block); fe::block_fill = 0; }
  }
#endif
}

#if MU_FE_AURORA
// The game reads its four controllers (only reached when no runner script drives them).
// The netplay object (patches/net-0001, input suggestions), weak: told right before each poll.
extern "C" __attribute__((weak)) void mu_net_suggest_note_poll(void);
extern "C" int mu_frontend_pad_read(MuPadStatus out[4]) {
  if (!fe::options.play || !fe::running.load(std::memory_order_relaxed)) return 0;
  const uint64_t t0 = host::now_ns();
  if (mu_net_suggest_note_poll) mu_net_suggest_note_poll();   // a change read from here on is in the next frame's pad
  if (!fe::auto_vs.step(out)) {
    host::PadState pads[4];
    {
      std::lock_guard<std::mutex> lock(fe::input_mutex);
      fe::last_poll_retrace.store(host::retrace_count(), std::memory_order_relaxed);
      host::input_poll(pads);   // the player's remapping and controller setup are applied inside (menu_frame below)
    }
    static_assert(sizeof(host::PadState) == sizeof(MuPadStatus), "the two pad structures have the same fields in the same order");
    std::memcpy(out, pads, sizeof pads);
  }
  const uint64_t t1 = host::now_ns();
  fe::pad_ns_frame += t1 - t0;
  fe::last_pad_ns.store(t1, std::memory_order_relaxed);
  return 1;
}
extern "C" void mu_frontend_pad_rumble(int32_t port, int32_t on) {
  if (!fe::options.play || !fe::running.load(std::memory_order_relaxed) || port < 0 || port >= 4) return;
  host::gcadapter_rumble(port, on != 0);
  // A controller Aurora's PAD put on the port (Pro Controller, Joy-Con, a pad seen through SDL), unless its
  // saved configuration turned rumble off. The adapter's ports are rumbled by the adapter code above.
  if (on == 0 || fe::app_rumble_allowed(port)) host::gamepad_rumble(port, on != 0);
}

namespace fe {
bool menu_masks(host::PadState pads[4]);   // below: the in-game menu's hand-over masks, without changing them
namespace {
// MU_SUGGEST_LEAD_MS=N (tests only): a controller script changes its pads exactly at a poll, so between polls there is
// never anything new to suggest. With this set, a read N ms before the next poll returns the pad the script gives that
// poll: a change made N ms before the poll, the same inputs either way. Time-keyed scripts only (no scene blocks).
struct Lead {
  bool on = false;
  double seconds = 0;
  host::InputScript script;
  Lead() {
    const char* v = std::getenv("MU_SUGGEST_LEAD_MS");
    const double ms = v && *v ? std::atof(v) : 0.0;
    if (ms <= 0.0 || ms >= 16.0 || options.play_script.empty()) return;
    std::ifstream file(options.play_script);
    on = file && script.load(file);
    seconds = ms / 1000.0;
  }
};
}  // namespace
}  // namespace fe

// The netplay object's input-suggestion thread (patches/net-0001), about a thousand times a second during an online
// game: the local player's controller on `port`, as the game will get it at its next poll, in wire form. The app's
// whole input path (GameCube adapter, SDL gamepads through Aurora's PAD, the player's remapping), never at the same
// time as the game's own poll; the in-game menu is left alone (no suggestion while it is open, and what it keeps from
// the game is kept from the suggestion too). 0 when there is nothing to read.
extern "C" int mu_net_suggest_read(int port, uint8_t out[8]) {
  if (!fe::options.play || !fe::running.load(std::memory_order_relaxed) || port < 0 || port > 3) return 0;
  static const bool generated = std::getenv("MELEE_ONLINE_TEST_INPUTS") != nullptr;   // the game's own test inputs play
  if (generated || fe::auto_vs.phase == fe::AutoVs::Running) return 0;
#ifdef __SWITCH__
  // The sampler thread (started by the netplay object at an online game's start) runs beside the render thread on core
  // 1, just above it: about a thousand wake-ups a second of a few microseconds each. Core 2 stays the game's alone, core 0
  // the decoder's and the GameCube adapter reader's.
  static thread_local bool placed = false;
  if (!placed) {
    placed = true;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x27);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1u << 1);
  }
#endif
  host::PadState pads[4];
  static fe::Lead lead;
  if (lead.on) {
    const uint32_t polled = fe::last_poll_retrace.load(std::memory_order_relaxed);
    const bool ahead = double(host::now_ns() - fe::last_pad_ns.load(std::memory_order_relaxed)) * 1e-9 >= 1.0 / 60.0 - lead.seconds;
    const auto sample = lead.script.sample(ahead ? polled + 1 : polled);
    for (int i = 0; i < 4; ++i) {
      pads[i] = {};
      pads[i].err = sample[i].connected ? 0 : -1;
      pads[i].button = sample[i].buttons;
      pads[i].stick_x = sample[i].sx; pads[i].stick_y = sample[i].sy;
      pads[i].sub_x = sample[i].cx; pads[i].sub_y = sample[i].cy;
    }
  } else {
    std::lock_guard<std::mutex> lock(fe::input_mutex);
    fe::suggestion_read = true;
    host::input_poll(pads);
    fe::suggestion_read = false;
    if (!fe::menu_masks(pads)) return 0;
  }
  const host::PadState& p = pads[port];
  if (p.err != 0) return 0;
  out[0] = uint8_t(p.button >> 8); out[1] = uint8_t(p.button);
  out[2] = uint8_t(p.stick_x); out[3] = uint8_t(p.stick_y); out[4] = uint8_t(p.sub_x); out[5] = uint8_t(p.sub_y);
  out[6] = p.trig_l; out[7] = p.trig_r;
  return 1;
}

// ---- what the app's sources expect from the rest of the app ----
namespace host {
uint32_t retrace_count() { return linux_host::counters().retraces; }
void audio_session_report() {}
void window_gamepad_rumble(int port, bool on) { gamepad_rumble(port, on); }
// game_menu.cpp's menu_frame, compiled under this name (CMakeLists.txt): the app's in-game menu.
void app_menu_frame(PadState pads[4]);
// input_poll calls this with every source merged. The app (app mode): the app's in-game menu, opened with
// L + R + Start held for half a second, while the game runs. Plain play mode keeps the earlier way out:
// L + R + Start held, without A (Melee's own L + R + A + Start is left alone), for a second and a half, ends the run.
// Hand-over between the menu and the game (app mode). While the menu is open the game sees neutral pads (the
// menu does that). Whatever is held at the moment it closes (the B or Start that closed it, a direction) is
// swallowed until that button is let go, so the closing press never reaches the game; everything else goes through.
// A few log lines record what the menu and the game were given around an open menu (console diagnosis).
namespace {
uint16_t swallow[4] = {};                // per port: buttons held when the menu closed, not yet released
bool swallow_triggers[4] = {};
bool menu_was_open = false;
bool combo_hold[4] = {};                 // per port: the menu's L + R + Start is held (kept from the game until let go)
uint16_t logged_buttons[4] = {};
uint32_t log_until = 0;                  // retrace until which button changes after a close are logged
void log_changes(const PadState pads[4], const char* who) {
  for (int i = 0; i < 4; ++i) {
    const uint16_t b = pads[i].err == 0 ? pads[i].button : 0;
    if (b == logged_buttons[i]) continue;
    logged_buttons[i] = b;
    log("menu: %s, pad %d buttons %04X (stick %d %d)", who, i + 1, b, pads[i].stick_x, pads[i].stick_y);
  }
}
}  // namespace
void menu_frame(PadState pads[4]) {
  // The app: the player's remapping and controller setup (app.cpp, controls.cpp) come first, so the menu, its hand-over
  // masks and the chords see the pads as the game would. Lane 1: the input-suggestion reads keep their own filter state.
  PadState unmapped[4];
  if (fe::options.app) {
    std::memcpy(unmapped, pads, sizeof unmapped);
    fe::app_pad_postprocess(pads, fe::suggestion_read ? 1 : 0);
  }
  if (fe::suggestion_read) return;                 // a read for an input suggestion: fe::menu_masks applies the masks
  if (fe::options.app) {
    if (!fe::running.load(std::memory_order_relaxed)) return;
    fe::controls_chords(pads);                     // Start + D-pad Down / Up + Z (upstream's chords), Training's lone Start
    PadState raw[4];
    std::memcpy(raw, pads, sizeof raw);
    if (menu_was_open) log_changes(raw, "the menu reads");
    if (fe::controls_capture_frame(unmapped)) {    // a binding is being captured: the press is not the menu's
      PadState neutral[4];
      for (int i = 0; i < 4; ++i) { neutral[i] = {}; neutral[i].err = pads[i].err; }
      app_menu_frame(neutral);
      for (int i = 0; i < 4; ++i) { pads[i] = neutral[i]; }
    } else {
      app_menu_frame(pads);
    }
    const bool open = menu_is_open();
    if (menu_was_open && !open) {
      for (int i = 0; i < 4; ++i) {
        swallow[i] = raw[i].err == 0 ? raw[i].button : 0;
        swallow_triggers[i] = raw[i].err == 0 && (raw[i].trig_l > 30 || raw[i].trig_r > 30);
        logged_buttons[i] = 0;
      }
      log_until = retrace_count() + 600;
      log("menu: closed with buttons %04X %04X %04X %04X held: kept from the game until let go", swallow[0], swallow[1], swallow[2], swallow[3]);
    }
    if (!open) {
      for (int i = 0; i < 4; ++i) {
        if (raw[i].err != 0) { swallow[i] = 0; swallow_triggers[i] = false; combo_hold[i] = false; continue; }
        // The menu's own combo never reaches the game: held for the half second it takes to open the menu, L + R +
        // Start left the game ignoring A and Start afterwards (Online Play page, reproduced on Linux without the menu
        // opening). Melee's L + R + A + Start (A held) still goes through, as the menu leaves it alone.
        const uint16_t b = raw[i].button;
        if ((b & 0x1060) == 0x1060 && !(b & 0x0100)) combo_hold[i] = true;
        else if (!(b & 0x1060)) combo_hold[i] = false;
        if (combo_hold[i]) { pads[i].button &= uint16_t(~0x1060); pads[i].trig_l = 0; pads[i].trig_r = 0; }
        swallow[i] &= raw[i].button;                       // released bits stop being swallowed (the controller's own state)
        if (swallow_triggers[i] && raw[i].trig_l <= 30 && raw[i].trig_r <= 30) swallow_triggers[i] = false;
        pads[i].button &= uint16_t(~swallow[i]);
        if (swallow_triggers[i]) { pads[i].trig_l = 0; pads[i].trig_r = 0; }
      }
      if (retrace_count() < log_until) log_changes(pads, "the game gets");
    }
    menu_was_open = open;
    return;
  }
  bool held = false;
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0) continue;
    const uint32_t b = pads[i].button;
    if ((b & 0x1060) == 0x1060 && !(b & 0x0100)) held = true;   // L 0x40, R 0x20, Start 0x1000; A 0x100
  }
  const uint32_t now = retrace_count();
  if (!held) { fe::quit_counting = false; return; }
  if (!fe::quit_counting) { fe::quit_counting = true; fe::quit_since = now; }
  if (!fe::quit_sent && now - fe::quit_since >= 90) {
    fe::quit_sent = true;
    log("play: L+R+Start held: stopping");
    linux_host::request_stop(0);
  }
}
}  // namespace host
namespace fe {
// For an input-suggestion read (input_mutex held): false while the in-game menu is open; otherwise the masks the last
// poll left (the button that closed the menu, the menu's own L + R + Start) applied as the game would get them.
bool menu_masks(host::PadState pads[4]) {
  if (!options.app) return true;
  if (host::menu_is_open()) return false;
  for (int i = 0; i < 4; ++i) {
    if (pads[i].err != 0) continue;
    if (host::combo_hold[i]) { pads[i].button &= uint16_t(~0x1060); pads[i].trig_l = 0; pads[i].trig_r = 0; }
    pads[i].button &= uint16_t(~host::swallow[i]);
    if (host::swallow_triggers[i]) { pads[i].trig_l = 0; pads[i].trig_r = 0; }
  }
  return true;
}
}  // namespace fe
namespace host {
#ifdef __SWITCH__
// The app's adapter code (gc_adapter_usbhs.cpp, compiled with its poll renamed) looks for the adapter every two
// seconds from whoever polls it, here the simulation thread, for as long as there is none. Three looks at the
// start, then one a minute, until an adapter has answered once.
uint32_t gcadapter_poll_usb(PadState out[4]);
uint32_t gcadapter_poll(PadState out[4]) {
  static bool seen = false; static unsigned looks = 0; static uint64_t next = 0;
  const uint64_t now = now_ns();
  if (!seen && now < next) return 0;
  const uint32_t mask = gcadapter_poll_usb(out);
  if (mask) seen = true;
  else if (!seen) next = now + (++looks < 3 ? 2100000000ull : 60000000000ull);
  return mask;
}
#endif
}  // namespace host
namespace slippi::jukebox { void mix(int16_t*, size_t, int) {} }   // the Slippi music player is not part of this program

// The screens before the game (switch_signin.cpp with patches/0105) read the controllers here: the app's whole
// input path (GameCube adapter, SDL gamepads through Aurora's PAD, the MELEE_PAD_FILE test file), as PADStatus.
namespace host {
void signin_pad_read(PADStatus status[4]) {
  PadState pads[4];
  input_poll(pads);
  for (int i = 0; i < 4; ++i) {
    status[i] = {};
    status[i].err = pads[i].err == 0 ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
    status[i].button = pads[i].button;
    status[i].stickX = pads[i].stick_x; status[i].stickY = pads[i].stick_y;
    status[i].substickX = pads[i].sub_x; status[i].substickY = pads[i].sub_y;
    status[i].triggerLeft = pads[i].trig_l; status[i].triggerRight = pads[i].trig_r;
  }
}
}  // namespace host
#endif
