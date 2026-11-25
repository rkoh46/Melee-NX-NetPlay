// Play mode's calls into the feature modules (feature_hooks.h, src/features/). The modules are linked only
// when the build includes them (CMake option MU_FE_FEATURES): the dispatch functions are referenced weakly
// here, so play mode works the same with no module linked.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "feature_hooks.h"

namespace fe::features {
__attribute__((weak)) void dispatch_tables(MuHostApi* host, MuGameApi* game);
__attribute__((weak)) int32_t dispatch_command(uint8_t command, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity,
                                               uint32_t* response_size);
__attribute__((weak)) void dispatch_retrace(const Retrace& info);
__attribute__((weak)) bool dispatch_setting(const std::string& key, const std::string& value);
__attribute__((weak)) void dispatch_run_end();
__attribute__((weak)) void dispatch_reply(uint8_t command, const uint8_t* response, uint32_t size);
__attribute__((weak)) void dispatch_exchange(uint8_t command, const uint8_t* payload, uint32_t payload_size, const uint8_t* response,
                                            uint32_t response_size, int32_t result, bool resim);
__attribute__((weak)) const std::vector<MenuEntry>& menu_entries();
__attribute__((weak)) std::vector<HudLine> hud_lines();
__attribute__((weak)) bool take_restart_request(std::vector<std::string>* extra_args);   // replays.cpp: "Watch it"
__attribute__((weak)) std::vector<std::string> replays_list();
__attribute__((weak)) std::string online_replay_for_report();                   // replays.cpp: Slippi's replay upload

// cosmetics.cpp (skins): called by mods.cpp around its disc table work (see there).
__attribute__((weak)) void cosmetics_disc_ready(bool can_go_online, const std::string& active_profile);
__attribute__((weak)) void cosmetics_apply_picks();
__attribute__((weak)) void cosmetics_view(bool retail_files);
__attribute__((weak)) bool cosmetics_online_copy(const std::string& slot, const std::vector<uint8_t>& bytes, std::string* reason);
__attribute__((weak)) int cosmetics_disc_read(uint64_t offset, void* out, size_t bytes);
__attribute__((weak)) size_t cosmetics_disc_skin_count(const std::string& profile);
// gecko.cpp (player Gecko codes): the panel's rows and switch.
__attribute__((weak)) std::vector<GeckoRow> gecko_rows();
__attribute__((weak)) std::string gecko_file();
__attribute__((weak)) void gecko_toggle(const std::string& name);

inline bool linked() { return &dispatch_retrace != nullptr; }
inline void call_tables(MuHostApi* host, MuGameApi* game) { if (&dispatch_tables) dispatch_tables(host, game); }
inline int32_t call_command(uint8_t command, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity, uint32_t* response_size) {
  return &dispatch_command ? dispatch_command(command, payload, size, response, capacity, response_size) : INT32_MIN;
}
inline void call_retrace(const Retrace& info) { if (&dispatch_retrace) dispatch_retrace(info); }
inline bool call_setting(const std::string& key, const std::string& value) { return &dispatch_setting && dispatch_setting(key, value); }
inline void call_run_end() { if (&dispatch_run_end) dispatch_run_end(); }
inline void call_reply(uint8_t command, const uint8_t* response, uint32_t size) { if (&dispatch_reply) dispatch_reply(command, response, size); }
inline void call_exchange(uint8_t command, const uint8_t* payload, uint32_t payload_size, const uint8_t* response, uint32_t response_size,
                          int32_t result, bool resim) {
  if (&dispatch_exchange) dispatch_exchange(command, payload, payload_size, response, response_size, result, resim);
}
inline std::vector<MenuEntry> call_menu_entries() { return &menu_entries ? menu_entries() : std::vector<MenuEntry>{}; }
inline std::vector<HudLine> call_hud_lines() { return &hud_lines ? hud_lines() : std::vector<HudLine>{}; }
inline bool call_take_restart(std::vector<std::string>* extra) { return &take_restart_request && take_restart_request(extra); }
inline std::vector<std::string> call_replays_list() { return &replays_list ? replays_list() : std::vector<std::string>{}; }
}  // namespace fe::features
