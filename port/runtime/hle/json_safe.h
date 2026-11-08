// Reading JSON that comes from a server: a field can be missing, null or of an unexpected type,
// and none of those may throw (an uncaught exception on a background thread aborts the app, which
// is how a null "dailyGlobalPlacement" in a Slippi profile crashed the dashboard). jget returns the
// default instead.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <string>

template <class T>
T jget(const nlohmann::json& j, const char* key, T fallback) {
  if (!j.is_object()) return fallback;
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return fallback;
  try { return it->template get<T>(); } catch (...) { return fallback; }
}
inline std::string jget(const nlohmann::json& j, const char* key, const char* fallback) { return jget<std::string>(j, key, std::string(fallback)); }
