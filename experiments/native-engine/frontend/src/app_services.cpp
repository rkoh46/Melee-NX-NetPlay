// The app's screens before the game, services program only: the update from the player's build computer
// (launcher.ini update_host=http://address:port; port/runtime/host/self_update.*, the same latest.json and the
// same verify-then-rename install as the earlier app) and the Slippi sign-in (port/runtime/host/switch_signin.cpp,
// unchanged but for patches/0105). Both are drawn on the render thread, which owns Aurora before the game starts
// (frontend.cpp screens_run). Nothing here runs when the app is started offline (launcher.ini offline=1).
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
// The Slippi release this program's netplay implements (the netplay object is built with SLIPPI_PIN_VERSION 3.7.0,
// ../build.sh NET_DEF): the manifest's slippi_version is compared with it.
constexpr const char* kSlippiVersion = MU_SLIPPI_VERSION;

struct Notice { std::string headline; std::vector<std::string> lines; const char* hint = nullptr; bool allow_b = false; host::NoticeChoice choice = host::NoticeChoice::Quit; };
void notice_job(void* frontend, void* user) {
  auto* n = static_cast<Notice*>(user);
  auto& f = *static_cast<host::MetalFrontend*>(frontend);
  n->choice = n->hint ? host::switch_prompt_run(f, n->headline, n->lines, n->hint, n->allow_b)
                      : (host::switch_notice_run(f, n->headline, n->lines) ? host::NoticeChoice::A : host::NoticeChoice::Quit);
}
host::NoticeChoice show(const std::string& headline, const std::vector<std::string>& lines, const char* hint = nullptr, bool allow_b = false) {
  Notice n{headline, lines, hint, allow_b};
  if (!screens_run(notice_job, &n)) return host::NoticeChoice::A;
  return n.choice;
}

struct Download {
  su::Manifest manifest; std::string update_host, nro, error;
  bool ok = false, cancelled = false;
};
// Downloads on a worker thread while this (render) thread draws the progress, as the app's run_self_update.
void download_job(void* frontend, void* user) {
  auto* d = static_cast<Download*>(user);
  auto& f = *static_cast<host::MetalFrontend*>(frontend);
  const su::FileEntry entry = d->manifest.files.at("game");
  std::atomic<uint64_t> done{0};
  std::atomic<bool> finished{false}, cancel{false};
  std::thread worker([&] {
    const su::Fetch fetch = su::curl_fetch(3000, 20);
    d->ok = su::download_verified(fetch, su::join_url(d->update_host, entry.name), entry, d->nro,
                                  [&](uint64_t got, uint64_t) { done = got; return !cancel.load(); }, d->error);
    finished = true;
  });
  while (!finished) {
    char line[96];
    std::snprintf(line, sizeof line, "Downloading the game: %llu of %llu MB", (unsigned long long)(done.load() >> 20), (unsigned long long)(entry.size >> 20));
    if (!host::switch_progress_frame(f, "UPDATING", {line, "", "Keep the console on. The installed version stays in place until the download has been checked."},
                                     float(double(done.load()) / double(std::max<uint64_t>(entry.size, 1)))))
      cancel = true;
  }
  worker.join();
  d->cancelled = cancel.load();
}

struct SignIn { std::string user_dir, name, code; host::SignInResult result = host::SignInResult::Quit; };
void signin_job(void* frontend, void* user) {
  auto* s = static_cast<SignIn*>(user);
  s->result = host::switch_signin_run(*static_cast<host::MetalFrontend*>(frontend), s->user_dir, s->name, s->code);
}
}  // namespace

bool app_notice_run(const std::string& headline, const std::vector<std::string>& lines) {
  return show(headline, lines) != host::NoticeChoice::Quit;
}

AppStart app_services_run(bool check_update, bool need_signin, const std::string& user_dir) {
  const std::string nro = options.self_path;
  // An update downloaded and verified in an earlier session whose swap could not finish (the earlier app swaps in
  // userAppInit; this program has no RomFS holding its file open, so normally the swap is done right after the download).
  {
    std::string error;
    const su::Swap swap = su::apply_pending(nro, error);
    if (swap != su::Swap::Nothing) say("update: at start, a waiting update %s%s%s", swap == su::Swap::Done ? "was installed" : "could not be installed", error.empty() ? "" : ": ", error.c_str());
    if (swap == su::Swap::Done) {
      show("UPDATE INSTALLED", {"The update downloaded last time is now in place.", "", "The app closes now. Start it again."});
      return AppStart::Quit;
    }
  }
  const std::string update_host = app_ini("update_host", "");
  if (check_update && !update_host.empty()) {
    const std::string url = su::join_url(update_host, "latest.json");
    if (url.empty()) say("update: update_host is not an http:// address; ignored");
    else {
      // One short request, at most two seconds of waiting (the app's start_update_host_check).
      struct Check { std::mutex lock; bool ok = false; su::Manifest manifest; std::string error; std::atomic<bool> done{false}; };
      auto check = std::make_shared<Check>();
      std::thread([url, check] {
        std::string text, error;
        const bool fetched = su::curl_fetch(1500, 3)(url, [&](const void* data, size_t size) {
          if (text.size() + size > 64 * 1024) return false;
          text.append(static_cast<const char*>(data), size);
          return true;
        }, error);
        su::Manifest manifest;
        const bool ok = fetched && su::parse_manifest(text, manifest, error);
        { std::lock_guard<std::mutex> guard(check->lock); check->ok = ok; check->manifest = manifest; check->error = error; }
        check->done = true;
      }).detach();
      for (int i = 0; i < 20 && !check->done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      su::Manifest manifest; bool have = false; std::string error = "no answer within 2 s";
      if (check->done) { std::lock_guard<std::mutex> guard(check->lock); have = check->ok; manifest = check->manifest; error = check->error; }
      if (!have) say("update: no answer from the build computer (%s)", error.c_str());
      else {
        say("update: the build computer has version %s (Slippi %s); this is %s (Slippi %s)", manifest.app_version.c_str(), manifest.slippi_version.c_str(), MU_APP_VERSION, kSlippiVersion);
        if (su::should_offer(manifest, MU_APP_VERSION, kSlippiVersion) && manifest.files.count("game")) {
          const host::NoticeChoice choice = show("UPDATE AVAILABLE", {
              "Your build computer has version " + manifest.app_version + " (Slippi " + manifest.slippi_version + ").",
              std::string("This console has version ") + MU_APP_VERSION + " (Slippi " + kSlippiVersion + ").", "",
              "Install it now? It is downloaded over your home network and checked before anything is replaced."},
              "A  Install     B  Later     +  Quit", true);
          say("update: offered; the player chose %s", choice == host::NoticeChoice::A ? "to install" : choice == host::NoticeChoice::B ? "later" : "to quit");
          if (choice == host::NoticeChoice::Quit) return AppStart::Quit;
          if (choice == host::NoticeChoice::A) {
            Download d; d.manifest = manifest; d.update_host = update_host; d.nro = nro;
            screens_run(download_job, &d);
            say("update: the game %s%s%s", d.ok ? "downloaded and verified" : "not installed", d.ok ? "" : ": ", d.ok ? "" : d.error.c_str());
            if (!d.ok) {
              std::remove(su::marker_path(nro).c_str()); std::remove(su::staged_path(nro).c_str());
              show("UPDATE NOT INSTALLED", {"The download did not finish: " + (d.cancelled ? std::string("cancelled") : d.error) + ".", "",
                                            "Nothing was changed. The installed version starts now; the update is offered again next time."});
            } else {
              std::string swap_error;
              const su::Swap own = su::apply_pending(nro, swap_error);
              say("update: %s", own == su::Swap::Done ? "the new version is in place" : ("the new version is swapped in at the next start (" + swap_error + ")").c_str());
              show("UPDATE INSTALLED", {
                  "Version " + manifest.app_version + " (Slippi " + manifest.slippi_version + ") has been downloaded and checked.", "",
                  own == su::Swap::Done ? "The app closes now. Start it again." : "The app closes now. Start it again: it finishes the update and asks you to start it once more."});
              return AppStart::Quit;
            }
          }
        }
      }
    }
  }
  if (need_signin) {
    SignIn s; s.user_dir = user_dir;
    if (!screens_run(signin_job, &s)) { say("app: no picture for the sign-in screen: the Online Play page will show Log-in"); return AppStart::PlayOffline; }
    switch (s.result) {
      case host::SignInResult::SignedIn: say("app: sign-in: signed in as %s (%s)", s.name.c_str(), s.code.c_str()); return AppStart::Play;
      case host::SignInResult::PlayOffline: say("app: sign-in: playing offline"); return AppStart::PlayOffline;
      case host::SignInResult::Quit: say("app: sign-in: quit"); return AppStart::Quit;
    }
  }
  return AppStart::Play;
}
}  // namespace fe
