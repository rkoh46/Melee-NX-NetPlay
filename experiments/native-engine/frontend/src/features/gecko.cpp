// melee-unlocked's player Gecko codes on the Source Port (its GECKO CODES page, "Your codes"): upstream's user_gecko.cpp,
// compiled from the fetched tree (frontend/patches/0122), with the game image's own writer and reader (shim/mu_gecko.c:
// mu_user_gecko_write and mu_user_gecko_read, found by name, frontend/patches/0121).
//   - The codes are read from <app folder>/GeckoCodes.ini (Dolphin's format: [Gecko], "$Name", "*note", "XXXXXXXX YYYYYYYY"); a
//     code is switched on the panel's GECKO CODES page; the choice is kept in settings.ini as upstream keeps it (`geckocode NAME`
//     lines and `geckochosen 1`; until then the file's [Gecko_Enabled] section decides).
//   - What runs is upstream's rule for the Source Port: every byte a code writes or reads must land in one game variable the
//     native game keeps with the console's layout (user_gecko_targets.h): types 00-06, the serial write 08, the pointer forms
//     10-18, 42/4A/44/4C, the ifs 20-2E with E2 and E0, the Gecko registers 80-88. Refused, with the reason on the row, as
//     upstream refuses them: PowerPC injection (C0, C2, C6), pointer loads from game memory (40, 48), loops and jumps (60-68),
//     register compares (A0-AE), writes outside those variables, Slippi's own required codes and its lag reduction. Upstream
//     also runs a code that rewrites a game function into "return N at once" by overwriting the native function's entry; the
//     console does not let a program rewrite its own code, so such a code is refused here with that reason.
//   - A code made only of patches the game already carries (the General Codes, widescreen, screen shake...) is shown as built in.
//   - Offline only, as upstream: nothing runs online (the game's writer itself refuses during a network match), in replay
//     playback or in a vanilla-only session; every code is off until the player switches it on. The codes change the
//     simulation, so they are never part of an online session (upstream's "Agreed Gecko codes online" is its own stub).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "user_gecko.h"          // melee-unlocked port/runtime/host (the fetched tree)
#include "gameplay_policy.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace linux_host { void* fe_game_symbol(const char* name); }   // patches/0121

// user_gecko.cpp's Static Recomp path (console memory writes, functions run from RAM): never reached here, because code
// patches are not allowed (user_gecko::set_code_patches_allowed stays false); defined as refusals so the copy links.
namespace host {
void wr8(uint32_t, uint8_t) {}
void wr16(uint32_t, uint16_t) {}
void wr32(uint32_t, uint32_t) {}
}
namespace ppc { bool redirect_function_at(uint32_t) { return false; } }

namespace fe::features {
namespace {
std::vector<std::string> g_enabled_names;    // settings.ini geckocode lines
bool g_chosen = false;                       // settings.ini geckochosen 1
bool g_loaded = false;
std::atomic<bool> g_running{false};          // the codes ran in the last frame (for the log)
std::mutex g_rows_mutex;
std::vector<GeckoRow> g_rows;                // what the panel shows (render thread reads a copy)

std::string file_path() { return options.out_dir + "/GeckoCodes.ini"; }

void publish_rows() {
  std::vector<GeckoRow> rows;
  for (const user_gecko::Code& c : user_gecko::codes()) {
    GeckoRow r;
    r.name = c.name;
    r.enabled = c.enabled;
    const char* builtin = user_gecko::native_equivalent(c);
    r.supported = c.supported && !builtin;
    r.reason = builtin ? std::string("Built in: ") + builtin + " is the switch that applies here" : c.supported ? std::string() : "Cannot run on the Source Port: this code " + c.reason;
    rows.push_back(std::move(r));
  }
  std::lock_guard<std::mutex> lock(g_rows_mutex);
  g_rows.swap(rows);
}

// settings.ini keeps the switched-on codes as upstream does: one `geckocode NAME` line each, and `geckochosen 1`.
void persist() {
  const std::string path = settings_path();
  if (path.empty()) return;
  std::vector<std::string> lines;
  { std::ifstream in(path); std::string l; while (std::getline(in, l)) if (l.rfind("geckocode ", 0) != 0 && l.rfind("geckochosen ", 0) != 0) lines.push_back(l); }
  for (const user_gecko::Code& c : user_gecko::codes()) if (c.enabled || c.wanted) lines.push_back("geckocode " + c.name);
  lines.push_back("geckochosen 1");
  const std::string tmp = path + ".tmp";
  if (FILE* f = std::fopen(tmp.c_str(), "w")) {
    for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
  }
}

bool setting(const std::string& key, const std::string& value) {
  if (key == "geckocode") { if (!value.empty()) g_enabled_names.push_back(value); return true; }
  if (key == "geckochosen") { g_chosen = value == "1"; return true; }
  return false;
}

void load_codes() {
  if (g_loaded) return;
  g_loaded = true;
  host::gameplay::vanilla_only_session = settings_vanilla_only();
  user_gecko::set_code_patches_allowed(false);   // the Source Port: game variables only
  user_gecko::set_sources_allowed(!settings_vanilla_only());
  user_gecko::set_native_writer(reinterpret_cast<user_gecko::NativeWrite>(linux_host::fe_game_symbol("mu_user_gecko_write")));
  user_gecko::set_native_reader(reinterpret_cast<user_gecko::NativeRead>(linux_host::fe_game_symbol("mu_user_gecko_read")));
  user_gecko::load(file_path(), g_enabled_names, g_chosen);
  int supported = 0, on = 0;
  for (const auto& c : user_gecko::codes()) { supported += c.supported; on += c.enabled; }
  if (!user_gecko::codes().empty())
    say("gecko: %zu codes in %s, %d can run on the Source Port, %d switched on%s", user_gecko::codes().size(), file_path().c_str(), supported, on,
        settings_vanilla_only() ? " (a vanilla-only session: none runs)" : "");
  publish_rows();
}

void tables(MuHostApi*, MuGameApi*) { if (options.play) load_codes(); }

// Once per frame, as upstream applies them each frame (it does so at the pad read; here right after the frame, the same
// point of every frame). Offline only.
void on_retrace(const Retrace& info) {
  if (!g_loaded || !user_gecko::any_enabled()) return;
  const bool run = !info.online && !replay_playing() && !vanilla();
  if (run) user_gecko::apply(); else user_gecko::suspend_function_patches();
  if (run != g_running.exchange(run)) say("gecko: player codes %s", run ? "run (offline)" : "suspended (online, a replay or a vanilla session)");
}
}  // namespace

// The panel (mu_panel.cpp): the rows, and a switch from its queue (simulation thread).
std::vector<GeckoRow> gecko_rows() { std::lock_guard<std::mutex> lock(g_rows_mutex); return g_rows; }
std::string gecko_file() { return file_path(); }
void gecko_toggle(const std::string& name) {
  load_codes();
  for (user_gecko::Code& c : user_gecko::codes()) {
    if (c.name != name) continue;
    if (!c.supported || user_gecko::native_equivalent(c)) return;
    c.enabled = !c.enabled; c.wanted = c.enabled;
    say("gecko: \"%s\" %s", c.name.c_str(), c.enabled ? "on (offline)" : "off");
    break;
  }
  persist();
  publish_rows();
}

void register_gecko() {
  add_setting(setting);
  add_tables(tables);
  add_retrace(on_retrace);
}
}  // namespace fe::features
