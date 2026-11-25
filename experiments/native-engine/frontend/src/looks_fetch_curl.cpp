// Get mod looks' downloads (src/modlooks.cpp) through the services program's libcurl, as melee-unlocked's WinHTTP does them:
// HTTPS only, redirects never down to HTTP, its browser-like user agent, a size limit. Linked into the services program only.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <curl/curl.h>
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
size_t to_file(char* p, size_t s, size_t n, void* f) { return std::fwrite(p, s, n, static_cast<FILE*>(f)); }
}

bool fe_looks_https_get(const std::string& url, const std::string& path, uint64_t limit, std::string* error) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { *error = "The download could not be saved."; return false; }
  CURL* c = curl_easy_init();
  if (!c) { std::fclose(f); *error = "The download could not start."; return false; }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) MeleeUnlocked");   // upstream's
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTPS));
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTPS));
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, curl_off_t(limit));
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  const CURLcode r = curl_easy_perform(c);
  long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  const bool ok = std::fclose(f) == 0 && r == CURLE_OK;
  if (!ok) *error = code && code != 200 ? "The download page answered " + std::to_string(code) + "." : "The download stopped before it finished.";
  return ok;
}
