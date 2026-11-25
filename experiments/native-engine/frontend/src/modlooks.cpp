// melee-unlocked's "Get mod looks" (its port/runtime/host/cosmetic_mods.cpp, One-click mod looks, Windows only upstream): Diet
// Melee (Diet Classic, Crystal, Diet 64) and Animelee (Complete Edition, and its Dragon Ball Z edition) installed in one click
// from the authors' own downloads, as upstream does it:
//   - the authors' MediaFire page is fetched (upstream's browser-like user agent) and only the file's download address it names
//     is followed, on MediaFire's own download hosts; a page without one (changed, or asking for a person) stops the install
//     and is opened in the player's browser for them;
//   - the file must match the size and SHA-256 upstream pins before anything is unpacked;
//   - the patches inside are checked against their pins and applied to the player's own retail disc (also checked) with the mod
//     installer's VCDIFF decoder (mu-engine/modinstall, which reads xdelta3's patches), as the authors' patchers apply them:
//     Diet Classic from the retail disc, Crystal and Diet 64 by applying their difference to the Classic patch first;
//   - each built disc must match its pin. Nothing is re-hosted.
// Ported from the macOS build of this engine (rkoh46/melee-unlocked-macOS, commit 1864d3f, fe/features/modlooks.cpp) to the Switch:
// each built disc goes into the app's mods folder as a profile of its own (mods/<profile>/<profile>.iso), so it can be played as a
// mod and its costumes are offered as skins (features/cosmetics.cpp); the Diet and Crystal look presets draw with these discs
// (mods.cpp, offline, from the next start). Downloads go through the services program's libcurl (src/looks_fetch_curl.cpp; the
// program without services says so). A page that asks for a person cannot be opened on the console: the row says to get the file
// on a computer. Tests (MU_LOOKS_TEST_DIR): every download is answered from files in that folder (named after the address) and the
// pins come from its pins.txt; nothing reaches the network.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features/features_internal.h"
#include "frontend.h"
#include "modinstall/mi_io.h"
#include "modinstall/mi_archive.h"
#include "modinstall/mi_vcdiff.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// src/looks_fetch_curl.cpp, in the services program only: one HTTPS download with upstream's user agent, redirects on HTTPS only,
// at most `limit` bytes. Absent (weak) in the program without services.
__attribute__((weak)) bool fe_looks_https_get(const std::string& url, const std::string& path, uint64_t limit, std::string* error);

namespace fe::features {
// gfx_render.cpp refers to this so the object is linked (registry.cpp reaches register_modlooks weakly).
extern const int modlooks_linked = 1;
namespace {
namespace mi = modinstall;

struct Pin { std::string name; uint64_t size; std::string sha; };
// Upstream's pins (cosmetic_mods.cpp, melee-unlocked v1.0.5).
const char* const kDietPage = "https://www.mediafire.com/file/v6v5yuqd9le7jav/DietMeleeLinuxPatcher.tar.gz/file";
const Pin kDietArchive{"DietMeleeLinuxPatcher.tar.gz", 196228815, "3f27e034447aa9cdf93b401d78dc5c8f802cb504d416370e55dfb600b2af0a31"};
const Pin kDietClassicPatch{"DietMeleeClassic.xdelta", 186901802, "46eb93b7df529fdf7d988acb01483f5aee9e9dae6873e3fd28a282629864ab2e"};
const Pin kDietDiffs[2] = {{"crystaldiff.xdelta", 10666646, "e4256addf1ae46c9c61ca3b648a6e0a1afc7cd8bda30a19203c3bf91a759b76d"},
                           {"64diff.xdelta", 13726850, "f8992c8c5efdf66b1a0979ce361263059dc0615921a6df3b7ce7a3e9c6cfe00a"}};
const Pin kDietDiscs[3] = {{"Diet Classic.iso", 287071536, "6164d2e36bbc60e1b8f4ff6665c4eeb0dc43cece1803fe592b6346f2da517f58"},
                           {"Crystal.iso", 251487184, "6bb7bd8bc6b824623ab54f1db932a24983286f7b9040a27385d93f7153947bbf"},
                           {"Diet 64.iso", 258289764, "132bc40e3a9281e35d0294c8259f22df48b6ef121edc113f0568a025445fc60f"}};
const char* const kAnimeleePages[2] = {"https://www.mediafire.com/file/2ztqob93nrug8yg/ANIMELEE_-_COMPLETE_EDITION_-_PATCH_1.0.zip/file",
                                       "https://www.mediafire.com/file/pr8in3kum9kkvg4/ANIMELEE_-_CE_-_DRAGON_BALL_Z_v1.0.zip/file"};
const Pin kAnimeleeArchives[2] = {{"Animelee CE 1.0.zip", 31549722, "8ed24c56cbcb46a7030fd56560563b85836cd7f44fc547de23fa5aa7a1621a3c"},
                                  {"Animelee DBZ 1.0.zip", 31109499, "8c5d7c091b3ba261dbd6cc9ebc46f8a6ce3d9ed4add25c3f5b35d9f26765b41c"}};
const Pin kAnimeleePatches[2] = {{"ANIMELEE - COMPLETE EDITION.xdelta", 13882404, "4a3fde4a2119ce57e7894de99fce640063052f051c2395168c492fcc606d5b76"},
                                 {"ANIMELEE - CE - DRAGON BALL Z.xdelta", 13432818, "18a63bd7d46eb516ffd7415b26e46abc31df6ad0e8dbf2c07d6c979a76a54d78"}};
const Pin kAnimeleeDiscs[2] = {{"Animelee CE 1.0.iso", 1434723876, "77607e4624f6576f2da35584e71116809db44863ecd768c9d94c0e742da1a94d"},
                               {"Animelee DBZ 1.0.iso", 1437484160, "ecc4750c121c948bb7bff7818dcc037e12b20f39e42ffc76496d72ea8b648bc3"}};
const Pin kRetail{"retail", 1459978240, "0de05981a34156b9cedcef73c73d4244ac05cf6149ab3c9cfed917698819e464"};
const char* const kNames[3] = {"Diet Melee", "Animelee", "Animelee x DBZ"};
const char* const kProfiles[5] = {"diet-classic", "crystal", "diet-64", "animelee", "animelee-dbz"};

// ---- tests: pins and downloads from MU_LOOKS_TEST_DIR ----
std::string test_dir() { const char* d = std::getenv("MU_LOOKS_TEST_DIR"); return d ? d : ""; }
Pin pin(const Pin& p) {
  const std::string dir = test_dir();
  if (dir.empty()) return p;
  std::ifstream in(dir + "/pins.txt");
  std::string line;
  while (std::getline(in, line)) {   // name<TAB>size<TAB>sha
    std::istringstream f(line); std::string name, size, sha;
    if (std::getline(f, name, '\t') && std::getline(f, size, '\t') && std::getline(f, sha, '\t') && name == p.name) return {p.name, std::stoull(size), sha};
  }
  return p;
}
std::string test_file_for(const std::string& url) {
  std::string n;
  for (char c : url) n += std::isalnum((unsigned char)c) ? c : '_';
  return test_dir() + "/" + n;
}

// ---- status for the rows ----
std::mutex g_mutex;
int g_busy = -1;
std::string g_status[3];
void status(int which, const std::string& s) { std::lock_guard<std::mutex> l(g_mutex); g_status[which] = s; say("looks: %s: %s", kNames[which], s.c_str()); }

// ---- downloads: libcurl as upstream's WinHTTP does it (HTTPS only, redirects never down to HTTP, its user agent) ----
bool https_get(const std::string& url, const std::string& path, uint64_t limit, std::string* error) {
  if (!test_dir().empty()) {   // tests: the recorded answer
    const std::string src = test_file_for(url);
    if (!mi::file_exists(src)) { *error = "The download could not be reached."; return false; }
    if (mi::file_size(src) > limit) { *error = "The download stopped before it finished."; return false; }
    std::ifstream in(src, std::ios::binary); std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << in.rdbuf(); return bool(out);
  }
  if (url.rfind("https://", 0) != 0) { *error = "The download address is not valid."; return false; }
  if (!&fe_looks_https_get) { *error = "Mod looks download in the app (this program has no network)."; return false; }
  return fe_looks_https_get(url, path, limit, error);
}
bool matches(const std::string& path, const Pin& p) {
  const Pin want = pin(p);
  return mi::file_size(path) == want.size && mi::sha256_file(path) == want.sha;
}
// The authors' MediaFire page names the file's current download address; only that address is followed, and only on
// MediaFire's own download hosts (upstream's fetch_official, the same test).
bool fetch_official(int which, const std::string& page_url, const Pin& file, const std::string& out, std::string* error) {
  const std::string page_path = out + ".page";
  if (!https_get(page_url, page_path, 4u << 20, error)) return false;
  std::string page; mi::read_text(page_path, &page, 4u << 20); mi::remove_file(page_path);
  const size_t at = page.find("href=\"https://download");
  const size_t end = at == std::string::npos ? at : page.find('"', at + 6);
  const std::string link = at == std::string::npos || end == std::string::npos ? std::string() : page.substr(at + 6, end - at - 6);
  const size_t host_end = link.find('/', 8);
  if (link.empty() || host_end == std::string::npos || host_end < 22 || link.compare(host_end - 14, 14, ".mediafire.com") != 0) {
    *error = "The authors' download page has changed or asks for a person: open " + page_url + " on a computer for now.";
    return false;
  }
  status(which, "Downloading from the authors' page...");
  if (!https_get(link, out, pin(file).size, error)) return false;
  if (!matches(out, file)) { *error = "The downloaded file is not the one the authors published; nothing was installed."; return false; }
  return true;
}

// ---- unpacking: the Diet archive (.tar.gz) and Animelee's (.zip), by name only, never outside the work folder ----
bool untar_named(const std::string& archive, const std::string& dest, const std::vector<std::string>& wanted, std::string* error) {
  gzFile gz = gzopen(archive.c_str(), "rb");
  if (!gz) { *error = "The download is damaged; try again."; return false; }
  std::vector<char> block(512), buf(1 << 20);
  bool ok = true;
  while (gzread(gz, block.data(), 512) == 512) {
    if (block[0] == 0) break;
    std::string name(block.data(), strnlen(block.data(), 100));
    const std::string prefix(block.data() + 345, strnlen(block.data() + 345, 155));
    if (!prefix.empty()) name = prefix + "/" + name;
    const uint64_t size = std::strtoull(std::string(block.data() + 124, 12).c_str(), nullptr, 8);
    const char type = block[156];
    const std::string base = name.substr(name.find_last_of('/') + 1);
    const bool keep = (type == '0' || type == 0) && std::find(wanted.begin(), wanted.end(), base) != wanted.end();
    FILE* out = keep ? std::fopen(mi::join(dest, base).c_str(), "wb") : nullptr;
    uint64_t left = (size + 511) & ~uint64_t(511), body = size;
    while (left) {
      const int n = gzread(gz, buf.data(), unsigned(std::min<uint64_t>(left, buf.size())));
      if (n <= 0) { ok = false; break; }
      if (out) { const size_t w = size_t(std::min<uint64_t>(body, uint64_t(n))); std::fwrite(buf.data(), 1, w, out); body -= w; }
      left -= uint64_t(n);
    }
    if (out) std::fclose(out);
    if (!ok) break;
  }
  gzclose(gz);
  if (!ok) *error = "The download is damaged; try again.";
  return ok;
}
class FileSink : public mi::OutputSink {
 public:
  explicit FileSink(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {}
  ~FileSink() override { if (f_) std::fclose(f_); }
  bool ok() const { return f_ != nullptr; }
  bool write(const uint8_t* d, size_t n) override { return std::fwrite(d, 1, n, f_) == n; }
  bool close() { const bool r = f_ && std::fclose(f_) == 0; f_ = nullptr; return r; }
 private:
  FILE* f_;
};
bool apply_patch(const std::string& source, const std::string& patch, const std::string& out, std::string* error) {
  mi::FileSource p(patch);
  mi::RandomFile s(source);
  FileSink o(out);
  if (!p.ok() || !s.ok() || !o.ok()) { *error = "The patch could not be opened."; return false; }
  std::string why;
  if (!mi::vcdiff_decode(p, s, o, [](uint64_t, uint64_t) { return true; }, &why)) { *error = "The patch did not apply: " + why; return false; }
  return o.close();
}
std::string retail_problem(const std::string& iso) {
  char head[8] = {};
  std::ifstream f(iso, std::ios::binary);
  if (!f || !f.read(head, 8)) return "The game disc file could not be read.";
  if (std::memcmp(head, "GALE01", 6) != 0) return "These looks need the full North American Melee disc as a plain .iso.";
  if (test_dir().empty() && uint8_t(head[7]) != 2) return "These looks need NTSC version 1.02 of the disc.";
  if (!matches(iso, kRetail)) return "The game disc is not the clean NTSC 1.02 disc the authors' patches are made for.";
  return {};
}
// A built disc into the launcher's Mods folder as a profile of its own.
bool place(const std::string& part, const std::string& profile, const std::string& disc, std::string* error) {
  (void)disc;
  const std::string dir = mi::join(options.out_dir + "/mods", profile);
  mi::make_dirs(dir);
  if (!mi::replace_file(part, mi::join(dir, profile + ".iso"))) { *error = "The built disc could not be put in the mods folder."; return false; }
  return true;
}

bool install(int which, std::string* message) {
  const std::string work = options.out_dir + "/mods/Downloads/.looks";
  for (const auto& f : mi::list_dir(work)) mi::remove_file(mi::join(work, f));
  mi::make_dirs(work);
  status(which, "Checking your game disc...");
  if (const std::string p = retail_problem(options.iso); !p.empty()) { *message = p; return false; }
  const std::string retail = options.iso;
  if (which == 0) {
    const std::string archive = mi::join(work, kDietArchive.name);
    if (!fetch_official(which, kDietPage, kDietArchive, archive, message)) return false;
    status(which, "Unpacking...");
    if (!untar_named(archive, work, {kDietClassicPatch.name, kDietDiffs[0].name, kDietDiffs[1].name}, message)) return false;
    mi::remove_file(archive);
    const std::string classic_patch = mi::join(work, kDietClassicPatch.name);
    if (!matches(classic_patch, kDietClassicPatch)) { *message = "The download does not hold the patch files these looks need (version 1.0.2)."; return false; }
    status(which, "Building Diet Classic, Crystal and Diet 64 from your disc (a few minutes)...");
    const std::string classic = mi::join(work, kDietDiscs[0].name + ".part");
    if (!apply_patch(retail, classic_patch, classic, message)) return false;
    if (!matches(classic, kDietDiscs[0])) { *message = "The built disc did not come out as expected."; return false; }
    if (!place(classic, kProfiles[0], kDietDiscs[0].name, message)) return false;
    for (int i = 0; i < 2; ++i) {
      const std::string diff = mi::join(work, kDietDiffs[i].name);
      if (!matches(diff, kDietDiffs[i])) { *message = "The patch file " + kDietDiffs[i].name + " is missing or different."; return false; }
      // As the patcher does: the difference applied to the Classic patch, the result to the retail disc.
      const std::string joined = mi::join(work, "joined.xdelta"), out = mi::join(work, kDietDiscs[i + 1].name + ".part");
      if (!apply_patch(classic_patch, diff, joined, message) || !apply_patch(retail, joined, out, message)) return false;
      mi::remove_file(joined);
      if (!matches(out, kDietDiscs[i + 1])) { *message = "The built disc did not come out as expected."; return false; }
      if (!place(out, kProfiles[i + 1], kDietDiscs[i + 1].name, message)) return false;
    }
    *message = "Installed: Diet Classic, Crystal and Diet 64 are mod profiles; the Diet and Crystal looks use them from the next start.";
  } else {
    const int v = which - 1;
    const std::string archive = mi::join(work, kAnimeleeArchives[v].name);
    if (!fetch_official(which, kAnimeleePages[v], kAnimeleeArchives[v], archive, message)) return false;
    status(which, "Unpacking...");
    std::string why;
    auto zip = mi::open_archive(archive, &why);
    const long entry = zip ? mi::find_entry(*zip, kAnimeleePatches[v].name) : -1;
    auto src = entry >= 0 ? zip->open(size_t(entry), &why) : nullptr;
    const std::string patch = mi::join(work, kAnimeleePatches[v].name);
    if (!src) { *message = "The " + std::string(kNames[which]) + " patch inside the download is missing."; return false; }
    {
      FileSink out(patch); std::vector<uint8_t> buf(1 << 20); long n;
      while ((n = src->read(buf.data(), buf.size())) > 0) if (!out.write(buf.data(), size_t(n))) { n = -1; break; }
      if (n < 0 || !out.close()) { *message = "The download is damaged; try again."; return false; }
    }
    mi::remove_file(archive);
    if (!matches(patch, kAnimeleePatches[v])) { *message = "The " + std::string(kNames[which]) + " patch inside the download is different."; return false; }
    status(which, std::string("Building ") + kNames[which] + " from your disc...");
    const std::string part = mi::join(work, kAnimeleeDiscs[v].name + ".part");
    if (!apply_patch(retail, patch, part, message)) return false;
    if (!matches(part, kAnimeleeDiscs[v])) { *message = "The built disc did not come out as expected."; return false; }
    if (!place(part, kProfiles[3 + v], kAnimeleeDiscs[v].name, message)) return false;
    *message = std::string("Installed: ") + kNames[which] + " is a mod profile on the Mods page; its costumes are skins.";
  }
  for (const auto& f : mi::list_dir(work)) mi::remove_file(mi::join(work, f));
  return true;
}
void start(int which) {
  { std::lock_guard<std::mutex> l(g_mutex); if (g_busy >= 0) return; g_busy = which; }
  std::thread([which] {
    std::string message;
    const bool ok = install(which, &message);
    status(which, message);
    say("looks: %s install %s", kNames[which], ok ? "done" : "failed");
    std::lock_guard<std::mutex> l(g_mutex); g_busy = -1;
  }).detach();
}
bool installed(int which) {
  const std::string mods = options.out_dir + "/mods/";
  if (which == 0) {
    for (int i = 0; i < 3; ++i) if (mi::file_size(mods + kProfiles[i] + "/" + kProfiles[i] + ".iso") != pin(kDietDiscs[i]).size) return false;
    return true;
  }
  return mi::file_size(mods + kProfiles[2 + which] + "/" + kProfiles[2 + which] + ".iso") == pin(kAnimeleeDiscs[which - 1]).size;
}
template <int W> std::string row_value() {
  std::lock_guard<std::mutex> l(g_mutex);
  if (g_busy == W || !g_status[W].empty()) return g_status[W];
  return installed(W) ? "Installed" : "Get (from the authors' page)";
}
template <int W> void row_press() { start(W); }
void retrace(const Retrace& info) {   // tests: MU_LOOKS_TEST_INSTALL=<0..2> starts that install at retrace 60
  static const char* t = std::getenv("MU_LOOKS_TEST_INSTALL");
  if (t && info.retrace == 60) start(std::atoi(t) % 3);
}
}  // namespace

void register_modlooks() {
  add_retrace(retrace);
  add_menu_entry({"Looks", "Get mod looks: Diet Melee (Diet Classic, Crystal, Diet 64)", row_value<0>, nullptr, row_press<0>, nullptr});
  add_menu_entry({"Looks", "Get mod looks: Animelee", row_value<1>, nullptr, row_press<1>, nullptr});
  add_menu_entry({"Looks", "Get mod looks: Animelee x DBZ", row_value<2>, nullptr, row_press<2>, nullptr});
}
}  // namespace fe::features
