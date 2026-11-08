// HTTPS through libcurl. On the Switch this is devkitPro's switch-curl over
// mbedtls; the certificate bundle is the console's own (libnx exposes it to
// mbedtls through the ssl service), so no CA file ships with the port.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_http.h"
#include <curl/curl.h>
#include <cstring>
#include <mutex>

namespace slippi::report {
namespace {
size_t sink(char* data, size_t size, size_t count, void* user) {
  static_cast<std::string*>(user)->append(data, size * count);
  return size * count;
}
void ensure_global() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}
}  // namespace

bool http_request(const char* method, const std::string& url, const std::string& headers, const std::string& body,
                  const char* user_agent, int* status, std::string* response, std::string* error) {
  ensure_global();
  CURL* curl = curl_easy_init();
  if (!curl) { if (error) *error = "curl_easy_init failed"; return false; }
  struct curl_slist* list = nullptr;
  size_t pos = 0;
  while (pos < headers.size()) {
    size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos) end = headers.size();
    if (end > pos) list = curl_slist_append(list, headers.substr(pos, end - pos).c_str());
    pos = end + 2;
  }
  std::string out;
  char message[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : "Melee-NX-NetPlay");
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
  if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  if (!body.empty() || std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  const CURLcode result = curl_easy_perform(curl);
  long code = 0;
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (list) curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) {
    if (error) *error = message[0] ? message : curl_easy_strerror(result);
    return false;
  }
  if (status) *status = static_cast<int>(code);
  if (response) *response = std::move(out);
  return true;
}
}  // namespace slippi::report
