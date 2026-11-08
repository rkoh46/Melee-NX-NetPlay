// Overlay drawing through Aurora's ImGui pass (Switch). See overlay_imgui.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "overlay.h"
namespace host {
// Call inside an Aurora frame (between aurora_begin_frame and aurora_end_frame).
void overlay_imgui_draw(const OverlayFrame& frame);
}  // namespace host
