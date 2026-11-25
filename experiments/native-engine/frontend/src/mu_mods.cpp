// See mu_mods.h. The wording follows melee-unlocked's launcher Mods page (port/app/launcher_mods.inl) where the Switch does
// the same thing, and the old menu's Mods page where it does not (the profile at the next start).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mu_mods.h"
#include "frontend.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace fe::mu_mods {
namespace {
using modinstall::State;
std::mutex mu;
std::vector<modinstall::ModStatus> cached;
std::chrono::steady_clock::time_point cached_at;
std::string armed;   // the mod whose Remove was pressed once

void refresh_locked(bool now) {
  const auto t = std::chrono::steady_clock::now();
  if (!now && !cached.empty() && t - cached_at < std::chrono::milliseconds(500)) return;
  cached = mod_manager().list();
  cached_at = t;
}
const modinstall::ModStatus* find_locked(const std::string& id) {
  for (const auto& s : cached) if (s.mod.id == id) return &s;
  return nullptr;
}
}  // namespace

std::vector<Card> cards(std::string* error) {
  std::lock_guard<std::mutex> lock(mu);
  auto& manager = mod_manager();
  if (!manager.catalog_ok()) { if (error) *error = "The mod catalog was refused: " + manager.catalog_error(); return {}; }
  refresh_locked(false);
  const std::string next = mod_profile_next();
  const bool online = session_online();
  std::vector<Card> out;
  for (const auto& s : cached) {
    Card c; c.status = s;
    const auto& m = s.mod;
    switch (s.state) {
      case State::Installed: c.state = "\xE2\x9C\x93 Installed " + s.installed_version; c.state_ok = true; break;
      case State::UpdateAvailable: c.state = "\xE2\x9C\x93 Installed " + s.installed_version + "  |  " + m.version + " is out"; break;
      case State::Working: c.state = s.line; break;
      default: c.state = m.one_click || s.automatic ? "Not installed" : "Not installed. Download it from its page, then put the file in mods/Downloads.";
    }
    if (s.failed && s.state != State::Working) c.state = s.line;
    std::string size = m.download_bytes ? modinstall::size_text(m.download_bytes) + " download" : std::string("size not known");
    if (m.disc_bytes) size += ", " + modinstall::size_text(m.disc_bytes) + " disc";
    c.detail = size + "  |  " + m.license + (s.automatic ? "  |  Install downloads it" : "  |  download it yourself");
    if (s.state == State::Working) c.primary = "Cancel";
    else if (s.state == State::UpdateAvailable) c.primary = "Update to " + m.version;
    else if (s.state == State::Installed) c.primary = next == m.id ? "Chosen for the next start" : "Play it at the next start";
    else if (s.automatic) c.primary = "Get";
    else if (s.file_ready) c.primary = "Add file (mods/Downloads)";
    else { c.primary = "Open download page"; c.primary_enabled = false; }
    if (online && s.state != State::Working) c.primary_enabled = false;   // not while a session is online
    c.can_remove = (s.state == State::Installed || s.state == State::UpdateAvailable) && !online;
    c.remove_armed = armed == m.id;
    if (!s.automatic && s.state == State::NotInstalled && !s.file_ready) c.get_it = "Get it at " + m.page + ", then put " + m.asset_pattern + " in mods/Downloads/";
    out.push_back(std::move(c));
  }
  return out;
}

bool primary(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu);
  refresh_locked(true);
  const auto* s = find_locked(id);
  if (!s) return false;
  armed.clear();
  auto& manager = mod_manager();
  std::string why;
  bool changed = false;
  if (s->state == State::Working) { manager.cancel(); say("mods: cancel %s", id.c_str()); }
  else if (s->state == State::Installed) { if (mod_profile_next() != id) { mod_profile_set(id); changed = true; } say("mods: %s chosen for the next start", id.c_str()); }
  else if (manager.install(id, &why)) say("mods: install %s", id.c_str());
  else say("mods: %s: %s", id.c_str(), why.c_str());
  refresh_locked(true);
  return changed;
}

bool remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu);
  if (armed != id) { armed = id; return false; }
  armed.clear();
  std::string why;
  bool changed = false;
  if (mod_manager().remove(id, &why)) {
    say("mods: removed %s", id.c_str());
    if (mod_profile_next() == id) { mod_profile_set(""); changed = true; }
  } else {
    say("mods: %s: %s", id.c_str(), why.c_str());
  }
  refresh_locked(true);
  return changed;
}

std::string profile_next() { return mod_profile_next(); }
std::vector<std::string> profile_choices() { return mod_profile_choices(); }
bool profile_set(const std::string& profile) { say("mods: profile at the next start: %s", profile.empty() ? "none" : profile.c_str()); return mod_profile_set(profile); }
bool downloads_available() { return mod_downloads_available(); }
}  // namespace fe::mu_mods
