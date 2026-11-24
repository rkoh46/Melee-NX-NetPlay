// The checks follow melee-unlocked's port/app/launcher_mod_catalog.cpp (parse, valid_sha256, valid_id; GPL-2.0-or-later,
// used here under GPL-3.0-or-later), without its Windows parts.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_catalog.h"
#include "mi_hash.h"

#include "nlohmann/json.hpp"

#include <set>
#include <utility>

namespace modinstall {
namespace {
using Json = nlohmann::json;
bool hex(const std::string& s, size_t count) {
  return s.size() == count && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}
bool text_field(const Json& j, const char* name, std::string* value) {
  const auto it = j.find(name);
  if (it == j.end()) return true;
  if (!it->is_string()) return false;
  *value = it->get<std::string>();
  return value->size() <= 4096 && value->find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
}
bool https(const std::string& s) {
  return s.rfind("https://", 0) == 0 && s.size() > 8 && s.find_first_of("\"\\ \t") == std::string::npos;
}
// Sizes of the files the catalog pins, measured on those files (download bytes, finished disc bytes).
struct KnownSize { const char* sha256; uint64_t download, disc; };
const KnownSize kKnownSizes[] = {
  {"61834b65d921668ff11702c458a1ae6946172b0a4c8321a47e2c2ddff9aef51d", 90176, 0},              // 20XX TE v2d r4 (.gci)
  {"7d09a2ba854fa8507988ace819ccbfbf0a6bfd305a44a659369176ecc9689325", 468359, 1459978240},    // Training Mode CE v1.4 dev2
  {"7d46ce47b27272a369150cb105f4cb3b046b2e7790d233595ff4537ef442919f", 246177787, 1459978240}, // Akaneia 1.0.1
  {"1560d157334ea7cd1e2ef4ed7720f3f8aba846f245482e05a6d1d21d744b18cf", 1192946365, 1692377632}, // ACE 2.0.1
  {"a0aa4958a2228de7cc9513c6014a8bb545bec7ee539cb67fb2468c10ad50e4e3", 677694750, 1459978240}, // 20XX Hack Pack 5.0.2
  {"ad34e72e1ae66c68690a329ff1aa04af5bf82c8c8af1d1a26ba693e290d9cff4", 1446550, 1488060416},   // Melee 3D 1.0
  {"659ad362a7e5dce7970223d5d945ae2b9ff327ee44c9b67b66f9891645013056", 7278918, 1459978240},   // Melee Maker 1.3.8
  {"b0e987a3944f71750694d89c7af94cbf04c3c88ad7ca7516945c37d8a9538b9e", 1009454, 1459978240},   // rogueMelee v3 (.xdelta)
};
}  // namespace

bool valid_sha256(const std::string& s) { return hex(s, 64); }
bool valid_id(const std::string& s) {
  return !s.empty() && s.size() <= 64 && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
bool CatalogMod::direct_file() const {
  // A GitHub release asset is the file itself; anything else (MediaFire's /file pages) is a page with a button.
  return url.rfind("https://github.com/", 0) == 0 && url.find("/releases/download/") != std::string::npos;
}

bool parse_catalog(const std::string& text, std::vector<CatalogMod>* mods, std::string* error) {
  const Json j = Json::parse(text, nullptr, false);
  if (!j.is_object() || !j.count("mods") || !j["mods"].is_array() || j["mods"].size() > 128) {
    *error = "The mod catalog could not be read."; return false;
  }
  std::vector<CatalogMod> parsed;
  std::set<std::string> ids;
  for (const auto& row : j["mods"]) {
    if (!row.is_object()) { *error = "Invalid catalog entry."; return false; }
    CatalogMod m;
    bool ok = true;
    const std::pair<const char*, std::string*> fields[] = {
        {"id", &m.id}, {"name", &m.name}, {"credits", &m.credits}, {"license", &m.license},
        {"page", &m.page}, {"repo", &m.repo}, {"tag", &m.tag}, {"asset_pattern", &m.asset_pattern},
        {"kind", &m.kind}, {"sha256", &m.sha256}, {"output_md5", &m.output_md5}, {"version", &m.version},
        {"url", &m.url}, {"tool_sha256", &m.tool_sha256}, {"patch_sha256", &m.patch_sha256}, {"policy", &m.policy},
        {"output_sha256", &m.output_sha256}, {"note", &m.note}};
    for (const auto& field : fields) ok &= text_field(row, field.first, field.second);
    if (row.count("one_click")) {
      ok &= row["one_click"].is_boolean();
      if (row["one_click"].is_boolean()) m.one_click = row["one_click"].get<bool>();
    }
    ok &= valid_id(m.id) && ids.insert(m.id).second && !m.name.empty() && https(m.page);
    ok &= m.kind == "gci" || m.kind == "xdelta_zip" || m.kind == "xdelta_7z";
    ok &= m.url.empty() || https(m.url);
    ok &= m.sha256.empty() || valid_sha256(m.sha256);
    ok &= m.tool_sha256.empty() || valid_sha256(m.tool_sha256);
    ok &= m.patch_sha256.empty() || valid_sha256(m.patch_sha256);
    ok &= m.output_sha256.empty() || valid_sha256(m.output_sha256);
    ok &= m.output_md5.empty() || hex(m.output_md5, 32);
    const size_t slash = m.repo.find('/');
    ok &= slash != std::string::npos && slash > 0 && slash + 1 < m.repo.size() &&
          m.repo.find('/', slash + 1) == std::string::npos &&
          m.repo.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./") == std::string::npos &&
          m.repo.find("..") == std::string::npos;
    // The installer writes the download under its asset name: no path separators or wildcards.
    ok &= m.asset_pattern.empty() || (m.asset_pattern.find_first_of("/\\:\"\r\n*?") == std::string::npos && m.asset_pattern != "." && m.asset_pattern != "..");
    if (!ok) { *error = "The catalog contains invalid verification metadata (" + (m.id.empty() ? std::string("an entry") : m.id) + ")."; return false; }
    // A catalog must record a licensing/permission basis before enabling automatic downloads.
    if (m.policy != "licensed" && m.policy != "permission_granted") m.one_click = false;
    m.sha256 = lower(m.sha256); m.patch_sha256 = lower(m.patch_sha256); m.output_sha256 = lower(m.output_sha256); m.output_md5 = lower(m.output_md5);
    for (const auto& k : kKnownSizes) if (m.sha256 == k.sha256) { m.download_bytes = k.download; m.disc_bytes = k.disc; }
    parsed.push_back(std::move(m));
  }
  *mods = std::move(parsed);
  return true;
}

}  // namespace modinstall
