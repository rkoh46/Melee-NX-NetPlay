// The console's transport for self_update.h: a streaming GET through libcurl with short connect
// timeouts (the build computer is on the home network) and a stall timeout instead of a total one,
// so a 70 MB download over Wi-Fi is not cut off but a dead connection is.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "self_update.h"
#include <curl/curl.h>
#include <mutex>

namespace host::self_update {
namespace {
struct Sink { const std::function<bool(const void*, size_t)>* write; bool stopped = false; };
size_t on_data(char* data, size_t size, size_t count, void* user) {
  Sink* sink = static_cast<Sink*>(user);
  if (!(*sink->write)(data, size * count)) { sink->stopped = true; return 0; }
  return size * count;
}
}  // namespace

Fetch curl_fetch(long connect_timeout_ms, long stall_seconds) {
  return [=](const std::string& url, const std::function<bool(const void*, size_t)>& write, std::string& error) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) { error = "the network library did not start"; return false; }
    Sink sink{&write};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Melee-NX-NetPlay updater");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);        // under 1 KB/s ...
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, stall_seconds); // ... for this long: give up
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);               // a 404 is a failure, not a body
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (sink.stopped) { error = "stopped"; return false; }
    if (result != CURLE_OK) { error = status >= 400 ? "the build computer answered " + std::to_string(status) : std::string("cannot reach the build computer (") + curl_easy_strerror(result) + ")"; return false; }
    if (status != 200) { error = "the build computer answered " + std::to_string(status); return false; }
    return true;
  };
}
}  // namespace host::self_update
