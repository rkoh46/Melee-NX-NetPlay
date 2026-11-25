// melee-unlocked's phob_store.cpp names its setups with the controller profiles' rule (profile_clean_name); here that rule is
// src/controls_files.cpp's copy (fe::controls), forwarded by src/phob_menu_glue.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace host { std::string profile_clean_name(const std::string& name); }
