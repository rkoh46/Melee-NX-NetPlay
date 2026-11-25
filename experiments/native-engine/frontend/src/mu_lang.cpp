// melee-unlocked's launcher languages (port/app/launcher_lang.h: its list, its file format and its lookup by English
// wording), on the embedded copies of its lang/*.txt (GPL-2.0-or-later, build.sh sync -> gen/mu_ui/lang).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mu_lang.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#define MU_LANG_FILE(id, code) extern "C" const uint8_t mu_lang_##id[], mu_lang_##id##_end[];
#include "lang_files.inc"
#undef MU_LANG_FILE

namespace fe::mu_lang {
namespace {
struct Embedded { const char* code; const uint8_t* begin; const uint8_t* end; };
#define MU_LANG_FILE(id, code) {code, mu_lang_##id, mu_lang_##id##_end},
const Embedded kFiles[] = {
#include "lang_files.inc"
};
#undef MU_LANG_FILE
using Table = std::map<std::string, std::string>;
std::mutex g_mutex;
std::string g_code = "en";
std::shared_ptr<const Table> g_table;

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}
std::string unescape(const std::string& s) {   // launcher_lang.h detail::unescape
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[i + 1];
      if (n == 'n') { out += '\n'; ++i; continue; }
      if (n == 't') { out += '\t'; ++i; continue; }
      if (n == '\\') { out += '\\'; ++i; continue; }
    }
    out += s[i];
  }
  return out;
}
}  // namespace

const std::vector<Language>& languages() {   // launcher_lang.h languages(), in its order
  static const std::vector<Language> list = {
    {"en", "English", 0}, {"es", "Español", 0}, {"pt-BR", "Português (Brasil)", 0}, {"fr", "Français", 0}, {"de", "Deutsch", 0},
    {"ru", "Русский", 0}, {"ja", "日本語", 1}, {"zh-Hans", "简体中文", 2}, {"ko", "한국어", 3}};
  return list;
}
const Language* find(const std::string& code) {
  for (const auto& l : languages()) if (code == l.code) return &l;
  return nullptr;
}
bool set(const std::string& code) {
  if (!find(code) || code == "en") {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_code = "en"; g_table.reset();
    return code == "en" || code.empty();
  }
  for (const Embedded& f : kFiles) {
    if (code != f.code) continue;
    auto table = std::make_shared<Table>();
    std::istringstream in(std::string(reinterpret_cast<const char*>(f.begin), size_t(f.end - f.begin)));
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {   // launcher_lang.h read_file
      if (first && line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF) line.erase(0, 3);
      first = false;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = trim(line);
      if (t.empty() || t[0] == '#' || t[0] == ';') continue;
      const size_t eq = t.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = unescape(trim(t.substr(0, eq)));
      if (!key.empty()) (*table)[key] = unescape(trim(t.substr(eq + 1)));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_code = code; g_table = std::move(table);
    return true;
  }
  return false;
}
std::string current() { std::lock_guard<std::mutex> lock(g_mutex); return g_code; }
std::string tx(const std::string& english) {
  if (english.empty()) return english;
  std::shared_ptr<const Table> table;
  { std::lock_guard<std::mutex> lock(g_mutex); table = g_table; }
  if (!table) return english;
  const auto it = table->find("@" + english);
  return it != table->end() && !it->second.empty() ? it->second : english;
}
// A line of the current language by upstream's key (the peer lobby's notices, src/lobby_shim/launcher_lang.h); "" for English.
std::string by_key(const std::string& key) {
  std::shared_ptr<const Table> table;
  { std::lock_guard<std::mutex> lock(g_mutex); table = g_table; }
  if (!table) return {};
  const auto it = table->find(key);
  return it != table->end() ? it->second : std::string();
}
}  // namespace fe::mu_lang
