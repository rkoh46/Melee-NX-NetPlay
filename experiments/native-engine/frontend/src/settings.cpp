// The player's game options in play mode. melee-unlocked's game asks its host for five option words and
// a few numbers (port/runtime/abi/mu_host.h in the fetched tree); its Windows host builds them from the
// settings panel (port/app/source_host.cpp, h_game_options and the functions around it). This does the
// same from a text file, with that host's rules: `vanilla 1` clears what it clears there, and while the
// session is online (settings_online: Slippi's online screens or a search or match of the netplay host) the
// words are what that host sends in a network session, with the offline-only bits cleared as well.
// Key names are upstream's port-settings.ini names where it has one. One "key value" per line, # comments.
// The scripted runs never get here: mu_frontend_option answers only in play mode.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "mu_host.h"
#include "hackpack_source.h"     // melee-unlocked (fetched tree): the Hack Pack's saved choices
#include "feature_calls.h"       // the feature modules' settings keys (src/features/)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fe {
namespace {
struct Settings {
  // Game (upstream's Game tab)
  bool unlock_all = true, results_screen = true, pal_stock_icons = false, no_screen_shake = false;
  int screen_shake = 100;                 // percent, 0..200
  bool stadium_frozen = false, cpu_20xx = false;
  int cpu_tech = 0, cpu_getup = 0, cpu_di = 0, cpu_sdi = 0;
  bool cpu_no_taunt = false, cpu_lcancel = false, cpu_no_rapid_jab = false, cpu_no_transform = false;
  int language = 0;                       // 0 the save's, 1 Japanese, 2 English
  bool vanilla = false;                   // ours: the retail game without the always-on code set
  // Picture
  bool widescreen = false, player_tags = false, hide_ko_explosions = false, hide_hud_emblems = false, freeze_fd = false;
  bool low_poly = false, fod_reflections = false, css_background_free = false, training_hud = true;
  int stock_hud_scale = 100, damage_hud_scale = 100;   // 75..175
  uint32_t community_codes = 0;           // MU_DISPLAY_OPTION_* community code bits, hex
  bool arcade_hitboxes = false, arcade_projectile_hitboxes = false, arcade_hitboxes_behind = false, arcade_camera_2d = false;
  int arcade_backdrop = 0;                // 2: grid
  uint32_t arcade_grid_rgb = 0xFFFFFF;
  // Sound
  bool mute_voices = false, audio_early_keyon = false;
  int music = 100;
  // Mods
  std::string mod_profile;                // <out>/mods/Profiles/<name>.ini, or the folder <out>/mods/<name>/
  bool mod_te_enabled = true;             // 20XX TE's features, when its save is in the profile
  uint32_t te_options = 0x10u, te_options2 = 0;   // 20XX TE's two feature words (its in-game menu changes them)
  int te_menu_music = 0;
  bool slippi_menus = false;              // ours: Slippi's menu files and online menu rows
  // Online (upstream's onlinedelay / onlinedelayauto; the existing app's default is automatic)
  int online_delay = 2;                   // 1..9 frames, used when online_delay_auto is off
  bool online_delay_auto = true;
} settings;
// Online: set by play mode once per retrace, read by the game through the option functions (same thread).
std::atomic<bool> online{false};
bool active = false, boot_to_css = false;
uint32_t cpu_word = 0;                    // option word 3's CPU bits: from the cpu_* keys, then the Hack Pack's menu
bool cpu_word_from_menu = false;
std::string state_path, file_path;
std::set<std::string> given;              // keys settings.ini set
std::mutex file_mutex;                    // settings.ini is written from the menu (simulation thread) only, but keep it simple
bool app_slippi_default = false;          // the app's default for slippi_menus (settings_app_defaults)

uint32_t cpu_word_of_settings();

const char kTemplate[] =
  "# mu-native-gfx play mode settings. One \"key value\" per line; names are melee-unlocked's where it has one.\n"
  "# Remove the # to change a line. Read once, when the program starts.\n"
  "#unlock_all 1            # 0: the memory card decides which fighters and stages are open\n"
  "#results_screen 1        # 0: back to the character select after a match\n"
  "#palstockicons 0\n"
  "#noscreenshake 0\n"
  "#screenshake 100         # percent, 0..200\n"
  "#stockhudscale 100       # 75..175\n"
  "#damagehudscale 100      # 75..175\n"
  "#playertags 0            # P1/P2 markers stay up all match\n"
  "#hide_ko_explosions 0\n"
  "#hide_hud_emblems 0\n"
  "#community_codes 0       # hex bits: 1000 HUD fade, 2000 respawn platform colours, 4000 Nana percent, 10000 no GO!,\n"
  "#                        # 20000/40000/80000 hitbox id colours/order/angles, 200000 V-cancel sound, 1000000 no fighter\n"
  "#                        # shadows, 4000000 centred 2P HUD, 100000 full FoD reflections, 400000 Instant Melee, 800000 free pause\n"
  "#                        # camera, 2000000 boot to CSS\n"
  "#stadium_frozen 0\n"
  "#freeze_fd 0\n"
  "#widescreen 0            # the 16:9 code; the picture then fills the screen\n"
  "#arcade_hitboxes 0\n"
  "#arcade_projectile_hitboxes 0\n"
  "#arcade_hitboxes_behind 0\n"
  "#arcade_backdrop 0       # 2: a grid behind the stage\n"
  "#arcade_grid_rgb FFFFFF\n"
  "#cpu_20xx 0\n"
  "#cpu_tech 0              # 1 in place, 2 roll forward, 3 roll back, 4 miss, 5 random\n"
  "#cpu_getup 0             # 1 stand, 2 roll forward, 3 roll back, 4 attack, 5 random\n"
  "#cpu_di 0                # 1 none, 2 random, 3 survival\n"
  "#cpu_sdi 0               # 1 none, 2 random, 3 against the hit, 4 with it, 5 up, 6 down\n"
  "#cpu_notaunt 0\n"
  "#cpu_lcancel 0\n"
  "#cpu_nojab 0\n"
  "#cpu_notransform 0\n"
  "#mute_voices 0\n"
  "#gamelanguage 0          # 1 Japanese, 2 English\n"
  "#music 100               # 0..100\n"
  "#vanilla 0               # 1: the retail game without the built-in code set (and without the options above that need it)\n"
  "#slippi_menus 0          # 1: Slippi's menu files (the online menu rows on the main menu). With the services program and\n"
  "#                        # a signed-in user file, Online Play works from there; mod profiles are then not loaded.\n"
  "#onlinedelayauto 1       # online input delay chosen from the opponent's ping (2..5 frames)\n"
  "#onlinedelay 2           # 1..9 frames, with onlinedelayauto 0\n"
  "#mod_profile hackpack    # mod files: mods/Profiles/<name>.ini (\"iso = file\" and \"gci = file\" lines), or every disc image\n"
  "#                        # and save in the folder mods/<name>/. Its own memory card folder is card-<name>.\n"
  "#mod_te_enabled 1        # 0: a 20XX TE save in the profile does not switch its features on\n";

void set(const std::string& key, const std::string& value) {
  auto& s = settings;
  const int n = std::atoi(value.c_str());
  const bool on = value == "1";
  if (key == "unlock_all") s.unlock_all = value != "0";
  else if (key == "results_screen") s.results_screen = value != "0";
  else if (key == "palstockicons") s.pal_stock_icons = on;
  else if (key == "noscreenshake") s.no_screen_shake = on;
  else if (key == "screenshake") s.screen_shake = std::clamp(n, 0, 200);
  else if (key == "stockhudscale") s.stock_hud_scale = std::clamp(n, 75, 175);
  else if (key == "damagehudscale") s.damage_hud_scale = std::clamp(n, 75, 175);
  else if (key == "playertags") s.player_tags = on;
  else if (key == "hide_ko_explosions") s.hide_ko_explosions = on;
  else if (key == "hide_hud_emblems") s.hide_hud_emblems = on;
  else if (key == "community_codes") s.community_codes = uint32_t(std::strtoul(value.c_str(), nullptr, 16)) & (MU_DISPLAY_OPTION_CODES_ALL | MU_DISPLAY_OPTION_OFFLINE_CODES);
  else if (key == "stadium_frozen") s.stadium_frozen = on;
  else if (key == "freeze_fd") s.freeze_fd = on;
  else if (key == "widescreen") s.widescreen = on;
  else if (key == "arcade_hitboxes") s.arcade_hitboxes = on;
  else if (key == "arcade_projectile_hitboxes") s.arcade_projectile_hitboxes = on;
  else if (key == "arcade_hitboxes_behind") s.arcade_hitboxes_behind = on;
  else if (key == "arcade_camera_2d") s.arcade_camera_2d = on;
  else if (key == "arcade_backdrop") s.arcade_backdrop = std::clamp(n, 0, 2);
  else if (key == "arcade_grid_rgb") s.arcade_grid_rgb = uint32_t(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFFu;
  else if (key == "cpu_20xx") s.cpu_20xx = on;
  else if (key == "cpu_tech") s.cpu_tech = std::clamp(n, 0, 5);
  else if (key == "cpu_getup") s.cpu_getup = std::clamp(n, 0, 5);
  else if (key == "cpu_di") s.cpu_di = std::clamp(n, 0, 3);
  else if (key == "cpu_sdi") s.cpu_sdi = std::clamp(n, 0, 6);
  else if (key == "cpu_notaunt") s.cpu_no_taunt = on;
  else if (key == "cpu_lcancel") s.cpu_lcancel = on;
  else if (key == "cpu_nojab") s.cpu_no_rapid_jab = on;
  else if (key == "cpu_notransform") s.cpu_no_transform = on;
  else if (key == "mute_voices") s.mute_voices = on;
  else if (key == "audio_early_keyon") s.audio_early_keyon = on;
  else if (key == "gamelanguage") s.language = value == "1" ? 1 : value == "2" ? 2 : 0;
  else if (key == "music") s.music = std::clamp(n, 0, 100);
  else if (key == "lowpoly") s.low_poly = on;
  else if (key == "fodreflections") s.fod_reflections = value != "0";
  else if (key == "cssbackgroundfree") s.css_background_free = on;
  else if (key == "training_hud") s.training_hud = value != "0";
  else if (key == "vanilla") s.vanilla = on;
  else if (key == "slippi_menus") s.slippi_menus = on;
  else if (key == "onlinedelay") { if (n >= 1 && n <= 9) s.online_delay = n; }
  else if (key == "onlinedelayauto") s.online_delay_auto = on;
  else if (key == "mod_profile") s.mod_profile = value;
  else if (key == "mod_te_enabled") s.mod_te_enabled = value != "0";
  else if (key == "te_options") s.te_options = uint32_t(std::strtoul(value.c_str(), nullptr, 16));
  else if (key == "te_options2") s.te_options2 = uint32_t(std::strtoul(value.c_str(), nullptr, 16));
  else if (key == "te_menu_music") s.te_menu_music = n;
  else if (key == "hp_settings") source_port::hackpack::settings_parse(value);
  else if (key == "cpu_word") { cpu_word = uint32_t(std::strtoul(value.c_str(), nullptr, 16)) & MU_GAME_OPTION3_CPU_ALL; cpu_word_from_menu = true; }
  else if (features::call_setting(key, value)) {}   // a feature module's key
  else say("play: settings: unknown key \"%s\"", key.c_str());
}
std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%X", v); return b; }
std::string num(int v) { return std::to_string(v); }
std::string bit(bool v) { return v ? "1" : "0"; }
// The value settings.ini would hold for `key` (as set() reads it); empty for a key this file does not know.
std::string get(const std::string& key) {
  const auto& s = settings;
  if (key == "unlock_all") return bit(s.unlock_all);
  if (key == "results_screen") return bit(s.results_screen);
  if (key == "palstockicons") return bit(s.pal_stock_icons);
  if (key == "noscreenshake") return bit(s.no_screen_shake);
  if (key == "screenshake") return num(s.screen_shake);
  if (key == "stockhudscale") return num(s.stock_hud_scale);
  if (key == "damagehudscale") return num(s.damage_hud_scale);
  if (key == "playertags") return bit(s.player_tags);
  if (key == "hide_ko_explosions") return bit(s.hide_ko_explosions);
  if (key == "hide_hud_emblems") return bit(s.hide_hud_emblems);
  if (key == "community_codes") return hex(s.community_codes);
  if (key == "stadium_frozen") return bit(s.stadium_frozen);
  if (key == "freeze_fd") return bit(s.freeze_fd);
  if (key == "widescreen") return bit(s.widescreen);
  if (key == "arcade_hitboxes") return bit(s.arcade_hitboxes);
  if (key == "arcade_projectile_hitboxes") return bit(s.arcade_projectile_hitboxes);
  if (key == "arcade_hitboxes_behind") return bit(s.arcade_hitboxes_behind);
  if (key == "arcade_camera_2d") return bit(s.arcade_camera_2d);
  if (key == "arcade_backdrop") return num(s.arcade_backdrop);
  if (key == "arcade_grid_rgb") return hex(s.arcade_grid_rgb);
  if (key == "cpu_20xx") return bit(s.cpu_20xx);
  if (key == "cpu_tech") return num(s.cpu_tech);
  if (key == "cpu_getup") return num(s.cpu_getup);
  if (key == "cpu_di") return num(s.cpu_di);
  if (key == "cpu_sdi") return num(s.cpu_sdi);
  if (key == "cpu_notaunt") return bit(s.cpu_no_taunt);
  if (key == "cpu_lcancel") return bit(s.cpu_lcancel);
  if (key == "cpu_nojab") return bit(s.cpu_no_rapid_jab);
  if (key == "cpu_notransform") return bit(s.cpu_no_transform);
  if (key == "mute_voices") return bit(s.mute_voices);
  if (key == "audio_early_keyon") return bit(s.audio_early_keyon);
  if (key == "gamelanguage") return num(s.language);
  if (key == "music") return num(s.music);
  if (key == "lowpoly") return bit(s.low_poly);
  if (key == "fodreflections") return bit(s.fod_reflections);
  if (key == "cssbackgroundfree") return bit(s.css_background_free);
  if (key == "training_hud") return bit(s.training_hud);
  if (key == "vanilla") return bit(s.vanilla);
  if (key == "slippi_menus") return bit(s.slippi_menus);
  if (key == "onlinedelay") return num(s.online_delay);
  if (key == "onlinedelayauto") return bit(s.online_delay_auto);
  if (key == "mod_profile") return s.mod_profile;
  if (key == "mod_te_enabled") return bit(s.mod_te_enabled);
  return std::string();
}
// Writes `key value` into settings.ini, keeping every other line and comment: an existing line for the key is
// replaced, else its commented template line, else the line is added at the end.
void save(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(file_mutex);
  if (file_path.empty()) return;
  std::vector<std::string> lines;
  { std::ifstream in(file_path); std::string line; while (std::getline(in, line)) lines.push_back(line); }
  const std::string wanted = key + " " + value;
  const auto first_word = [](const std::string& line, size_t from) {
    size_t a = line.find_first_not_of(" \t", from); if (a == std::string::npos) return std::string();
    size_t b = line.find_first_of(" \t#", a); return line.substr(a, b == std::string::npos ? std::string::npos : b - a);
  };
  bool done = false;
  for (auto& line : lines) {
    const size_t start = line.find_first_not_of(" \t");
    if (start != std::string::npos && line[start] != '#' && first_word(line, start) == key) { line = wanted; done = true; }
  }
  if (!done)
    for (auto& line : lines)
      if (!line.empty() && line[0] == '#' && first_word(line, 1) == key) { line = wanted; done = true; break; }
  if (!done) lines.push_back(wanted);
  const std::string temporary = file_path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return;
    for (const auto& line : lines) out << line << '\n';
  }
  std::remove(file_path.c_str());
  if (std::rename(temporary.c_str(), file_path.c_str()) != 0) std::remove(temporary.c_str());
}

// As source_host.cpp's te_words_mask1/2 and kHpTeOptions/2.
constexpr uint32_t kPackTe1 = MU_GAME_OPTION_TE_NO_STAR_KO | MU_GAME_OPTION_TE_TAUNT_CANCEL | MU_GAME_OPTION_TE_FIXED_CAMERA;
constexpr uint32_t kPackTe2 = MU_GAME_OPTION2_TE_NO_SCREEN_RUMBLE | MU_GAME_OPTION2_TE_LCANCEL_FLASH | 0x00780000u | MU_GAME_OPTION2_TE_BUBBLES |
                              MU_GAME_OPTION2_TE_INPUT_DISPLAY | MU_GAME_OPTION2_TE_COLOR_OVERLAYS;
bool te_on() { return mods_te_owned() && settings.mod_te_enabled; }
uint32_t te_mask1() { return te_on() ? 0x00007FF0u : mods_hackpack() ? kPackTe1 : 0u; }
uint32_t te_mask2() { return te_on() ? 0xFFFFFFFFu : mods_hackpack() ? kPackTe2 : 0u; }

// The words, as port/app/source_host.cpp assembles them for a session that is no replay. Online (its
// slippi::online::session_mode() >= 0): the viewer's own picture and sound choices stay, as there; what only
// has a meaning offline is cleared (there the game is trusted to ignore most of it: mu_option2_offline,
// mu_options3_offline; here it is not sent at all), and no mod files are loaded in a session that can go online.
uint32_t word_options() {
  const auto& s = settings;
  const bool net = online.load(std::memory_order_relaxed);
  return (s.no_screen_shake ? MU_GAME_OPTION_NO_SCREEN_SHAKE : 0u) | (s.pal_stock_icons ? MU_GAME_OPTION_PAL_STOCK_ICONS : 0u) |
         (s.widescreen ? MU_GAME_OPTION_WIDESCREEN : 0u) |
         ((uint32_t(s.language) << MU_GAME_OPTION_LANGUAGE_SHIFT) & MU_GAME_OPTION_LANGUAGE_MASK) |
         (s.vanilla ? MU_GAME_OPTION_VANILLA : 0u) |
         (slippi_menus_on() ? MU_GAME_OPT_SLIPPI_MENUS : 0u) |
         // 20XX TE's own bits only with its save in the profile; with the Hack Pack and no TE save, the pack's share.
         (s.vanilla || net ? 0u : s.te_options & te_mask1());
  // Not offered: the Training Lab bits.
}
uint32_t word_options2() {
  const auto& s = settings;
  if (s.vanilla || online.load(std::memory_order_relaxed)) return 0;
  return (s.te_options2 & te_mask2() & ~(MU_GAME_OPTION2_STADIUM_FROZEN | MU_GAME_OPTION2_ARCADE_PACK)) |
         (s.cpu_20xx ? MU_GAME_OPTION2_TE_20XX_CPUS : 0u) | (s.stadium_frozen ? MU_GAME_OPTION2_STADIUM_FROZEN : 0u);
}
uint32_t cpu_word_of_settings() {
  const auto& s = settings;
  const auto field = [](int value, int max, int shift) { return value >= 1 && value <= max ? uint32_t(value) << shift : 0u; };
  const int sdi = s.cpu_sdi == 3 ? 4 : s.cpu_sdi == 4 ? 3 : s.cpu_sdi;   // the panel's row order to the game's (render_options.h)
  return field(s.cpu_tech, 5, MU_GAME_OPTION3_CPU_TECH_SHIFT) | field(s.cpu_getup, 5, MU_GAME_OPTION3_CPU_GETUP_SHIFT) |
         field(s.cpu_di, 3, MU_GAME_OPTION3_CPU_DI_SHIFT) | field(sdi, 6, MU_GAME_OPTION3_CPU_SDI_SHIFT) |
         (s.cpu_no_taunt ? MU_GAME_OPTION3_CPU_NO_TAUNT : 0u) | (s.cpu_lcancel ? MU_GAME_OPTION3_CPU_LCANCEL : 0u) |
         (s.cpu_no_rapid_jab ? MU_GAME_OPTION3_CPU_NO_RAPID_JAB : 0u) | (s.cpu_no_transform ? MU_GAME_OPTION3_CPU_NO_TRANSFORM : 0u);
}
uint32_t word_options3() {
  const auto& s = settings;
  if (online.load(std::memory_order_relaxed)) return 0;      // a network session has every fighter and stage, and no results screen option
  return (s.vanilla ? 0u : (cpu_word & MU_GAME_OPTION3_CPU_ALL) | mods_stage_bits()) | (s.unlock_all ? 0u : MU_GAME_OPTION3_LOCKED_CONTENT) |
         (s.results_screen ? 0u : MU_GAME_OPTION3_SKIP_RESULTS);
}
uint32_t word_display() {
  const auto& s = settings;
  const bool net = online.load(std::memory_order_relaxed);
  return (s.low_poly ? MU_DISPLAY_OPTION_LOW_POLY : 0u) | (s.training_hud ? 0u : MU_DISPLAY_OPTION_NO_TRAINING_HUD) |
         (s.player_tags ? MU_DISPLAY_OPTION_PLAYER_TAGS : 0u) | (s.fod_reflections ? MU_DISPLAY_OPTION_FOD_REFLECTIONS : 0u) |
         (s.css_background_free ? MU_DISPLAY_OPTION_CSS_BACKGROUND_FREE : 0u) | (s.freeze_fd ? MU_DISPLAY_OPTION_FREEZE_FD : 0u) |
         (s.hide_ko_explosions ? MU_DISPLAY_OPTION_NO_KO_EXPLOSIONS : 0u) | (s.hide_hud_emblems ? MU_DISPLAY_OPTION_NO_HUD_EMBLEMS : 0u) |
         (s.community_codes & MU_DISPLAY_OPTION_CODES_ALL) |
         // Never in a network session: the offline codes (menu timing, the pause), the Arcade look's hitboxes and its camera.
         (net ? 0u : (s.community_codes & MU_DISPLAY_OPTION_OFFLINE_CODES) |
                     (s.arcade_hitboxes ? MU_DISPLAY_OPTION_HITBOXES : 0u) | (s.arcade_projectile_hitboxes ? MU_DISPLAY_OPTION_PROJECTILE_HITBOXES : 0u) |
                     (s.arcade_hitboxes_behind ? MU_DISPLAY_OPTION_HITBOXES_BEHIND : 0u) | (s.arcade_camera_2d ? MU_DISPLAY_OPTION_ARCADE_CAMERA : 0u) |
                     (boot_to_css ? MU_DISPLAY_OPTION_BOOT_TO_CSS : 0u)) |  // --auto-vs
         (s.arcade_backdrop == 2 ? MU_DISPLAY_OPTION_GRID : 0u);
}
uint32_t word_audio() {
  // Mute voices everywhere; faster sound effects never in a network session (upstream's h_audio_options).
  return (settings.mute_voices ? MU_AUDIO_OPTION_MUTE_VOICES : 0u) |
         (settings.audio_early_keyon && !online.load(std::memory_order_relaxed) ? MU_AUDIO_OPTION_EARLY_KEYON : 0u);
}
}  // namespace

bool settings_vanilla() { return settings.vanilla; }
void settings_online(bool on) { online.store(on, std::memory_order_relaxed); }
void settings_deactivate() { active = false; }
int settings_online_delay() { return settings.online_delay_auto ? 0 : settings.online_delay; }
bool settings_slippi_menus() { return settings.slippi_menus && !settings.vanilla; }
const std::string& settings_profile() { return settings.mod_profile; }
uint32_t& settings_cpu_word() { return cpu_word; }
uint32_t& settings_te_word(int which) { return which == 1 ? settings.te_options : settings.te_options2; }
int& settings_te_music() { return settings.te_menu_music; }
// What the game changes from its own menus (20XX TE's settings menu, the Hack Pack's menu and choices) is kept
// beside the settings file and read after it at the next start. Called from the simulation thread, rarely.
void settings_state_changed() {
  cpu_word_from_menu = true;
  if (state_path.empty()) return;
  if (FILE* out = std::fopen(state_path.c_str(), "w")) {
    std::fprintf(out, "# written by mu-native-gfx: what was changed from inside the game\nte_options %X\nte_options2 %X\nte_menu_music %d\ncpu_word %X\n",
                 settings.te_options, settings.te_options2, settings.te_menu_music, cpu_word);
    if (!source_port::hackpack::settings_are_default()) std::fprintf(out, "hp_settings %s\n", source_port::hackpack::settings_text().c_str());
    std::fclose(out);
  }
}
bool settings_widescreen() { return settings.widescreen; }
void settings_boot_to_css(bool on) { boot_to_css = on; }

void settings_load(const std::string& path) {
  active = true;
  file_path = path;
  FILE* in = std::fopen(path.c_str(), "r");
  if (!in) {
    std::string text = kTemplate;
    if (app_slippi_default) {   // the app's default: Slippi's menus (Online Play on the main menu)
      const size_t at = text.find("#slippi_menus 0 ");
      if (at != std::string::npos) text.replace(at, 16, "#slippi_menus 1 ");
    }
    if (FILE* out = std::fopen(path.c_str(), "w")) { std::fputs(text.c_str(), out); std::fclose(out); say("play: settings: %s written with every line commented out (the defaults)", path.c_str()); }
    else say("play: settings: no %s; defaults", path.c_str());
  } else {
    char line[512]; int count = 0;
    while (std::fgets(line, sizeof line, in)) {
      if (char* hash = std::strchr(line, '#')) *hash = 0;
      char key[64], value[256];
      if (std::sscanf(line, " %63s %255s", key, value) == 2) { set(key, value); given.insert(key); ++count; }
    }
    std::fclose(in);
    say("play: settings: %d lines read from %s", count, path.c_str());
  }
  if (!cpu_word_from_menu) cpu_word = cpu_word_of_settings();
  state_path = path + ".state";
  if (FILE* state = std::fopen(state_path.c_str(), "r")) {
    char line[512];
    while (std::fgets(line, sizeof line, state)) {
      if (char* hash = std::strchr(line, '#')) *hash = 0;
      char key[64], value[256];
      if (std::sscanf(line, " %63s %255s", key, value) == 2) set(key, value);
    }
    std::fclose(state);
  }
}

std::string settings_get(const std::string& key) { return get(key); }
bool settings_given(const std::string& key) { return given.count(key) != 0; }
void settings_write_key(const std::string& key, const std::string& value) { save(key, value); say("play: settings: %s %s (written to %s)", key.c_str(), value.c_str(), file_path.c_str()); }
void settings_set_quiet(const std::string& key, const std::string& value) { set(key, value); }
bool settings_change(const std::string& key, const std::string& value) {
  if (get(key).empty() && key != "mod_profile") return false;
  set(key, value);
  if (key.rfind("cpu_", 0) == 0 && key != "cpu_20xx") cpu_word = cpu_word_of_settings();   // the CPU rows feed option word 3
  save(key, get(key));
  say("play: settings: %s %s (written to %s)", key.c_str(), get(key).c_str(), file_path.c_str());
  return true;
}
void settings_app_defaults(bool online_possible) { app_slippi_default = online_possible; settings.slippi_menus = online_possible; }
bool settings_online_now() { return online.load(std::memory_order_relaxed); }
const std::string& settings_path() { return file_path; }

// After the host has started (the profile's layers are known): what the game will be told.
void settings_report() {
  say("play: options %08X %08X %08X, display %08X, audio %X, HUD %d/%d%%, screen shake %d%%, music %d%%", word_options(), word_options2(), word_options3(),
      word_display(), word_audio(),
      settings.stock_hud_scale, settings.damage_hud_scale, settings.screen_shake, settings.music);
}
}  // namespace fe

// Hook of the host services (patches/0101): see there for `which`.
extern "C" int mu_frontend_option(int which, uint32_t* value) {
  using namespace fe;
  if (!active) return 0;
  if (which < 0) return 1;
  switch (which) {
    case 0: *value = word_options(); return 1;
    case 1: *value = word_options2(); return 1;
    case 2: *value = word_options3(); return 1;
    case 3: *value = word_display(); return 1;
    case 4: *value = word_audio(); return 1;
    case 5: *value = uint32_t(settings.stock_hud_scale) | (uint32_t(settings.damage_hud_scale) << 8); return 1;
    case 6: *value = uint32_t(settings.music); return 1;
    case 7: { const float scale = settings.screen_shake / 100.0f; std::memcpy(value, &scale, 4); return 1; }
    case 8: *value = settings.arcade_grid_rgb; return 1;
    case 9: *value = mods_flags(); return 1;
    default: return 0;
  }
}
