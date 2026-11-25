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
//   - search (upstream's search box, through the console's software keyboard: every word in the game's line or file name);
//   - the watch queue (upstream's: replays added in the order picked, played one after another);
//   - "Show it as it was played" (launcher.ini/settings.ini replayasplayed 1, upstream's --as-experienced): a replay with a
//     session trace beside it is paced as the online session ran (waits held, rollbacks on the network panel), net_trace_read
//     and its Schedule/Pacer compiled from the fetched tree; the network and timing graph of the picked game's trace
//     (launcher_trace_view.cpp's overview, drawn by the home screen);
//   - "Compress all replays now (.slpz)" and, with compressreplays 1, earlier sessions' .slp compressed at the start (each
//     checked to read back identical before its .slp goes; files under a minute old stay);
//   - deleting moves the replay and its trace to the folder's .trash (upstream uses the Recycle Bin), "Undo the last delete";
//   - the folder (settings.ini replay_dir) and any replay file are chosen on the home screen's SD card browser (mu_home.cpp).
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
// The session trace reader (its Schedule and Pacer) and the launcher's whole-session overview (fetched tree, GPL-2.0-or-later).
#include "../host/net_trace_read.cpp"
#include "../../app/launcher_trace_view.cpp"
#include "../../app/replay_slpz.h"
#include <filesystem>

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
#include <dirent.h>
#include <sstream>
#include <cctype>
#include <array>

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
std::string query;                    // the search (lower case words)
std::vector<std::string> queue;       // the watch queue: paths, in the order picked
std::vector<std::string> queue_playing;   // a queue being played: the ones still to come
std::vector<std::pair<std::string, std::string>> trashed;   // (where it was, where it is in .trash), the newest last
bool as_played = false;               // replayasplayed
std::atomic<bool> compressing{false};
std::atomic<int> compressed_count{0}, compress_total{0};
launcher::trace::Overview trace_view; std::string trace_name;   // the picked game's session trace, read with its stats

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
bool matches(const std::string& name, const Quick& q) {
  if (query.empty()) return true;
  const std::string hay = lower(name + " " + q.line);
  std::istringstream words(query);
  std::string w;
  while (words >> w) if (hay.find(w) == std::string::npos) return false;
  return true;
}
std::string trace_of(const std::string& path) {   // upstream's trace_path: the replay's name with .trace
  std::string p = path;
  if (p.size() > 5 && p.compare(p.size() - 5, 5, ".slpz") == 0) p.resize(p.size() - 5);
  else if (p.size() > 4 && p.compare(p.size() - 4, 4, ".slp") == 0) p.resize(p.size() - 4);
  return p + ".trace";
}

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
  for (const auto& n : names) if (!(hide_short && quick[n].seconds >= 0 && quick[n].seconds < 30) && matches(n, quick[n])) list.push_back(n);
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
      launcher::trace::Overview view = launcher::trace::load(std::filesystem::path(join_path(name)));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      lock.lock();
      if (stats_name == name) {
        trace_view = std::move(view); trace_name = name;
        if (trace_view.present) say("library: session trace of %s: %zu ticks, %zu waits, %zu rollbacks, %.0f s", name.c_str(), trace_view.ticks, trace_view.waits, trace_view.rollbacks, trace_view.seconds);
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
// The page can be read after the run has ended (the settings panel draws over the last frame, the home screen before the
// game): a worker started then is stopped at exit, not left joinable for the static destructors (std::terminate).
struct StopAtExit { ~StopAtExit() { stop_worker(); } } stop_at_exit;

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
  // To the folder's .trash with its session trace (upstream: the Recycle Bin), so "Undo the last delete" brings it back.
  const std::string path = join_path(name), trash = replay_folder() + "/.trash";
  mkdir(trash.c_str(), 0777);
  const bool ok = std::rename(path.c_str(), (trash + "/" + name).c_str()) == 0;
  if (ok) {
    const std::string tr = trace_of(path), tr_to = trace_of(trash + "/" + name);
    if (std::rename(tr.c_str(), tr_to.c_str()) != 0) {}   // no trace: nothing to move
    std::lock_guard<std::mutex> lock(lib_mutex);
    trashed.push_back({path, trash + "/" + name});
    queue.erase(std::remove(queue.begin(), queue.end(), path), queue.end());
  }
  say("library: %s %s", ok ? "moved to .trash:" : "COULD NOT DELETE", path.c_str());
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    last_note = ok ? "Deleted " + name + " (Undo the last delete brings it back)" : "Could not delete " + name;
    if (ok) { shown.erase(std::remove(shown.begin(), shown.end(), name), shown.end()); quick.erase(name); picked = shown.empty() ? std::string() : shown.front(); }
  }
  poke(true);
}

void undo_delete() {
  std::pair<std::string, std::string> last;
  { std::lock_guard<std::mutex> lock(lib_mutex); if (trashed.empty()) { last_note = "Nothing to undo"; return; } last = trashed.back(); trashed.pop_back(); }
  const bool ok = std::rename(last.second.c_str(), last.first.c_str()) == 0;
  if (ok) std::rename(trace_of(last.second).c_str(), trace_of(last.first).c_str());
  say("library: undo delete: %s %s", ok ? "back in place:" : "COULD NOT RESTORE", last.first.c_str());
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    const std::string name = last.first.substr(last.first.find_last_of('/') + 1);
    last_note = ok ? "Restored " + name : "Could not restore " + name;
    if (ok) picked = name;
  }
  poke(true);
}
std::string undo_value() { std::lock_guard<std::mutex> lock(lib_mutex); return trashed.empty() ? std::string() : std::to_string(trashed.size()) + " in .trash"; }

// Search: the console's keyboard on a worker thread (it takes the screen until OK); the words arrive at a later frame.
std::atomic<bool> search_busy{false};
void search_press() {
  if (search_busy.exchange(true)) return;
  std::string start; { std::lock_guard<std::mutex> lock(lib_mutex); start = query; }
  std::thread([start] {
    std::string text = start;
    if (software_keyboard("Search replays", "Characters, a stage, or a file name.", text)) {
      { std::lock_guard<std::mutex> lock(lib_mutex); query = lower(text); }
      say("library: search \"%s\"", text.c_str());
      poke(true);
    }
    search_busy = false;
  }).detach();
}
std::string search_value() { std::lock_guard<std::mutex> lock(lib_mutex); return query.empty() ? std::string() : "\"" + query + "\""; }
void search_clear() { { std::lock_guard<std::mutex> lock(lib_mutex); query.clear(); } say("library: search cleared"); poke(true); }

// The watch queue.
void queue_toggle() {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (picked.empty()) return;
  const std::string path = join_path(picked);
  const auto it = std::find(queue.begin(), queue.end(), path);
  const bool add = it == queue.end();
  if (add) queue.push_back(path); else queue.erase(it);
  say("library: watch queue %zu (%s %s)", queue.size(), add ? "added" : "removed", picked.c_str());
}
std::string queue_value() {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (picked.empty()) return "";
  const auto it = std::find(queue.begin(), queue.end(), join_path(picked));
  return it == queue.end() ? std::string("Not queued") : "Number " + std::to_string(it - queue.begin() + 1) + " in the queue";
}
void queue_watch() {
  std::string first;
  {
    std::lock_guard<std::mutex> lock(lib_mutex);
    if (queue.empty()) return;
    queue_playing.assign(queue.begin() + 1, queue.end());
    first = queue.front();
  }
  say("library: watching the queue (%zu replays)", queue_playing.size() + 1);
  replay_watch(first);
}
std::string queue_watch_value() { std::lock_guard<std::mutex> lock(lib_mutex); return queue.empty() ? std::string("(empty)") : std::to_string(queue.size()) + " replays"; }

// "Compress all replays now (.slpz)" (upstream's compress_replay_folder): the finished .slp a minute old or more, on a worker.
void compress_folder(const char* why) {
  if (compressing.exchange(true)) return;
  std::thread([why] {
    const std::string folder = replay_folder();
    const time_t cutoff = std::time(nullptr) - 60;
    std::vector<std::string> todo;
    if (DIR* d = opendir(folder.c_str())) {
      while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() < 5 || n[0] == '.' || n.compare(n.size() - 4, 4, ".slp") != 0) continue;
        struct stat st{};
        if (stat((folder + "/" + n).c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime < cutoff) todo.push_back(folder + "/" + n);
      }
      closedir(d);
    }
    compress_total = int(todo.size()); compressed_count = 0;
    int done = 0;
    for (const auto& p : todo) { if (!slpz::compress_file(p).empty()) ++done; compressed_count = compressed_count + 1; }
    say("library: %s: %d of %zu replays compressed to .slpz", why, done, todo.size());
    { std::lock_guard<std::mutex> lock(lib_mutex); last_note = "Compressed " + std::to_string(done) + " of " + std::to_string(todo.size()) + " replays"; }
    compressing = false;
    poke(true);
  }).detach();
}
std::string compress_value() {
  if (compressing) return "Compressing " + std::to_string(compressed_count.load()) + " of " + std::to_string(compress_total.load());
  return "";
}

bool setting(const std::string& key, const std::string& value) {
  if (key == "replayasplayed") { as_played = value == "1"; return true; }
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
  // compressreplays 1: earlier sessions' .slp become .slpz at the start (upstream main.cpp), never this session's.
  static bool compressed_at_start = false;
  if (host && !compressed_at_start && replays_compress_on()) { compressed_at_start = true; compress_folder("earlier sessions' replays at the start"); }
  static bool tested = false;   // once per process: the test's Watch it starts the game again in this process
  if (host && !tested && feature_test("replay-library")) { tested = true; std::thread(library_test).detach(); }
}
void run_end() { stop_worker(); }
// "As it was played" (upstream replay_control.cpp load_trace / show_trace): the replay's session trace laid out by replay frame,
// its records shown on the network panel and the picture held where the session waited.
struct AsPlayed { bool checked = false, on = false; std::string path; net_trace::Schedule schedule; net_trace::Pacer pacer; int32_t last = INT32_MIN;
                  std::vector<net_trace::Pacer::Step> steps; };
AsPlayed as;
}  // namespace

void library_replay_opened(const std::string& path) { as = AsPlayed{}; as.path = path; }
void library_replay_frame(int32_t frame, bool paced) {
  if (!as_played || as.path.empty()) return;
  if (!as.checked) {
    as.checked = true;
    net_trace::Trace trace; std::string error;
    const std::string tp = trace_of(as.path);
    if (!net_trace::read_trace(tp, &trace, &error) || trace.records.empty()) {
      say("replay: no session trace to show (%s: %s); playing the replay normally", tp.c_str(), error.empty() ? "it has no rows" : error.c_str());
      return;
    }
    int32_t offset = net_trace::kOnlineToReplay;
    if (const char* v = std::getenv("MELEE_TRACE_FRAME_OFFSET")) offset = int32_t(std::strtol(v, nullptr, 10));
    as.schedule.build(trace, offset);
    if (as.schedule.empty() || std::abs(as.schedule.first_frame() - frame) > 5 * 60 + 130) {
      say("replay: the session trace %s does not match this replay (trace frames from %d, replay at %d); playing the replay normally",
          tp.c_str(), as.schedule.empty() ? 0 : as.schedule.first_frame(), frame);
      return;
    }
    as.pacer.set(&as.schedule);
    as.on = true;
    say("replay: showing the session as it was played (%zu trace rows)", as.schedule.size());
  }
  if (!as.on) return;
  if (as.last == INT32_MIN || frame != as.last + 1) as.pacer.reset();
  as.last = frame;
  double real = 0.0;
  as.pacer.next(frame, &as.steps, &real);
  double held = 0.0;
  for (const auto& step : as.steps) {
    if (paced && step.wait > 0.0) { usleep(useconds_t(step.wait * 1e6)); held += step.wait; }
    net_trace_show_replay_record(&as.schedule.record(step.record));
  }
  static int notes = 0;
  if (paced && real >= 0.25 && notes++ < 20) say("replay: as played: the game stood still for %.2f s at frame %d (held %.2f s)", real, frame, held);
}
std::string library_queue_next() {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (queue_playing.empty()) return {};
  const std::string next = queue_playing.front();
  queue_playing.erase(queue_playing.begin());
  return next;
}
// The home screen's graph: the picked game's session trace in equal spans of time (interval, work, a wait, a rollback, a mark).
bool library_trace(std::vector<std::array<float, 5>>* bins, std::string* summary) {
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (trace_name != picked || !trace_view.present) return false;
  bins->clear();
  for (const auto& b : trace_view.bins) bins->push_back({float(b.interval_ms), b.work_ms, float(b.waits), float(b.rollbacks), float(b.flags)});
  char line[160];
  std::snprintf(line, sizeof line, "%.0f s, %zu waits, %zu rollbacks, %zu marks", trace_view.seconds, trace_view.waits, trace_view.rollbacks,
                trace_view.marks[0] + trace_view.marks[1] + trace_view.marks[2] + trace_view.marks[3]);
  *summary = line;
  return true;
}
void library_set_query(const std::string& text) { { std::lock_guard<std::mutex> lock(lib_mutex); query = lower(text); } poke(true); }
void library_search() { search_press(); }
std::string library_query() { return search_value(); }
void library_queue_toggle() { queue_toggle(); }
std::string library_queue_state() { return queue_value(); }
void library_queue_watch() { queue_watch(); }
std::string library_queue_start() {   // the home screen: the game starts with the first; the rest follow
  std::lock_guard<std::mutex> lock(lib_mutex);
  if (queue.empty()) return {};
  queue_playing.assign(queue.begin() + 1, queue.end());
  say("library: watching the queue (%zu replays)", queue.size());
  return queue.front();
}
size_t library_queue_size() { std::lock_guard<std::mutex> lock(lib_mutex); return queue.size(); }
void library_compress() { compress_folder("Compress all replays now"); }
std::string library_compress_state() { return compress_value(); }
void library_undo_delete() { undo_delete(); }
size_t library_trash_size() { std::lock_guard<std::mutex> lock(lib_mutex); return trashed.size(); }
bool library_as_played() { return as_played; }
void library_set_as_played(bool on) { as_played = on; settings_write_key("replayasplayed", on ? "1" : "0"); say("library: show as it was played %s", on ? "on" : "off"); }
std::string library_note() { std::lock_guard<std::mutex> lock(lib_mutex); return last_note; }

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
  add_menu_entry({"Replays", "Undo the last delete", undo_value, nullptr, undo_delete});
  add_menu_entry({"Replays", "Search replays", search_value, nullptr, search_press});
  add_menu_entry({"Replays", "Clear the search", nullptr, nullptr, search_clear});
  add_menu_entry({"Replays", "Add to the watch queue / remove", queue_value, nullptr, queue_toggle});
  add_menu_entry({"Replays", "Watch the queue", queue_watch_value, nullptr, queue_watch});
  add_menu_entry({"Replays", "Show it as it was played (uses the .trace file)", [] { return std::string(as_played ? "On" : "Off"); },
                  [](int) { as_played = !as_played; }, nullptr, [](fe::features::MenuEntry::Write w) { w("replayasplayed", as_played ? "1" : "0"); }});
  add_menu_entry({"Replays", "Compress all replays now (.slpz)", compress_value, nullptr, [] { compress_folder("Compress all replays now"); }});
}
}  // namespace fe::features
