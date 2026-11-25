// RetroArch shader presets over the game picture (melee-unlocked's gx/user_shader.cpp, settings.ini shaderpreset): a .slangp
// preset (or a single .slang shader) from the Shaders folder, compiled from its slang GLSL to SPIR-V by glslang on the device
// and run as a chain of passes in Aurora's present pass (frontend/patches/0134; Dawn's SPIR-V reader takes the modules).
// Presentation only: nothing here reaches the game. Weak: user_shader.cpp is built only where glslang is (build.sh glslang).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace fe::shaders {
// Render thread, once a frame: follows gfx::live().shader_preset (a name under Shaders/, empty: off).
__attribute__((weak)) void frame();
// The presets and single shaders under Shaders/ (upstream's presets(): .slangp at any depth, .slang at the top), as names
// relative to it, sorted. Read again after refresh_presets().
__attribute__((weak)) std::vector<std::string> presets();
__attribute__((weak)) void refresh_presets();
// "Off", "Running: N passes", "Compiling...", or why the chosen preset is not running.
__attribute__((weak)) std::string status();
// Before Aurora shuts down: the chain leaves the present pass.
__attribute__((weak)) void shutdown();
}  // namespace fe::shaders
