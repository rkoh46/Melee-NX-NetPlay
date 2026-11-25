// The host services melee-unlocked's PhobGCC menu (gx/phob_menu.cpp) and its setups store (host/phob_store.cpp) call, for the
// frontend's build of them (build/native-engine-gfx/gen/phob, copied from the fetched tree by build.sh sync). The adapter calls
// are answered by src/phob_adapter.cpp (the console's adapter reader, frontend/patches/0123) in src/phob_menu_glue.cpp.
// Not melee-unlocked's host.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "phob_adapter.h"   // phob.h and host::GCScopeSample

namespace host {
void log(const char* fmt, ...);
void gcadapter_phob_set(int port, const phob::Settings& settings);
phob::Settings gcadapter_phob_get(int port);
bool gcadapter_phob_raw(int port, phob::HardwareInput& in);
int gcadapter_scope(int port, GCScopeSample* out, int max);
void gcadapter_test_script_start();
double gcadapter_poll_rate_hz();
void phob_store_load(const std::string& settings_path);
bool phob_store_save(int port, const phob::Settings& s);
std::string phob_store_folder();
std::vector<std::string> phob_setup_list();
bool phob_setup_delete(const std::string& name);
bool phob_setup_save(const std::string& name, const phob::Settings& s, bool with_calibration);
bool phob_setup_load(const std::string& name, phob::Settings& s);
}  // namespace host
