// In-client updater: asks GitHub for the latest release of hero88go/melee-unlocked, and on request
// downloads its zip and hands over to a small batch script that swaps the files in after the
// game exits and relaunches. Everything network-side runs on a background thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace host::updater {
enum class State { Idle, Checking, UpToDate, UpdateAvailable, Downloading, ReadyToInstall, Failed };
void check(const std::string& current_version);   // background; compares against the newest release tag
State state();
std::string latest_version();                     // tag of the newest release when known (without the leading v)
std::string message();                            // short status text for the settings panel
void download_and_install();                      // background download, then writes update.bat and exits the game to run it
void shutdown();                                  // joins the background thread; call before process exit
}  // namespace host::updater
