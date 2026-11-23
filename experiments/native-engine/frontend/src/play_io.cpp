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
#include "window.h"
#include "jukebox.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe {
namespace {
std::atomic<bool> running{false};        // between io_start and io_stop
std::atomic<uint64_t> last_pad_ns{0};
std::mutex held_mutex;
std::string held;                        // host::log lines of the run, written afterwards
bool held_overflow = false;
FILE* raw_out = nullptr;
bool sound_open = false;
uint8_t block[640]; size_t block_fill = 0;   // one 5 ms AI frame, as the app's audio_push takes it
uint32_t quit_since = 0; bool quit_counting = false, quit_sent = false;

// --auto-vs: from boot into a VS match with one controller, in the app's script format (retrace, then the
// state held until the next line). Found by running it: the same retraces work with an empty and with an
// existing memory card. On the console real controllers add to the script, so the player takes over when
// the match starts (or earlier).
const char kAutoVs[] =
  "130 A\n140\n"            // the memory card message
  "400 START\n410\n"        // title screen
  "502 sy=-127\n508\n"      // main menu: down to VS Mode
  "520 START\n530\n"        // into VS Mode
  "562 A\n570\n"            // Melee
  "802 sx=61 sy=127\n832\n" // character select: hand up to Ness
  "842 A\n852\n"
  "900 sy=-127\n910\n"      // down to the second player's N/A tag
  "930 A\n940\n"            // a computer player with a random character
  "1000 START\n1010\n"      // to the stage select
  "1200 sy=100\n1240\n"     // up to Onett
  "1260 A\n1270\n";
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
  if (options.play && options.auto_vs && options.play_script.empty()) {
    options.play_script = options.out_dir + "/auto-vs.txt";
    if (FILE* out = std::fopen(options.play_script.c_str(), "w")) { std::fputs(kAutoVs, out); std::fclose(out); }
  }
  if (options.play && !options.play_script.empty())
    say("picture: controller script %s: %s", options.play_script.c_str(), host::input_load_script(options.play_script.c_str()) ? "loaded" : "COULD NOT BE LOADED");
#endif
  block_fill = 0; quit_counting = false; quit_sent = false;
  running.store(true);
}

void io_stop() {
  running.store(false);
#if MU_FE_AURORA
  if (sound_open) {
    uint64_t silent_ms = 0; const uint64_t underruns = host::audio_underruns(&silent_ms);
    double low = 1, high = 1; host::audio_rate_range(&low, &high);
    say("picture: sound: %.1f s handed to the output, %llu blocks dropped (ring full), %llu underruns (%llu ms of held sample), playback rate %.4f..%.4f of nominal",
        double(host::audio_pushed_frames()) / 32000.0, (unsigned long long)host::audio_dropped_blocks(), (unsigned long long)underruns, (unsigned long long)silent_ms, low, high);
    host::audio_close();
    sound_open = false;
  }
  if (options.play) host::gcadapter_shutdown();
#endif
  if (raw_out) { std::fclose(raw_out); raw_out = nullptr; }
  std::string lines;
  { std::lock_guard<std::mutex> lock(held_mutex); lines.swap(held); }
  if (!lines.empty()) {
    say("picture: lines held back while the game ran%s:", held_overflow ? " (the first megabyte)" : "");
    size_t at = 0;
    while (at < lines.size()) {
      const size_t end = lines.find('\n', at);
      say("  %s", lines.substr(at, end - at).c_str());
      at = end == std::string::npos ? lines.size() : end + 1;
    }
  }
}
}  // namespace fe

// ---- hooks called by the host services (patches/0101) ----

// host::log (the decoder, the mixer, the app's input code): nothing goes to the SD card or the network
// while the game runs.
extern "C" int mu_frontend_log(const char* line) {
  if (!fe::running.load(std::memory_order_relaxed)) return 0;
  std::lock_guard<std::mutex> lock(fe::held_mutex);
  if (fe::held.size() > (1u << 20)) { fe::held_overflow = true; return 1; }
  fe::held += line; fe::held += '\n';
  return 1;
}

// Each block the mixer finishes, on the simulation thread. The app's audio_push takes what the
// console's audio interface would: big-endian right/left pairs in 5 ms blocks. It only copies into a
// ring; the device is fed from SDL's audio thread.
extern "C" void mu_frontend_audio(const int16_t* rl, uint32_t frames) {
  if (!fe::running.load(std::memory_order_relaxed)) return;
  if (fe::raw_out) std::fwrite(rl, 4, frames, fe::raw_out);
#if MU_FE_AURORA
  if (!fe::sound_open) return;
  for (uint32_t i = 0; i < frames * 2; ++i) {
    fe::block[fe::block_fill++] = uint8_t(uint16_t(rl[i]) >> 8);
    fe::block[fe::block_fill++] = uint8_t(rl[i]);
    if (fe::block_fill == sizeof fe::block) { host::audio_push(fe::block, sizeof fe::block); fe::block_fill = 0; }
  }
#endif
}

#if MU_FE_AURORA
// The game reads its four controllers (only reached when no runner script drives them).
extern "C" int mu_frontend_pad_read(MuPadStatus out[4]) {
  if (!fe::options.play || !fe::running.load(std::memory_order_relaxed)) return 0;
  host::PadState pads[4];
  host::input_poll(pads);
  static_assert(sizeof(host::PadState) == sizeof(MuPadStatus), "the two pad structures have the same fields in the same order");
  std::memcpy(out, pads, sizeof pads);
  fe::last_pad_ns.store(host::now_ns(), std::memory_order_relaxed);
  return 1;
}
extern "C" void mu_frontend_pad_rumble(int32_t port, int32_t on) {
  if (fe::options.play && fe::running.load(std::memory_order_relaxed) && port >= 0 && port < 4) host::gcadapter_rumble(port, on != 0);
}

// ---- what the app's sources expect from the rest of the app ----
namespace host {
uint32_t retrace_count() { return linux_host::counters().retraces; }
void audio_session_report() {}
void window_gamepad_rumble(int port, bool on) { gamepad_rumble(port, on); }
// The app's in-game menu sits here (input_poll calls it with every source merged). This program has no
// menu; it keeps the menu's way out: L + R + Start held, without A (Melee's own L + R + A + Start is left
// alone), here for a second and a half, ends the run.
void menu_frame(PadState pads[4]) {
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
namespace slippi::jukebox { void mix(int16_t*, size_t, int) {} }   // the Slippi music player is not part of this program
#endif
