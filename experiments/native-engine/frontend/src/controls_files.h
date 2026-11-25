// Controller profiles and Dolphin profile import (no SDL, no game: tools/controls_check.cpp tests them).
//
// Profiles: melee-unlocked's format (port/runtime/host/controller_profiles.cpp, GPL-2.0-or-later), one small text
// file per profile in <app folder>/ControllerProfiles/NAME.profile:
//   # Melee Unlocked controller profile
//   device gcadapter
//   A 256
//   ...
// "gcadapter" profiles use upstream's numbers (the adapter's pad bits), so one saved by melee-unlocked on a PC loads
// here and the other way round. SDL controllers (Pro Controller, Joy-Con, USB pads through the system) are a kind
// upstream does not have: "sdlgamepad", bit b = SDL gamepad button b, bits 28 and 29 the triggers. The analog
// settings follow as "opt.<key> <value>" lines, which upstream's reader skips as unknown names.
//
// Dolphin: a GameCube controller profile (User/Config/Profiles/GCPad/NAME.ini) or GCPadNew.ini ([GCPad1]..[GCPad4]),
// copied by the player to <app folder>/Dolphin/. Adapted from upstream's dolphin_profile.cpp (GPL-2.0-or-later):
// here SDL and XInput devices are read (the console has no keyboard), `A | B` expressions of plain controls are
// read as several sources, and stick axes become the stick swap and inversion.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
#include "controls_core.h"

namespace fe::controls {

// A whole controller's setup as a profile holds it. For Sdl the twelve buttons sit in mask[0..11] like the rest
// (controls.cpp moves them to and from the app's ControllerMap), together with the app map's own values.
struct Profile {
  Config config;
  bool has_app = false;                 // Sdl: the app map's values below were read
  int stick_deadzone = 24, cstick_deadzone = 27, trigger_press = 78;   // the app's ControllerMap (percent)
  bool swap_sticks = false, rumble = true;
};

const char* kind_key(Kind kind);        // "sdlgamepad", "gcadapter", "keyboard"
std::string profile_clean_name(const std::string& name);   // upstream's rule: letters, digits, space - _ . ; at most 40
std::vector<std::string> profile_list(const std::string& folder, Kind kind);
bool profile_save(const std::string& folder, const std::string& name, const Profile& p);
// Upstream's rule: actions before the stick directions that the file does not mention are unbound; the later ones
// keep what `p` holds. False (p untouched) when the file is missing or for another kind.
bool profile_load(const std::string& folder, const std::string& name, Profile& p);
bool profile_delete(const std::string& folder, const std::string& name, Kind kind);
std::string profile_free_name(const std::string& folder, const std::string& stem);   // "Profile 1", "Profile 2"...

struct DolphinEntry { std::string file, section, name; };   // a profile file, or one [GCPadN] of GCPadNew.ini
std::vector<DolphinEntry> dolphin_list(const std::string& folder);
struct DolphinResult {
  bool ok = false;
  Profile profile;                       // an Sdl profile (the app map's values included)
  int bound = 0, skipped = 0;            // actions read; bindings present and not understood
  std::string device, message;
};
// `text` is the file; `section` the [section] to read ("" = the whole file).
DolphinResult dolphin_read(const std::string& text, const std::string& section);
DolphinResult dolphin_read_entry(const DolphinEntry& e);

}  // namespace fe::controls
