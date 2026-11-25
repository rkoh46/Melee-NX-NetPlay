// melee-unlocked's test_env.h for jukebox.cpp: the environment as it is (there is no YouGame session here).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
namespace host::test_env {
inline const char* getenv(const char* name) { return std::getenv(name); }
}  // namespace host::test_env
