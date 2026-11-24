// The Mods screens of melee-unlocked's interface on the Switch (the home screen's Mods page, mu_home.cpp, and the settings
// panel's MODS category, mu_panel.cpp) over the app's mod installer (mods_service.h, modinstall::Manager): one view of the
// catalog and one set of actions, so both screens say and do the same. Replaces the old menu's mods_page.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mods_service.h"

#include <string>
#include <vector>

namespace fe::mu_mods {
struct Card {
  modinstall::ModStatus status;
  std::string state;          // "Installed 4.07" / "Not installed" / what the running job does
  bool state_ok = false;      // installed and current (melee-unlocked's green line)
  std::string detail;         // size and licence ("246 MB download, 1.4 GB disc  |  GPL-3.0  |  Install downloads it")
  std::string primary;        // Install / Update to X / Cancel / Play it at the next start / Chosen for the next start / Install from mods/Downloads
  bool primary_enabled = true;
  bool can_remove = false;
  bool remove_armed = false;
  std::string get_it;         // a mod the catalog does not let us fetch: where to download it and where to put it
};
// The catalog with each mod's state (read at most every half second; any thread).
std::vector<Card> cards(std::string* error = nullptr);
// Actions: true when a saved setting changed (the profile). Any thread; the installer runs its job on its own thread.
bool primary(const std::string& id);
bool remove(const std::string& id);          // the first press arms it, the second removes
std::string profile_next();                  // "" none
std::vector<std::string> profile_choices();  // "" first
bool profile_set(const std::string& profile);
bool downloads_available();
}  // namespace fe::mu_mods
