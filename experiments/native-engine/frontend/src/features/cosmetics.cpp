// Cosmetics: skins for the fighters' costumes and the stages, from the player's SD card and from the mod discs in the
// mods folder, picked on the select screens; and the online rule that keeps a look-only costume on in online play.
//
// A portable reimplementation of the parts of melee-unlocked's Windows-only cosmetics store (fetched tree:
// port/runtime/host/cosmetic_mods.cpp, disc_skin_scan.h, pack_skin_rule.*, and the commands in port/app/source_host.cpp)
// that this app can offer, with upstream's rules:
//   - a skin is a costume file ("PlFxGr.dat": a fighter's colour slot) or a stage file ("GrNBa.dat"); it is served to the
//     game in place of the slot's file, under a new entry number each time it changes (the game's preload drops the old copy);
//   - the online rule is upstream's code, compiled from the fetched tree (build.sh sync extracts its skeleton checks from
//     cosmetic_mods.cpp into gen/cosmetics_skeleton.inc): a costume stays on in online play only when its skeleton matches
//     the standard costume's joint for joint (flags, hierarchy, rest pose within upstream's tolerance) and the game can draw
//     it (every blended mesh's bones have bind matrices); the copy served online then carries the standard costume's rest
//     pose bit for bit (costume_take_standard_bones), so hurtboxes, hitboxes and the ECB are the standard ones and the
//     simulation cannot differ from a Slippi peer's. Any other skin, and every stage skin, gives the standard file online;
//   - skins never change during an online session or a replay (the commands answer "unchanged").
//
// Where skins come from:
//   - <app folder>/Cosmetics/Costumes/: costume files named for their slot, optionally with a label after the slot name
//     ("PlFxGr.dat", "PlFxGr - Arwing.dat"), or a folder per skin ("Arwing Fox/PlFxGr.dat");
//   - <app folder>/Cosmetics/Stages/: stage files named for the stage ("GrNBa.dat", "GrNBa - Night.dat");
//   - every mod disc in <app folder>/mods/<profile>/ (disc_skin_scan): each costume file that differs from the retail disc's
//     joins the slot's skins as "<profile> disc". The scan is kept in <out>/mods-cache/skins-<profile>.txt while the disc's
//     size and time stamp are unchanged;
//   - the active mod profile's stage alternates named for a held button ("GrNBa-Name(L).dat" in a folder, or "GrNBa.lat" /
//     "GrNBa.rat"): at a match's start, holding L, R, Z, B, X or Y picks the alternate named for it (upstream's order).
// Picks: L / R on a highlighted costume on the character select (command 0xF9), X / Y on a highlighted stage on the stage
// select (0xFB). They are kept in <app folder>/Cosmetics/selection.txt. settings.ini `cosmetics 0` turns all of it off.
// In a session that can go online (the services program) a mod profile is not loaded; only its costumes that pass the
// online rule are served (mods.cpp, through cosmetics_online_copy), in their online form.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "feature_calls.h"
#include "pack_skin_rule.h"   // gen/: melee-unlocked's costume file names (fetched tree, copied by build.sh sync)

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

namespace linux_host {   // patches/0101: the host's disc table
int32_t fe_disc_find(const char* path, uint32_t* start, uint32_t* length);
int32_t fe_disc_set(const char* path, uint32_t start, uint32_t length);
void fe_disc_remove(const char* path);
bool fe_disc_read_base(uint32_t offset, void* out, uint32_t bytes);
}

// melee-unlocked's skeleton and drawing checks, compiled from the fetched tree (GPL-2.0-or-later).
namespace host::cosmetics {
namespace {
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
#include "cosmetics_skeleton.inc"
}  // namespace host::cosmetics
#include "pack_skin_rule.cpp"

namespace fe::features {
namespace {
// Upstream's costume families (fetched tree, cosmetic_mods.cpp `families`, extracted by build.sh sync).
struct FighterFamily { const char* file_code; const char* root_name; const char* display_name; const char* colors; };
#include "cosmetics_families.inc"

constexpr uint32_t kRangeBase = 0xE8000000u, kRangeEnd = 0xF0000000u;   // past the mod files (mods.cpp stops at kRangeBase)
struct Source { std::string path; uint64_t offset = 0; uint32_t length = 0; };   // a file, or a member of a disc image
struct Skin {
  std::string name, slot;   // slot: lower case with a leading slash ("/plfxgr.dat", "/grnba.dat")
  Source source;
  bool stage = false;
  bool checked = false, online_ok = false; std::string reason;
  uint32_t button = 0;      // a stage alternate's held button (pad bit), 0 for none
};
struct Served { uint32_t start, length; std::shared_ptr<const std::vector<uint8_t>> bytes; std::string slot; };

std::mutex mu;
bool enabled = true, ready = false, retail_view = false, online_session = false;
std::string folder;                                 // <app folder>/Cosmetics
std::map<std::string, std::vector<Skin>> skins;     // slot -> skins, in the order they cycle after the standard file
std::map<std::string, std::string> picks;           // slot -> skin name (persisted)
std::map<std::string, std::pair<uint32_t, uint32_t>> retail;   // slot -> the retail disc's start and length
std::vector<Served> served;                         // ranges handed out, in start order
uint32_t next_start = kRangeBase;
std::map<std::string, std::string> match_alternate; // stage slot -> the alternate a held button picked for this match

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string base(const std::string& p) { const size_t s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }
bool read_file(const std::string& path, uint64_t offset, uint32_t length, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.resize(length);
  bool ok = fseeko(f, off_t(offset), SEEK_SET) == 0 && std::fread(out.data(), 1, length, f) == length;
  std::fclose(f);
  return ok;
}
bool read_source(const Source& s, std::vector<uint8_t>& out) { return read_file(s.path, s.offset, s.length, out); }
bool read_retail(const std::string& slot, std::vector<uint8_t>& out) {
  const auto r = retail.find(slot);
  if (r == retail.end() || !r->second.second) return false;
  out.resize(r->second.second);
  return linux_host::fe_disc_read_base(r->second.first, out.data(), r->second.second);
}
std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) if (e->d_name[0] != '.') out.push_back(e->d_name); closedir(d); }
  std::sort(out.begin(), out.end());
  return out;
}
bool is_dir(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

// The online verdict (upstream: costume_skeleton_matches && costume_draw_safe), once per skin.
void judge(Skin& s, const std::vector<uint8_t>& bytes) {
  if (s.checked) return;
  s.checked = true;
  if (s.stage) { s.online_ok = false; s.reason = "stage skins are offline only"; return; }
  std::vector<uint8_t> clean;
  if (!read_retail(s.slot, clean)) { s.online_ok = false; s.reason = "no standard costume to compare"; return; }
  std::string why;
  const bool skeleton = host::cosmetics::costume_skeleton_matches(clean, bytes, &why);
  std::string draw;
  const bool drawable = host::cosmetics::costume_draw_safe(bytes, &draw);
  s.online_ok = skeleton && drawable;
  s.reason = !drawable ? draw : why;
}
// What the game is served for a skin now: its own bytes offline; online (or in a session that can go online) its online
// form (the standard rest pose), or nothing (the standard file) when it does not pass the rule.
std::shared_ptr<const std::vector<uint8_t>> form(Skin& s, bool online) {
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  if (!read_source(s.source, *bytes)) return nullptr;
  judge(s, *bytes);
  if (!online) return bytes;
  if (!s.online_ok) return nullptr;
  std::vector<uint8_t> clean;
  if (!read_retail(s.slot, clean)) return nullptr;
  host::cosmetics::costume_take_standard_bones(clean, *bytes);
  return bytes;
}
// Serves `bytes` for `slot` under a new entry number; null bytes: the retail file again.
void publish(const std::string& slot, std::shared_ptr<const std::vector<uint8_t>> bytes) {
  const auto r = retail.find(slot);
  if (!bytes) {
    if (r != retail.end()) { linux_host::fe_disc_remove(slot.c_str()); linux_host::fe_disc_set(slot.c_str(), r->second.first, r->second.second); }
    return;
  }
  const uint32_t length = uint32_t(bytes->size()), span = (length + 0x7FFFu) & ~0x7FFFu;
  if (next_start + uint64_t(span) > kRangeEnd) { say("cosmetics: the skin range is full; %s keeps its file", slot.c_str()); return; }
  // The slot's older copies go, but the newest one before this (a read the game started under the old entry finishes).
  {
    size_t kept = 0;
    for (size_t i = served.size(); i-- > 0;)
      if (served[i].slot == slot && ++kept > 1) served.erase(served.begin() + ptrdiff_t(i));
  }
  served.push_back({next_start, length, std::move(bytes), slot});
  linux_host::fe_disc_remove(slot.c_str());
  linux_host::fe_disc_set(slot.c_str(), next_start, length);
  next_start += span;
}
Skin* find_skin(const std::string& slot, const std::string& name) {
  auto it = skins.find(slot);
  if (it == skins.end()) return nullptr;
  for (auto& s : it->second) if (s.name == name) return &s;
  return nullptr;
}
bool online_form() { return retail_view || online_session; }
// The pick of a slot, served in the form the session needs. True when something is served.
bool apply(const std::string& slot) {
  const auto p = picks.find(slot);
  const std::string chosen = match_alternate.count(slot) ? match_alternate[slot] : p == picks.end() ? std::string() : p->second;
  Skin* s = chosen.empty() ? nullptr : find_skin(slot, chosen);
  if (!s) return false;
  auto bytes = form(*s, online_form());
  publish(slot, bytes);
  return bytes != nullptr;
}
void save_picks() {
  if (folder.empty()) return;
  mkdir(folder.c_str(), 0777);
  if (FILE* f = std::fopen((folder + "/selection.txt").c_str(), "w")) {
    std::fprintf(f, "# The skin picked for each slot (cosmetics.cpp): slot file, tab, skin name.\n");
    for (const auto& [slot, name] : picks) std::fprintf(f, "%s\t%s\n", slot.c_str(), name.c_str());
    std::fclose(f);
  }
}
void load_picks() {
  if (FILE* f = std::fopen((folder + "/selection.txt").c_str(), "r")) {
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
      if (line[0] == '#') continue;
      std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t tab = l.find('\t');
      if (tab != std::string::npos) picks[l.substr(0, tab)] = l.substr(tab + 1);
    }
    std::fclose(f);
  }
}

// ---- where skins come from ----
// "PlFxGr.dat", "PlFxGr - Arwing.dat", "PlFxGr_Arwing.dat" -> slot "/plfxgr.dat" and label "Arwing".
bool costume_name(const std::string& file, std::string* slot, std::string* label) {
  const std::string low = lower(file);
  if (low.size() < 10) return false;
  const std::string head = low.substr(0, 6) + (low.size() >= 10 && low.compare(low.size() - 4, 4, ".usd") == 0 ? ".usd" : ".dat");
  if (!source_port::skins::is_costume_file(head)) return false;
  if (low.compare(low.size() - 4, 4, ".dat") != 0 && low.compare(low.size() - 4, 4, ".usd") != 0) return false;
  *slot = "/" + head;
  std::string rest = file.substr(6, file.size() - 10);
  while (!rest.empty() && (rest[0] == ' ' || rest[0] == '-' || rest[0] == '_')) rest.erase(0, 1);
  *label = rest;
  return true;
}
const char* const kStageTokens[] = {"grbb", "grcn", "grcs", "grek", "grfs", "grgb", "grgd", "grgr", "grhr", "grim", "grip", "grit",
                                    "grkg", "grkr", "grmc", "grnba", "grnbr", "grnfg", "grnkr", "grnla", "grnpo", "grnsr", "grnzr",
                                    "grod", "grok", "grop", "grot", "grps", "grps1", "grps2", "grps3", "grps4", "grpu", "grrc",
                                    "grsh", "grst", "grtcl", "grto", "grve", "gryt", "grzs"};
// "GrNBa.dat", "GrNBa - Night.dat" -> slot "/grnba.dat"; the stage must exist on the retail disc.
bool stage_name(const std::string& file, std::string* slot, std::string* label) {
  const std::string low = lower(file);
  if (low.size() < 8 || low.compare(0, 2, "gr") != 0 || low.compare(low.size() - 4, 4, ".dat") != 0) return false;
  size_t end = 2; while (end < low.size() && std::isalnum(static_cast<unsigned char>(low[end]))) ++end;
  const std::string token = low.substr(0, end);
  if (std::find_if(std::begin(kStageTokens), std::end(kStageTokens), [&](const char* t) { return token == t; }) == std::end(kStageTokens)) return false;
  *slot = "/" + token + ".dat";
  std::string rest = file.substr(end, file.size() - 4 - end);
  while (!rest.empty() && (rest[0] == ' ' || rest[0] == '-' || rest[0] == '_' || rest[0] == '.')) rest.erase(0, 1);
  *label = rest;
  return true;
}
void add_skin(Skin s) {
  if (!retail.count(s.slot)) return;   // only slots the game has
  auto& list = skins[s.slot];
  std::string name = s.name; int n = 2;
  while (std::any_of(list.begin(), list.end(), [&](const Skin& o) { return o.name == name; })) name = s.name + " " + std::to_string(n++);
  s.name = name;
  list.push_back(std::move(s));
}
void scan_imports() {
  const std::string cdir = folder + "/Costumes";
  for (const auto& e : list_dir(cdir)) {
    std::string slot, label;
    if (is_dir(cdir + "/" + e)) {   // a folder per skin: its costume files take the folder's name
      for (const auto& f : list_dir(cdir + "/" + e))
        if (costume_name(f, &slot, &label)) {
          struct stat st{}; stat((cdir + "/" + e + "/" + f).c_str(), &st);
          add_skin({e, slot, {cdir + "/" + e + "/" + f, 0, uint32_t(st.st_size)}});
        }
      continue;
    }
    if (!costume_name(e, &slot, &label)) continue;
    struct stat st{}; stat((cdir + "/" + e).c_str(), &st);
    add_skin({label.empty() ? "Imported" : label, slot, {cdir + "/" + e, 0, uint32_t(st.st_size)}});
  }
  const std::string sdir = folder + "/Stages";
  for (const auto& e : list_dir(sdir)) {
    std::string slot, label;
    if (!stage_name(e, &slot, &label)) continue;
    struct stat st{}; stat((sdir + "/" + e).c_str(), &st);
    Skin s{label.empty() ? "Imported" : label, slot, {sdir + "/" + e, 0, uint32_t(st.st_size)}};
    s.stage = true;
    add_skin(std::move(s));
  }
}
// A disc image's file table: (path, offset, length) of every file.
struct DiscFile { std::string path; uint32_t offset, length; };
bool disc_files(const std::string& iso, std::vector<DiscFile>& out) {
  std::vector<uint8_t> h;
  if (!read_file(iso, 0, 0x440, h)) return false;
  const auto w = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
  const uint32_t fst_offset = w(&h[0x424]), fst_size = w(&h[0x428]);
  if (fst_size < 12 || fst_size > (2u << 20)) return false;
  std::vector<uint8_t> fst;
  if (!read_file(iso, fst_offset, fst_size, fst)) return false;
  const uint32_t count = w(&fst[8]);
  if (count < 1 || uint64_t(count) * 12 > fst_size) return false;
  const size_t names = size_t(count) * 12;
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};
  for (uint32_t i = 1; i < count; ++i) {
    while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
    const uint8_t* e = &fst[size_t(i) * 12];
    const uint32_t name_at = w(e) & 0xFFFFFFu;
    if (names + name_at >= fst.size()) return false;
    const std::string name = lower(std::string(reinterpret_cast<const char*>(&fst[names + name_at])));
    const std::string path = dirs.back().second + "/" + name;
    if (e[0]) dirs.push_back({w(e + 8), path});
    else out.push_back({path, w(e + 4), w(e + 8)});
  }
  return true;
}
void scan_disc(const std::string& profile, const std::string& iso, const std::string& cache_dir) {
  struct stat st{};
  if (stat(iso.c_str(), &st) != 0) return;
  const std::string cache = cache_dir + "/skins-" + profile + ".txt";
  const std::string stamp = std::to_string(uint64_t(st.st_size)) + " " + std::to_string(uint64_t(st.st_mtime));
  std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> found;
  bool cached = false;
  if (FILE* f = std::fopen(cache.c_str(), "r")) {
    char line[512];
    if (std::fgets(line, sizeof line, f) && stamp + "\n" == line) {
      cached = true;
      char path[256]; unsigned off = 0, len = 0;
      while (std::fgets(line, sizeof line, f)) if (std::sscanf(line, "%255s %u %u", path, &off, &len) == 3) found.push_back({path, {off, len}});
    }
    std::fclose(f);
  }
  if (!cached) {
    std::vector<DiscFile> files;
    if (!disc_files(iso, files)) { say("cosmetics: %s: not a disc image this can read", iso.c_str()); return; }
    std::vector<uint8_t> mine, theirs;
    for (const auto& f : files) {
      if (!source_port::skins::is_costume_file(f.path)) continue;
      const auto r = retail.find(f.path);
      if (r == retail.end()) continue;
      if (f.length == r->second.second) {   // same size: compare the bytes
        if (!read_file(iso, f.offset, f.length, mine) || !read_retail(f.path, theirs) || mine == theirs) continue;
      }
      found.push_back({f.path, {f.offset, f.length}});
    }
    mkdir(cache_dir.c_str(), 0777);
    if (FILE* out = std::fopen(cache.c_str(), "w")) {
      std::fprintf(out, "%s\n", stamp.c_str());
      for (const auto& [path, at] : found) std::fprintf(out, "%s %u %u\n", path.c_str(), at.first, at.second);
      std::fclose(out);
    }
  }
  for (const auto& [path, at] : found) add_skin({profile + " disc", path, {iso, at.first, at.second}});
  say("cosmetics: %s: %zu costume%s join the skins%s", base(iso).c_str(), found.size(), found.size() == 1 ? "" : "s", cached ? " (from the cache)" : "");
}
// The active profile's stage alternates named for a held button.
uint32_t alternate_button(const std::string& variant) {
  static const struct { const char* tag; uint32_t bit; } kButtons[] = {{"(l)", 0x40}, {"(r)", 0x20}, {"(z)", 0x10}, {"(b)", 0x200}, {"(x)", 0x400}, {"(y)", 0x800}};
  const std::string v = lower(variant);
  for (const auto& b : kButtons) if (v.find(b.tag) != std::string::npos) return b.bit;
  if (v == "alt l") return 0x40;
  if (v == "alt r") return 0x20;
  return 0;
}
void scan_alternates(const std::string& iso) {
  std::vector<DiscFile> files;
  if (!disc_files(iso, files)) return;
  size_t n = 0;
  for (const auto& f : files) {
    const std::string name = base(f.path);
    std::string slot, label, variant;
    const size_t dash = name.find('-'), dot = name.rfind('.');
    if (dash != std::string::npos && dot != std::string::npos && dot > dash + 1 && name.compare(dot, 4, ".dat") == 0) {
      if (!stage_name(name.substr(0, dash) + ".dat", &slot, &label)) continue;
      variant = name.substr(dash + 1, dot - dash - 1);
    } else if (dot != std::string::npos && name.size() == dot + 4 && (name.compare(dot, 4, ".lat") == 0 || name.compare(dot, 4, ".rat") == 0)) {
      if (!stage_name(name.substr(0, dot) + ".dat", &slot, &label)) continue;
      variant = name.compare(dot, 4, ".lat") == 0 ? "alt L" : "alt R";
    } else continue;
    const uint32_t button = alternate_button(variant);
    if (!button) continue;
    Skin s{"alternate " + variant, slot, {iso, f.offset, f.length}};
    s.stage = true; s.button = button;
    add_skin(std::move(s)); ++n;
  }
  if (n) say("cosmetics: %s: %zu stage alternate%s picked by a held button", base(iso).c_str(), n, n == 1 ? "" : "s");
}

// ---- commands ----
std::string costume_slot_file(int css_character, int costume) {   // upstream's costume_slot_file
  static constexpr const char* codes[] = {"Ca", "Dk", "Fx", "Gw", "Kb", "Kp", "Lk", "Lg", "Mr", "Ms", "Mt", "Ns", "Pe",
                                          "Pk", "Pp", "Pr", "Ss", "Ys", "Zd", "Sk", "Fc", "Cl", "Dr", "Fe", "Pc", "Gn"};
  if (css_character < 0 || css_character >= int(sizeof codes / sizeof codes[0]) || costume < 0) return {};
  for (const auto& family : families) {
    if (std::string(family.file_code) != codes[css_character]) continue;
    const std::string colors = family.colors;
    const size_t at = size_t(costume) * 3;
    if (at + 2 > colors.size()) return {};
    return lower(std::string("/pl") + family.file_code + colors.substr(at, 2) + ".dat");
  }
  return {};
}
// The next pick of a slot in cycle order (the standard file, then each skin), skipping held-button alternates.
std::string step_pick(const std::string& slot, int direction) {
  const auto& list = skins[slot];
  std::vector<std::string> order{""};
  for (const auto& s : list) if (!s.button) order.push_back(s.name);
  const std::string now = picks.count(slot) ? picks[slot] : std::string();
  size_t at = 0;
  for (size_t i = 0; i < order.size(); ++i) if (order[i] == now) at = i;
  at = (at + (direction > 0 ? 1 : order.size() - 1)) % order.size();
  return order[at];
}
bool may_change() {
  if (!enabled || !ready) return false;
  if (online() || replay_playing()) return false;   // never during an online session or a replay
  return true;
}
int skin_cycle(uint8_t, const uint8_t* p, uint32_t size, std::vector<uint8_t>& reply) {   // 0xF9
  std::lock_guard<std::mutex> lock(mu);
  reply.assign(1, 0);
  if (size < 4 || !may_change()) return 1;
  const int character = p[1], direction = p[3] ? 1 : -1;
  int costume = p[2];
  const uint32_t allowed = size >= 5 ? p[4] : 0;
  std::string slot = costume_slot_file(character, costume);
  if (slot.empty() || (skins[slot].empty() && !allowed)) return 1;
  if (skins[slot].empty()) return 1;   // upstream also steps to another colour's skins; here a colour's own only
  const std::string next = step_pick(slot, direction);
  if (next.empty()) picks.erase(slot); else picks[slot] = next;
  if (!apply(slot)) { publish(slot, nullptr); if (!next.empty()) { picks.erase(slot); } }
  save_picks();
  const std::string label = picks.count(slot) ? picks[slot] : std::string("Standard");
  reply[0] = 1;
  if (allowed) reply.push_back(uint8_t(costume));
  reply.insert(reply.end(), label.begin(), label.begin() + std::min<size_t>(label.size(), 63));
  say("cosmetics: character select picked %s for %s", label.c_str(), slot.c_str());
  return 1;
}
std::string stage_slot(const uint8_t* name, uint32_t size) {
  std::string file;
  for (uint32_t i = 0; i < size && name[i]; ++i) file.push_back(char(name[i]));
  file = lower(file);
  if (file.empty()) return {};
  if (file[0] != '/') file = "/" + file;
  return file;
}
int stage_cycle(uint8_t, const uint8_t* p, uint32_t size, std::vector<uint8_t>& reply) {   // 0xFB
  std::lock_guard<std::mutex> lock(mu);
  reply.assign(1, 0);
  if (size < 2 || !may_change()) return 1;
  const std::string slot = stage_slot(p + 1, size - 1);
  if (skins[slot].empty()) return 1;
  const std::string next = step_pick(slot, p[0] ? 1 : -1);
  if (next.empty()) picks.erase(slot); else picks[slot] = next;
  if (!apply(slot)) publish(slot, nullptr);
  save_picks();
  reply[0] = 1;
  say("cosmetics: stage select picked %s for %s", next.empty() ? "Standard" : next.c_str(), slot.c_str());
  return 1;
}
int skin_growth(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>& reply) {   // 0xFC: the four largest growths of picked costumes
  std::lock_guard<std::mutex> lock(mu);
  std::vector<uint32_t> growth;
  for (const auto& [slot, name] : picks) {
    const Skin* s = find_skin(slot, name);
    const auto r = retail.find(slot);
    if (s && !s->stage && r != retail.end() && s->source.length > r->second.second) growth.push_back(s->source.length - r->second.second);
  }
  std::sort(growth.rbegin(), growth.rend());
  uint32_t sum = 0;
  for (size_t i = 0; i < growth.size() && i < 4; ++i) sum += growth[i];
  reply = {uint8_t(sum >> 24), uint8_t(sum >> 16), uint8_t(sum >> 8), uint8_t(sum)};
  return 1;
}
int stage_match(uint8_t, const uint8_t* p, uint32_t size, std::vector<uint8_t>& reply) {   // 0xFD: a match's stage, before its preload
  std::lock_guard<std::mutex> lock(mu);
  reply.assign(1, 0);
  if (!enabled || !ready || replay_playing()) return 1;
  const std::string slot = stage_slot(p, size);
  const bool had = match_alternate.erase(slot) > 0;
  // Held buttons (upstream's order: L, R, Z, B, X, Y) pick an alternate the disc names for one; offline only.
  if (!online()) {
    uint32_t held = 0;
    if (const MuPadStatus* pads = pads_raw()) for (int i = 0; i < 4; ++i) held |= pads[i].button;
    for (const uint32_t bit : {0x40u, 0x20u, 0x10u, 0x200u, 0x400u, 0x800u}) {
      if (!(held & bit)) continue;
      for (const auto& s : skins[slot]) if (s.button == bit) { match_alternate[slot] = s.name; break; }
      if (match_alternate.count(slot)) break;
    }
  }
  if (match_alternate.count(slot)) {
    say("cosmetics: held button picks %s for %s", match_alternate[slot].c_str(), slot.c_str());
    if (apply(slot)) reply[0] = 1;
  } else if (had) {   // the last match's alternate goes: the pick (or the standard stage) again
    if (!apply(slot)) publish(slot, nullptr);
    reply[0] = 1;
  }
  return 1;
}
bool setting(const std::string& key, const std::string& value) {
  if (key != "cosmetics") return false;
  enabled = value != "0";
  return true;
}
}  // namespace

// ---- calls from mods.cpp (feature_calls.h) ----
// The retail disc's table is loaded (before any mod layer changes it): remember the slots, find the skins, serve the picks.
void cosmetics_disc_ready(bool can_go_online, const std::string& active_profile) {
  std::lock_guard<std::mutex> lock(mu);
  online_session = can_go_online;
  if (!enabled) return;
  folder = options.out_dir + "/Cosmetics";
  // Every costume and stage slot of the retail disc.
  for (const auto& family : families) {
    const std::string colors = family.colors;
    for (size_t at = 0; at + 2 <= colors.size(); at += 3) {
      const std::string slot = lower(std::string("/pl") + family.file_code + colors.substr(at, 2) + ".dat");
      uint32_t start = 0, length = 0;
      if (linux_host::fe_disc_find(slot.c_str(), &start, &length) >= 0) retail[slot] = {start, length};
    }
  }
  for (const char* token : kStageTokens) {
    const std::string slot = std::string("/") + token + ".dat";
    uint32_t start = 0, length = 0;
    if (linux_host::fe_disc_find(slot.c_str(), &start, &length) >= 0) retail[slot] = {start, length};
  }
  scan_imports();
  const std::string mods = options.out_dir + "/mods";
  for (const auto& profile : list_dir(mods)) {
    if (profile == "Downloads" || profile == "Profiles" || !is_dir(mods + "/" + profile)) continue;
    for (const auto& f : list_dir(mods + "/" + profile)) {
      const std::string low = lower(f);
      if (low.size() < 4 || (low.compare(low.size() - 4, 4, ".iso") != 0 && low.compare(low.size() - 4, 4, ".gcm") != 0)) continue;
      if (profile == active_profile) { if (!can_go_online) scan_alternates(mods + "/" + profile + "/" + f); continue; }   // its own files are the profile's
      scan_disc(profile, mods + "/" + profile + "/" + f, options.out_dir + "/mods-cache");
    }
  }
  load_picks();
  size_t costumes = 0, stages = 0;
  for (const auto& [slot, list] : skins) for (const auto& s : list) (s.stage ? stages : costumes) += 1;
  ready = true;
  // --feature-test cosmetics-verdicts: every costume skin judged now, with upstream's online rule and its reason.
  if (feature_test("cosmetics-verdicts"))
    for (auto& [slot, list] : skins)
      for (auto& s : list) {
        if (s.stage) continue;
        std::vector<uint8_t> bytes;
        if (!read_source(s.source, bytes)) continue;
        judge(s, bytes);
        say("cosmetics: verdict %s \"%s\": %s online (%s)", slot.c_str(), s.name.c_str(), s.online_ok ? "stays on" : "the standard costume", s.reason.c_str());
      }
  if (costumes || stages) say("cosmetics: %zu costume skins and %zu stage skins; %zu picks kept", costumes, stages, picks.size());
}
// After the mod layers are on the table: the picks over them.
void cosmetics_apply_picks() {
  std::lock_guard<std::mutex> lock(mu);
  if (!enabled || !ready) return;
  for (const auto& [slot, name] : picks) {
    if (!apply(slot)) { say("cosmetics: %s for %s is not served now%s", name.c_str(), slot.c_str(), online_form() ? " (online: the standard file)" : ""); continue; }
    const Skin* s = find_skin(slot, name);
    say("cosmetics: %s serves %s%s", slot.c_str(), name.c_str(), online_form() && s && s->online_ok ? " (online form: the standard rest pose)" : "");
  }
}
// The content view changed (command 0xF5; mods.cpp has put its files for that view back): the picks in that view's form.
void cosmetics_view(bool retail_files) {
  std::lock_guard<std::mutex> lock(mu);
  retail_view = retail_files;
  if (!enabled || !ready) return;
  for (const auto& [slot, name] : picks) if (!apply(slot)) publish(slot, nullptr);
}
// A mod profile's costume in a session that can go online (the pack skin rule): its online form, served from this module's
// range, when it passes upstream's rule. False: the standard file stays.
bool cosmetics_online_copy(const std::string& slot, const std::vector<uint8_t>& bytes, std::string* reason) {
  std::lock_guard<std::mutex> lock(mu);
  std::vector<uint8_t> clean;
  if (!read_retail(slot, clean)) { *reason = "no standard costume"; return false; }
  std::string why, draw;
  const bool ok = host::cosmetics::costume_skeleton_matches(clean, bytes, &why) && host::cosmetics::costume_draw_safe(bytes, &draw);
  *reason = draw.empty() ? why : draw;
  if (!ok) return false;
  auto copy = std::make_shared<std::vector<uint8_t>>(bytes);
  host::cosmetics::costume_take_standard_bones(clean, *copy);
  publish(slot, std::move(copy));
  return true;
}
// A read inside this module's range: -1 not ours, 0 failed, 1 done.
int cosmetics_disc_read(uint64_t offset, void* out, size_t bytes) {
  if (offset < kRangeBase || offset >= kRangeEnd) return -1;
  std::lock_guard<std::mutex> lock(mu);
  auto after = std::upper_bound(served.begin(), served.end(), offset, [](uint64_t at, const Served& s) { return at < s.start; });
  if (after == served.begin()) return 0;
  const Served& s = *(after - 1);
  const uint64_t at = offset - s.start;
  if (at > s.length || bytes > ((uint64_t(s.length) + 0x1F) & ~uint64_t(0x1F)) - std::min<uint64_t>(at, s.length)) return 0;
  const size_t real = size_t(std::min<uint64_t>(bytes, s.length - at));
  std::memcpy(out, s.bytes->data() + at, real);
  std::memset(static_cast<uint8_t*>(out) + real, 0, bytes - real);   // the disc pads a file to 32 bytes
  return 1;
}
// For the Mods page: how many costume skins a mod disc adds (0: none or not scanned).
size_t cosmetics_disc_skin_count(const std::string& profile) {
  std::lock_guard<std::mutex> lock(mu);
  size_t n = 0;
  for (const auto& [slot, list] : skins) for (const auto& s : list) n += s.name == profile + " disc";
  return n;
}

// The note when skins are off online (mu_panel.cpp, upstream's draw_skin_online_notice): the picked skins served as the
// standard costume in this online session, and the picked stage skins (always offline only). Render thread; 0 offline.
uint32_t cosmetics_off_online(uint32_t* stages) {
  std::lock_guard<std::mutex> lock(mu);
  uint32_t off = 0, off_stages = 0;
  if (enabled && online_form()) {
    for (const auto& [slot, name] : picks) {
      const auto it = skins.find(slot);
      if (it == skins.end()) continue;
      for (const Skin& s : it->second) {
        if (s.name != name) continue;
        if (s.stage) ++off_stages;
        else if (s.checked && !s.online_ok) ++off;
      }
    }
  }
  if (stages) *stages = off_stages;
  return off;
}

void register_cosmetics() {
  add_setting(setting);
  add_command(0xF9, skin_cycle);
  add_command(0xFB, stage_cycle);
  add_command(0xFC, skin_growth);
  add_command(0xFD, stage_match);
}
}  // namespace fe::features
