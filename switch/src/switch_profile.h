// Sampling profiler for the simulation thread. See switch_profile.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <switch.h>

// Starts sampling `target` at about `hz` samples a second. Returns false when the kernel refuses
// (thread context reads need the thread paused, which hbloader allows for the process's own threads).
bool switch_profile_start(Thread* target, unsigned hz, Thread* second = nullptr);   // `second` is sampled alternately (pc1/lr1 tables)
// Stops sampling and writes the hot addresses to `path`, as offsets from the NRO's load base so
// `aarch64-none-elf-addr2line -f -e melee-nx-netplay.elf <offset>` names them on the host.
void switch_profile_stop(const char* path);
