// The host side of the engine's own online play, for any program that hosts the game with
// melee-unlocked's host services: the runner (runner_main.cpp) or a frontend with its own main.
//
// What it does: answers the game's slippi_command (state saves and loads on the hot-page store,
// the engine's rollback self-test, everything else through this repository's Slippi 3.7.0 netplay,
// netplay_bridge.*), brackets re-simulation, asks for the match, and reports rollbacks, frame times
// and a final-state check at the end. Pads are whatever the host's pad_read gives, unless generated
// test inputs are asked for with --net-inputs.
//
// Calls, in order:
//   net_host::Options o;  for each argument: net_host::parse_option(arg, next, &o)
//   if (net_host::wanted(o)):
//     o.out_dir, o.iso_path, o.embedded_image = ...;  net_host::start(o, log)      before the host is initialised
//     in the host's tables hook, with the host table:  net_host::install(host)
//     use linux_host::Config::pace = true and no input script
//     every retrace, after the frame:                  net_host::on_retrace(row, length, fighting, work_ms)
//     after the game stops, before host shutdown:      net_host::stop_tracking()
//     then:                                            net_host::report();  net_host::shutdown()
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mu_host.h"
#include "native_savestate.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace net_host {
struct Options {
  std::string net;                  // --net INDEX:LOCAL_PORT:REMOTE_IP:REMOTE_PORT (two instances peer directly)
  std::string online;               // --online unranked|direct:CODE|teams|party (services builds only)
  std::string user_dir;             // --user-dir: where user.json is (default: the app's folder on the console)
  bool fake_user = false;           // --fake-user: tests against a local mock service only
  int retry_seconds = 0;            // --net-retry: with --net, keep asking for the match this long when the peer is not there yet
  int delay = 2;                    // --net-delay: Slippi input delay, frames
  bool delay_auto = false;          // --net-delay auto: the app's automatic delay (2..5 frames from the ping at each game's start)
  int character = -1;               // --net-character: external character id (-1: 2, or 9 for player 1 of --net)
  uint32_t inputs = 0;              // --net-inputs: seed of the game's generated test inputs (0 with --online: the host's pads)
  bool no_check = false;            // --net-no-check: do not collect the game's per-frame state checksums
  bool engine_store = false;        // --net-store engine: melee-unlocked's own snapshot engine instead of the hot-page store
  std::string selftest;             // --selftest EVERY:DEPTH: the engine's own rollback self-test
  bool fixed_rate = false;          // --net-fixed-rate: tests: the time sync's frame rate is logged, not applied (the 0.4.2 behaviour)
  // Exchanged with another Melee-NX-NetPlay build (patches/net-0004), and the Unranked skip list; play mode fills them from
  // settings.ini (directpause, portcolour1/2, shield_color_online, blockcode1..3).
  bool direct_pause = true;         // --net-direct-pause 0|1
  int port_colour[2] = {-1, -1};    // --net-port-colour FIRST[,SECOND] (0 red, 1 blue, 2 yellow, 3 green)
  uint32_t shield_online = 0;       // --net-shield RRGGBB
  std::string skip[3];              // --net-skip CODE (up to three)
  bool as_slippi = false;           // --net-as-slippi: tests: no build message, as Slippi Dolphin
  double clock_skew = 0;            // --net-clock-skew PERCENT: tests: this instance's frame clock runs that much fast (negative: slow)
  // Filled in by the caller, not by arguments:
  std::string out_dir;              // where the made-up user of --net and digest-net-confirmed.csv go
  std::string iso_path;             // the disc image (services: the game report's disc hash)
  bool embedded_image = true;       // the game image is the one embedded in the program (its snapexcl list applies)
};
using Log = void (*)(const char* format, ...);

// True when `arg` was a netplay option (its value, if any, taken with next()). --track-remap is one of them.
bool parse_option(const std::string& arg, const std::function<std::string()>& next, Options* options);
// The two usage lines for those options.
void usage(Log log);
bool wanted(const Options& options);
// Starts the netplay device. 0 on success, otherwise the exit code to end with (the reason is logged).
int start(const Options& options, Log log);
bool active();
// Puts the handlers into the host's table (slippi_command, online_test_match, resim_phase, log, and
// no-op replay sinks where the host has none). Call from the tables hook before the game sees the table.
void install(MuHostApi* host);
// Once per retrace after the frame: the digest row just produced (ending in a newline), whether
// fighters were simulated in it, and the frame's wall time in ms from the end of the previous call to
// the start of this one (the host's pacing sleep is taken out here).
void on_retrace(const char* row, size_t length, bool fighting, double frame_ms);
// Time spent writing log lines (console, nxlink, SD card), so a slow frame can be told from a slow log.
// A host that logs wraps each write in a LogTime; nested ones count once.
void add_log_time(uint64_t nanoseconds, bool outermost);
struct LogTime {
  LogTime(); ~LogTime();
  uint64_t start;
};
// Makes the tracked pages writable again; before linux_host::shutdown unmaps them.
void stop_tracking();
// The end-of-run lines, and digest-net-confirmed.csv in out_dir.
void report();
void shutdown();
// Slippi's time sync changes the frame rate (pacing is the host's, patches/0004); a program with its own sound output
// gets each new speed here too (any thread; 1.0 normal, about 0.995..1.01 in a match).
void set_speed_hook(void (*hook)(double speed));
// The replay of the game just reported, when Slippi's game report asks for it to be uploaded (the reporter's thread; may wait for
// the program's replay writer). Without a hook nothing is uploaded.
void set_replay_path_hook(std::string (*hook)());
// State loads of online rollbacks so far (any thread may read it; the simulation thread counts).
uint64_t rollbacks();

// What a rollback snapshot covers right now: the game's ranges minus its exclusions, minus the
// sound-driver ranges of the embedded image's build (melee_game.snapexcl). Also used by the
// runner's other rollback modes.
std::vector<native_savestate::Span> snapshot_spans(bool embedded_image);
}
