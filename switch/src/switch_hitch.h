// Hitch catcher: logs where the game thread is when a frame runs long. See switch_hitch.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <switch.h>
#ifdef __cplusplus
bool switch_hitch_start(Thread* target, uint64_t stack_bytes);
void switch_hitch_stop(void);
extern "C" void switch_hitch_frame_begin(void);
#endif
