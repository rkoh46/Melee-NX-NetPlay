// Stand-in for melee-unlocked's port/app/launcher_lang.h, for its peer lobby (gen/lobby/launcher_lobby_p2p.cpp): the same
// calls (tr, fill, has_key, en) on the same tables. English is upstream's launcher_lang_en.inl (copied beside the lobby by
// build.sh sync); the player's language is the home screen's (src/mu_lang.cpp, upstream's lang/*.txt, looked up by key).
// Upstream's header also carries Win32 font and date code, which the lobby does not use.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fe::mu_lang { std::string by_key(const std::string& key); }   // src/mu_lang.cpp: "" when the language has no line

namespace launcher::lang {
struct Entry { const char* key; const char* text; };
inline const Entry english_entries[] = {
#include "launcher_lang_en.inl"
};
using Table = std::map<std::string, std::string>;
using Args = std::initializer_list<std::pair<const char*, std::string>>;
namespace detail {
inline const Table& english() {
  static const Table table = [] {
    Table t;
    for (const auto& e : english_entries) t.emplace(e.key, e.text);
    return t;
  }();
  return table;
}
}  // namespace detail
// Replaces {name} with the argument called name, in one pass (upstream's rule: a name typed by another player that looks like
// a placeholder is never filled in itself).
inline std::string fill(const std::string& text, const std::vector<std::pair<std::string, std::string>>& args) {
  std::string out; out.reserve(text.size() + 32);
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '{') {
      const size_t close = text.find('}', i + 1);
      if (close != std::string::npos && close - i <= 32) {
        const std::string name = text.substr(i + 1, close - i - 1);
        bool found = false;
        for (const auto& a : args) if (a.first == name) { out += a.second; found = true; break; }
        if (found) { i = close + 1; continue; }
      }
    }
    out += text[i++];
  }
  return out;
}
inline std::string fill(const std::string& text, Args args) {
  std::vector<std::pair<std::string, std::string>> list;
  for (const auto& a : args) list.emplace_back(a.first, a.second);
  return fill(text, list);
}
inline std::string en(const char* key) {
  const auto& t = detail::english();
  const auto it = t.find(key);
  return it == t.end() ? std::string(key) : it->second;
}
inline std::string en(const char* key, Args args) { return fill(en(key), args); }
inline bool has_key(const std::string& key) { return detail::english().count(key) != 0; }
inline std::string tr(const char* key) {
  const std::string local = fe::mu_lang::by_key(key);
  return local.empty() ? en(key) : local;
}
inline std::string tr(const std::string& key) { return tr(key.c_str()); }
inline std::string tr(const char* key, Args args) { return fill(tr(key), args); }
inline std::string tr(const std::string& key, Args args) { return fill(tr(key.c_str()), args); }
}  // namespace launcher::lang
