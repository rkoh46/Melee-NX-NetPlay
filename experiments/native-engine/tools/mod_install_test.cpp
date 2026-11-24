// Host test of the mod installer (frontend/src/modinstall/), built and run by tools/mod_install_test.sh on macOS or Linux:
//   1. the catalog rules (melee-unlocked's): a tampered entry is refused, one_click without a recorded licence or permission
//      is turned off;
//   2. the licence gate and the manual path: a link-only mod is never fetched, its status says where to get the file;
//   3. the free-space check refuses before anything is written;
//   4. a download interrupted and continued (a range request) gives the release's exact bytes;
//   5. every catalog mod whose official file can be fetched (GitHub release assets; the MediaFire entries cannot be, as for
//      melee-unlocked's launcher) is installed through the Manager into a scratch mods folder, and the built disc's
//      SHA-256 is compared with the hand-patched copy in <disc folder>/mods/<id>/ (and with the catalog's output_sha256).
// Downloads only from the catalog's own URLs, into one cache folder (argument 3), so each file is fetched once.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_catalog.h"
#include "mi_fetch_curl.h"
#include "mi_hash.h"
#include "mi_io.h"
#include "mod_install.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace modinstall;
static int checks = 0, failed = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failed; std::printf("FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

namespace {
// Counts what the Manager asks to fetch (and fetches it with libcurl when `real`).
struct CountingFetcher : Fetcher {
  Fetcher* real = nullptr;
  std::vector<std::string> urls;
  Result fetch(const std::string& url, const std::string& path, uint64_t expected, const std::function<bool(uint64_t, uint64_t)>& progress) override {
    urls.push_back(url);
    if (!real) { Result r; r.error = "no network in this check"; return r; }
    return real->fetch(url, path, expected, progress);
  }
};
struct TinyDisk : Platform { uint64_t free_bytes(const std::string&) override { return 10u << 20; } };

bool link_or_copy(const std::string& from, const std::string& to) {
  remove_file(to);
  if (link(from.c_str(), to.c_str()) == 0) return true;
  FILE* in = std::fopen(from.c_str(), "rb");
  FILE* out = std::fopen(to.c_str(), "wb");
  if (!in || !out) { if (in) std::fclose(in); if (out) std::fclose(out); return false; }
  std::vector<char> buf(1u << 20);
  size_t n;
  bool ok = true;
  while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) ok &= std::fwrite(buf.data(), 1, n, out) == n;
  std::fclose(in);
  ok &= std::fclose(out) == 0;
  return ok;
}
// Never follows a symbolic link (mods/Downloads is one, to the download cache): the link itself is removed.
void remove_tree(const std::string& dir) {
  for (const auto& name : list_dir(dir)) {
    const std::string p = join(dir, name);
    struct stat st{};
    if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p); else unlink(p.c_str());
  }
  rmdir(dir.c_str());
}
// The SHA-256 of a hand-patched disc, kept beside the test's output while the file's size and time are the same.
std::string reference_sha(const std::string& path, const std::string& memo_dir) {
  const std::string memo = join(memo_dir, base_name(path) + ".sha256");
  const std::string key = std::to_string(file_size(path)) + " " + std::to_string(file_mtime(path));
  std::string text;
  if (read_text(memo, &text) && text.rfind(key + " ", 0) == 0) return text.substr(key.size() + 1, 64);
  const std::string sha = sha256_file(path);
  if (!sha.empty()) write_text(memo, key + " " + sha);
  return sha;
}
std::string find_reference_disc(const std::string& dir) {
  for (const auto& name : list_dir(dir)) {
    const std::string l = lower(name);
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".iso") == 0) return join(dir, name);
  }
  return {};
}
const CatalogMod* by_id(const std::vector<CatalogMod>& mods, const std::string& id) {
  for (const auto& m : mods) if (m.id == id) return &m;
  return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) { std::printf("usage: %s CATALOG.json DISC_FOLDER CACHE_DIR SCRATCH_DIR [ids...]\n", argv[0]); return 2; }
  const std::string catalog_path = argv[1], disc_folder = argv[2], cache = argv[3], scratch = argv[4];
  std::vector<std::string> only(argv + 5, argv + argc);
  std::string catalog_text;
  if (!read_text(catalog_path, &catalog_text)) { std::printf("cannot read %s\n", catalog_path.c_str()); return 2; }
  make_dirs(cache); make_dirs(scratch);
  const auto logline = [](const std::string& s) { std::printf("  %s\n", s.c_str()); std::fflush(stdout); };

  // 1. Catalog rules.
  std::vector<CatalogMod> mods;
  std::string error;
  CHECK(parse_catalog(catalog_text, &mods, &error));
  std::printf("catalog: %zu mods\n", mods.size());
  for (const auto& m : mods)
    std::printf("  %-12s %-10s policy %-18s one_click %d  direct %d  licence \"%s\"\n", m.id.c_str(), m.kind.c_str(), m.policy.c_str(), m.one_click, m.direct_file(), m.license.c_str());
  CHECK(by_id(mods, "te") && by_id(mods, "te")->one_click);
  CHECK(by_id(mods, "roguemelee") && !by_id(mods, "roguemelee")->one_click);
  {
    std::string t = catalog_text, e;
    std::vector<CatalogMod> out;
    const size_t at = t.find("\"policy\": \"link_only\"");
    CHECK(at != std::string::npos);
    // one_click true with no licence or permission recorded: listed, but never fetched automatically.
    std::string forced = t;
    for (size_t p = 0; (p = forced.find("\"one_click\": false", p)) != std::string::npos;) forced.replace(p, 18, "\"one_click\": true");
    CHECK(parse_catalog(forced, &out, &e));
    for (const auto& m : out) if (m.policy != "licensed" && m.policy != "permission_granted") CHECK(!m.one_click);
    std::string bad = t;
    const size_t s = bad.find("\"sha256\": \"");
    bad.replace(s + 11, 4, "zzzz");
    CHECK(!parse_catalog(bad, &out, &e));
    std::string http = t;
    const size_t u = http.find("\"url\": \"https://");
    http.replace(u + 8, 8, "http://");
    CHECK(!parse_catalog(http, &out, &e));
  }

  // 2. The licence gate: rogueMelee (link_only) without its file. The Manager must not fetch it and must say where to get it.
  {
    const std::string mods_dir = join(scratch, "gate/mods");
    remove_tree(join(scratch, "gate"));
    make_dirs(mods_dir);
    CountingFetcher counter;
    Config c;
    c.mods_dir = mods_dir; c.disc_path = join(disc_folder, "GALE01.iso"); c.catalog_json = catalog_text; c.fetcher = &counter;
    Manager m(c);
    CHECK(m.catalog_ok());
    const ModStatus before = m.status("roguemelee");
    CHECK(!before.automatic && !before.file_ready && before.state == State::NotInstalled);
    CHECK(m.install("roguemelee"));
    m.wait();
    const ModStatus after = m.status("roguemelee");
    std::printf("gate: %s\n", after.line.c_str());
    CHECK(after.failed && after.line.find("github.com/hatsonhats/rogueMelee-v2") != std::string::npos && after.line.find("mods/Downloads") != std::string::npos);
    CHECK(counter.urls.empty());
    // MediaFire entries are pages, not files: never fetched either.
    for (const auto& mod : m.catalog()) if (mod.url.find("mediafire.com") != std::string::npos) CHECK(!m.status(mod.id).automatic);
  }

  // 3. Free space: 10 MiB free refuses TM-CE (a 1.4 GB disc) before anything is fetched or written.
  {
    const std::string mods_dir = join(scratch, "space/mods");
    remove_tree(join(scratch, "space"));
    make_dirs(mods_dir);
    CountingFetcher counter;
    TinyDisk tiny;
    Config c;
    c.mods_dir = mods_dir; c.disc_path = join(disc_folder, "GALE01.iso"); c.catalog_json = catalog_text; c.fetcher = &counter; c.platform = &tiny;
    Manager m(c);
    CHECK(m.install("tmce"));
    m.wait();
    const ModStatus s = m.status("tmce");
    std::printf("space: %s\n", s.line.c_str());
    CHECK(s.failed && s.line.find("Not enough free space") != std::string::npos && counter.urls.empty());
    CHECK(!file_exists(join(mods_dir, "Downloads/TM-CE.zip.part")));
  }

  // 4. Downloads, each from the catalog's own URL into the cache, once. Melee Maker (7 MB) first, interrupted after 1 MB
  //    and continued, to check the range request.
  Fetcher* net = curl_fetcher();
  const auto fetch_into_cache = [&](const CatalogMod& m) -> bool {
    const std::string path = join(cache, m.asset_pattern);
    if (file_exists(path) && sha256_file(path) == m.sha256) return true;
    // The player's own copy from the official release, when the disc folder has one (same name and SHA-256): linked, not fetched again.
    const std::string seed = join(join(join(disc_folder, "mods"), m.id), m.asset_pattern);
    if (m.download_bytes > (50u << 20) && file_exists(seed) && file_size(seed) == m.download_bytes && sha256_file(seed) == m.sha256 && link_or_copy(seed, path)) {
      std::printf("cache: %s from the disc folder's copy of the official file (SHA-256 checked)\n", m.asset_pattern.c_str());
      return true;
    }
    if (!m.direct_file()) return false;
    std::printf("cache: fetching %s\n", m.url.c_str());
    const auto r = net->fetch(m.url, path + ".part", m.download_bytes, nullptr);
    if (!r.ok) { std::printf("cache: %s: %s\n", m.id.c_str(), r.error.c_str()); return false; }
    return replace_file(path + ".part", path) && sha256_file(path) == m.sha256;
  };
  if (const CatalogMod* mm = by_id(mods, "meleemaker")) {
    const std::string path = join(cache, mm->asset_pattern);
    if (!file_exists(path)) {
      const std::string part = path + ".part";
      remove_file(part);
      const auto first = net->fetch(mm->url, part, mm->download_bytes, [](uint64_t done, uint64_t) { return done < (1u << 20); });
      std::printf("resume: first part stopped at %llu bytes (cancelled %d)\n", (unsigned long long)file_size(part), first.cancelled);
      CHECK(!first.ok && first.cancelled && file_size(part) >= (1u << 20) && file_size(part) < mm->download_bytes);
      const auto second = net->fetch(mm->url, part, mm->download_bytes, nullptr);
      std::printf("resume: continued to %llu bytes: %s\n", (unsigned long long)file_size(part), second.ok ? "ok" : second.error.c_str());
      CHECK(second.ok && file_size(part) == mm->download_bytes && sha256_file(part) == mm->sha256);
      if (second.ok) replace_file(part, path);
    } else {
      std::printf("resume: %s is already cached (delete it to run the interrupted download again)\n", mm->asset_pattern.c_str());
    }
  }

  // 5. Every mod. The Manager's Downloads folder is the cache itself (a symbolic link), so a one-click mod the Manager fetches
  //    is fetched once and kept (keep_downloads).
  std::map<std::string, std::string> results;
  for (const auto& mod : mods) {
    if (!only.empty() && std::find(only.begin(), only.end(), mod.id) == only.end()) continue;
    std::printf("== %s (%s, %s)\n", mod.id.c_str(), mod.name.c_str(), mod.version.c_str());
    if (!mod.direct_file()) {
      std::printf("  skipped: %s is a page, not a file (melee-unlocked's launcher cannot fetch it either): get it by hand\n", mod.url.c_str());
      results[mod.id] = "manual (not a direct link)";
      continue;
    }
    // Link-only mods: the test fetches the official file as the player would and puts it in Downloads.
    if (!mod.one_click && !fetch_into_cache(mod)) { results[mod.id] = "FAILED: could not fetch"; ++failed; continue; }
    if (mod.one_click) {
      // Seed from the disc folder's copy when there is one; else the Manager fetches it itself.
      const std::string seed = join(join(join(disc_folder, "mods"), mod.id), mod.asset_pattern);
      if (mod.download_bytes > (50u << 20) && !file_exists(join(cache, mod.asset_pattern)) && file_exists(seed)) fetch_into_cache(mod);
    }
    const std::string root = join(scratch, "install-" + mod.id);
    remove_tree(root);
    const std::string mods_dir = join(root, "mods");
    make_dirs(mods_dir);
    symlink(cache.c_str(), join(mods_dir, "Downloads").c_str());
    CountingFetcher counter;
    counter.real = net;
    Config c;
    c.mods_dir = mods_dir; c.disc_path = join(disc_folder, "GALE01.iso"); c.catalog_json = catalog_text; c.fetcher = &counter;
    c.keep_downloads = true; c.log = logline;
    Manager m(c);
    const auto start = std::chrono::steady_clock::now();
    std::string why;
    CHECK(m.install(mod.id, &why));
    std::string last;
    while (m.busy()) {
      const ModStatus s = m.status(mod.id);
      if (s.line != last && (s.percent < 0 || s.percent % 25 == 0)) { std::printf("  .. %s\n", s.line.c_str()); last = s.line; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    m.wait();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const ModStatus s = m.status(mod.id);
    std::printf("  -> %s (%.1f s; fetched by the Manager: %zu)\n", s.line.c_str(), secs, counter.urls.size());
    for (const auto& url : counter.urls) CHECK(url == mod.url);
    if (s.state != State::Installed) { results[mod.id] = "FAILED: " + s.line; ++failed; continue; }
    CHECK(m.profiles() == std::vector<std::string>{mod.id});
    if (!mod.is_disc()) {
      const std::string sha = sha256_file(join(join(mods_dir, mod.id), mod.asset_pattern));
      CHECK(sha == mod.sha256);
      results[mod.id] = sha == mod.sha256 ? "save installed, SHA-256 = the release's" : "FAILED: save differs";
      CHECK(m.remove(mod.id));
      CHECK(m.status(mod.id).state == State::NotInstalled);
      remove_tree(root);
      continue;
    }
    const std::string built = join(join(mods_dir, mod.id), mod.id + ".iso");
    std::string installed_sha;
    {
      std::string text;
      read_text(join(join(mods_dir, mod.id), "installed.txt"), &text);
      const size_t at = text.find("sha256=");
      if (at != std::string::npos) installed_sha = text.substr(at + 7, 64);
    }
    const std::string ref_disc = find_reference_disc(join(join(disc_folder, "mods"), mod.id));
    std::string verdict;
    if (!ref_disc.empty()) {
      const std::string ref = reference_sha(ref_disc, scratch);
      const bool same = ref == installed_sha;
      CHECK(same);
      verdict = std::string(same ? "IDENTICAL to" : "DIFFERS from") + " the hand-patched " + base_name(ref_disc) + " (" + installed_sha.substr(0, 16) + "...)";
    } else if (!mod.output_sha256.empty()) {
      const bool same = mod.output_sha256 == installed_sha;
      CHECK(same);
      verdict = std::string(same ? "equals" : "DIFFERS from") + " the catalog's output_sha256 (no hand-patched copy)";
    } else {
      verdict = "no reference to compare with";
    }
    CHECK(mod.output_sha256.empty() || mod.output_sha256 == installed_sha);
    CHECK(file_size(built) == mod.disc_bytes || !mod.disc_bytes);
    results[mod.id] = verdict + "; " + std::to_string(int(secs)) + " s";
    CHECK(m.remove(mod.id));
    CHECK(!file_exists(built) && m.status(mod.id).state == State::NotInstalled);
    remove_tree(root);
  }
  std::printf("\nresults:\n");
  for (const auto& [id, r] : results) std::printf("  %-12s %s\n", id.c_str(), r.c_str());
  std::printf("mod_install_test: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
