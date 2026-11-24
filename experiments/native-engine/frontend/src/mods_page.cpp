// The in-game menu's Mods page: a thin view of the mod installer (mods_service.h). One mod at a time (Left/Right on the
// first row picks it), its state, size and licence, Install / Update / Cancel and Remove, where to get a file the catalog
// does not let us fetch, and the profile the next start loads. Everything it shows comes from modinstall::Manager, so
// another screen can replace this one without touching the installer.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "mods_service.h"
#include "app/game_menu.h"       // MenuExtraPage (the build-time copy with patches/0104)

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace fe {
namespace {
using modinstall::ModStatus;
using modinstall::State;
using modinstall::size_text;

enum class Act { None, Pick, Main, Remove, Profile };
struct Row { host::MenuExtraItem item; Act act = Act::None; };

std::mutex mu;
size_t selected = 0;
bool remove_armed = false;
// The installer's view, read at most every half second (the menu asks for the rows every frame).
std::vector<ModStatus> cached;
std::chrono::steady_clock::time_point cached_at;
std::vector<std::string> profile_choices;

void refresh(bool now) {
  const auto t = std::chrono::steady_clock::now();
  if (!now && !cached.empty() && t - cached_at < std::chrono::milliseconds(500)) return;
  cached = mod_manager().list();
  profile_choices = mod_profile_choices();
  cached_at = t;
}

std::string sizes(const ModStatus& s) {
  const auto& m = s.mod;
  if (!m.download_bytes) return "Size: not known";
  std::string text = "Size: " + size_text(m.download_bytes) + " download";
  if (m.disc_bytes) text += ", " + size_text(m.disc_bytes) + " disc";
  return text;
}

std::string state_text(const ModStatus& s) {
  if (s.state == State::Working || s.failed || (!s.line.empty() && s.percent >= 0)) return s.line;
  switch (s.state) {
    case State::Installed: return "Installed, version " + s.installed_version;
    case State::UpdateAvailable: return "Installed, version " + s.installed_version + "; version " + s.mod.version + " is out";
    default: return s.line.empty() ? "Not installed (version " + s.mod.version + ")" : s.line;
  }
}

std::vector<Row> rows() {
  std::vector<Row> out;
  auto& manager = mod_manager();
  if (!manager.catalog_ok()) { out.push_back({{"The mod catalog was refused: " + manager.catalog_error(), ""}, Act::None}); return out; }
  refresh(false);
  if (cached.empty()) return out;
  selected = std::min(selected, cached.size() - 1);
  const ModStatus& s = cached[selected];
  const bool online = session_online();
  out.push_back({{"Mod", s.mod.name + "  " + std::to_string(selected + 1) + "/" + std::to_string(cached.size())}, Act::Pick});
  out.push_back({{state_text(s), ""}, Act::None});
  out.push_back({{sizes(s), ""}, Act::None});
  out.push_back({{"Licence: " + s.mod.license + (s.automatic ? "; Install downloads it" : "; download it yourself"), ""}, Act::None});
  std::string main;
  if (s.state == State::Working) main = "Cancel";
  else if (s.state == State::UpdateAvailable) main = "Update to " + s.mod.version;
  else if (s.state == State::Installed) main = profile_choices.size() && mod_profile_next() == s.mod.id ? "Chosen for the next start" : "Play it at the next start";
  else if (s.automatic) main = "Install";
  else if (s.file_ready) main = "Install from mods/Downloads";
  else main = "Install (put the file in mods/Downloads first)";
  out.push_back({{main, "", !online || s.state == State::Working}, Act::Main});
  if (s.state == State::Installed || s.state == State::UpdateAvailable)
    out.push_back({{remove_armed ? "Remove: press A again" : "Remove", "", !online}, Act::Remove});
  if (!s.automatic && s.state == State::NotInstalled && !s.file_ready) {
    out.push_back({{"Get it: " + s.mod.page, ""}, Act::None});
    out.push_back({{"Put " + s.mod.asset_pattern + " in mods/Downloads/", ""}, Act::None});
  }
  const std::string next = mod_profile_next();
  host::MenuExtraItem profile{"Mod profile", next.empty() ? "None" : next};
  profile.next_start = true;
  out.push_back({profile, Act::Profile});
  return out;
}

bool act(int index, int dir) {
  std::lock_guard<std::mutex> lock(mu);
  const std::vector<Row> list = rows();
  if (index < 0 || index >= int(list.size()) || cached.empty()) return false;
  const Act a = list[size_t(index)].act;
  if (a != Act::Remove) remove_armed = false;
  auto& manager = mod_manager();
  const ModStatus s = cached[selected];
  switch (a) {
    case Act::Pick: {
      const int n = int(cached.size());
      selected = size_t(((int(selected) + (dir ? dir : 1)) % n + n) % n);
      return false;
    }
    case Act::Main: {
      std::string why;
      if (s.state == State::Working) { manager.cancel(); say("mods page: cancel %s", s.mod.id.c_str()); }
      else if (s.state == State::Installed) { if (mod_profile_next() != s.mod.id) mod_profile_set(s.mod.id); say("mods page: %s chosen for the next start", s.mod.id.c_str()); }
      else if (manager.install(s.mod.id, &why)) say("mods page: install %s", s.mod.id.c_str());
      else say("mods page: %s: %s", s.mod.id.c_str(), why.c_str());
      refresh(true);
      return s.state == State::Installed;
    }
    case Act::Remove: {
      if (!remove_armed) { remove_armed = true; return false; }
      remove_armed = false;
      std::string why;
      if (manager.remove(s.mod.id, &why)) {
        say("mods page: removed %s", s.mod.id.c_str());
        if (mod_profile_next() == s.mod.id) mod_profile_set("");
      } else {
        say("mods page: %s: %s", s.mod.id.c_str(), why.c_str());
      }
      refresh(true);
      return true;
    }
    case Act::Profile: {
      const auto& choices = profile_choices;
      if (choices.empty()) return false;
      const auto it = std::find(choices.begin(), choices.end(), mod_profile_next());
      const int n = int(choices.size()), at = it == choices.end() ? 0 : int(it - choices.begin());
      const std::string next = choices[size_t(((at + (dir ? dir : 1)) % n + n) % n)];
      mod_profile_set(next);
      say("mods page: profile at the next start: %s", next.empty() ? "none" : next.c_str());
      return true;
    }
    case Act::None: return false;
  }
  return false;
}
}  // namespace

// Appends the Mods page (std::vector<host::MenuExtraPage>), last, so the other pages keep their places.
void mods_menu_pages(void* extra_pages) {
  auto* extra = static_cast<std::vector<host::MenuExtraPage>*>(extra_pages);
  host::MenuExtraPage page;
  page.title = "Mods";
  page.items = [] {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<host::MenuExtraItem> items;
    for (auto& r : rows()) items.push_back(r.item);
    return items;
  };
  page.change = [](int item, int dir) { return act(item, dir); };
  extra->push_back(std::move(page));
}
}  // namespace fe
