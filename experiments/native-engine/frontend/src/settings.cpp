// The player's game options in play mode. melee-unlocked's game asks its host for five option words and
// a few numbers (port/runtime/abi/mu_host.h in the fetched tree); its Windows host builds them from the
// settings panel (port/app/source_host.cpp, h_game_options and the functions around it). This does the
// same from a text file, with that host's rules: what is offline-only there is given here because play
// mode has no network session, replay or tournament; `vanilla 1` clears what it clears there.
// Key names are upstream's port-settings.ini names where it has one. One "key value" per line, # comments.
// The scripted runs never get here: mu_frontend_option answers only in play mode.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "mu_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
} settings;
bool active = false, boot_to_css = false;

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
  "#                        # shadows, 4000000 centred 2P HUD, 400000 Instant Melee, 800000 free pause camera, 2000000 boot to CSS\n"
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
  "#vanilla 0               # 1: the retail game without the built-in code set (and without the options above that need it)\n";

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
  else say("play: settings: unknown key \"%s\"", key.c_str());
}

// The words, as port/app/source_host.cpp assembles them for an offline session that is no replay.
uint32_t word_options() {
  const auto& s = settings;
  return (s.no_screen_shake ? MU_GAME_OPTION_NO_SCREEN_SHAKE : 0u) | (s.pal_stock_icons ? MU_GAME_OPTION_PAL_STOCK_ICONS : 0u) |
         (s.widescreen ? MU_GAME_OPTION_WIDESCREEN : 0u) |
         ((uint32_t(s.language) << MU_GAME_OPTION_LANGUAGE_SHIFT) & MU_GAME_OPTION_LANGUAGE_MASK) |
         (s.vanilla ? MU_GAME_OPTION_VANILLA : 0u);
  // Not offered: the 20XX TE bits (they need its save, which that host checks) and the Training Lab bits.
}
uint32_t word_options2() {
  const auto& s = settings;
  if (s.vanilla) return 0;
  return (s.cpu_20xx ? MU_GAME_OPTION2_TE_20XX_CPUS : 0u) | (s.stadium_frozen ? MU_GAME_OPTION2_STADIUM_FROZEN : 0u);
}
uint32_t word_options3() {
  const auto& s = settings;
  const auto field = [](int value, int max, int shift) { return value >= 1 && value <= max ? uint32_t(value) << shift : 0u; };
  const int sdi = s.cpu_sdi == 3 ? 4 : s.cpu_sdi == 4 ? 3 : s.cpu_sdi;   // the panel's row order to the game's (render_options.h)
  const uint32_t cpu = field(s.cpu_tech, 5, MU_GAME_OPTION3_CPU_TECH_SHIFT) | field(s.cpu_getup, 5, MU_GAME_OPTION3_CPU_GETUP_SHIFT) |
                       field(s.cpu_di, 3, MU_GAME_OPTION3_CPU_DI_SHIFT) | field(sdi, 6, MU_GAME_OPTION3_CPU_SDI_SHIFT) |
                       (s.cpu_no_taunt ? MU_GAME_OPTION3_CPU_NO_TAUNT : 0u) | (s.cpu_lcancel ? MU_GAME_OPTION3_CPU_LCANCEL : 0u) |
                       (s.cpu_no_rapid_jab ? MU_GAME_OPTION3_CPU_NO_RAPID_JAB : 0u) | (s.cpu_no_transform ? MU_GAME_OPTION3_CPU_NO_TRANSFORM : 0u);
  return (s.vanilla ? 0u : cpu & MU_GAME_OPTION3_CPU_ALL) | (s.unlock_all ? 0u : MU_GAME_OPTION3_LOCKED_CONTENT) |
         (s.results_screen ? 0u : MU_GAME_OPTION3_SKIP_RESULTS);
}
uint32_t word_display() {
  const auto& s = settings;
  return (s.low_poly ? MU_DISPLAY_OPTION_LOW_POLY : 0u) | (s.training_hud ? 0u : MU_DISPLAY_OPTION_NO_TRAINING_HUD) |
         (s.player_tags ? MU_DISPLAY_OPTION_PLAYER_TAGS : 0u) | (s.fod_reflections ? MU_DISPLAY_OPTION_FOD_REFLECTIONS : 0u) |
         (s.css_background_free ? MU_DISPLAY_OPTION_CSS_BACKGROUND_FREE : 0u) | (s.freeze_fd ? MU_DISPLAY_OPTION_FREEZE_FD : 0u) |
         (s.hide_ko_explosions ? MU_DISPLAY_OPTION_NO_KO_EXPLOSIONS : 0u) | (s.hide_hud_emblems ? MU_DISPLAY_OPTION_NO_HUD_EMBLEMS : 0u) |
         (s.community_codes & MU_DISPLAY_OPTION_CODES_ALL) |
         (s.community_codes & MU_DISPLAY_OPTION_OFFLINE_CODES) |        // offline codes: never in a network session (there is none here)
         (s.arcade_hitboxes ? MU_DISPLAY_OPTION_HITBOXES : 0u) | (s.arcade_projectile_hitboxes ? MU_DISPLAY_OPTION_PROJECTILE_HITBOXES : 0u) |
         (s.arcade_hitboxes_behind ? MU_DISPLAY_OPTION_HITBOXES_BEHIND : 0u) | (s.arcade_camera_2d ? MU_DISPLAY_OPTION_ARCADE_CAMERA : 0u) |
         (s.arcade_backdrop == 2 ? MU_DISPLAY_OPTION_GRID : 0u) |
         (boot_to_css ? MU_DISPLAY_OPTION_BOOT_TO_CSS : 0u);            // --auto-vs
}
}  // namespace

bool settings_vanilla() { return settings.vanilla; }
bool settings_widescreen() { return settings.widescreen; }
void settings_boot_to_css(bool on) { boot_to_css = on; }

void settings_load(const std::string& path) {
  active = true;
  FILE* in = std::fopen(path.c_str(), "r");
  if (!in) {
    if (FILE* out = std::fopen(path.c_str(), "w")) { std::fputs(kTemplate, out); std::fclose(out); say("play: settings: %s written with every line commented out (the defaults)", path.c_str()); }
    else say("play: settings: no %s; defaults", path.c_str());
  } else {
    char line[512]; int count = 0;
    while (std::fgets(line, sizeof line, in)) {
      if (char* hash = std::strchr(line, '#')) *hash = 0;
      char key[64], value[128];
      if (std::sscanf(line, " %63s %127s", key, value) == 2) { set(key, value); ++count; }
    }
    std::fclose(in);
    say("play: settings: %d lines read from %s", count, path.c_str());
  }
  say("play: options %08X %08X %08X, display %08X, audio %X, HUD %d/%d%%, screen shake %d%%, music %d%%", word_options(), word_options2(), word_options3(),
      word_display(), (settings.mute_voices ? MU_AUDIO_OPTION_MUTE_VOICES : 0u) | (settings.audio_early_keyon ? MU_AUDIO_OPTION_EARLY_KEYON : 0u),
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
    case 4: *value = (settings.mute_voices ? MU_AUDIO_OPTION_MUTE_VOICES : 0u) | (settings.audio_early_keyon ? MU_AUDIO_OPTION_EARLY_KEYON : 0u); return 1;
    case 5: *value = uint32_t(settings.stock_hud_scale) | (uint32_t(settings.damage_hud_scale) << 8); return 1;
    case 6: *value = uint32_t(settings.music); return 1;
    case 7: { const float scale = settings.screen_shake / 100.0f; std::memcpy(value, &scale, 4); return 1; }
    case 8: *value = settings.arcade_grid_rgb; return 1;
    default: return 0;
  }
}
