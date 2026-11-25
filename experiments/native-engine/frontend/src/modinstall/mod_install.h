// The mod installer: lists melee-unlocked's catalog with each mod's installed state, and installs, updates and
// removes mods for the player as melee-unlocked's launcher does (port/app/launcher_mods.inl), with this program's
// own code doing every step: the download (HTTPS through the platform's Fetcher, continued after an interruption),
// the SHA-256 checks, the archive (zip or 7z, read as a stream), the xdelta3 patch applied to the player's vanilla
// disc, and the check of the finished disc against the catalog's hash. No program from a mod is ever run.
//
// Folders (under `mods_dir`, which is <out>/mods, where frontend/src/mods.cpp looks for profiles):
//   Downloads/          downloads, and where the player puts a file the catalog does not let us fetch
//   .work/              the disc being built (<id>.iso.part) and the vanilla disc's checked MD5
//   <id>/<id>.iso       a built disc: the folder is the mod profile <id> (settings.ini mod_profile <id>)
//   <id>/<asset>.gci    20XX TE's save: the folder is the profile "te"; mods.cpp recognises the save
//   <id>/installed.txt  what was installed (id, version, the file and its SHA-256)
// One job runs at a time, on its own thread; every step reports progress and stops when cancelled. Nothing large is
// held in memory: the disc is read and written in blocks.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mi_catalog.h"
#include "mi_io.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace modinstall {

enum class State { NotInstalled, Installed, UpdateAvailable, Working };

struct ModStatus {
  CatalogMod mod;
  State state = State::NotInstalled;
  bool automatic = false;            // Install downloads it (the catalog records a licence or a permission)
  bool file_ready = false;           // its download is complete in Downloads/ (fetched, or put there by the player)
  std::string installed_version;
  std::string line;                  // what is happening, or how the last job ended
  int percent = -1;                  // progress of the current step, -1 when none
  bool failed = false;               // the last job for this mod failed (line says why)
  std::string downloads_path;        // where its file is looked for: Downloads/<asset>
};

struct Config {
  std::string mods_dir;              // <out>/mods
  std::string disc_path;             // the player's GALE01.iso (vanilla NTSC 1.02)
  std::string catalog_json;          // melee-unlocked's mods-catalog-2.json text
  Fetcher* fetcher = nullptr;        // null: no downloads (every mod is then "put the file in Downloads/")
  Platform* platform = nullptr;      // null: the POSIX default (statvfs)
  bool keep_downloads = false;       // keep a fetched archive after its disc is built (default: deleted to save space)
  std::string cache_dir;             // <out>/mods-cache (mods.cpp's comparison cache): an entry is dropped on remove
  std::function<void(const std::string&)> log;   // one line per step (any thread)
};

class Manager {
 public:
  explicit Manager(Config config);
  ~Manager();                        // cancels a running job and waits for it
  bool catalog_ok() const { return catalog_error_.empty(); }
  const std::string& catalog_error() const { return catalog_error_; }
  const std::vector<CatalogMod>& catalog() const { return catalog_; }
  // A newer catalog (upstream's refresh from its raw catalog URL): taken when it passes the same rules as the built-in one;
  // a mod's credits and note are kept from the old entry when the new one has none (upstream's merge). Call it from the
  // thread that reads list()/status()/install(), and not while a job runs (false then, nothing changed).
  bool replace_catalog(const std::string& json, std::string* error);
  std::vector<ModStatus> list();
  ModStatus status(const std::string& id);
  // Install or Update (a job on the worker thread). False with *why set when it cannot start.
  bool install(const std::string& id, std::string* why = nullptr);
  bool remove(const std::string& id, std::string* why = nullptr);   // the disc or save and its record; saves on the profile's card stay
  void cancel();
  bool busy() const { return busy_.load(); }
  std::string busy_id();
  void wait();                       // until the running job ends (tests)
  // Profiles frontend/src/mods.cpp can load: folders under mods_dir with a disc image or a save, and Profiles/*.ini.
  std::vector<std::string> profiles();
  const Config& config() const { return config_; }

 private:
  struct Progress;
  void run_job(const CatalogMod& mod);
  bool job(const CatalogMod& mod, std::string* why);
  bool ensure_download(const CatalogMod& mod, bool* fetched, std::string* why);
  bool check_vanilla(std::string* why);
  bool build_disc(const CatalogMod& mod, const std::string& input, std::string* output_sha256, std::string* why);
  void set_line(const std::string& id, const std::string& text, int percent = -1);
  void log(const std::string& text);
  bool cancelled() const { return cancel_.load(); }
  std::string read_installed(const std::string& id, std::string* version, std::string* file);
  uint64_t free_bytes();

  Config config_;
  Platform default_platform_;
  std::vector<CatalogMod> catalog_;
  std::string catalog_error_;
  std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> busy_{false}, cancel_{false};
  std::string job_id_;
  struct Line { std::string text; int percent = -1; bool failed = false; };
  std::vector<std::pair<std::string, Line>> lines_;
};

std::string size_text(uint64_t bytes);   // "246 MB", "1.4 GB"

}  // namespace modinstall
