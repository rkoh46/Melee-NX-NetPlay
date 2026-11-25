// Mod files for play mode: a profile is an ordered list of layers, and what its layers change on the
// disc is served to the game in place of (or beside) the retail disc's files. A reimplementation, for
// this host, of the part of melee-unlocked's Windows host that does this (port/runtime/host/
// mod_profile.*, source_mod_overlay.* and the mod section of port/app/source_host.cpp in the fetched
// tree), with its rules:
//   - a layer is a patched NTSC 1.02 disc image (compared with the retail disc file by file: only files
//     that differ or are new are taken, and they are read from the image on demand) or a memory card
//     file; later layers win over earlier ones;
//   - only files apply. Nothing from a mod's main.dol or from a save's code is ever run;
//   - a 20XX Hack Pack disc is recognised by its own files; its menu files and the files that carry its
//     code are not served (melee-unlocked's hackpack_source.h, compiled from the fetched tree, decides
//     which, and answers the game's requests for the pack's data tables);
//   - the 20XX Tournament Edition save is not a file layer: recognised, it switches the game's native TE
//     features on. Here it is recognised by the SHA-256 of the released file (upstream compares the
//     payload regions and so also accepts a copy that has been played on);
//   - a profile has its own memory card folder, so a modded session never writes the ordinary card;
//   - mod files are for offline play: an online mode that plays the retail game gets the retail files
//     back (command 0xF5, the content view).
// The comparison of a whole disc image takes minutes from an SD card, so its result is kept beside the
// other output (<out>/mods-cache) and reused while the image's size and time stamp are unchanged.
// The scripted runs never get here.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "feature_calls.h"

#include "host_services.h"
#include "hackpack_source.h"     // melee-unlocked (fetched tree): the Hack Pack's host side, header only
#include "sha256.h"              // this repository, port/runtime/host
#include "looks_live.h"          // the Diet and Crystal presets use a built look pack (mods_configure)

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <utility>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <functional>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace linux_host {   // patches/0101: the host's disc table
int32_t fe_disc_find(const char* path, uint32_t* start, uint32_t* length);
int32_t fe_disc_set(const char* path, uint32_t start, uint32_t length);
void fe_disc_remove(const char* path);
bool fe_disc_read_base(uint32_t offset, void* out, uint32_t bytes);
}

namespace fe {
namespace {
namespace hackpack = source_port::hackpack;
constexpr uint32_t kBase = 0x70000000u;          // as upstream: past the disc and its system file range
constexpr uint64_t kLimit = 0xE8000000ull;   // the skins (features/cosmetics.cpp) are served from 0xE8000000 up
constexpr char kTeSha256[] = "61834b65d921668ff11702c458a1ae6946172b0a4c8321a47e2c2ddff9aef51d";   // 20XXTE-v2d-r4-USA.gci

struct Layer { enum Kind { Iso, Gci } kind; std::string path; };
struct ModFile {
  std::string path;              // lower case, leading slash
  uint32_t start = 0, length = 0;
  int image = -1;                // index into images
  uint64_t image_offset = 0;
  std::string layer;
  bool served = false, added = false;
  uint32_t retail_start = 0, retail_length = 0;
};
struct Image { std::string path; int fd = -1; };

std::string profile_name, mods_dir, card_dir;
std::vector<Layer> layers;
std::vector<Image> images;
std::vector<ModFile> files;      // sorted by start
bool te_owned = false, pack_loaded = false, retail_view = false, configured = false;
// A session that can go online (the services program): only the profile's costumes that pass melee-unlocked's online rule
// are served (the pack skin rule, features/cosmetics.cpp), in their online form; nothing else of the profile loads.
bool costumes_only = false;
bool offline_only_program = true;   // set false by mods_session_can_go_online (play_main: the netplay host runs)
bool configured_offline_only() { return offline_only_program; }
uint32_t replaced = 0, added = 0, kept_retail = 0;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool read_at(int fd, uint64_t offset, void* out, size_t size) {
  size_t done = 0;
  while (done < size) {
    const ssize_t n = pread(fd, static_cast<uint8_t*>(out) + done, size - done, off_t(offset + done));
    if (n <= 0) return false;
    done += size_t(n);
  }
  return true;
}
bool ends_with(const std::string& text, const char* tail) { const size_t n = std::strlen(tail); return text.size() >= n && text.compare(text.size() - n, n, tail) == 0; }
std::string lower(std::string text) { for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c))); return text; }
std::string base_name(const std::string& path) { const size_t slash = path.find_last_of('/'); return slash == std::string::npos ? path : path.substr(slash + 1); }
bool read_whole(const std::string& path, std::vector<uint8_t>* out, size_t limit) {
  FILE* in = std::fopen(path.c_str(), "rb");
  if (!in) return false;
  out->clear();
  uint8_t block[65536]; size_t n;
  while ((n = std::fread(block, 1, sizeof block, in)) > 0 && out->size() < limit) out->insert(out->end(), block, block + n);
  std::fclose(in);
  return true;
}

// What a patched disc image changes: every file that is new or differs from the retail disc's.
struct Changed { std::string path; uint64_t offset; uint32_t length; };
bool compare_image(int fd, uint64_t image_size, const std::string& label, std::vector<Changed>* out, std::string* error) {
  uint8_t header[0x440];
  if (image_size < sizeof header || !read_at(fd, 0, header, sizeof header)) { *error = "cannot read the disc header"; return false; }
  static const uint8_t ids[3][8] = {{'G', 'A', 'L', 'E', '0', '1', 0, 2}, {'G', 'T', 'M', 'E', '0', '1', 0, 2}, {'G', 'A', 'L', 'E', 'R', 'M', 0, 2}};
  if (std::memcmp(header, ids[0], 8) && std::memcmp(header, ids[1], 8) && std::memcmp(header, ids[2], 8)) { *error = "not a patched NTSC 1.02 Melee disc (GALE01 revision 2)"; return false; }
  const uint64_t fst_offset = be32(header + 0x424), fst_size = be32(header + 0x428);
  if (fst_size < 12 || fst_size > 2u * 1024 * 1024 || fst_offset + fst_size > image_size) { *error = "invalid filesystem table"; return false; }
  std::vector<uint8_t> fst(static_cast<size_t>(fst_size));
  if (!read_at(fd, fst_offset, fst.data(), fst.size())) { *error = "cannot read the filesystem table"; return false; }
  const uint32_t count = be32(fst.data() + 8);
  if (count < 1 || count > 4096 || uint64_t(count) * 12 > fst_size || fst[0] == 0) { *error = "invalid filesystem entries"; return false; }
  const size_t names = size_t(count) * 12;
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};
  std::vector<uint8_t> mine(65536), theirs(65536);
  std::set<std::string> seen;
  for (uint32_t i = 1; i < count; ++i) {
    while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
    const uint8_t* entry = fst.data() + size_t(i) * 12;
    const uint32_t name_offset = be32(entry) & 0xFFFFFFu;
    if (name_offset >= fst_size - names) { *error = "invalid file name offset"; return false; }
    const char* begin = reinterpret_cast<const char*>(fst.data() + names + name_offset);
    const char* end = static_cast<const char*>(std::memchr(begin, 0, size_t(fst_size) - names - name_offset));
    if (!end || end == begin) { *error = "invalid file name"; return false; }
    const std::string path = dirs.back().second + "/" + lower(std::string(begin, end));
    if (path.find("..") != std::string::npos || !seen.insert(path).second) { *error = "invalid or duplicate path " + path; return false; }
    if (entry[0]) {
      const uint32_t next = be32(entry + 8);
      if (next <= i || next > dirs.back().first) { *error = "invalid directory range"; return false; }
      dirs.push_back({next, path});
      continue;
    }
    const uint64_t offset = be32(entry + 4), length = be32(entry + 8);
    if (offset > image_size || length > image_size - offset) { *error = "invalid file " + path; return false; }
    uint32_t retail_start = 0, retail_length = 0;
    bool identical = false;
    if (linux_host::fe_disc_find(path.c_str(), &retail_start, &retail_length) >= 0 && retail_length == length) {
      identical = true;
      for (uint64_t at = 0; at < length && identical; at += mine.size()) {
        const uint32_t n = uint32_t(std::min<uint64_t>(mine.size(), length - at));
        if (!read_at(fd, offset + at, mine.data(), n) || !linux_host::fe_disc_read_base(retail_start + uint32_t(at), theirs.data(), n)) { *error = "cannot compare " + path; return false; }
        if (std::memcmp(mine.data(), theirs.data(), n)) identical = false;
      }
    }
    if (identical) continue;
    if (length > 256u * 1024 * 1024) {
      // As upstream: a changed movie too large for the virtual range keeps the retail one; anything else is an error.
      if (ends_with(path, ".mth")) { say("mods: %s keeps the retail disc's %s (the changed movie is too large)", label.c_str(), path.c_str()); continue; }
      *error = "replacement file is too large: " + path; return false;
    }
    out->push_back({path, offset, uint32_t(length)});
  }
  return true;
}

// The comparison's result, kept while the image's size and time stamp stay the same.
std::string cache_path(const std::string& image) { return options.out_dir + "/mods-cache/" + base_name(image) + ".changed"; }
bool cache_load(const std::string& image, const struct stat& st, std::vector<Changed>* out) {
  FILE* in = std::fopen(cache_path(image).c_str(), "r");
  if (!in) return false;
  char line[1024]; unsigned long long size = 0, time = 0; unsigned count = 0;
  bool ok = std::fgets(line, sizeof line, in) && std::sscanf(line, "changed-files 1 %llu %llu %u", &size, &time, &count) == 3 &&
            size == static_cast<unsigned long long>(st.st_size) && time == static_cast<unsigned long long>(st.st_mtime);
  while (ok && out->size() < count && std::fgets(line, sizeof line, in)) {
    unsigned long long offset = 0; unsigned length = 0; char path[900];
    if (std::sscanf(line, "%llx %x %899s", &offset, &length, path) != 3) { ok = false; break; }
    out->push_back({path, offset, length});
  }
  std::fclose(in);
  if (!ok || out->size() != count) { out->clear(); return false; }
  return true;
}
void cache_save(const std::string& image, const struct stat& st, const std::vector<Changed>& changed) {
  mkdir((options.out_dir + "/mods-cache").c_str(), 0777);
  FILE* out = std::fopen(cache_path(image).c_str(), "w");
  if (!out) return;
  std::fprintf(out, "changed-files 1 %llu %llu %u\n", static_cast<unsigned long long>(st.st_size), static_cast<unsigned long long>(st.st_mtime), unsigned(changed.size()));
  for (const auto& file : changed) std::fprintf(out, "%llx %x %s\n", static_cast<unsigned long long>(file.offset), file.length, file.path.c_str());
  std::fclose(out);
}

bool add_image(const std::string& path, std::string* error) {
  const int fd = open(path.c_str(), O_RDONLY);
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); *error = "cannot open " + path; return false; }
  const std::string label = base_name(path);
  std::vector<Changed> changed;
  const bool cached = cache_load(path, st, &changed);
  if (!cached) {
    say("mods: comparing %s with the retail disc (once; the result is kept)...", label.c_str());
    if (!compare_image(fd, uint64_t(st.st_size), label, &changed, error)) { close(fd); return false; }
    cache_save(path, st, changed);
  }
  images.push_back({path, fd});
  uint64_t next = files.empty() ? kBase : uint64_t(files.back().start) + std::max<uint64_t>(0x8000u, (uint64_t(files.back().length) + 0x7FFFu) & ~uint64_t(0x7FFFu));
  size_t superseded = 0;
  for (const auto& file : changed) {
    // Later layers win: an earlier layer's copy of the same path is dropped.
    const auto earlier = std::find_if(files.begin(), files.end(), [&](const ModFile& f) { return f.path == file.path; });
    if (earlier != files.end()) { files.erase(earlier); ++superseded; }
    const uint64_t span = std::max<uint64_t>(0x8000u, (uint64_t(file.length) + 0x7FFFu) & ~uint64_t(0x7FFFu));
    if (next + span > kLimit || files.size() >= 4096) { *error = "the layers exceed the virtual disc range or 4096 files"; return false; }
    ModFile mod;
    mod.path = file.path; mod.start = uint32_t(next); mod.length = file.length;
    mod.image = int(images.size()) - 1; mod.image_offset = file.offset; mod.layer = label;
    files.push_back(mod);
    next += span;
  }
  say("mods: %s: %zu files differ from the retail disc%s%s", label.c_str(), changed.size(), cached ? " (from the kept comparison)" : "",
      superseded ? "; some replace an earlier layer's" : "");
  return true;
}

bool read_mod(const ModFile& file, uint64_t at, void* out, size_t size) { return read_at(images[size_t(file.image)].fd, file.image_offset + at, out, size); }

// A memory card file: the recognised 20XX Tournament Edition save switches its native features on and is
// not put on the card (its code is never loaded); any other save is copied to the profile's card once.
void add_card_file(const std::string& path) {
  std::vector<uint8_t> bytes;
  if (!read_whole(path, &bytes, 4u << 20)) { say("mods: cannot read %s", path.c_str()); return; }
  if (host::memory_sha256(bytes.data(), bytes.size()) == kTeSha256) {
    te_owned = true;
    say("mods: %s is the 20XX Tournament Edition save (v2d r4): its features are the game's native ones; the save itself is not loaded", base_name(path).c_str());
    return;
  }
  const std::string target = card_dir + "/" + base_name(path);
  struct stat st{};
  if (stat(target.c_str(), &st) == 0) { say("mods: %s is already on the profile's memory card", base_name(path).c_str()); return; }
  mkdir(card_dir.c_str(), 0777);
  if (FILE* out = std::fopen(target.c_str(), "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), out); std::fclose(out);
    say("mods: %s copied to the profile's memory card (not recognised as 20XX TE: an ordinary save for the game to read)", base_name(path).c_str());
  }
}

void publish(bool retail) {
  for (auto& file : files) {
    if (!file.served) continue;
    if (!retail) linux_host::fe_disc_set(file.path.c_str(), file.start, file.length);
    else if (file.added) linux_host::fe_disc_remove(file.path.c_str());
    else linux_host::fe_disc_set(file.path.c_str(), file.retail_start, file.retail_length);
  }
  retail_view = retail;
}

void append_be32(std::vector<uint8_t>& out, uint32_t v) { out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
}  // namespace

// ---- the other mod discs melee-unlocked's host recognises (port/app/source_host.cpp, after the layers load) ----
// Training Mode CE: its /TM/ files feed the game's native TM-CE (MU_GAME_OPTION_TMCE, added to the options word
// by the feature modules offline); like upstream, not with Akaneia. Akaneia: one disc with the m-ex tables and its
// fighters' files. rogueMelee: one disc whose id is GALERM (MU_MOD_ROGUE). Melee Maker: one disc whose main.dol
// carries the supported build id (MU_MOD_MAKER; its bone tables reach the game through command 0xEE, as
// port/runtime/host/maker_source.h serves them; reimplemented here without that header's dependencies).
std::string mods_layer_fingerprint(const std::string& iso, std::string* error);   // mod_fingerprint.cpp
namespace {
bool tmce = false, akaneia = false, rogue = false, maker = false, arcade_pack = false;
// melee-unlocked's known packs by their layer fingerprint (port/app/source_host.cpp kKnownPacks): the 2D-style m-ex build's two
// versions switch its native camera and HUD codes on (MU_GAME_OPTION2_ARCADE_PACK, offline and in its replays) and the Arcade
// look's hitboxes and 2D camera; the others confirm the computation.
struct KnownPack { const char* fingerprint; const char* name; };
constexpr KnownPack kKnownPacks[] = {
  {"9a1a48b999a2f8f4deaee2554e5f177cb265666b95676a508406db80b181246b", "Akaneia 1.0.1"},
  {"23859e58d0f6153bbc9a813f4eac2b3ae1b69e90d5e5a0ef1f57829b1ed588ff", "2D-style build 2.0"},
  {"59933964cb939eb7e32c05f4b9cdfb332dd12043a3c82ccb9d1f204c7a314891", "2D-style build demo"},
};
// The fingerprint reads every file the disc changes; it is kept in mods-cache while the image's size and time are the same.
std::string cached_fingerprint(const std::string& iso) {
  struct stat st{};
  if (stat(iso.c_str(), &st) != 0) return {};
  const std::string cache = options.out_dir + "/mods-cache/" + base_name(iso) + ".fingerprint";
  const std::string stamp = std::to_string(uint64_t(st.st_size)) + " " + std::to_string(uint64_t(st.st_mtime));
  if (FILE* f = std::fopen(cache.c_str(), "r")) {
    char line[256] = {};
    const bool got = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    std::string text = got ? std::string(line) : std::string();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.size() > stamp.size() + 1 && text.compare(0, stamp.size() + 1, stamp + " ") == 0) return text.substr(stamp.size() + 1);
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::string error;
  const std::string fp = mods_layer_fingerprint(iso, &error);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (fp.empty()) { say("mods: %s: no content fingerprint (%s)", iso.c_str(), error.c_str()); return {}; }
  say("mods: fingerprinted %s in %.1f s", iso.c_str(), seconds);
  mkdir((options.out_dir + "/mods-cache").c_str(), 0777);
  if (FILE* f = std::fopen(cache.c_str(), "w")) { std::fprintf(f, "%s %s\n", stamp.c_str(), fp.c_str()); std::fclose(f); }
  return fp;
}
std::string fingerprint;                         // the replay's modProfile: empty for the retail files
constexpr char kMakerBuild[] = "1.3.8-1865b7";
struct MakerDol { std::vector<uint8_t> dol; uint32_t offset[18] = {}, address[18] = {}, size[18] = {}; } maker_dol;
uint32_t maker_cache_base = 0, maker_cache_size = 0;
bool maker_cache_tried = false;

bool maker_attach(const std::string& iso) {
  const int fd = open(iso.c_str(), O_RDONLY);
  if (fd < 0) return false;
  uint8_t head[4]{}, hdr[0x100]{};
  bool ok = read_at(fd, 0x420, head, 4) && read_at(fd, be32(head), hdr, sizeof hdr);
  uint32_t extent = sizeof hdr;
  for (int i = 0; ok && i < 18; ++i) {
    maker_dol.offset[i] = be32(hdr + i * 4); maker_dol.address[i] = be32(hdr + 0x48 + i * 4); maker_dol.size[i] = be32(hdr + 0x90 + i * 4);
    if (maker_dol.size[i]) extent = std::max(extent, maker_dol.offset[i] + maker_dol.size[i]);
  }
  if (ok && extent <= 16u * 1024u * 1024u) { maker_dol.dol.resize(extent); ok = read_at(fd, be32(head), maker_dol.dol.data(), extent); }
  else ok = false;
  close(fd);
  const char* end = kMakerBuild + sizeof kMakerBuild;      // with its terminator
  if (!ok || std::search(maker_dol.dol.begin(), maker_dol.dol.end(), kMakerBuild, end) == maker_dol.dol.end()) { maker_dol = MakerDol{}; return false; }
  return true;
}

// After the layers: which of these the profile is, and its content identity for replays.
void identify(const std::function<bool(const char*)>& has) {
  std::vector<const Layer*> isos;
  for (const auto& layer : layers) if (layer.kind == Layer::Iso) isos.push_back(&layer);
  const bool one_iso = isos.size() == 1;
  akaneia = one_iso && has("/mxdt.dat") && has("/plsn.dat") && has("/plts.dat");
  if (akaneia) say("mods: an m-ex disc with added fighters (Akaneia or ACE) found; its fighters are the game's native ones (offline; online matches get the retail files)");
  if (has("/tm/eventmenu.dat")) {
    if (akaneia) say("mods: Training Mode CE files found, but Training Mode CE needs the vanilla game; left off");
    else { tmce = true; say("mods: Training Mode CE files found; its events run natively (Event Match)"); }
  }
  if (one_iso) {
    char id[6] = {};
    if (FILE* disc = std::fopen(isos[0]->path.c_str(), "rb")) { if (std::fread(id, 1, sizeof id, disc) != sizeof id) id[0] = 0; std::fclose(disc); }
    if (std::memcmp(id, "GALERM", sizeof id) == 0) { rogue = true; say("mods: rogueMelee disc found; Rogue Mode runs natively (1-P Mode > Regular Match)"); }
    else if (maker_attach(isos[0]->path)) { maker = true; say("mods: Melee Maker %s disc found; its builds run natively", kMakerBuild); }
    const std::string fp = cached_fingerprint(isos[0]->path);
    const KnownPack* known = nullptr;
    for (const auto& pack : kKnownPacks) if (fp == pack.fingerprint) known = &pack;
    say("mods: content %s%s%s", fp.substr(0, 16).c_str(), known ? ", recognized as " : "", known ? known->name : "");
    if (known && std::strncmp(known->name, "2D-style build", 14) == 0) {
      arcade_pack = true;
      say("mods: %s found; its camera and HUD codes run natively (offline), with the Arcade look's hitboxes and 2D camera", known->name);
    }
  }
  // Content identity (metadata key modProfile): what the layers change, not upstream's whole-disc hash (that
  // reads every disc in full); the same profile gives the same identity. Saves alone change no disc file.
  if (std::any_of(files.begin(), files.end(), [](const ModFile& f) { return f.served; })) {
    std::string text;
    for (const auto& file : files)
      if (file.served) text += file.path + "\t" + std::to_string(file.length) + "\t" + base_name(images[size_t(file.image)].path) + "\n";
    fingerprint = host::memory_sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }
}

// Command 0xEE, Melee Maker's data (maker_source.h): 0x01 addr(4) len(2) main.dol bytes at a console address;
// 0x02 the build id; 0x03 its loading cache (32 MB right after the game's addressable memory: here nothing is
// mapped for it, the reply is empty and the game loads its moves into the match heap, as upstream does when
// the cache cannot be mapped).
void maker_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  if (!maker || n < 1) return;
  if (p[0] == MU_MAKER_OP_DOL_READ && n == 7) {
    const uint32_t address = be32(p + 1), size = uint32_t(p[5]) << 8 | p[6];
    if (!size || size > MU_SLIPPI_RESPONSE_CAPACITY) return;
    for (int i = 0; i < 18; ++i) {
      if (!maker_dol.size[i] || address < maker_dol.address[i] || address - maker_dol.address[i] >= maker_dol.size[i] ||
          size > maker_dol.size[i] - (address - maker_dol.address[i])) continue;
      const uint8_t* src = maker_dol.dol.data() + maker_dol.offset[i] + (address - maker_dol.address[i]);
      reply.assign(src, src + size);
      return;
    }
  } else if (p[0] == MU_MAKER_OP_BUILD) {
    reply.assign(kMakerBuild, kMakerBuild + sizeof kMakerBuild - 1);
  } else if (p[0] == MU_MAKER_OP_CACHE && n == 1) {
    if (!maker_cache_tried) { maker_cache_tried = true; say("mods: Melee Maker cache not mapped on this host; its moves load into the match heap"); }
    if (maker_cache_size) { append_be32(reply, maker_cache_base); append_be32(reply, maker_cache_size); }
  }
}
}  // namespace

bool mods_tmce() { return tmce; }
bool mods_arcade_pack() { return arcade_pack && !retail_view; }
std::string mods_fingerprint() { return retail_view ? std::string() : fingerprint; }
bool mods_te_owned() { return te_owned; }
bool mods_hackpack() { return pack_loaded; }
uint32_t mods_flags() {
  return (std::any_of(files.begin(), files.end(), [](const ModFile& f) { return f.served; }) ? MU_MOD_ASSETS_PRESENT : 0u) | (pack_loaded ? MU_MOD_HACKPACK : 0u) |
         (rogue ? MU_MOD_ROGUE : 0u) | (maker ? MU_MOD_MAKER : 0u);
}
uint32_t mods_stage_bits() { return hackpack::stage_bits(); }

namespace {
void add_profile_layers(const std::string& profile) {
  if (profile.empty()) return;
  // <out>/mods/Profiles/<name>.ini in upstream's form ("iso = path", "gci = path", one layer per line, in load
  // order; relative paths from <out>/mods), or without it every .iso and then every .gci in <out>/mods/<name>/.
  const std::string file = mods_dir + "/Profiles/" + profile + ".ini";
  if (FILE* in = std::fopen(file.c_str(), "r")) {
    char line[1024];
    while (std::fgets(line, sizeof line, in)) {
      if (char* hash = std::strchr(line, '#')) *hash = 0;
      char kind[16], value[900];
      if (std::sscanf(line, " %15[a-z] = %899[^\r\n]", kind, value) != 2) continue;
      std::string path = value;
      while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
      if (path.find(":/") == std::string::npos && path[0] != '/') path = mods_dir + "/" + path;
      if (!std::strcmp(kind, "iso")) layers.push_back({Layer::Iso, path});
      else if (!std::strcmp(kind, "gci")) layers.push_back({Layer::Gci, path});
      else say("mods: %s: \"%s\" layers are not supported here (iso and gci are)", file.c_str(), kind);
    }
    std::fclose(in);
  } else {
    const std::string folder = mods_dir + "/" + profile;
    std::vector<std::string> isos, saves;
    if (DIR* dir = opendir(folder.c_str())) {
      while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name, low = lower(name);
        if (ends_with(low, ".iso") || ends_with(low, ".gcm")) isos.push_back(folder + "/" + name);
        else if (ends_with(low, ".gci")) saves.push_back(folder + "/" + name);
      }
      closedir(dir);
    }
    std::sort(isos.begin(), isos.end()); std::sort(saves.begin(), saves.end());
    for (const auto& path : isos) layers.push_back({Layer::Iso, path});
    for (const auto& path : saves) layers.push_back({Layer::Gci, path});
  }
}
}  // namespace

void mods_session_can_go_online() { offline_only_program = false; }
// A session that can go online: the profile's look-only costumes and nothing else (no memory card of its own).
void mods_configure_costumes(const std::string& profile) {
  configured = true; costumes_only = true;
  if (profile.empty()) return;
  profile_name = profile;
  mods_dir = options.out_dir + "/mods";
  for (size_t at = 0; at <= profile.size();) {
    const size_t comma = std::min(profile.find(',', at), profile.size());
    add_profile_layers(profile.substr(at, comma - at));
    at = comma + 1;
  }
  layers.erase(std::remove_if(layers.begin(), layers.end(), [](const Layer& l) { return l.kind != Layer::Iso; }), layers.end());
  say("mods: profile \"%s\" in a session that can go online: only its costumes that stay on online (the standard skeleton) are loaded", profile.c_str());
}

// Before the host starts: which layers, and which memory card folder. Returns that folder (empty: no profile).
std::string mods_configure(const std::string& profile_in) {
  configured = true;
  std::string profile = profile_in;
  bool look_pack = false;
  if (profile.empty()) {
    // The Diet and Crystal look presets draw with the player's own build of that artwork when Get mod looks has made it
    // (src/modlooks.cpp: mods/diet-classic, diet-64, crystal): that disc's files over the retail ones, offline (an online session
    // is given no profile), on the ordinary memory card (it is a look, not a mod). Without it the built-in stand-ins draw them.
    static const char* const kPack[] = {nullptr, nullptr, nullptr, "diet-classic", "diet-64", "crystal", "crystal"};
    const int preset = fe::looks::preset_of();
    if (preset >= 3 && preset <= 6) {
      const std::string disc = options.out_dir + "/mods/" + kPack[preset] + "/" + kPack[preset] + ".iso";
      struct stat st{};
      if (stat(disc.c_str(), &st) == 0) { profile = kPack[preset]; look_pack = true; say("mods: the look preset %d draws with your build of its artwork (%s)", preset, disc.c_str()); }
    }
    if (profile.empty()) return {};
  }
  profile_name = profile;
  mods_dir = options.out_dir + "/mods";
  std::string folder_name = profile;
  std::replace(folder_name.begin(), folder_name.end(), ',', '+');
  card_dir = options.out_dir + "/card-" + folder_name;
  // Several names, separated by commas, are loaded one after the other (later layers win).
  for (size_t at = 0; at <= profile.size();) {
    const size_t comma = std::min(profile.find(',', at), profile.size());
    add_profile_layers(profile.substr(at, comma - at));
    at = comma + 1;
  }
  if (layers.empty()) { say("mods: profile \"%s\": no layers found under %s (Profiles/<name>.ini or the folder <name>/); playing the retail game", profile.c_str(), mods_dir.c_str()); profile_name.clear(); return {}; }
  if (look_pack) { say("mods: look pack \"%s\", %zu layers, the ordinary memory card", profile.c_str(), layers.size()); card_dir.clear(); return {}; }
  say("mods: profile \"%s\", %zu layers, memory card folder %s", profile.c_str(), layers.size(), card_dir.c_str());
  return card_dir;
}
}  // namespace fe

// ---- hooks of the host services (patches/0101) ----

// The retail disc's table is loaded: put the profile's files over it.
extern "C" void mu_frontend_disc_ready(void) {
  using namespace fe;
  // The skins first, while the table is the retail disc's (features/cosmetics.cpp); their picks go on after the layers.
  if (options.play && &features::cosmetics_disc_ready) features::cosmetics_disc_ready(costumes_only || !configured_offline_only(), profile_name);
  struct Picks { ~Picks() { if (fe::options.play && &fe::features::cosmetics_apply_picks) fe::features::cosmetics_apply_picks(); } } picks_after;
  if (!configured) return;
  if (costumes_only) {   // the pack skin rule: each costume the profile changes, in its online form when it passes
    for (const auto& layer : layers) { std::string error; if (!add_image(layer.path, &error)) say("mods: %s NOT LOADED: %s", layer.path.c_str(), error.c_str()); }
    unsigned kept = 0, reverted = 0;
    for (const auto& file : files) {
      if (file.path.size() != 11 || file.path.compare(0, 3, "/pl") != 0 || !&features::cosmetics_online_copy) continue;
      if (linux_host::fe_disc_find(file.path.c_str(), nullptr, nullptr) < 0) continue;
      std::vector<uint8_t> bytes(file.length);
      if (!read_mod(file, 0, bytes.data(), bytes.size())) continue;
      std::string reason;
      if (features::cosmetics_online_copy(file.path, bytes, &reason)) { ++kept; say("mods: %s stays on online: %s", file.path.c_str(), reason.c_str()); }
      else if (!reason.empty() && reason != "no standard costume") { ++reverted; say("mods: %s is the standard costume online: %s", file.path.c_str(), reason.c_str()); }
    }
    say("mods: %u costumes of the profile stay on (online form), %u are the standard costume", kept, reverted);
    return;
  }
  if (settings_slippi_menus()) slippi_files_build();      // from the retail files, before any layer changes the table
  for (const auto& layer : layers) {
    if (layer.kind == Layer::Gci) { add_card_file(layer.path); continue; }
    std::string error;
    if (!add_image(layer.path, &error)) say("mods: %s NOT LOADED: %s", layer.path.c_str(), error.c_str());
  }
  const auto has = [](const char* path) { return std::any_of(files.begin(), files.end(), [&](const ModFile& f) { return f.path == path; }); };
  // The 20XX Hack Pack, by its own files (hackpack_source.h): its data tables go to the game through command
  // 0xFA; its menu files and the files that carry its code stay out.
  int pack_image = -1;
  for (const auto& file : files)
    if (hackpack::is_pack_marker(file.path) && has("/mnslchr.0sd")) {
      pack_image = file.image;
      hackpack::attach(images[size_t(file.image)].path, file.image_offset, file.length);
      pack_loaded = true;
      say("mods: 20XX Hack Pack files found in %s: its content loads with the retail menus, none of its code runs", file.layer.c_str());
    }
  for (auto& file : files) {
    if (pack_image >= 0 && file.image == pack_image && hackpack::keeps_retail(file.path)) { ++kept_retail; continue; }
    uint32_t start = 0, length = 0;
    const int32_t entry = linux_host::fe_disc_find(file.path.c_str(), &start, &length);
    if (entry == -2) { say("mods: %s is a directory on the retail disc: not served", file.path.c_str()); continue; }
    if (entry >= 0) {
      // As upstream: in place of one of the game's archives, a file whose own header names another size
      // can never be loaded; the game's file stays.
      uint8_t own[4] = {}, game[4] = {};
      if ((ends_with(file.path, ".dat") || ends_with(file.path, ".usd")) && length >= 0x20 && linux_host::fe_disc_read_base(start, game, 4) && be32(game) == length &&
          !(file.length >= 4 && read_mod(file, 0, own, 4) && be32(own) == file.length)) {
        say("mods: %s in %s is not a whole archive: the game's own file stays", file.path.c_str(), file.layer.c_str());
        continue;
      }
      file.retail_start = start; file.retail_length = length; ++replaced;
    } else { file.added = true; ++added; }
    file.served = true;
  }
  publish(false);
  identify(has);
  if (!layers.empty()) say("mods: %u disc files replaced, %u added%s", replaced, added, kept_retail ? " (the Hack Pack's menu and code files are not served)" : "");
  // Slippi's menus need Slippi's copies: they replace a layer's copy of the same file, in both views (as upstream).
  for (const std::string& path : slippi_files_publish())
    for (auto& file : files) if (file.path == path) file.served = false;
}

extern "C" int mu_frontend_disc_read(uint64_t offset, void* out, size_t bytes) {
  using namespace fe;
  if (const int system = slippi_file_read(offset, out, bytes); system >= 0) return system;
  if (&features::cosmetics_disc_read) { const int skin = features::cosmetics_disc_read(offset, out, bytes); if (skin >= 0) return skin; }
  if (offset < kBase || files.empty() || costumes_only) return -1;
  // The file whose range holds the offset (files are in start order).
  auto after = std::upper_bound(files.begin(), files.end(), offset, [](uint64_t at, const ModFile& f) { return at < f.start; });
  if (after == files.begin()) return -1;
  const ModFile& file = *(after - 1);
  const uint64_t at = offset - file.start;
  if (at > file.length || bytes > (uint64_t(file.length) + 0x1Fu & ~uint64_t(0x1Fu)) - std::min<uint64_t>(at, file.length)) return 0;
  // Through a buffer of our own: the kernel must not write into a page of the game's that is write-tracked.
  static std::vector<uint8_t> bounce;
  bounce.resize(bytes);
  const size_t real = size_t(std::min<uint64_t>(bytes, file.length - at));
  if (real && !read_mod(file, at, bounce.data(), real)) return 0;
  std::memset(bounce.data() + real, 0, bytes - real);      // the disc pads a file to 32 bytes
  std::memcpy(out, bounce.data(), bytes);
  return 1;
}

// slippi_command: the commands of the mod features. Everything here is offline: play mode has no network session.
extern "C" int32_t mu_frontend_command(uint8_t command, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity, uint32_t* response_size) {
  using namespace fe;
  // The feature modules first (feature_hooks.h); play mode only, so the scripted runs never reach them.
  if (options.play) { const int32_t taken = features::call_command(command, payload, size, response, capacity, response_size); if (taken != INT32_MIN) return taken; }
  if (!configured) return INT32_MIN;
  // Tests only (MU_TEST_TE_LOCK=1, the 20XX TE save loaded): TE's in-game settings menu's own command (0xF8) sent here as the
  // menu sends it: a change while "lock settings" is on is refused (only the lock itself may go off), then accepted once it is off.
  static bool te_lock_tested = false;
  if (!te_lock_tested && te_owned && std::getenv("MU_TEST_TE_LOCK")) {
    te_lock_tested = true;
    uint8_t r[8]; uint32_t n = 0;
    const auto send = [&](uint32_t a, uint32_t b) {
      const uint8_t p[8] = {uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a), uint8_t(b >> 24), uint8_t(b >> 16), uint8_t(b >> 8), uint8_t(b)};
      mu_frontend_command(0xF8, p, 8, r, sizeof r, &n);
      return std::pair<uint32_t, uint32_t>(be32(r), be32(r + 4));
    };
    const uint32_t w1 = settings_te_word(1), w2 = settings_te_word(2);
    const auto locked = send(w1 ^ 0x0020u, w2 | MU_GAME_OPTION2_TE_LOCK_SETTINGS);   // a change of word 1 with the lock still on
    say("mods: test: TE lock settings: words %08X %08X; a change while locked leaves %08X %08X (%s)", w1, w2, locked.first, locked.second,
        locked.first == w1 && locked.second == w2 ? "refused" : "TAKEN");
    const auto unlocked = send(w1, w2 & ~MU_GAME_OPTION2_TE_LOCK_SETTINGS);            // the lock off
    const auto changed = send(w1 ^ 0x0020u, unlocked.second);                           // the change again
    say("mods: test: TE lock settings off: %08X %08X; the change now gives %08X %08X (%s)", unlocked.first, unlocked.second, changed.first,
        changed.second, changed.first == (w1 ^ 0x0020u) ? "taken" : "REFUSED");
    send(w1, unlocked.second | (w2 & MU_GAME_OPTION2_TE_LOCK_SETTINGS));                // back as it was (the lock can only be set here by the test)
    settings_te_word(2) = w2;
  }
  std::vector<uint8_t> reply;
  const auto read32 = [&](uint32_t at) { return be32(payload + at); };
  if (command == MU_HP_COMMAND && size >= 1 && payload[0] == MU_HP_OP_CPU_WORD) {         // the pack's menu: CPU training options
    if (pack_loaded) {
      if (size == 5) { settings_cpu_word() = read32(1) & MU_GAME_OPTION3_CPU_ALL; settings_state_changed(); }
      append_be32(reply, settings_cpu_word() & MU_GAME_OPTION3_CPU_ALL);
    }
  } else if (command == MU_HP_COMMAND && size >= 1) {
    const uint32_t before = hackpack::settings().changes.load();
    hackpack::command(payload, size, reply, true);
    if (hackpack::settings().changes.load() != before) settings_state_changed();
  } else if (command == 0xF6 && size == 1) {                                               // TE: Frozen Mode from the stage select
    if (payload[0] == 0) { settings_te_word(1) ^= MU_GAME_OPTION_TE_FROZEN_STAGES; settings_state_changed(); }
    reply.push_back((settings_te_word(1) & MU_GAME_OPTION_TE_FROZEN_STAGES) ? 1 : 0);
  } else if (command == 0xF7 && size == 1) {                                               // TE: menu song from Sound Test (0x80 asks)
    if (payload[0] != 0x80) { settings_te_music() = int8_t(payload[0]); settings_state_changed(); }
    reply.push_back(uint8_t(int8_t(settings_te_music())));
  } else if (command == 0xF8 && (size == 0 || size == 8)) {                                // TE's (or the pack's) in-game settings menu
    uint32_t& w1 = settings_te_word(1); uint32_t& w2 = settings_te_word(2);
    constexpr uint32_t pack1 = MU_GAME_OPTION_TE_NO_STAR_KO | MU_GAME_OPTION_TE_TAUNT_CANCEL | MU_GAME_OPTION_TE_FIXED_CAMERA;
    constexpr uint32_t pack2 = MU_GAME_OPTION2_TE_NO_SCREEN_RUMBLE | MU_GAME_OPTION2_TE_LCANCEL_FLASH | 0x00780000u | MU_GAME_OPTION2_TE_BUBBLES |
                               MU_GAME_OPTION2_TE_INPUT_DISPLAY | MU_GAME_OPTION2_TE_COLOR_OVERLAYS;
    if (size == 8 && !te_owned && pack_loaded) {
      w1 = (w1 & ~pack1) | (read32(0) & pack1); w2 = (w2 & ~pack2) | (read32(4) & pack2);
      settings_state_changed();
    } else if (size == 8 && te_owned && (w1 & MU_GAME_OPTION_TE)) {
      const uint32_t next1 = read32(0), next2 = read32(4);
      if (w2 & MU_GAME_OPTION2_TE_LOCK_SETTINGS) { if (!(next2 & MU_GAME_OPTION2_TE_LOCK_SETTINGS)) w2 &= ~MU_GAME_OPTION2_TE_LOCK_SETTINGS; }
      else { w1 = (w1 & ~0x7FE0u) | (next1 & 0x7FE0u); w2 = next2 & ~MU_GAME_OPTION2_TE_LCANCEL_WHEELS; }
      settings_state_changed();
    }
    append_be32(reply, w1); append_be32(reply, w2);
  } else if (command == MU_MAKER_COMMAND) {                                                // Melee Maker's data
    maker_command(payload, size, reply);
  } else if (command == 0xF5 && size == 1) {                                               // content view: 0xFF offline, 0xFE asks, else an online mode
    if (payload[0] != 0xFE) {
      const bool retail = payload[0] != 0xFF && std::any_of(files.begin(), files.end(), [](const ModFile& f) { return f.served; });
      if (retail != retail_view) {
        publish(retail); linux_host::log("content: %s view", retail ? "retail" : "mod");
        if (&features::cosmetics_view) features::cosmetics_view(retail);   // the picked skins in that view's form
      }
    }
    reply.push_back(retail_view ? 1 : 0);
  } else {
    return INT32_MIN;
  }
  if (reply.size() > capacity) return -2;
  if (!reply.empty()) std::memcpy(response, reply.data(), reply.size());
  if (response_size) *response_size = uint32_t(reply.size());
  return 0;
}
