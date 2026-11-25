// The registry behind ../feature_hooks.h. Modules register from register_* functions called here once,
// instead of from static initialisers: the modules are linked from a static library, where an object
// nothing refers to would be left out together with its initialiser.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "features/present/present.h"

#include <cstring>
#include <mutex>

namespace fe::features {
namespace {
struct Registry {
  std::vector<std::pair<uint8_t, CommandFn>> commands;
  std::vector<TablesFn> tables;
  std::vector<RetraceFn> retraces;
  std::vector<SettingFn> settings;
  std::vector<RunEndFn> run_ends;
  std::vector<MenuEntry> menu;
  std::vector<HudFn> huds;
  std::vector<ReplyFn> replies;
  std::vector<ExchangeFn> exchanges;
};
Registry& registry() { static Registry r; return r; }
void ensure() {
  static std::once_flag once;
  std::call_once(once, [] {
    register_options(); register_training(); register_replays(); register_overlays(); register_spectator(); register_netoverlay(); register_search(); register_proai(); register_looks(); register_cosmetics(); register_egg(); register_audio(); register_gecko(); register_phob();
    if (present::linked()) { present::register_settings(); present::configure_from_arguments(options.rest); }
  });
}
}  // namespace

void add_command(uint8_t command, CommandFn fn) { registry().commands.emplace_back(command, fn); }
void add_tables(TablesFn fn) { registry().tables.push_back(fn); }
void add_retrace(RetraceFn fn) { registry().retraces.push_back(fn); }
void add_setting(SettingFn fn) { registry().settings.push_back(fn); }
void add_run_end(RunEndFn fn) { registry().run_ends.push_back(fn); }
void add_menu_entry(const MenuEntry& entry) { registry().menu.push_back(entry); }
void add_hud(HudFn fn) { registry().huds.push_back(fn); }
void add_reply(ReplyFn fn) { registry().replies.push_back(fn); }
void add_exchange(ExchangeFn fn) { registry().exchanges.push_back(fn); }

void dispatch_tables(MuHostApi* host, MuGameApi* game) {
  ensure();
  for (TablesFn fn : registry().tables) fn(host, game);
}

int32_t dispatch_command(uint8_t command, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity,
                         uint32_t* response_size) {
  ensure();
  if (response_size) *response_size = 0;
  std::vector<uint8_t> reply;
  for (const auto& [byte, fn] : registry().commands) {
    if (byte != command) continue;
    reply.clear();
    const int taken = fn(command, payload, size, reply);
    if (taken == 0) continue;
    if (taken < 0) return taken;
    if (reply.size() > capacity) return -2;
    if (!reply.empty()) std::memcpy(response, reply.data(), reply.size());
    if (response_size) *response_size = uint32_t(reply.size());
    return 0;
  }
  return INT32_MIN;
}

void dispatch_retrace(const Retrace& info) {
  ensure();
  for (RetraceFn fn : registry().retraces) fn(info);
}

bool dispatch_setting(const std::string& key, const std::string& value) {
  ensure();
  for (SettingFn fn : registry().settings) if (fn(key, value)) return true;
  return false;
}

void dispatch_run_end() {
  ensure();
  for (RunEndFn fn : registry().run_ends) fn();
}

void dispatch_reply(uint8_t command, const uint8_t* response, uint32_t size) {
  ensure();
  for (ReplyFn fn : registry().replies) fn(command, response, size);
}

void dispatch_exchange(uint8_t command, const uint8_t* payload, uint32_t payload_size, const uint8_t* response, uint32_t response_size,
                       int32_t result, bool resim) {
  ensure();
  for (ExchangeFn fn : registry().exchanges) fn(command, payload, payload_size, response, response_size, result, resim);
}

const std::vector<MenuEntry>& menu_entries() { ensure(); return registry().menu; }

std::vector<HudLine> hud_lines() {
  ensure();
  std::vector<HudLine> out;
  for (HudFn fn : registry().huds) fn(out);
  return out;
}
}  // namespace fe::features
