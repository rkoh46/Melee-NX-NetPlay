// melee-unlocked's Lab view (port/runtime/gx/lab_view.cpp and .h, compiled from the fetched tree, GPL-2.0-or-later; its drawing
// follows Slippi Lab, MIT, github.com/frankborden/slippilab): the match as flat Slippi-Lab-style figures over a plain stage,
// drawn with Dear ImGui over the game picture. build.sh sync copies it into gen/lab with its host services renamed to the
// stand-ins in src/lab_shim; earcut (mapbox, ISC) comes along.
//   - simulation thread (features/gfx.cpp's retrace): while the view or one of its looks is on, the game image's read-only
//     exports mu_look_snapshot_v1, mu_look_items_v1 and mu_look_extras_v1 (upstream's native_retrace calls the same) fill the
//     snapshot lab::native_frame takes. Nothing is written into the game; with the view off nothing is called;
//   - render thread (overlay_draw.cpp): lab::draw, which covers the picture while a match runs; its fighter packs (Slippi Lab's
//     silhouettes converted by upstream's tools/build_lab_assets.py) are <app folder>/Lab/<id>.lab, fetched by the services
//     program from melee-unlocked's content release (lab_download, SHA-256 checked) when the player asks.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lab_view.cpp"

#include "gfx_live.h"
#include "mu_look_snapshot.h"

#include "modinstall/mi_archive.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// The services program's download (src/proai/proai_fetch_curl.cpp, SHA-256 checked); absent in the other programs.
__attribute__((weak)) bool fe_proai_fetch_verified(const std::string& url, const std::string& sha256, const std::string& dest,
                                                   std::atomic<uint64_t>* progress, std::string* why);

namespace host {
LabOptions lab_options;
uint8_t* lab_ram = nullptr;
float lab_window_aspect() {
  int w = 1280, h = 720;
  fe::window_size(&w, &h);
  return h > 0 ? float(w) / float(h) : 16.0f / 9.0f;
}
}  // namespace host

namespace linux_host { void* fe_game_symbol(const char* name); }   // patches/0121

namespace fe::gfx {
// gfx_render.cpp refers to this, so this object is linked whenever the renderer side is (the feature modules reach the Lab
// view only through weak references, which do not take an object out of a static library).
extern const int lab_linked = 1;
namespace {
void (*look_snapshot)(MuLookSnapshot*) = nullptr;
void (*look_items)(MuLookItems*) = nullptr;
void (*look_extras)(MuLookExtras*) = nullptr;
bool looked_up = false;
}  // namespace

// Simulation thread, once per retrace (features/gfx.cpp).
void lab_retrace(const std::string names[4]) {
  static bool configured = false;
  if (!configured) {
    configured = true;
    host::lab_options.lab_dir = folder() + "/Lab";
  }
  auto& l = live();
  const bool on = l.lab.load(std::memory_order_relaxed);
  lab::set_enabled(on);
  if (!on) { lab::set_style(0, 0); return; }
  lab::set_style(l.lab_fighters.load(std::memory_order_relaxed), l.lab_stage.load(std::memory_order_relaxed));
  // The silhouette look found no pack: fetched once a run, as upstream does (the stick figure and capsule looks need none).
  static std::atomic<bool> missing{false};
  static const bool wired = (lab::set_missing_packs_handler([](const std::string&) { missing.store(true); }), true);
  (void)wired;
  static bool tried = false;
  if (missing.load() && !tried && l.lab_fighters.load() == 0 && !std::getenv("MELEE_LAB_NO_DOWNLOAD")) { tried = true; lab_download(); }
  if (!looked_up) {
    looked_up = true;
    look_snapshot = reinterpret_cast<void (*)(MuLookSnapshot*)>(linux_host::fe_game_symbol("mu_look_snapshot_v1"));
    look_items = reinterpret_cast<void (*)(MuLookItems*)>(linux_host::fe_game_symbol("mu_look_items_v1"));
    look_extras = reinterpret_cast<void (*)(MuLookExtras*)>(linux_host::fe_game_symbol("mu_look_extras_v1"));
    say("gfx: Lab view: the game image's look snapshot %s, items %s, extras %s", look_snapshot ? "found" : "MISSING",
        look_items ? "found" : "missing", look_extras ? "found" : "missing");
  }
  if (!look_snapshot || !lab::active()) return;
  static MuLookSnapshot snapshot;
  static MuLookItems items;
  static MuLookExtras extras;
  snapshot = MuLookSnapshot{};
  look_snapshot(&snapshot);
  if (look_items) { items = MuLookItems{}; look_items(&items); }
  if (look_extras) { extras = MuLookExtras{}; look_extras(&extras); }
  lab::native_frame(snapshot, look_items ? &items : nullptr, look_extras ? &extras : nullptr, names);
}

// Render thread, inside the ImGui frame (overlay_draw.cpp), before the other overlays.
void lab_draw(int window_w, int window_h) {
  auto& l = live();
  uint32_t players[4];
  for (int i = 0; i < 4; ++i) players[i] = l.lab_colour[i].load(std::memory_order_relaxed);
  lab::set_colors(players, l.lab_background.load(std::memory_order_relaxed), l.lab_stage_colour.load(std::memory_order_relaxed));
  lab::draw(l.lab.load(std::memory_order_relaxed), float(window_w), float(window_h));
  l.lab_covering.store(lab::covering(), std::memory_order_relaxed);
}

// ---- the fighter packs: melee-unlocked's content release (its host/updater.cpp lab_packs_download: the zip, its SHA-256,
// 27 packs named <internal id>.lab) through the services program's libcurl (fe_proai_fetch_verified, weak: the program
// without services has no network and says so). Started when the silhouette look finds no pack, or from the menu.
namespace {
constexpr int kLabPacks = 27;
constexpr const char* kLabUrl = "https://github.com/hero88go/melee-unlocked/releases/download/content-1/MeleeUnlocked-Lab.zip";
constexpr const char* kLabSha256 = "6fcb60586ee0b73a42b3b7521546b554d3b105299bc97d74720b849075febb75";
std::thread lab_fetch;
std::atomic<int> lab_fetch_state{0};   // 0 idle, 1 running, 2 done, 3 failed
std::atomic<uint64_t> lab_fetch_bytes{0};
std::mutex lab_fetch_mutex;
std::string lab_fetch_message;
void lab_message(const std::string& m) {
  { std::lock_guard<std::mutex> lock(lab_fetch_mutex); lab_fetch_message = m; }
  lab::set_notice(m);
}
bool packs_present(const std::string& dir) {
  for (int i = 0; i < kLabPacks; ++i) {
    struct stat st{};
    if (stat((dir + "/" + std::to_string(i) + ".lab").c_str(), &st) != 0) return false;
  }
  return true;
}
void fetch_packs(std::string dir) {
  mkdir(dir.c_str(), 0777);
  const std::string zip = dir + "/MeleeUnlocked-Lab.zip";
  const char* mirror = std::getenv("MU_LAB_URL");   // tests: a local stand-in for the release (with MELEE_PRO_AI_TEST_MIRROR)
  std::string why;
  say("gfx: Lab view: downloading the fighters (about 85 MB) from %s", mirror ? mirror : "melee-unlocked's content release");
  lab_message("Downloading the Lab view's fighters (about 85 MB)...");
  if (!fe_proai_fetch_verified(mirror ? mirror : kLabUrl, kLabSha256, zip, &lab_fetch_bytes, &why)) {
    lab_message("Lab view fighters: download failed (" + why + ")");
    say("gfx: Lab view: download failed: %s", why.c_str());
    lab_fetch_state.store(3);
    return;
  }
  lab_message("Unpacking the Lab view's fighters...");
  std::string error;
  auto archive = modinstall::open_archive(zip, &error);
  int placed = 0;
  if (archive) {
    const auto& entries = archive->entries();
    for (size_t i = 0; i < entries.size(); ++i) {
      const std::string& name = entries[i].name;
      const size_t slash = name.find_last_of('/');
      const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
      if (entries[i].directory || base.size() < 5 || base.substr(base.size() - 4) != ".lab") continue;
      auto in = archive->open(i, &error);
      if (!in) break;
      const std::string out_path = dir + "/" + base, part = out_path + ".part";
      FILE* out = std::fopen(part.c_str(), "wb");
      if (!out) { error = "cannot write " + part; break; }
      std::vector<uint8_t> buffer(1 << 16);
      long n = 0; bool ok = true;
      while ((n = in->read(buffer.data(), buffer.size())) > 0) ok = ok && std::fwrite(buffer.data(), 1, size_t(n), out) == size_t(n);
      ok = std::fclose(out) == 0 && ok && n == 0;
      if (!ok) { std::remove(part.c_str()); error = in->error().empty() ? "cannot write " + out_path : in->error(); break; }
      std::remove(out_path.c_str());
      std::rename(part.c_str(), out_path.c_str());
      ++placed;
    }
  }
  std::remove(zip.c_str());
  if (!packs_present(dir)) {
    lab_message("Lab view fighters: the download is incomplete (" + (error.empty() ? std::to_string(placed) + " packs" : error) + ")");
    say("gfx: Lab view: unpacking failed: %s (%d packs placed)", error.c_str(), placed);
    lab_fetch_state.store(3);
    return;
  }
  say("gfx: Lab view: %d fighter packs installed in %s", placed, dir.c_str());
  lab_message("");
  lab::packs_changed();
  lab_fetch_state.store(2);
}
}  // namespace

void lab_download() {
  if (lab_fetch_state.load() == 1) return;
  if (lab_fetch.joinable()) lab_fetch.join();
  if (!&fe_proai_fetch_verified) { lab_message("The Lab view's fighters download in the app (this program has no network)"); lab_fetch_state.store(3); return; }
  lab_fetch_state.store(1);
  lab_fetch = std::thread(fetch_packs, folder() + "/Lab");
}
std::string lab_status() {
  const std::string dir = folder() + "/Lab";
  const int state = lab_fetch_state.load();
  if (state == 1) return "Downloading: " + std::to_string(lab_fetch_bytes.load() >> 20) + " MB";
  if (state == 3) { std::lock_guard<std::mutex> lock(lab_fetch_mutex); return lab_fetch_message; }
  return packs_present(dir) ? std::string("Installed") : std::string("Not installed (about 85 MB)");
}
void lab_end() { if (lab_fetch.joinable()) lab_fetch.join(); }
}  // namespace fe::gfx
