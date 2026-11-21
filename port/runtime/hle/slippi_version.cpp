// Launch-time Slippi release check (see slippi_version.h). One HTTPS GET to GitHub's public
// releases API on a background thread; it sends no account or device data. The answer is kept in
// the cache folder so a launch without network still knows what the last check found.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_version.h"
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#include "host.h"
#include "json_safe.h"
#include "slippi_http.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace slippi::version {
namespace {
const char* const kLatestReleaseUrl = "https://api.github.com/repos/project-slippi/Ishiiruka/releases/latest";
std::mutex g_mutex;
std::string g_newer;               // newest release known to be newer than kSemver
std::atomic<bool> g_done{false};
std::thread g_thread;

bool parse(const std::string& text, int out[3]) {
  size_t pos = !text.empty() && (text[0] == 'v' || text[0] == 'V') ? 1 : 0;
  for (int i = 0; i < 3; ++i) {
    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;
    long value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { value = value * 10 + (text[pos++] - '0'); if (value > 100000) return false; }
    out[i] = (int)value;
    if (i < 2) { if (pos >= text.size() || text[pos] != '.') return false; ++pos; }
  }
  return pos == text.size();   // "3.8.0-dev" and the like are not releases
}
std::string strip_v(const std::string& tag) { return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag; }

void note(const std::string& latest, const char* source) {
  const std::string plain = strip_v(latest);
  if (!is_newer(plain, kSemver)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_newer.empty() && !is_newer(plain, g_newer)) return;
  g_newer = plain;
  host::log("slippi: Slippi %s has been released; this build implements %s (%s). Online play needs an update: run the installer again on the computer.",
            plain.c_str(), kSemver, source);
}

std::string read_small_file(const std::string& path) {
  std::string out;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buffer[64];
    const size_t n = std::fread(buffer, 1, sizeof buffer - 1, f);
    std::fclose(f);
    out.assign(buffer, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  }
  return out;
}
}  // namespace

bool is_newer(const std::string& theirs, const std::string& ours) {
  int a[3], b[3];
  if (!parse(theirs, a) || !parse(ours, b)) return false;
  for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] > b[i];
  return false;
}

void start_update_check(const std::string& cache_dir) {
  static std::once_flag once;
  std::call_once(once, [&] {
    const std::string cache_file = cache_dir.empty() ? std::string() : cache_dir + "/slippi_latest_release.txt";
    if (!cache_file.empty()) { const std::string cached = read_small_file(cache_file); if (!cached.empty()) note(cached, "from the last check"); }
    g_thread = std::thread([cache_file] {
      int status = 0;
      std::string body, error;
      const bool ok = report::http_request("GET", kLatestReleaseUrl, "Accept: application/vnd.github+json", "", "Melee-NX-NetPlay", &status, &body, &error);
      if (!ok || status != 200) {
        host::log("slippi: release check skipped (%s)", ok ? ("HTTP " + std::to_string(status)).c_str() : error.c_str());
      } else {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const std::string tag = j.is_discarded() ? std::string() : jget(j, "tag_name", "");
        int parts[3];
        if (!parse(tag, parts)) host::log("slippi: release check: no usable release tag in the answer");
        else {
          const std::string plain = strip_v(tag);
          if (is_newer(plain, kSemver)) note(plain, "release check");
          else host::log("slippi: release check: Slippi %s is the latest release; this build implements %s", plain.c_str(), kSemver);
          if (!cache_file.empty()) if (FILE* f = std::fopen(cache_file.c_str(), "wb")) { std::fputs(plain.c_str(), f); std::fclose(f); }
        }
      }
      g_done.store(true);
    });
    g_thread.detach();   // one bounded request (5 s connect, 30 s total); it touches only the statics above
  });
}

bool update_check_finished() { return g_done.load(); }
std::string newer_release() { std::lock_guard<std::mutex> lock(g_mutex); return g_newer; }
void note_service_requires(const std::string& latest) { note(latest, "matchmaking service"); }
std::string update_notice() {
  const std::string newer = newer_release();
  if (newer.empty()) return {};
  return "Slippi " + newer + " is out. This build plays Slippi " + kSemver + ", so online play will be refused until it is updated.";
}
}  // namespace slippi::version
#else
namespace slippi::version {
bool is_newer(const std::string&, const std::string&) { return false; }
void start_update_check(const std::string&) {}
bool update_check_finished() { return true; }
std::string newer_release() { return {}; }
void note_service_requires(const std::string&) {}
std::string update_notice() { return {}; }
}  // namespace slippi::version
#endif
