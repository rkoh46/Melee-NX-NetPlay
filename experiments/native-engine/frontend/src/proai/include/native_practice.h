// What the Practice menu asks of upstream's native_practice (hle/native_practice.cpp), answered by features/proai.cpp:
// Training again with the CPU as a pro's character, and the warm-up room's way back (no warm-up room here).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace slippi::native_practice {
void submit_train_against(int character, int costume);
void submit_warmup_return();
}
