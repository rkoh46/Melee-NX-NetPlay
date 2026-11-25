// The Switch's software keyboard (the system applet, libnx swkbd) for the screens that need words typed: the settings
// panel's search, the Replay library's search. melee-unlocked types into an ImGui text box on a PC; a controller has no
// keys, so the console's own keyboard takes the screen until the player presses OK or cancels.
// Off the console (Linux tests) there is no applet: MU_TEST_KEYBOARD="words|more words|-" answers the calls in order, one
// entry each ("-" cancels); without it every call is cancelled.
// A USB keyboard in use (usb_keyboard.cpp, also MU_TEST_KEYS off the console) takes the place of both: a box over the game.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include <cstdlib>
#include <mutex>
#include <string>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe {
// usb_keyboard.cpp (builds with a picture): with a USB keyboard in use, the words are typed on it in a box over the game.
__attribute__((weak)) bool kb_prompt(const char* header, const char* guide, std::string& text, bool* ok);

bool software_keyboard(const char* header, const char* guide, std::string& text) {
  bool typed = false;
  if (&kb_prompt && kb_prompt(header, guide, text, &typed)) return typed;
#ifdef __SWITCH__
  static std::mutex one_at_a_time;   // the applet is one at a time
  std::lock_guard<std::mutex> lock(one_at_a_time);
  SwkbdConfig config;
  if (R_FAILED(swkbdCreate(&config, 0))) return false;
  swkbdConfigMakePresetDefault(&config);
  swkbdConfigSetHeaderText(&config, header);
  swkbdConfigSetGuideText(&config, guide);
  swkbdConfigSetStringLenMax(&config, 90);
  swkbdConfigSetInitialText(&config, text.c_str());
  char out[256] = {};
  const Result rc = swkbdShow(&config, out, sizeof out);
  swkbdClose(&config);
  if (R_FAILED(rc)) { say("keyboard: %s: cancelled", header); return false; }
  text = out;
  say("keyboard: %s: %zu characters", header, text.size());
  return true;
#else
  static std::mutex mutex;
  static size_t next = 0;
  std::lock_guard<std::mutex> lock(mutex);
  const char* script = std::getenv("MU_TEST_KEYBOARD");
  (void)guide;
  if (!script) { say("keyboard: %s: no software keyboard off the console", header); return false; }
  const std::string all = script;
  size_t at = 0;
  for (size_t i = 0; i < next && at != std::string::npos; ++i) { at = all.find('|', at); if (at != std::string::npos) ++at; }
  ++next;
  if (at == std::string::npos || at > all.size()) { say("keyboard: %s: no more test answers", header); return false; }
  const size_t end = all.find('|', at);
  const std::string answer = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
  if (answer == "-") { say("keyboard: %s: cancelled (test)", header); return false; }
  text = answer;
  say("keyboard: %s: \"%s\" (test)", header, text.c_str());
  return true;
#endif
}
}  // namespace fe
