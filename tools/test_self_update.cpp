// Host test for the console's self-update logic (port/runtime/host/self_update.*): manifest parsing,
// the offer decision, SHA-256, and the download / verify / swap sequence against a temporary folder
// with a fake transport.
//   c++ -std=c++17 -Wall -Wextra -Iport/runtime/host -Iport/third_party tools/test_self_update.cpp \
//       port/runtime/host/self_update.cpp -o /tmp/test_self_update && /tmp/test_self_update
// SPDX-License-Identifier: GPL-2.0-or-later
#include "self_update.h"
#include "sha256.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace host::self_update;
namespace fs = std::filesystem;
static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { ++g_failed; std::cerr << "FAILED line " << __LINE__ << ": " #x "\n"; } } while (0)

static std::string read_file(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void write_file(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }
static std::string sha(const std::string& s) { return host::memory_sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
static std::string body(size_t size, unsigned seed) { std::string s(size, '\0'); for (size_t i = 0; i < size; ++i) s[i] = (char)((i * 131 + seed * 17 + (i >> 9)) & 0xFF); return s; }

// Serves `data` in uneven chunks, optionally stopping early.
static Fetch serve(const std::string& data, size_t stop_after = SIZE_MAX, bool report_failure = true) {
  return [=](const std::string&, const std::function<bool(const void*, size_t)>& sink, std::string& error) {
    size_t pos = 0, chunk = 1;
    while (pos < data.size()) {
      if (pos >= stop_after) { if (report_failure) { error = "connection lost"; return false; } return true; }
      const size_t n = std::min(std::min(chunk, data.size() - pos), stop_after - pos);
      if (!sink(data.data() + pos, n)) { error = "stopped"; return false; }
      pos += n; chunk = chunk * 3 + 7; if (chunk > 70000) chunk = 5;
    }
    return true;
  };
}

int main() {
  // ---- SHA-256: the streaming class against the one-shot helper, across block boundaries
  for (size_t size : {0u, 1u, 55u, 56u, 63u, 64u, 65u, 119u, 120u, 1000u, 100000u}) {
    const std::string data = body(size, (unsigned)size);
    Sha256 h; size_t pos = 0, step = 1;
    while (pos < data.size()) { const size_t n = std::min(step, data.size() - pos); h.update(data.data() + pos, n); pos += n; step = step * 2 + 1; }
    CHECK(h.hex() == sha(data));
  }
  { Sha256 h; h.update("abc", 3); CHECK(h.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }

  // ---- versions and the offer decision
  CHECK(version_newer("0.3.9", "0.3.8")); CHECK(version_newer("0.4.0", "0.3.99")); CHECK(version_newer("v1.0.0", "0.9.9"));
  CHECK(!version_newer("0.3.8", "0.3.8")); CHECK(!version_newer("0.3.7", "0.3.8")); CHECK(!version_newer("0.3", "0.3.8"));
  CHECK(!version_newer("0.3.9-dev", "0.3.8")); CHECK(!version_newer("", "0.3.8")); CHECK(!version_newer("9.9.9", "junk"));

  const std::string game = body(2 * 1024 * 1024 + 123, 1), replays = body(1024 * 1024 + 7, 2);
  auto manifest_text = [&](const std::string& app, const std::string& slippi, const std::string& extra = "") {
    return "{\"schema\":1,\"app_version\":\"" + app + "\",\"slippi_version\":\"" + slippi + "\",\"built\":\"2026-01-01T00:00:00Z\",\"commit\":\"abc\","
           "\"files\":{\"game\":{\"name\":\"melee-nx-netplay-aaaa.nro\",\"size\":" + std::to_string(game.size()) + ",\"sha256\":\"" + sha(game) + "\"},"
           "\"replays\":{\"name\":\"melee-nx-replays-bbbb.nro\",\"size\":" + std::to_string(replays.size()) + ",\"sha256\":\"" + sha(replays) + "\"}}" + extra + "}";
  };
  Manifest m; std::string error;
  CHECK(parse_manifest(manifest_text("0.3.9", "3.7.0"), m, error) && error.empty());
  CHECK(m.app_version == "0.3.9" && m.slippi_version == "3.7.0" && m.files.size() == 2 && m.files["game"].size == game.size() && m.pending_slippi.empty());
  CHECK(should_offer(m, "0.3.8", "3.7.0"));      // newer app
  CHECK(!should_offer(m, "0.3.9", "3.7.0"));     // the same build
  CHECK(!should_offer(m, "0.4.0", "3.7.0"));     // older than what runs
  CHECK(should_offer(m, "0.3.9", "3.6.4"));      // same app, rebuilt for a newer Slippi
  CHECK(!should_offer(m, "0.3.8", "3.8.0"));     // never back to an older Slippi
  CHECK(parse_manifest(manifest_text("0.3.9", "3.7.0", ",\"pending\":{\"slippi\":\"3.8.0\",\"reason\":\"needs a port\"}"), m, error));
  CHECK(m.pending_slippi == "3.8.0" && m.pending_reason == "needs a port");
  CHECK(parse_manifest(manifest_text("0.3.9", "3.7.0", ",\"pending\":null"), m, error) && m.pending_slippi.empty());
  // rejected manifests
  CHECK(!parse_manifest("", m, error) && !error.empty());
  CHECK(!parse_manifest("[1,2]", m, error));
  CHECK(!parse_manifest("{\"schema\":2}", m, error));
  CHECK(!parse_manifest(manifest_text("zero", "3.7.0"), m, error));
  for (const char* bad_name : {"../evil.nro", "a/b.nro", "sdmc:/x.nro", "game.exe", ".nro", "x..y.nro", "a b.nro"}) {
    std::string t = manifest_text("0.3.9", "3.7.0");
    t.replace(t.find("melee-nx-netplay-aaaa.nro"), 25, bad_name);
    CHECK(!parse_manifest(t, m, error));
  }
  { std::string t = manifest_text("0.3.9", "3.7.0"); t.replace(t.find(sha(game)), 64, std::string(64, 'Z')); CHECK(!parse_manifest(t, m, error)); }
  { std::string t = manifest_text("0.3.9", "3.7.0"); t.replace(t.find("\"size\":"), 7, "\"size\":-"); CHECK(!parse_manifest(t, m, error)); }
  CHECK(!parse_manifest("{\"schema\":1,\"app_version\":\"0.3.9\",\"slippi_version\":\"3.7.0\",\"files\":{}}", m, error));   // no game

  CHECK(join_url("http://10.0.0.5:8080", "latest.json") == "http://10.0.0.5:8080/latest.json");
  CHECK(join_url("http://10.0.0.5:8080/ ", "a.nro") == "http://10.0.0.5:8080/a.nro");
  CHECK(join_url("ftp://x", "a").empty() && join_url("10.0.0.5:8080", "a").empty() && join_url("http://", "a").empty() && join_url("http://a b", "a").empty());

  // ---- download, verify, swap in a temporary folder
  const fs::path dir = fs::temp_directory_path() / ("mnx-self-update-" + std::to_string((unsigned long long)std::rand()) + "-" + std::to_string((unsigned long long)time(nullptr)));
  fs::create_directories(dir);
  const std::string nro = (dir / "melee-nx-netplay.nro").string();
  const std::string installed = body(1500000, 9);
  write_file(nro, installed);
  CHECK(parse_manifest(manifest_text("0.3.9", "3.7.0"), m, error));
  const FileEntry entry = m.files["game"];

  // nothing waiting
  CHECK(apply_pending(nro, error) == Swap::Nothing && read_file(nro) == installed);

  // a download that stops early (reported, and silently): nothing staged, installed untouched
  CHECK(!download_verified(serve(game, 700000), "u", entry, nro, nullptr, error) && !error.empty());
  CHECK(!fs::exists(staged_path(nro)) && !fs::exists(marker_path(nro)) && read_file(nro) == installed);
  CHECK(!download_verified(serve(game, 700000, false), "u", entry, nro, nullptr, error) && error == "the download stopped early");
  CHECK(!fs::exists(staged_path(nro)) && !fs::exists(marker_path(nro)));
  CHECK(apply_pending(nro, error) == Swap::Nothing && read_file(nro) == installed);

  // right size, wrong content
  { std::string wrong = game; wrong[wrong.size() / 2] ^= 1;
    CHECK(!download_verified(serve(wrong), "u", entry, nro, nullptr, error) && error.find("checksum") != std::string::npos);
    CHECK(!fs::exists(staged_path(nro)) && !fs::exists(marker_path(nro)) && read_file(nro) == installed); }
  // longer than announced
  CHECK(!download_verified(serve(game + "extra"), "u", entry, nro, nullptr, error) && !fs::exists(staged_path(nro)));
  // cancelled from the progress callback
  CHECK(!download_verified(serve(game), "u", entry, nro, [](uint64_t done, uint64_t) { return done < 400000; }, error) && error.find("cancelled") != std::string::npos);
  CHECK(!fs::exists(staged_path(nro)) && read_file(nro) == installed);
  // a stray staged file without a marker (power lost mid-download) is removed, never installed
  write_file(staged_path(nro), game.substr(0, 1000));
  CHECK(apply_pending(nro, error) == Swap::Nothing && !fs::exists(staged_path(nro)) && read_file(nro) == installed);
  // a marker whose staged file has the wrong size is discarded
  write_file(staged_path(nro), game.substr(0, 1000)); write_file(marker_path(nro), std::to_string(game.size()) + " x\n");
  CHECK(apply_pending(nro, error) == Swap::Nothing && !fs::exists(staged_path(nro)) && !fs::exists(marker_path(nro)) && read_file(nro) == installed);

  // the good path, with progress
  uint64_t last = 0, calls = 0;
  CHECK(download_verified(serve(game), "u", entry, nro, [&](uint64_t done, uint64_t total) { CHECK(done >= last && total == game.size()); last = done; ++calls; return true; }, error));
  CHECK(last == game.size() && calls > 3 && fs::exists(staged_path(nro)) && fs::exists(marker_path(nro)));
  CHECK(read_file(nro) == installed);                    // still the old app until the swap
  CHECK(apply_pending(nro, error) == Swap::Done && error.empty());
  CHECK(read_file(nro) == game && read_file(backup_path(nro)) == installed);
  CHECK(!fs::exists(staged_path(nro)) && !fs::exists(marker_path(nro)));
  CHECK(apply_pending(nro, error) == Swap::Nothing);

  // the installed file cannot be moved aside (its folder entry is locked: here, .bak is a non-empty folder): Failed,
  // installed app untouched, the verified download kept for the next attempt
  const std::string second = body(game.size(), 77);
  FileEntry entry2 = entry; entry2.sha256 = sha(second);
  CHECK(download_verified(serve(second), "u", entry2, nro, nullptr, error));
  fs::remove(backup_path(nro)); fs::create_directories(fs::path(backup_path(nro)) / "x");
  CHECK(apply_pending(nro, error) == Swap::Failed && !error.empty());
  CHECK(read_file(nro) == game && fs::exists(staged_path(nro)) && fs::exists(marker_path(nro)));
  fs::remove_all(backup_path(nro));
  CHECK(apply_pending(nro, error) == Swap::Done && read_file(nro) == second && read_file(backup_path(nro)) == game);

  // power lost between the two renames: the installed name is missing, .bak and the verified download exist.
  // The next attempt (made by the other app at its launch) finishes the swap.
  CHECK(download_verified(serve(game), "u", entry, nro, nullptr, error));
  fs::rename(nro, backup_path(nro));
  CHECK(apply_pending(nro, error) == Swap::Done && read_file(nro) == game && read_file(backup_path(nro)) == second);

  fs::remove_all(dir);

  if (g_failed) { std::cerr << g_failed << " failed checks\n"; return 1; }
  std::cout << "all passed\n";
  return 0;
}
