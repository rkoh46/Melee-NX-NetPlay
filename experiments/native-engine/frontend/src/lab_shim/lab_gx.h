// What the Lab view takes from melee-unlocked's gx_core.h: a frame's scene and frame_in_match, the function itself taken from
// the fetched tree by build.sh sync (gen/lab/frame_in_match.inc). SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace labgx {
struct Frame { uint8_t scene_major = 0, scene_minor = 0; };
#include "frame_in_match.inc"
}  // namespace labgx
