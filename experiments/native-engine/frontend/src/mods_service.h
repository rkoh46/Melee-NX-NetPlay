// The app's mod installer (src/modinstall/mod_install.h) set up with the app's folders, melee-unlocked's catalog
// (built in from the fetched tree's mods-catalog-2.json) and the services program's libcurl. Any screen can use it:
// the minimal Mods page (mods_page.cpp) and a launcher drawn in melee-unlocked's look alike. See frontend/README.md,
// "Mod catalog, download and install".
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "modinstall/mod_install.h"

#include <string>
#include <vector>

namespace fe {
modinstall::Manager& mod_manager();                     // created on first use (after the app's folders are known)
bool mod_downloads_available();                         // this program can fetch (the services program, with libcurl)
std::string mod_profile_next();                         // settings.ini mod_profile: the profile the next start loads ("" none)
bool mod_profile_set(const std::string& profile);       // writes it (takes effect at the next start)
std::vector<std::string> mod_profile_choices();         // "" (none) first, then the profiles mods.cpp can load
}
