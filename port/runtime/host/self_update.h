// Updating the app from the player's own build computer (launcher.ini: update_host=http://address:port).
// The build computer publishes latest.json and the two NROs (tools/update-host.py, tools/update-serve.py);
// this is the console's half: read the manifest, decide whether it is newer, download an NRO beside the
// installed one, verify its size and SHA-256, and only then swap it in.
//
// Everything here is portable (standard library plus the bundled JSON reader) so the logic is tested on
// the build computer (tools/test_self_update.cpp). The transport is passed in: self_update_curl.cpp on the
// console, a fake in the test.
//
// Trust: the manifest is plain HTTP from an address the player typed, with no signature. The SHA-256
// check protects against a damaged or cut-off download, not against someone on the same network who can
// answer in the build computer's place. Hence: opt-in, home network only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace host::self_update {

struct FileEntry { std::string name; uint64_t size = 0; std::string sha256; };
struct Manifest {
  int schema = 0;
  std::string app_version, slippi_version, built, commit;
  std::map<std::string, FileEntry> files;      // "game", "replays"
  std::string pending_slippi, pending_reason;  // a Slippi release the build computer could not adopt by itself
};

// False with `error` set for anything that is not a well-formed schema-1 manifest. File names must be
// plain NRO names (letters, digits, '-', '_', '.'; ending in .nro): never a path.
bool parse_manifest(const std::string& text, Manifest& out, std::string& error);

// "1.2.3" against "1.2.3"; anything unparsable is never newer.
bool version_newer(const std::string& theirs, const std::string& ours);

// Is the published build one this app should offer? A newer app version, or the same app version built
// for a newer Slippi release (the build computer adopts game-side Slippi releases without a new app
// version). The same versions with another commit are not offered: telling them apart would mean hashing
// the installed 70 MB file at every launch, and every published change of the app carries a new version.
bool should_offer(const Manifest& manifest, const std::string& app_version, const std::string& slippi_version);

// "http://192.168.1.20:8080/" + "latest.json". Empty when `host` is not an http:// or https:// address.
std::string join_url(const std::string& host, const std::string& name);

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t size);
  std::string hex();   // finishes; call once
 private:
  void block(const uint8_t* bytes);
  uint32_t state_[8];
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint64_t total_ = 0;
};

// Streams `url` into `sink` (return false from it to stop). Returns false with `error` set on any failure,
// including an HTTP status other than 200.
using Fetch = std::function<bool(const std::string& url, const std::function<bool(const void* data, size_t size)>& sink, std::string& error)>;

// libcurl transport (self_update_curl.cpp; console builds). A dead connection ends after `stall_seconds`
// without progress; there is no limit on the whole transfer.
Fetch curl_fetch(long connect_timeout_ms, long stall_seconds);

// Paths used beside an installed NRO at `nro`.
std::string staged_path(const std::string& nro);   // nro + ".new": the download
std::string marker_path(const std::string& nro);   // nro + ".new.ok": written only after the download verified
std::string backup_path(const std::string& nro);   // nro + ".bak": the previous build, kept after a swap

// Downloads `entry` from `url` to staged_path(nro), checking size and SHA-256, then writes the marker.
// The installed file is never touched. On any failure the staged file and marker are removed and false is
// returned. `progress(done, total)` is called as bytes arrive; return false from it to cancel.
bool download_verified(const Fetch& fetch, const std::string& url, const FileEntry& entry, const std::string& nro,
                       const std::function<bool(uint64_t done, uint64_t total)>& progress, std::string& error);

enum class Swap { Nothing, Done, Failed };
// If a verified download is waiting (marker present and the staged file has the size the marker records):
// move the installed file to .bak, move the staged file into place, remove the marker. If the staged file
// cannot be moved into place the installed file is put back. Returns Nothing when no verified download is
// waiting (a stray staged file without a marker is deleted), Failed when the installed file could not be
// moved (for example it is open): the staged file and marker stay for the next attempt.
// Uses rename only: there is no moment at which a partly written file has the installed name.
Swap apply_pending(const std::string& nro, std::string& error);

}  // namespace host::self_update
