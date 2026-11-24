// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_install.h"
#include "mi_archive.h"
#include "mi_hash.h"
#include "mi_vcdiff.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace modinstall {
namespace {
constexpr const char* kVanillaMd5 = "0e63d4223b01d9aba596259dc155a174";   // Melee NTSC 1.02, as melee-unlocked checks it
constexpr uint64_t kMiB = 1024ull * 1024, kGiB = 1024 * kMiB;
constexpr uint64_t kDiscFallback = 1800 * kMiB;     // the largest built disc (ACE) is 1.6 GiB, as melee-unlocked reserves
constexpr uint64_t kSpare = 64 * kMiB;

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

// Hashes what passes through.
class HashingSource : public ByteSource {
 public:
  explicit HashingSource(ByteSource& inner) : inner_(inner) {}
  long read(uint8_t* out, size_t size) override {
    const long got = inner_.read(out, size);
    if (got > 0) { sha_.update(out, size_t(got)); pos_ += uint64_t(got); }
    if (got < 0) error_ = inner_.error();
    return got;
  }
  uint64_t total() const override { return inner_.total(); }
  std::string sha256() { return sha_.hex(); }
 private:
  ByteSource& inner_;
  Sha256 sha_;
};

// The new disc: written through a large buffer, hashed on the way.
class DiscSink : public OutputSink {
 public:
  explicit DiscSink(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    if (f_) std::setvbuf(f_, nullptr, _IOFBF, 1u << 20);
  }
  ~DiscSink() override { if (f_) std::fclose(f_); }
  bool ok() const { return f_ != nullptr; }
  bool write(const uint8_t* data, size_t size) override {
    sha_.update(data, size); md5_.update(data, size);
    bytes_ += size;
    return std::fwrite(data, 1, size, f_) == size;
  }
  bool close() { const bool ok = std::fclose(f_) == 0; f_ = nullptr; return ok; }
  std::string sha256() { return sha_.hex(); }
  std::string md5() { return md5_.hex(); }
  uint64_t bytes() const { return bytes_; }
 private:
  FILE* f_ = nullptr;
  Sha256 sha_;
  Md5 md5_;
  uint64_t bytes_ = 0;
};

std::string percent_text(uint64_t done, uint64_t total) {
  if (!total) return size_text(done);
  return std::to_string(int(done * 100 / total)) + "% (" + size_text(done) + " of " + size_text(total) + ")";
}
}  // namespace

std::string size_text(uint64_t bytes) {
  char buf[32];
  if (bytes >= kGiB) std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(kGiB));
  else if (bytes >= kMiB) std::snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(kMiB));
  else std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
  return buf;
}

Manager::Manager(Config config) : config_(std::move(config)) {
  if (!config_.platform) config_.platform = &default_platform_;
  if (!parse_catalog(config_.catalog_json, &catalog_, &catalog_error_)) {
    if (catalog_error_.empty()) catalog_error_ = "The mod catalog could not be read.";
    catalog_.clear();
  }
}

Manager::~Manager() {
  cancel();
  if (worker_.joinable()) worker_.join();
}

void Manager::log(const std::string& text) { if (config_.log) config_.log("mods: " + text); }

void Manager::set_line(const std::string& id, const std::string& text, int percent) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [key, line] : lines_) if (key == id) { line.text = text; line.percent = percent; line.failed = false; return; }
  lines_.push_back({id, Line{text, percent, false}});
}

uint64_t Manager::free_bytes() { return config_.platform->free_bytes(config_.mods_dir); }

std::string Manager::read_installed(const std::string& id, std::string* version, std::string* file) {
  std::string text;
  if (!read_text(join(join(config_.mods_dir, id), "installed.txt"), &text, 65536)) return {};
  std::istringstream in(text);
  std::string line, sha;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "version" && version) *version = value;
    else if (key == "file" && file) *file = value;
    else if (key == "sha256") sha = value;
  }
  return sha;
}

ModStatus Manager::status(const std::string& id) {
  ModStatus out;
  for (const auto& m : catalog_) if (m.id == id) out.mod = m;
  if (out.mod.id.empty()) return out;
  const CatalogMod& m = out.mod;
  out.automatic = m.one_click && m.direct_file() && config_.fetcher != nullptr;
  out.downloads_path = join(join(config_.mods_dir, "Downloads"), m.asset_pattern);
  out.file_ready = file_exists(out.downloads_path) && (!m.download_bytes || file_size(out.downloads_path) == m.download_bytes);
  std::string version, file;
  const std::string sha = read_installed(m.id, &version, &file);
  if (!sha.empty() && !file.empty() && file_exists(join(join(config_.mods_dir, m.id), file))) {
    out.installed_version = version;
    const std::string wanted = m.is_disc() ? m.output_sha256 : m.sha256;
    out.state = (!wanted.empty() && wanted != sha) || version != m.version ? State::UpdateAvailable : State::Installed;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (busy_.load() && job_id_ == m.id) out.state = State::Working;
  for (const auto& [key, line] : lines_) if (key == m.id) { out.line = line.text; out.percent = line.percent; out.failed = line.failed; }
  return out;
}

std::vector<ModStatus> Manager::list() {
  std::vector<ModStatus> out;
  for (const auto& m : catalog_) out.push_back(status(m.id));
  return out;
}

std::string Manager::busy_id() { std::lock_guard<std::mutex> lock(mutex_); return busy_.load() ? job_id_ : std::string(); }

bool Manager::install(const std::string& id, std::string* why) {
  std::string scratch;
  if (!why) why = &scratch;
  const CatalogMod* mod = nullptr;
  for (const auto& m : catalog_) if (m.id == id) mod = &m;
  if (!mod) { *why = "That mod is not in the catalog."; return false; }
  if (busy_.load()) { *why = "Another mod is being installed. Wait for it or cancel it first."; return false; }
  if (worker_.joinable()) worker_.join();
  cancel_ = false;
  { std::lock_guard<std::mutex> lock(mutex_); job_id_ = id; }
  busy_ = true;
  set_line(id, "Starting...");
  const CatalogMod copy = *mod;
  worker_ = std::thread([this, copy] { run_job(copy); });
  return true;
}

void Manager::run_job(const CatalogMod& mod) {
  std::string why;
  const bool ok = job(mod, &why);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, line] : lines_) if (key == mod.id) {
      line.percent = -1;
      if (!ok) { line.text = cancelled() ? "Cancelled. Nothing was installed." : why; line.failed = !cancelled(); }
    }
  }
  log(mod.id + ": " + (ok ? "installed" : cancelled() ? "cancelled" : "failed: " + why));
  busy_ = false;
}

void Manager::cancel() { if (busy_.load()) cancel_ = true; }

void Manager::wait() { if (worker_.joinable()) worker_.join(); }

bool Manager::ensure_download(const CatalogMod& mod, bool* fetched, std::string* why) {
  *fetched = false;
  const std::string dir = join(config_.mods_dir, "Downloads");
  const std::string path = join(dir, mod.asset_pattern);
  if (file_exists(path) && (!mod.download_bytes || file_size(path) == mod.download_bytes)) return true;
  const bool automatic = mod.one_click && mod.direct_file();
  if (!automatic) {
    // melee-unlocked offers these from their pages only (no licence or permission recorded, or not a direct file).
    *why = "Download " + mod.asset_pattern + " from " + mod.page + " and put it in mods/Downloads/, then choose Install again.";
    return false;
  }
  if (!config_.fetcher) { *why = "This build cannot download. Put " + mod.asset_pattern + " (from " + mod.page + ") in mods/Downloads/."; return false; }
  if (!make_dirs(dir)) { *why = "Cannot create the mods/Downloads folder."; return false; }
  const std::string part = path + ".part";
  const uint64_t have = file_size(part);
  const uint64_t need = (mod.download_bytes > have ? mod.download_bytes - have : 0) + (mod.is_disc() ? (mod.disc_bytes ? mod.disc_bytes : kDiscFallback) : 0) + kSpare;
  const uint64_t free = free_bytes();
  if (free != UINT64_MAX && free < need) {
    *why = "Not enough free space on the SD card: this needs " + size_text(need) + " and there is " + size_text(free) + ". Nothing was installed.";
    return false;
  }
  log(mod.id + ": downloading " + mod.url + (have ? " (continuing at " + size_text(have) + ")" : ""));
  set_line(mod.id, "Downloading...", 0);
  int shown = -2;
  const auto result = config_.fetcher->fetch(mod.url, part, mod.download_bytes ? mod.download_bytes : 2 * kGiB, [&](uint64_t done, uint64_t total) {
    const int pct = total ? int(done * 100 / total) : -1;
    if (pct != shown) { shown = pct; set_line(mod.id, "Downloading " + percent_text(done, total), pct); }
    return !cancelled();
  });
  if (!result.ok) {
    *why = cancelled() || result.cancelled ? "Cancelled. The part already downloaded is kept for next time." : "The download failed: " + result.error + ". Choose Install again to continue it.";
    return false;
  }
  if (mod.download_bytes && file_size(part) != mod.download_bytes) {
    remove_file(part);
    *why = "The download has the wrong size. Nothing was installed.";
    return false;
  }
  if (!replace_file(part, path)) { *why = "Cannot rename the finished download."; return false; }
  *fetched = true;
  return true;
}

bool Manager::check_vanilla(std::string* why) {
  const std::string disc = config_.disc_path;
  if (!file_exists(disc)) { *why = "Your Melee disc (" + disc + ") was not found."; return false; }
  // Its MD5, kept with the disc's size and time so it is read in full only once.
  const std::string memo = join(join(config_.mods_dir, ".work"), "vanilla.txt");
  const std::string key = std::to_string(file_size(disc)) + " " + std::to_string(file_mtime(disc)) + " " + disc;
  std::string text;
  if (read_text(memo, &text, 4096) && text == key + "\n" + kVanillaMd5 + "\n") return true;
  set_line(job_id_, "Checking your Melee disc...", 0);
  int shown = -1;
  const std::string md5 = md5_file(disc, [&](uint64_t done, uint64_t total) {
    const int pct = total ? int(done * 100 / total) : 0;
    if (pct != shown) { shown = pct; set_line(job_id_, "Checking your Melee disc " + std::to_string(pct) + "%", pct); }
    return !cancelled();
  });
  if (cancelled()) { *why = "Cancelled."; return false; }
  if (md5 != kVanillaMd5) {
    *why = "Mods are built from the unmodified Melee NTSC 1.02 disc, and " + base_name(disc) + " is not that disc (MD5 " + (md5.empty() ? "unreadable" : md5) + ").";
    return false;
  }
  make_dirs(join(config_.mods_dir, ".work"));
  write_text(memo, key + "\n" + kVanillaMd5 + "\n");
  return true;
}

bool Manager::build_disc(const CatalogMod& mod, const std::string& input, std::string* output_sha256, std::string* why) {
  // The patch: the bare .xdelta, or the first .xdelta inside the zip or 7z (as melee-unlocked's installer finds it).
  std::unique_ptr<Archive> archive;
  std::unique_ptr<ByteSource> patch;
  if (ends_with(input, ".xdelta")) {
    patch.reset(new FileSource(input));
    if (!static_cast<FileSource*>(patch.get())->ok()) { *why = "Cannot read " + base_name(input) + "."; return false; }
  } else {
    std::string error;
    archive = open_archive(input, &error);
    if (!archive) { *why = "The archive could not be read: " + error + "."; return false; }
    const long entry = find_entry(*archive, ".xdelta");
    if (entry < 0) { *why = "The verified patch was not found in the archive. Nothing was installed."; return false; }
    patch = archive->open(size_t(entry), &error);
    if (!patch) { *why = "The patch in the archive could not be read: " + error + "."; return false; }
    log(mod.id + ": patch " + archive->entries()[size_t(entry)].name + " (" + size_text(archive->entries()[size_t(entry)].size) + ")");
  }
  const uint64_t disc_need = (mod.disc_bytes ? mod.disc_bytes : kDiscFallback) + kSpare;
  const uint64_t free = free_bytes();
  if (free != UINT64_MAX && free < disc_need) {
    *why = "Not enough free space on the SD card: the disc needs " + size_text(disc_need) + " and there is " + size_text(free) + ". Nothing was installed.";
    return false;
  }
  const std::string work = join(config_.mods_dir, ".work");
  if (!make_dirs(work)) { *why = "Cannot create the mods/.work folder."; return false; }
  const std::string staged = join(work, mod.id + ".iso.part");
  RandomFile source(config_.disc_path);
  if (!source.ok()) { *why = "Cannot read your Melee disc."; return false; }
  HashingSource hashed(*patch);
  std::string error;
  VcdiffInfo info;
  bool ok;
  {
    DiscSink sink(staged);
    if (!sink.ok()) { *why = "Cannot create the new disc in mods/.work."; return false; }
    set_line(mod.id, "Building the disc...", 0);
    int shown = -1;
    ok = vcdiff_decode(hashed, source, sink, [&](uint64_t done, uint64_t total) {
      const int pct = total ? int(done * 100 / total) : -1;
      if (pct != shown) { shown = pct; set_line(mod.id, "Building the disc " + (pct >= 0 ? std::to_string(pct) + "%" : size_text(done)), pct); }
      return !cancelled();
    }, &error, &info);
    // The patch's own end: the CRC check of an archive entry runs on the read after its last byte.
    uint8_t probe;
    if (ok && hashed.read(&probe, 1) != 0) { ok = false; error = hashed.error().empty() ? "the patch has bytes after its last window" : hashed.error(); }
    const bool closed = sink.close();
    if (ok && !closed) { ok = false; error = "cannot finish writing the new disc (is the SD card full?)"; }
    if (ok) {
      set_line(mod.id, "Checking the finished disc...", 100);
      const std::string patch_sha = hashed.sha256();
      if (!mod.patch_sha256.empty() && patch_sha != mod.patch_sha256) { ok = false; error = "the patch did not match the verified catalog"; }
      *output_sha256 = sink.sha256();
      const std::string md5 = sink.md5();
      if (ok && ((!mod.output_sha256.empty() && *output_sha256 != mod.output_sha256) || (!mod.output_md5.empty() && md5 != mod.output_md5))) {
        ok = false; error = "the built disc did not match the official result";
      }
      log(mod.id + ": " + std::to_string(info.windows) + " windows, " + size_text(sink.bytes()) + ", secondary " + std::to_string(info.secondary) +
          ", patch sha256 " + patch_sha + ", disc sha256 " + *output_sha256);
    }
  }
  if (!ok) {
    remove_file(staged);
    *why = cancelled() ? "Cancelled. Nothing was installed." : "The disc could not be built: " + error + ". Your installed mods were kept.";
    return false;
  }
  const std::string dir = join(config_.mods_dir, mod.id);
  if (!make_dirs(dir) || !replace_file(staged, join(dir, mod.id + ".iso"))) { remove_file(staged); *why = "Cannot move the new disc into mods/" + mod.id + "/."; return false; }
  return true;
}

bool Manager::job(const CatalogMod& mod, std::string* why) {
  if (mod.sha256.empty()) { *why = "This mod has no verified download in the catalog."; return false; }
  if (mod.is_disc() && (mod.patch_sha256.empty() || (mod.output_sha256.empty() && mod.output_md5.empty()))) {
    *why = "This mod's patch has not been verified in the catalog. Nothing was installed."; return false;
  }
  if (mod.is_disc() && !check_vanilla(why)) return false;
  bool fetched = false;
  if (!ensure_download(mod, &fetched, why)) return false;
  const std::string input = join(join(config_.mods_dir, "Downloads"), mod.asset_pattern);
  set_line(mod.id, "Checking the download...", 0);
  int shown = -1;
  const std::string sha = sha256_file(input, [&](uint64_t done, uint64_t total) {
    const int pct = total ? int(done * 100 / total) : 0;
    if (pct != shown) { shown = pct; set_line(mod.id, "Checking the download " + std::to_string(pct) + "%", pct); }
    return !cancelled();
  });
  if (cancelled()) { *why = "Cancelled."; return false; }
  if (sha != mod.sha256) {
    if (fetched) remove_file(input);
    *why = "The file " + mod.asset_pattern + " did not match the verified release (SHA-256 " + (sha.empty() ? "unreadable" : sha.substr(0, 16) + "...") + "). Nothing was installed.";
    return false;
  }
  log(mod.id + ": " + mod.asset_pattern + " verified (sha256 " + sha + ")");
  std::string installed_sha, file;
  if (mod.is_disc()) {
    if (!build_disc(mod, input, &installed_sha, why)) return false;
    file = mod.id + ".iso";
  } else {
    // A save (20XX TE): copied into its profile folder, where frontend/src/mods.cpp recognises it.
    const std::string dir = join(config_.mods_dir, mod.id);
    const std::string target = join(dir, mod.asset_pattern);
    std::string text;
    if (!make_dirs(dir) || !read_text(input, &text, 8u << 20) || !write_text(target, text)) { *why = "Cannot copy the save into mods/" + mod.id + "/."; return false; }
    installed_sha = sha;
    file = mod.asset_pattern;
  }
  write_text(join(join(config_.mods_dir, mod.id), "installed.txt"),
             "id=" + mod.id + "\nversion=" + mod.version + "\nfile=" + file + "\nsha256=" + installed_sha + "\n");
  // A fetched download is deleted once its disc is built (it can be fetched again); a file the player put there stays.
  if (fetched && !config_.keep_downloads && mod.is_disc()) remove_file(input);
  set_line(mod.id, "Installed.", -1);
  return true;
}

bool Manager::remove(const std::string& id, std::string* why) {
  std::string scratch;
  if (!why) why = &scratch;
  if (busy_.load() && busy_id() == id) { *why = "This mod is being installed. Cancel it first."; return false; }
  if (!valid_id(id)) { *why = "Invalid mod."; return false; }
  std::string version, file;
  read_installed(id, &version, &file);
  const std::string dir = join(config_.mods_dir, id);
  if (file.empty() || file.find('/') != std::string::npos || file == "." || file == "..") { *why = "This mod was not installed here."; return false; }
  if (!remove_file(join(dir, file))) { *why = "Cannot remove mods/" + id + "/" + file + "."; return false; }
  remove_file(join(dir, "installed.txt"));
  if (!config_.cache_dir.empty()) remove_file(join(config_.cache_dir, file + ".changed"));
  remove_dir_if_empty(dir);
  set_line(id, "Removed. Its memory card (card-" + id + ") was kept.");
  log(id + ": removed");
  return true;
}

std::vector<std::string> Manager::profiles() {
  std::vector<std::string> out;
  for (const auto& name : list_dir(config_.mods_dir)) {
    if (name.empty() || name[0] == '.' || name == "Downloads" || name == "Profiles") continue;
    const std::string dir = join(config_.mods_dir, name);
    if (!dir_exists(dir)) continue;
    bool layer = false;
    for (const auto& f : list_dir(dir)) if (ends_with(f, ".iso") || ends_with(f, ".gcm") || ends_with(f, ".gci")) layer = true;
    if (layer && name.find(',') == std::string::npos) out.push_back(name);
  }
  for (const auto& name : list_dir(join(config_.mods_dir, "Profiles")))
    if (ends_with(name, ".ini") && name.size() > 4) {
      const std::string profile = name.substr(0, name.size() - 4);
      if (std::find(out.begin(), out.end(), profile) == out.end() && profile.find(',') == std::string::npos) out.push_back(profile);
    }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace modinstall
