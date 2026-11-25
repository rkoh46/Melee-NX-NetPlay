// melee-unlocked's interface on the Switch: its in-game settings panel (mu_panel.cpp) and its launcher as the app's
// home screen before the game (mu_home.cpp). Both draw through Aurora's ImGui pass with melee-unlocked's own drawing
// code, taken from the fetched tree at build time (scripts/mu_ui_extract.py), its fonts and its artwork (embedded by
// build.sh sync, never committed). See FEATURES.md, "Interface".
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

struct ImFont;

namespace host { struct PadState; }

namespace fe::mu_ui {
// Render thread, once, after Aurora is up and before its first frame: ImGui's own font stays the default (every other
// overlay keeps its look); melee-unlocked's Source Sans 3 Black (headings) and Bold (body) are added beside it.
void fonts_init();
ImFont* heading_font();          // Source Sans 3 Black, 28 px (melee-unlocked's heading font)
ImFont* body_font();             // Source Sans 3 Bold, 16 px (its body font is the system's Segoe UI / Source Sans 3 Regular)
// The launcher's artwork (Bailey, the Melee Unlocked mark), decoded from the embedded BMPs and uploaded on first use.
struct Art { void* texture = nullptr; int w = 0, h = 0; };
const Art& bailey();
const Art& mark();
// The settings panel. Render thread, inside an ImGui frame (the game's overlay pass, or the home screen's own frame).
void panel_draw(int window_w, int window_h);
// Feeds the panel's controller navigation (melee-unlocked's settings_imgui_input.h) with the merged pads of the frame.
// The home screen calls it before the panel is drawn; in the game the menu's poll does (simulation thread).
void panel_feed(const host::PadState pads[4]);
// Applies what the panel asked for (a changed row, a closed panel) on the calling thread: the game's simulation thread
// in a match, the home screen's thread before the game. Also the open combo and the chords' requests.
void panel_service(bool in_game);
bool panel_open();
void panel_home_mode(bool on);   // the home screen drives the panel (panel_feed, panel_service(false)) until it ends
bool panel_take_quit();          // Quit game was confirmed while the home screen drove the panel
void panel_open_on(const std::string& page_title);   // opens the panel (on the category holding that page)
void panel_close();
// The Practice menu (features/proai.cpp's practice_menu_chord, weak): the panel's GAME > Training section has a
// "Practice menu" row when it is linked; Start + D-pad Up + Z goes to it directly (controls.cpp).
bool practice_available();
}  // namespace fe::mu_ui

