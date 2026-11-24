// Presentation at the display's rate with optional sub-frame poses. See present.cpp. Render thread only, apart
// from configure_from_arguments (before the run), mark_discontinuity (any thread) and the settings keys.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace gx { class Backend; class FrameQueue; struct Frame; struct DrawMatrices; }

namespace fe::present {
// Every function is declared weak: the feature modules (src/features/*.cpp) call these, and this file
// (features/present/present.cpp) is compiled with the renderer's sources, so a build without it still links;
// the calls below check for it.
#define MU_PRESENT_WEAK __attribute__((weak))
// --frame-mode off|predict|interpolate (upstream's names authored / authored-interpolate are taken too), --fps N.
MU_PRESENT_WEAK void configure_from_arguments(const std::vector<std::string>& args);
// Whether play mode should use loop() instead of its own render loop (sub-frames on, or a presentation cap).
MU_PRESENT_WEAK bool active();
// Sub-frames need every draw's full state and its identity from the decoder (not per-draw deltas): the decoder's
// backend answers wants_state_deltas() with !wants_full_state().
MU_PRESENT_WEAK bool wants_full_state();
// A rollback load, a Training Lab load, a replay jump: the next game frame is not blended with the one before.
MU_PRESENT_WEAK void mark_discontinuity();
MU_PRESENT_WEAK double display_hz();                  // measured refresh rate (0 before it is known or without vsync)
// The render loop. `render` draws one frame (overrides: per-draw matrices, or null) and keeps play mode's own
// statistics; `pump` services the window once per presented frame; returns when `stop` is set and the queue is empty.
using Render = void (*)(gx::Backend& backend, const gx::Frame& frame, const gx::DrawMatrices* overrides);
MU_PRESENT_WEAK void loop(gx::Backend& backend, gx::FrameQueue& queue, const std::atomic<bool>& stop, void (*pump)(), Render render);
// The settings keys (subframe, fps) and, in Off mode, play mode's present hook (each game frame presented once per
// display refresh). Called by the feature registry.
MU_PRESENT_WEAK void register_settings();

inline bool linked() { return &register_settings != nullptr; }
inline void note_discontinuity() { if (&mark_discontinuity) mark_discontinuity(); }
}  // namespace fe::present
