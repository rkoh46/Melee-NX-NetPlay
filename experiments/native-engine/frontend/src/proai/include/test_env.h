// melee-unlocked's test switches (MELEE_*), read from the environment.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
namespace host::test_env {
inline const char* getenv(const char* name) { return std::getenv(name); }
}
