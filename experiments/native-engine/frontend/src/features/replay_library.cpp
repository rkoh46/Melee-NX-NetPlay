// The replay library and per-game stats on the in-game menu's Replays page, as melee-unlocked's launcher has them (its
// Replay Viewer: port/app/launcher_replays.inl, the Linux launcher's list in port/linux/launcher/launcher_linux.cpp, the
// stats in port/app/launcher_replay_data.cpp and launcher_replay_stats.inl). The data code is compiled from the fetched
// tree (GPL-2.0-or-later); the page is this file's.
//   - the replays in the Replays folder (.slp and .slpz), sorted (replay_sort: 0 most recent first, 1 oldest first,
//     2 longest first, 3 shortest first) and optionally without games under 30 seconds (replay_hide_short 1, upstream's
//     "Hide short games"); one shown at a time (left / right), with its date, characters, stage and length;
//   - Watch it (play mode starts the game again with it, replays.cpp);
//   - its Slippi-Launcher-style stats, read on a worker thread: kills, damage, openings per kill, neutral wins, counter
//     hits, conversions, L-cancels, inputs per minute, rolls and dodges, wavedashes, ledge grabs (one value per player);
//   - Delete this replay: A twice within five seconds removes the file.
// Not here (no keyboard or mouse in the menu): upstream's search box, the watch queue, the folder chooser (settings.ini
// replay_dir), and the network and timing graph (no .trace files are written on the Switch).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"

#include <ctime>
// launcher_replay_data.cpp is Windows code in two places (display_date): the C library's names for them here.
namespace {
time_t days_from_civil_utc(const std::tm& t) {     // timegm, which newlib does not have
  const int y = t.tm_year + 1900 - (t.tm_mon < 2), m = t.tm_mon + 1, d = t.tm_mday;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = (long long)era * 146097 + (long long)doe - 719468;
  return time_t(days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
}
}  // namespace
static inline time_t _mkgmtime(std::tm* t) { return days_from_civil_utc(*t); }
static inline int localtime_s(std::tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 1; }
// melee-unlocked (fetched tree), port/app, named from its port/runtime/abi folder (on the include path). Its slpz calls
// are the ones replays.cpp compiles.
#include "../../app/launcher_replay_data.cpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

// The app's in-game menu (app mode only; weak: the other programs have none), for the test's screenshots.
namespace host { __attribute__((weak)) void menu_open_extra(const std::string& title); __attribute__((weak)) void menu_close(); }

namespace fe::features {
namespace {
namespace lr = launcher::replay;

int sort_mode = 0;                    // replay_sort
bool hide_short = false;              // replay_hide_short
const char* const kSorts[4] = {"Most recent first", "Oldest first", "Longest first", "Shortest first"};

struct Quick {                        // what the list needs of a file (its first 4 KiB and its metadata)
  long long when = 0, size = 0, mtime = 0;
  int seconds = -1;
  bool valid = false;
  std::string line;                   // "Fox vs Falco, Onett, 0:50"
};
std::mutex lib_mutex;
std::condition_variable lib_cv;
std::map<std::string, Quick> quick;   // by file name
std::vector<std::string> shown;       // the list as the page shows it
std::string picked;                   // the file name shown on the page
std::string stats_name;               // the file the stats are for
lr::Info stats;
bool stats_ready = false, stats_failed = false;
bool listed_once = false;
std::chrono::steady_clock::time_point listed_at;
bool work = false, stop = false;
std::thread worker;
std::string delete_armed; std::chrono::steady_clock::time_point delete_at;
std::string last_note;                // the last thing the page did (Deleted ...)

std::string join_path(const std::string& name) { return replay_folder() + "/" + name; }
std::string clock_text(int seconds) {
  char b[16];
  std::snprintf(b, sizeof b, "%d:%02d", std::max(0, seconds) / 60, std::max(0, seconds) % 60);
  return b;
}

Quick read_quick(const std::string& name, long long size, long long mtime) {
  Quick q; q.size = size; q.mtime = mtime;
  const std::filesystem::path path = join_path(name);
  const lr::Info info = lr::inspect(path, false);
  q.valid = info.valid;
  q.seconds = info.last_frame > -123 ? (info.last_frame + 123) / 60 : -1;
  q.when = lr::game_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(std::time(nullptr) - mtime));
  std::string players;
  for (size_t p = 0; p < info.players.size(); ++p) {
    if (p) players += " vs ";
    players += lr::character_name(info.players[p].character);
    if (int(p) == info.winner) players += "*";
  }
  q.line = info.valid ? players + ", " + info.stage + (q.seconds >= 0 ? ", " + clock_text(q.seconds) : std::string()) : "not a replay this build reads";
  return q;
}

// Worker thread: the folder listed, new files read, the list sorted and filtered, then the shown file's stats.
void refresh_list_locked(std::unique_lock<std::mutex>& lock) {
  lock.unlock();
  const std::vector<std::string> names = replays_list();   // newest first by file time
  std::map<std::string, std::pair<long long, long long>> stat_of;
  for (const auto& n : names) {
    struct stat st{};
    if (stat(join_path(n).c_str(), &st) == 0) stat_of[n] = {(long long)st.st_size, (long long)st.st_mtime};
  }
  std::map<std::string, Quick> fresh;
  {
    std::lock_guard<std::mutex> g(lib_mutex);
    for (const auto& n : names) {
      const auto it = quick.find(n);
      if (it != quick.end() && it->second.size == stat_of[n].first && it->second.mtime == stat_of[n].second) fresh[n] = it->second;
    }
  }
  for (const auto& n : names) if (!fresh.count(n)) fresh[n] = read_quick(n, stat_of[n].first, stat_of[n].second);
  lock.lock();
  quick = std::move(fresh);
  std::vector<std::string> list;
  for (const auto& n : names) if (!(hide_short && quick[n].seconds >= 0 && quick[n].seconds < 30)) list.push_back(n);
  std::stable_sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) {
    const Quick &qa = quick[a], &qb = quick[b];
    switch (sort_mode) {
      case 1: return qa.when < qb.when;
      case 2: return qa.seconds > qb.seconds;
      case 3: return qa.seconds < qb.seconds;
      default: return qa.when > qb.when;
    }
  });
  shown = std::move(list);
  if (std::find(shown.begin(), shown.end(), picked) == shown.end()) picked = shown.empty() ? std::string() : shown.front();
  listed_once = true;
  listed_at = std::chrono::steady_clock::now();
}
void worker_main() {
  std::unique_lock<std::mutex> lock(lib_mutex);
  while (!stop) {
    lib_cv.wait(lock, [] { return stop || work; });
    if (stop) break;
    work = false;
    refresh_list_locked(lock);
    if (!picked.empty() && picked != stats_name) {
      const std::string name = picked;
      stats_ready = false; stats_failed = false; stats_name = name;
      lock.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      lr::Info info = lr::inspect(join_path(name), true);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      lock.lock();
      if (stats_name == name) {
        stats = std::move(info);
        stats_ready = stats.valid && stats.stats_loaded;
        stats_failed = !stats_ready;
        say("library: stats of %s read in %.0f ms (%zu players)", name.c_str(), ms, stats.players.size());
      }
    }
  }
}
// Menu thread (or the test): ask the worker to look again; the list is looked at again at most every 3 seconds.
void poke(bool force) {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (!worker.joinable()) { stop = false; worker = std::thread(worker_main); }
  if (force || !listed_once || std::chrono::steady_clock::now() - listed_at > std::chrono::seconds(3) || (!picked.empty() && picked != stats_name)) {
    work = true;
    lib_cv.notify_all();
  }
}
void stop_worker() {
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    stop = true;
    lib_cv.notify_all();
  }
  if (worker.joinable()) worker.join();
}

// ---- the page's values (menu thread) ----
std::string replay_value() {
  poke(false);
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (!listed_once) return "reading the folder...";
  if (shown.empty()) return quick.empty() ? "(none in " + replay_folder() + ")" : "(all shorter than 30 s)";
  return quick[picked].line;
}
void replay_change(int d) {
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    if (shown.empty()) return;
    size_t at = size_t(std::find(shown.begin(), shown.end(), picked) - shown.begin());
    if (at >= shown.size()) at = 0;
    at = (at + shown.size() + size_t(d < 0 ? -1 : 1)) % shown.size();
    picked = shown[at];
    delete_armed.clear();
  }
  poke(false);
}
std::string date_value() {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (picked.empty()) return "";
  const size_t at = size_t(std::find(shown.begin(), shown.end(), picked) - shown.begin());
  const std::string count = "  (" + std::to_string(at + 1) + " of " + std::to_string(shown.size()) + ")";
  if (stats_name == picked && stats_ready) return lr::display_date(stats) + count;
  return picked + count;
}
void watch_press() {
  std::string name;
  { std::lock_guard<std::mutex> lock(lib_mutex); name = picked; }
  if (!name.empty()) replay_watch(join_path(name));
}

// One row of stats: each player's value, " | " between them.
template <int Row> std::string stat_value() {
  poke(false);
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (picked.empty()) return "";
  if (stats_name != picked || (!stats_ready && !stats_failed)) return "reading...";
  if (stats_failed) return "-";
  const double minutes = std::max(1.0, double(stats.last_frame + 123)) / 3600.0;
  std::string out;
  for (size_t i = 0; i < stats.players.size(); ++i) {
    const lr::Player& p = stats.players[i];
    char b[96] = "";
    switch (Row) {
      case 0: std::snprintf(b, sizeof b, "P%d %s%s", p.port, lr::character_name(p.character), int(i) == stats.winner ? " (won)" : ""); break;
      case 1: std::snprintf(b, sizeof b, "%d", p.kills); break;
      case 2: std::snprintf(b, sizeof b, "%.0f%%", p.damage_done); break;
      case 3: if (p.kills) std::snprintf(b, sizeof b, "%.1f", double(p.openings) / p.kills); else std::snprintf(b, sizeof b, "-"); break;
      case 4: std::snprintf(b, sizeof b, "%d", p.neutral_wins); break;
      case 5: std::snprintf(b, sizeof b, "%d", p.counter_hits); break;
      case 6: std::snprintf(b, sizeof b, "%d/%d", p.successful_conversions, p.openings); break;
      case 7: if (!stats.l_cancel_available) std::snprintf(b, sizeof b, "-");
              else if (p.l_success + p.l_fail) std::snprintf(b, sizeof b, "%d/%d (%d%%)", p.l_success, p.l_success + p.l_fail, 100 * p.l_success / (p.l_success + p.l_fail));
              else std::snprintf(b, sizeof b, "0/0");
              break;
      case 8: std::snprintf(b, sizeof b, "%.0f", double(p.inputs) / minutes); break;
      case 9: std::snprintf(b, sizeof b, "%d %d %d", p.rolls, p.spot_dodges, p.air_dodges); break;
      case 10: std::snprintf(b, sizeof b, "%d %d %d", p.wavedashes, p.wavelands, p.dash_dances); break;
      case 11: std::snprintf(b, sizeof b, "%d", p.ledge_grabs); break;
      default: break;
    }
    if (i) out += " | ";
    out += b;
  }
  return out;
}

std::string delete_value() {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (!delete_armed.empty() && delete_armed == picked && std::chrono::steady_clock::now() - delete_at < std::chrono::seconds(5)) return "Press A again to delete it";
  return last_note;
}
void delete_press() {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    if (picked.empty()) return;
    if (delete_armed != picked || std::chrono::steady_clock::now() - delete_at >= std::chrono::seconds(5)) {
      delete_armed = picked; delete_at = std::chrono::steady_clock::now();
      say("library: delete %s? (A again within 5 s)", picked.c_str());
      return;
    }
    name = picked; delete_armed.clear();
  }
  const std::string path = join_path(name);
  const bool ok = std::remove(path.c_str()) == 0;
  say("library: %s %s", ok ? "deleted" : "COULD NOT DELETE", path.c_str());
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    last_note = ok ? "Deleted " + name : "Could not delete " + name;
    if (ok) { shown.erase(std::remove(shown.begin(), shown.end(), name), shown.end()); quick.erase(name); picked = shown.empty() ? std::string() : shown.front(); }
  }
  poke(true);
}

bool setting(const std::string& key, const std::string& value) {
  if (key == "replay_sort") sort_mode = std::clamp(std::atoi(value.c_str()), 0, 3);
  else if (key == "replay_hide_short") hide_short = value == "1";
  else return false;
  return true;
}

// --feature-test replay-library: the page driven without the menu (the values the rows show, then a delete), logged.
void library_test() {
  const auto wait_for = [](auto ready) {
    for (int i = 0; i < 400; ++i) { { std::lock_guard<std::mutex> lock(lib_mutex); if (ready()) return true; } usleep(25000); }
    return false;
  };
  poke(true);
  wait_for([] { return listed_once && (picked.empty() || (stats_name == picked && (stats_ready || stats_failed))); });
  static const char* const kRows[] = {"Players", "Kills", "Damage done", "Openings per kill", "Neutral wins", "Counter hits",
                                      "Successful conversions", "L-cancels", "Inputs per minute", "Rolls / spot / air dodges",
                                      "Wavedashes / wavelands / dash dances", "Ledge grabs"};
  std::string (*const rows[])() = {stat_value<0>, stat_value<1>, stat_value<2>, stat_value<3>, stat_value<4>, stat_value<5>,
                                   stat_value<6>, stat_value<7>, stat_value<8>, stat_value<9>, stat_value<10>, stat_value<11>};
  // In the app, the page itself is shown while the test goes through it (screenshots).
  const bool menu = options.app && &host::menu_open_extra && &host::menu_close;
  if (menu) { host::menu_open_extra("Replays"); say("library: the in-game menu opened on the Replays page"); }
  for (int pass = 0; pass < 2; ++pass) {
    if (menu) usleep(3000000);
    say("library: Replay: %s", replay_value().c_str());
    say("library: Date: %s", date_value().c_str());
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; ++r) say("library: %s: %s", kRows[r], rows[r]().c_str());
    replay_change(1);
    wait_for([] { return stats_name == picked && (stats_ready || stats_failed); });
  }
  // Delete the shown one (the test's copy): the first A arms it, the second deletes.
  say("library: test deletes the shown replay");
  delete_press();
  say("library: Delete this replay: %s", delete_value().c_str());
  delete_press();
  wait_for([] { return std::chrono::steady_clock::now() - listed_at < std::chrono::seconds(1); });
  say("library: Replay: %s; Delete this replay: %s", replay_value().c_str(), delete_value().c_str());
  const auto left = replays_list();
  say("library: %zu replay%s left in the folder", left.size(), left.size() == 1 ? "" : "s");
  if (menu) {
    usleep(3000000);
    say("library: test presses Watch it");
    watch_press();
    host::menu_close();
  }
}
void tables(MuHostApi* host, MuGameApi*) {
  static bool tested = false;   // once per process: the test's Watch it starts the game again in this process
  if (host && !tested && feature_test("replay-library")) { tested = true; std::thread(library_test).detach(); }
}
void run_end() { stop_worker(); }
}  // namespace

std::vector<std::string> library_list() {
  std::lock_guard<std::mutex> lock(lib_mutex);
  return shown;
}

void register_replay_library() {
  add_setting(setting);
  add_tables(tables);
  add_run_end(run_end);
  add_menu_entry({"Replays", "Replay", replay_value, replay_change, nullptr});
  add_menu_entry({"Replays", "Recorded", date_value, nullptr, nullptr});
  add_menu_entry({"Replays", "Watch it", nullptr, nullptr, watch_press});
  add_menu_entry({"Replays", "Sort", [] { return std::string(kSorts[sort_mode]); },
                  [](int d) { sort_mode = (sort_mode + 4 + (d < 0 ? -1 : 1)) % 4; poke(true); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("replay_sort", std::to_string(sort_mode)); }});
  add_menu_entry({"Replays", "Hide short games (under 30 s)", [] { return std::string(hide_short ? "On" : "Off"); },
                  [](int) { hide_short = !hide_short; poke(true); }, nullptr,
                  [](fe::features::MenuEntry::Write w) { w("replay_hide_short", hide_short ? "1" : "0"); }});
  add_menu_entry({"Replays", "Players", stat_value<0>, nullptr, nullptr});
  add_menu_entry({"Replays", "Kills", stat_value<1>, nullptr, nullptr});
  add_menu_entry({"Replays", "Damage done", stat_value<2>, nullptr, nullptr});
  add_menu_entry({"Replays", "Openings per kill", stat_value<3>, nullptr, nullptr});
  add_menu_entry({"Replays", "Neutral wins", stat_value<4>, nullptr, nullptr});
  add_menu_entry({"Replays", "Counter hits", stat_value<5>, nullptr, nullptr});
  add_menu_entry({"Replays", "Conversions (of openings)", stat_value<6>, nullptr, nullptr});
  add_menu_entry({"Replays", "L-cancels", stat_value<7>, nullptr, nullptr});
  add_menu_entry({"Replays", "Inputs per minute", stat_value<8>, nullptr, nullptr});
  add_menu_entry({"Replays", "Rolls, spot, air dodges", stat_value<9>, nullptr, nullptr});
  add_menu_entry({"Replays", "Wavedash, waveland, dash dance", stat_value<10>, nullptr, nullptr});
  add_menu_entry({"Replays", "Ledge grabs", stat_value<11>, nullptr, nullptr});
  add_menu_entry({"Replays", "Delete this replay", delete_value, nullptr, delete_press});
}
}  // namespace fe::features
