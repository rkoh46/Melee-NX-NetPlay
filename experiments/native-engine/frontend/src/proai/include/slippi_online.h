// The online settings the Practice menu's Search pages read: the codes skipped in Unranked (settings.ini
// unranked_skip_1..3 in this app, features/proai.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace slippi::online {
struct Config { std::string blocked_codes[3]; };
Config& config();
}
