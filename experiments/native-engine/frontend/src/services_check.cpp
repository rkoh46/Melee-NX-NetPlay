// Start-up self-check of the services program, before the netplay host starts: what the first HTTPS request and
// the matchmaking connection will need, exercised without contacting anything. libcurl's global set-up (the call
// that crashed the first console run, when it had not been linked at all), one easy handle with options set and
// never performed, the TLS library curl reports, and on the console one context of the console's ssl service (what
// curl's TLS backend there opens per connection) and a read of the random service it uses. Each step prints
// one line; a failure says which step, so a console run ends with a readable line instead of a crash.
// Only linked into the services program (CMakeLists.txt); play_main.cpp calls it through a weak declaration.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include <curl/curl.h>
#include <string>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe {
bool services_self_check() {
  const CURLcode global = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (global != CURLE_OK) { say("services check: FAILED: curl_global_init: %s", curl_easy_strerror(global)); return false; }
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  if (!info || !info->ssl_version || !*info->ssl_version) { say("services check: FAILED: libcurl %s has no TLS library", info ? info->version : "?"); return false; }
  CURL* curl = curl_easy_init();
  if (!curl) { say("services check: FAILED: curl_easy_init"); return false; }
  // Options of a real request; nothing is performed, so nothing is resolved or contacted.
  const bool options = curl_easy_setopt(curl, CURLOPT_URL, "https://example.invalid/") == CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L) == CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK;
  curl_easy_cleanup(curl);
  if (!options) { say("services check: FAILED: curl_easy_setopt"); return false; }
  std::string tls = "TLS context not set up here";
#ifdef __SWITCH__
  // devkitPro's libcurl does TLS through the console's ssl service (its libnx backend: sslInitialize and
  // csrngInitialize in curl_global_init above, one ssl context per connection). Set one context up and close it.
  u8 random[16] = {};
  const Result rng = csrngGetRandomBytes(random, sizeof random);
  if (R_FAILED(rng)) { say("services check: FAILED: csrngGetRandomBytes (the random service curl's TLS uses): 0x%X", unsigned(rng)); return false; }
  SslContext context{};
  const Result created = sslCreateContext(&context, SslVersion_Auto);
  if (R_FAILED(created)) { say("services check: FAILED: sslCreateContext (the console's ssl service): 0x%X", unsigned(created)); return false; }
  sslContextClose(&context);
  tls = "the console's ssl service gave a TLS context, the random service answered";
#endif
  say("services check: libcurl %s with %s: global set-up and a request handle OK; %s; nothing was contacted", info->version, info->ssl_version, tls.c_str());
  return true;
}
}  // namespace fe
