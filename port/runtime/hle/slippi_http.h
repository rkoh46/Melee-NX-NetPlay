// Synchronous HTTPS request. One contract for every platform: `headers` are
// CRLF-separated "Name: value" lines; a transport failure returns false with
// `error` filled; HTTP status codes come back through `status`.
// Implementations: slippi_http_curl.cpp (Switch and other libcurl platforms).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace slippi::report {
bool http_request(const char* method, const std::string& url, const std::string& headers, const std::string& body,
                  const char* user_agent, int* status, std::string* response, std::string* error);
}  // namespace slippi::report
