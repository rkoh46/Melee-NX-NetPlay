// The Slippi release this build implements, and the service constants that go with it.
// The version, endpoint and upstream commits come from tools/slippi_pin.json through CMake
// (SLIPPI_PIN_* definitions); nothing else in the tree names them. A Slippi release that only
// changes the game-side codes and files is adopted by tools/update-slippi.py; one that changes
// the host protocol needs a port first (BUILDING.md (Following Slippi releases)).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#if !defined(SLIPPI_PIN_VERSION) || !defined(SLIPPI_PIN_TAG) || !defined(SLIPPI_PIN_MATCHMAKING_URL)
#error "SLIPPI_PIN_VERSION / SLIPPI_PIN_TAG / SLIPPI_PIN_MATCHMAKING_URL must come from tools/slippi_pin.json (see CMakeLists.txt)"
#endif

namespace slippi::version {
inline constexpr const char* kSemver = SLIPPI_PIN_VERSION;                 // reported to matchmaking and in the user agent
inline constexpr const char* kUpstreamTag = SLIPPI_PIN_TAG;                // project-slippi/Ishiiruka tag the port follows
inline constexpr const char* kMatchmakingUrl = SLIPPI_PIN_MATCHMAKING_URL;

// Public STUN servers used to learn the netplay socket's public address, tried in order until two have
// answered (SlippiMatchmaking.h, STUN_SERVERS). Two answers tell a cone NAT from a symmetric one.
struct StunServer { const char* host; uint16_t port; };
inline constexpr StunServer kStunServers[] = {
    {"stun.l.google.com", 19302}, {"stun1.l.google.com", 19302}, {"stun.cloudflare.com", 3478}, {"global.stun.twilio.com", 3478}};
inline constexpr size_t kStunServerCount = sizeof kStunServers / sizeof kStunServers[0];
inline constexpr uint64_t kStunKeepaliveMs = 15000;

// Is Slippi release `theirs` newer than `ours` ("3.7.0" form; anything unparsable compares as not newer)?
bool is_newer(const std::string& theirs, const std::string& ours);

// Launch-time check (slippi_version.cpp): asks GitHub once, on a background thread, for Slippi's latest
// release. Never blocks the caller; offline or failing lookups leave "unknown".
void start_update_check(const std::string& cache_dir);
bool update_check_finished();
// Empty until known. The newest Slippi release seen (from the check, or from the matchmaking service
// refusing this build), only when it is newer than kSemver.
std::string newer_release();
void note_service_requires(const std::string& latest);
// One player-facing sentence when a newer release exists, else empty.
std::string update_notice();
}  // namespace slippi::version
