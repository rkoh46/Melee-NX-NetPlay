// One HTTPS download into a file through the app's libcurl and TLS (on the console devkitPro's libcurl over mbedTLS
// with the console's own certificates, as slippi_http_curl.cpp uses it). Linked into the services program only, the one
// that has libcurl; features/proai.cpp refers to it weakly. Hugging Face's resolve URLs answer with a redirect to its
// file host: redirects are followed, HTTPS only. The file is written beside its final name, its SHA-256 compared with
// the catalog's, and only then renamed into place.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <curl/curl.h>
#include "sha256.h"   // this repository, port/runtime/host
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>
#include <string>

namespace {
struct Sink { FILE* f; std::atomic<uint64_t>* progress; uint64_t got; };
size_t on_data(char* data, size_t size, size_t count, void* user) {
  Sink* s = static_cast<Sink*>(user);
  const size_t n = size * count;
  if (std::fwrite(data, 1, n, s->f) != n) return 0;
  s->got += n;
  if (s->progress) s->progress->store(s->got, std::memory_order_relaxed);
  return n;
}
}  // namespace

namespace {
bool download(const std::string& url, const std::string& dest, std::atomic<uint64_t>* progress, std::string* why) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  FILE* f = std::fopen(dest.c_str(), "wb");
  if (!f) { if (why) *why = "cannot write " + dest; return false; }
  CURL* curl = curl_easy_init();
  if (!curl) { std::fclose(f); if (why) *why = "curl_easy_init failed"; return false; }
  Sink sink{f, progress, 0};
  char message[CURL_ERROR_SIZE] = {};
  // MELEE_PRO_AI_TEST_MIRROR (tests: a local server in a container without network) may be plain HTTP on the loopback.
  const bool test_mirror = url.rfind("http://127.0.0.1:", 0) == 0 && std::getenv("MELEE_PRO_AI_TEST_MIRROR");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // The bit-mask forms (as self_update_curl.cpp uses them): devkitPro's and the test container's libcurl both take them.
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(test_mirror ? CURLPROTO_HTTP : CURLPROTO_HTTPS));
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Melee-NX-NetPlay");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // under 1 KB/s ...
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);      // ... for 30 s: give up
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  const bool closed = std::fclose(f) == 0;
  if (rc != CURLE_OK || !closed) {
    if (why) *why = rc != CURLE_OK ? (message[0] ? message : curl_easy_strerror(rc)) : "the file did not close";
    std::remove(dest.c_str());
    return false;
  }
  return true;
}
}  // namespace

bool fe_proai_fetch_verified(const std::string& url, const std::string& sha256, const std::string& dest, std::atomic<uint64_t>* progress,
                             std::string* why) {
  const std::string part = dest + ".part";
  if (!download(url, part, progress, why)) return false;
  std::ifstream in(part, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  const std::string got = host::memory_sha256(bytes.data(), bytes.size());
  if (got != sha256) {
    std::remove(part.c_str());
    if (why) *why = "SHA-256 " + got.substr(0, 16) + "... is not the catalog's " + sha256.substr(0, 16) + "...";
    return false;
  }
  std::remove(dest.c_str());
  if (std::rename(part.c_str(), dest.c_str()) != 0) { std::remove(part.c_str()); if (why) *why = "cannot rename into " + dest; return false; }
  return true;
}
