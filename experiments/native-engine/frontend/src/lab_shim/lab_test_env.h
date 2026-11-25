// The Lab view's test switches (host::test_env), plain environment variables here. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
namespace host::test_env { inline const char* getenv(const char* name) { return std::getenv(name); } }
