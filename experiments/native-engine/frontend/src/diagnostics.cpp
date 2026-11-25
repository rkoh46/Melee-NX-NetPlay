// melee-unlocked's crash report on the Switch (its port/app/launcher_crash_zip.h, launcher_crash_privacy.h and
// launcher_crash_text.h, compiled from the fetched tree, GPL-2.0-or-later): the home screen's "Save crash report" writes
// melee_crash_report.zip and its readable companion melee_crash_report.md to Logs/ on the SD card, every line scrubbed by
// upstream's rules first (connect codes, IP and e-mail addresses, user and computer names, the Slippi name of user.json, full
// file locations; a line that still shows a location is dropped). Nothing is sent anywhere: upstream posts the zip to its
// own relay (a Cloudflare worker, tools/crash_relay), which this app does not contact (FEATURES.md says what an upload needs).
//
// What goes in, by upstream's names: melee_port_crash.txt (Logs/crash.txt: the signal and the time, written by the handler
// below off the console; the console's own faults go to the system's error screen), melee_port.log (Logs/latest.log, with
// host.log after it), melee_port.prev.log (Logs/previous.log, the session before), session.trace (the marked rows of the last
// day's session traces in the replay folder).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "mu_ui.h"

#include "launcher_crash_text.h"   // fetched tree, port/app (with launcher_crash_zip.h, launcher_crash_privacy.h)

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fe::features { __attribute__((weak)) std::string replay_folder(); }

namespace fe {
namespace {
char g_crash_path[512] = "";
void on_crash(int sig) {
  // Async-signal-safe: open, write, close; then the default action.
  if (g_crash_path[0]) {
    const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      char line[160];
      const char* name = sig == SIGSEGV ? "SIGSEGV (bad memory access)" : sig == SIGABRT ? "SIGABRT (abort)" : sig == SIGBUS ? "SIGBUS" :
                         sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : "a signal";
      int n = std::snprintf(line, sizeof line, "Melee-NX-NetPlay %s stopped: %s\nunix time %ld\n", MU_APP_VERSION, name, long(std::time(nullptr)));
      if (n > 0 && ::write(fd, line, size_t(n)) < 0) {}
      ::close(fd);
    }
  }
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
std::string read_all(const std::string& path) { std::ifstream in(path, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
std::string user_field(const std::string& text, const char* key) {   // the Slippi name and code: only to remove them
  const std::string k = std::string("\"") + key + "\"";
  const size_t at = text.find(k);
  if (at == std::string::npos) return {};
  const size_t q1 = text.find('"', text.find(':', at + k.size()) + 1);
  const size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  return q1 == std::string::npos || q2 == std::string::npos ? std::string() : text.substr(q1 + 1, q2 - q1 - 1);
}
}  // namespace

void diagnostics_start() {
  const std::string logs = options.base_dir + "/Logs";
  std::snprintf(g_crash_path, sizeof g_crash_path, "%s/crash.txt", logs.c_str());
  for (int sig : {SIGABRT, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, on_crash);
  // SIGSEGV: the runner's dirty-page tracker installs its own and passes faults it does not own to this one.
  std::signal(SIGSEGV, on_crash);
}

namespace mu_ui {
bool crash_report_available() {
  const std::string logs = options.base_dir + "/Logs";
  for (const char* f : {"/crash.txt", "/previous.log", "/latest.log"})
    if (FILE* p = std::fopen((logs + f).c_str(), "rb")) { std::fclose(p); return true; }
  return false;
}
bool crash_report_save(std::string* where, std::string* error) {
  namespace lc = launcher::crash;
  const std::string logs = options.base_dir + "/Logs";
  std::vector<lc::File> files;
  // Personal names to remove besides upstream's own (the PC's user and computer names): the Slippi account's.
  std::vector<std::string> names = lc::environment_names();
  const std::string user = read_all((options.user_dir.empty() ? options.base_dir + "/User/Slippi" : options.user_dir) + "/user.json");
  for (const char* k : {"displayName", "connectCode", "uid"}) { const std::string v = user_field(user, k); if (!v.empty()) names.push_back(v); }
  const auto add = [&](const char* zip_name, std::vector<uint8_t> data, size_t cap) {
    if (data.empty()) return;
    files.emplace_back(zip_name, lc::private_report_text(zip_name, data, data.size() >= cap, names));
  };
  add("melee_port_crash.txt", lc::read_tail(logs + "/crash.txt", 256 * 1024), 256 * 1024);
  {
    std::vector<uint8_t> log = lc::read_tail(logs + "/latest.log", 2u * 1024 * 1024);
    const std::vector<uint8_t> host = lc::read_tail(logs + "/host.log", 512 * 1024);
    if (!host.empty()) { static const char sep[] = "\n---- host.log ----\n"; log.insert(log.end(), sep, sep + sizeof sep - 1); log.insert(log.end(), host.begin(), host.end()); }
    add("melee_port.log", log, 3u * 1024 * 1024);
  }
  add("melee_port.prev.log", lc::read_tail(logs + "/previous.log", 1u * 1024 * 1024), 1u * 1024 * 1024);
  // The last day's session traces (marked rows only), through upstream's own collector.
  if (&features::replay_folder) {
    for (auto& f : lc::collect_logs("/nonexistent-game-dir", "/nonexistent-launcher-dir", {features::replay_folder()}, false))
      if (f.first == "session.trace") files.emplace_back(f.first, lc::private_report_text(f.first, f.second, false, names));
  }
  if (files.empty()) { *error = "There are no logs to put in a report yet."; return false; }
  const std::vector<uint8_t> zip = lc::capped_zip(files);
  const std::string zip_path = logs + "/melee_crash_report.zip", md_path = logs + "/melee_crash_report.md";
  const std::string md = lc::make_markdown(files, MU_APP_VERSION, "Source Port (Switch)");
  if (!lc::save_report(zip_path, reinterpret_cast<const char*>(zip.data()), zip.size()) || !lc::save_report(md_path, md.data(), md.size())) {
    *error = "The report could not be written to the SD card.";
    say("crash report: could not write %s", zip_path.c_str());
    return false;
  }
  size_t bytes = 0;
  for (const auto& f : files) bytes += f.second.size();
  say("crash report: %s (%zu files, %zu bytes of scrubbed text, zip %zu bytes) and %s; nothing was sent", zip_path.c_str(), files.size(), bytes, zip.size(), md_path.c_str());
  *where = "switch/melee-nx-netplay/Logs/melee_crash_report.zip (and .md)";
  return true;
}
}  // namespace mu_ui
}  // namespace fe
