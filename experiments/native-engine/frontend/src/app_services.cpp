// The app's screens before the game, services program only: melee-unlocked's launcher as the home screen (mu_home.cpp),
// with the update from the player's build computer (launcher.ini update_host=http://address:port;
// port/runtime/host/self_update.*, the same latest.json and verify-then-rename install as the earlier app) and the Slippi
// sign-in (port/runtime/host/switch_signin.cpp, unchanged but for patches/0105) on its Play page. Drawn on the render
// thread, which owns Aurora before the game starts (frontend.cpp screens_run).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host.h"
#include "metal_frontend.h"
#include "self_update.h"
#include "switch_signin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fe {
namespace {
namespace su = host::self_update;
}  // namespace

bool mu_home_message(const std::string& headline, const std::vector<std::string>& lines);   // mu_home.cpp
AppStart mu_home_run(const std::string& user_dir, bool update_check);                        // mu_home.cpp

bool app_notice_run(const std::string& headline, const std::vector<std::string>& lines) {
  return mu_home_message(headline, lines);
}

// The screens before the game are melee-unlocked's launcher (mu_home.cpp): its Play page carries the update from the
// build computer and the Slippi sign-in. launcher.ini home=0 (test aid) skips it: the game starts at once.
AppStart app_services_run(bool check_update, bool need_signin, const std::string& user_dir) {
  const std::string nro = options.self_path;
  // An update downloaded and verified in an earlier session whose swap could not finish (the earlier app swaps in
  // userAppInit; this program has no RomFS holding its file open, so normally the swap is done right after the download).
  {
    std::string error;
    const su::Swap swap = su::apply_pending(nro, error);
    if (swap != su::Swap::Nothing) say("update: at start, a waiting update %s%s%s", swap == su::Swap::Done ? "was installed" : "could not be installed", error.empty() ? "" : ": ", error.c_str());
    if (swap == su::Swap::Done) {
      mu_home_message("Update installed", {"The update downloaded last time is now in place.", "The app closes now. Start it again."});
      return AppStart::Quit;
    }
  }
  if (app_ini("home", "1") == "0") {
    say("app: launcher.ini home=0: the home screen is skipped");
    return need_signin ? AppStart::PlayOffline : AppStart::Play;
  }
  return mu_home_run(user_dir, check_update);
}
}  // namespace fe
