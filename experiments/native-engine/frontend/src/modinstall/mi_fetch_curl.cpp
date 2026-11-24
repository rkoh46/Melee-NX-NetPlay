// The installer's downloads through libcurl: devkitPro's on the console (TLS through the console's ssl service),
// the system's on Linux and macOS. Linked into the services program only (the one that has libcurl).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_fetch_curl.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>
#include <unistd.h>

namespace modinstall {
namespace {
struct Transfer {
  FILE* file = nullptr;
  uint64_t start = 0, written = 0, limit = 0;
  bool restarted = false, too_big = false, cancelled = false;
  long status = 0;
  const std::function<bool(uint64_t, uint64_t)>* progress = nullptr;
  CURL* curl = nullptr;
};

size_t on_data(char* data, size_t size, size_t count, void* user) {
  auto* t = static_cast<Transfer*>(user);
  const size_t n = size * count;
  if (t->status == 0) curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &t->status);
  // A server that ignores the range answers 200 with the whole file: start the file again.
  if (t->status == 200 && t->start > 0 && !t->restarted) {
    t->restarted = true;
    if (std::fseek(t->file, 0, SEEK_SET) != 0 || ftruncate(fileno(t->file), 0) != 0) return 0;
    t->start = 0;
  }
  if (t->status != 200 && t->status != 206) return 0;
  if (t->limit && t->start + t->written + n > t->limit) { t->too_big = true; return 0; }
  if (std::fwrite(data, 1, n, t->file) != n) return 0;
  t->written += n;
  return n;
}

int on_progress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
  auto* t = static_cast<Transfer*>(user);
  if (t->progress && *t->progress) {
    const uint64_t whole = total > 0 ? t->start + uint64_t(total) : t->limit;
    if (!(*t->progress)(t->start + uint64_t(now), whole)) { t->cancelled = true; return 1; }
  }
  return 0;
}

class CurlFetcher : public Fetcher {
 public:
  Result fetch(const std::string& url, const std::string& path, uint64_t expected_size,
               const std::function<bool(uint64_t, uint64_t)>& progress) override {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    Result result;
    if (url.rfind("https://", 0) != 0) { result.error = "only https links are fetched"; return result; }
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) { result.error = "cannot write " + path; return result; }
    std::fclose(f);
    f = std::fopen(path.c_str(), "r+b");
    if (!f || std::fseek(f, 0, SEEK_END) != 0) { if (f) std::fclose(f); result.error = "cannot write " + path; return result; }
    Transfer t;
    t.file = f;
    t.start = uint64_t(ftello(f));
    t.limit = expected_size;
    t.progress = &progress;
    if (expected_size && t.start == expected_size) { std::fclose(f); result.ok = true; result.bytes = t.start; return result; }
    if (expected_size && t.start > expected_size) {   // longer than the release: begin again
      if (ftruncate(fileno(f), 0) != 0) { std::fclose(f); result.error = "cannot restart the download"; return result; }
      std::fseek(f, 0, SEEK_SET);
      t.start = 0;
    }
    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(f); result.error = "libcurl could not start"; return result; }
    t.curl = curl;
    char message[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Melee-NX-NetPlay mod installer");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 6L);
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTPS));   // a redirect to anything but https is refused
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);     // a stalled transfer ends after a minute; Install continues it
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    if (t.start) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, curl_off_t(t.start));
    // Each redirect's answer reaches on_data only for the final transfer; its status is read there.
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    const bool flushed = std::fclose(f) == 0;
    result.bytes = t.start + t.written;
    result.cancelled = t.cancelled;
    if (code == CURLE_OK && flushed && (status == 200 || status == 206)) {
      result.ok = !expected_size || result.bytes <= expected_size;
      if (!result.ok) result.error = "the file has the wrong size";
      return result;
    }
    if (status == 416 && expected_size && t.start == expected_size) { result.ok = true; return result; }
    result.error = t.too_big ? "the server sent more than the release's size" : t.cancelled ? "cancelled"
                 : message[0] ? message : curl_easy_strerror(code);
    if (status && status != 200 && status != 206) result.error += " (HTTP " + std::to_string(status) + ")";
    return result;
  }
};
}  // namespace

Fetcher* curl_fetcher() {
  static CurlFetcher fetcher;
  return &fetcher;
}

}  // namespace modinstall
