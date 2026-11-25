// melee-unlocked's character select easter egg (walter_egg.cpp, compiled from the fetched tree by src/egg.cpp) reads its
// test switches through host::test_env; here they are plain environment variables. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
namespace host::test_env { inline const char* getenv(const char* name) { return std::getenv(name); } }
