// SPDX-License-Identifier: GPL-2.0-or-later
#include "json_safe.h"
#include "headless_manifest.h"
#include "audio.h"
#include "audio_headless.h"
#include "exi_slippi.h"
#include "file_identity.h"
#include "gecko_data.h"
#include "gx_core.h"
#include "numeric.h"
#include "nlohmann/json.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MELEE_PORT_INPUT_MANIFEST_PATH
#define MELEE_PORT_INPUT_MANIFEST_PATH ""
#endif
#ifndef MELEE_PORT_INPUT_MANIFEST_SHA256
#define MELEE_PORT_INPUT_MANIFEST_SHA256 ""
#endif

namespace host {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
bool write_artifact(const fs::path& path, const Json& value, std::string& error) {
  fs::path temporary = path.string() + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wbx");
  if (!file) { error = "cannot exclusively reserve artifact: " + temporary.string(); return false; }
  std::string data = value.dump(2) + "\n";
  bool ok = ::fchmod(::fileno(file), S_IRUSR | S_IWUSR) == 0 &&
            std::fwrite(data.data(), 1, data.size(), file) == data.size() &&
            std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
  if (std::fclose(file) != 0) ok = false;
  std::error_code ec;
  if (ok) { fs::create_hard_link(temporary, path, ec); ok = !ec; }
  // Only this call's successfully reserved temporary file is removed. Hard-link
  // publication is atomic and fails if the final artifact already exists.
  fs::remove(temporary, ec);
  if (!ok) error = "cannot publish complete artifact without overwriting: " + path.string();
  return ok;
}
Json aot_snapshot() {
  const auto aot = ppc::aot_diagnostics();
  Json result = {{"strict", aot.strict}, {"missing_attempts", aot.missing_attempts},
    {"interpreted_calls", aot.interpreted_calls}, {"interpreted_instructions", aot.interpreted_instructions},
    {"unrecorded_attempts", aot.unrecorded_attempts}, {"missing_targets", Json::array()},
    {"instruction_visits", aot.instruction_visits}, {"pc_sequence_fnv1a64", aot.pc_sequence_hash},
    {"unrecorded_pc_visits", aot.unrecorded_pc_visits}, {"pcs", Json::array()},
    {"transfer_count", aot.transfer_count}, {"transfer_sequence_fnv1a64", aot.transfer_sequence_hash},
    {"transfers", Json::array()}, {"loaded_module_records", aot.loaded_module_records},
    {"unrecorded_modules", aot.unrecorded_modules}, {"module_count", aot.module_count},
    {"module_capacity", aot.modules.size()}, {"modules", Json::array()}};
  for (size_t i = 0; i < aot.missing_count; ++i) result["missing_targets"].push_back({
    {"address", aot.missing[i].addr}, {"first_lr", aot.missing[i].first_lr}, {"calls", aot.missing[i].calls}});
  for (size_t i = 0; i < aot.pc_count; ++i) result["pcs"].push_back({
    {"pc", aot.pcs[i].pc}, {"first_word", aot.pcs[i].first_word}, {"last_word", aot.pcs[i].last_word},
    {"visits", aot.pcs[i].visits}, {"code_changes", aot.pcs[i].code_changes}});
  for (size_t i = 0; i < aot.transfer_sample_count; ++i) result["transfers"].push_back({
    {"from", aot.transfers[i].from}, {"to", aot.transfers[i].to}, {"kind", aot.transfers[i].kind}});
  for (size_t i = 0; i < aot.module_count; ++i) result["modules"].push_back({
    {"name", aot.modules[i].name}, {"address", aot.modules[i].addr}, {"bytes", aot.modules[i].size},
    {"sha256_valid", aot.modules[i].sha256_valid}, {"sha256", aot.modules[i].sha256},
    {"name_truncated", aot.modules[i].name_truncated}, {"loads", aot.modules[i].loads},
    {"first_event", aot.modules[i].first_event}, {"last_event", aot.modules[i].last_event}});
  return result;
}
}  // namespace

bool HeadlessManifest::begin(const HeadlessOptions& options, int argc, const char* const* argv, std::string& error) {
  try {
    const auto& o = options.runtime;
    std::string executable, executable_hash, input_hash, iso_hash;
    if (!executable_path(executable, error) || !file_sha256(executable, executable_hash, error)) return false;
    if (!file_sha256(MELEE_PORT_INPUT_MANIFEST_PATH, input_hash, error)) return false;
    if (input_hash != MELEE_PORT_INPUT_MANIFEST_SHA256) { error = "build input manifest no longer matches this executable"; return false; }
    if (!file_sha1(o.iso, iso_hash, error)) return false;
    std::ifstream build_stream(MELEE_PORT_INPUT_MANIFEST_PATH);
    Json build = Json::parse(build_stream);
    Json command = Json::array();
    for (int i = 0; i < argc; ++i) command.push_back(argv[i]);
    Json inputs = {{"iso", {{"path", o.iso}, {"sha1", iso_hash}, {"bytes", fs::file_size(o.iso)}}},
                   {"dol_expected_sha1", "08e0bf20134dfcb260699671004527b2d6bb1a45"}};
    for (const char* name : {"codehandler.bin", "bootloader.gct", "GameSettings/GALE01r2.ini"}) {
      std::string path = (fs::path(o.sys_dir) / name).string(), hash, hash256, expected;
      if (!file_sha1(path, hash, error) || !file_sha256(path, hash256, error)) return false;
      if (build.count("slippi_sys_files_sha256")) expected = build["slippi_sys_files_sha256"].value(name, "");
      else if (build.count("slippi_sha256")) {
        const std::string suffix = "/" + std::string(name);
        for (auto it = build["slippi_sha256"].begin(); it != build["slippi_sha256"].end(); ++it) {
          if (it.key().size() >= suffix.size() && it.key().compare(it.key().size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (!expected.empty()) { error = "ambiguous Slippi asset identity in build manifest"; return false; }
            expected = it.value().get<std::string>();
          }
        }
      }
      if (expected.empty() || expected != hash256) { error = "runtime Slippi asset does not match the build manifest: " + std::string(name); return false; }
      inputs["slippi"][name] = {{"path", path}, {"sha1", hash}, {"sha256", hash256}, {"build_sha256", expected}, {"verified", true}};
    }
    if (build.count("slippi_sys_files_sha256")) {
      const auto& expected_files = build["slippi_sys_files_sha256"];
      Json actual_files = Json::object(), actual_sizes = Json::object();
      for (const auto& file : fs::recursive_directory_iterator(o.sys_dir)) {
        if (file.is_symlink()) { error = "runtime Sys tree must not contain symlinks"; return false; }
        if (!file.is_regular_file()) continue;
        std::string name = fs::relative(file.path(), o.sys_dir).generic_string(), hash;
        if (!file_sha256(file.path().string(), hash, error)) return false;
        actual_files[name] = hash;
        actual_sizes[name] = file.file_size();
      }
      if (actual_files != expected_files) { error = "runtime Sys file inventory or contents do not match the build manifest"; return false; }
      inputs["slippi_sys_files_sha256"] = actual_files;
      inputs["slippi_sys_files_bytes"] = actual_sizes;
      inputs["slippi_sys_dir"] = o.sys_dir;
      inputs["slippi_sys_tree_verified"] = true;
    } else {
      error = "build manifest lacks a complete runtime Sys inventory";
      return false;
    }
    if (!options.script.empty()) {
      std::string hash;
      if (!file_sha1(options.script, hash, error)) return false;
      inputs["script"] = {{"path", options.script}, {"sha1", hash}};
    }
    Json launch = {{"schema", "melee-native-headless-run-v1"}, {"phase", "prepared"}, {"command", command},
      {"executable", {{"path", executable}, {"sha256", executable_hash}}},
      {"input_manifest", {{"path", MELEE_PORT_INPUT_MANIFEST_PATH}, {"compiled_sha256", MELEE_PORT_INPUT_MANIFEST_SHA256}, {"actual_sha256", input_hash}}},
      {"inputs", inputs}, {"upstream_pin", jget(build, "upstream_pin", Json::object())},
      {"decomp_pin", jget(build, "decomp_pin", Json::object())}, {"gct_base", gecko::gct_base_used},
      {"bounds", {{"frames", o.frames}, {"time_base", o.time_base}, {"hang_watch_seconds", o.hang_watch}}},
      {"acceptance", {{"expect_scene", options.expect_scene}, {"expected_combined", options.expected_scene}}},
      {"capabilities", {{"offline", true}, {"strict_aot", !options.allow_interpreter}, {"gpu_presentation", false}, {"device_audio", false}}},
      {"fp_profile", ppc::fp_profile_name(ppc::fp_profile())},
      {"isolation", {{"profile_dir", o.profile_dir}, {"card_dir", o.card_dir}, {"cache_dir", o.cache_dir}, {"replay_dir", o.replay_dir}}},
      {"outputs", {{"log", o.log_file}, {"state_trace", o.state_trace}, {"audio_dump", o.audio_dump}}}};
    cache_ = o.cache_dir;
    initial_ = launch.dump();
    return write_artifact(fs::path(cache_) / "launch.json", launch, error);
  } catch (const std::exception& e) { error = std::string("cannot prepare launch identity: ") + e.what(); return false; }
}

bool HeadlessManifest::finish(int code, const std::string& cause, const std::string& detail, bool orderly_shutdown, bool producers_joined, std::string& error) {
  if (finished_) return true;
  if (initial_.empty()) { error = "launch manifest was not initialized"; return false; }
  try {
    Json result = Json::parse(initial_);
    result["phase"] = "finished";
    if (!producers_joined) { error = "refusing to seal run evidence while runtime producers remain active"; return false; }
    result["exit"] = {{"code", code}, {"cause", cause}, {"detail", detail}, {"orderly_shutdown", orderly_shutdown}, {"producers_joined", producers_joined}};
    FileArtifact log, replay;
    if (!closed_file_artifact(cache_, result["outputs"]["log"].get<std::string>(), true, log, error)) return false;
    if (!closed_file_artifact((fs::path(cache_) / "replays").string(), slippi::last_replay_path(), producers_joined, replay, error)) return false;
    result["artifacts"] = {{"log", Json::object()}, {"replay", nullptr},
      {"replay_status", replay.present ? "closed" : "none"}};
    if (log.present) result["artifacts"]["log"] = {{"path", log.path}, {"bytes", log.bytes}, {"sha256", log.sha256}, {"closed", true}};
    if (replay.present) result["artifacts"]["replay"] = {{"path", replay.path}, {"bytes", replay.bytes}, {"sha256", replay.sha256}, {"closed", true}};
    for (const char* name : {"state_trace", "audio_dump"}) {
      FileArtifact optional;
      if (!closed_file_artifact(cache_, result["outputs"][name].get<std::string>(), true, optional, error)) return false;
      result["artifacts"][name] = nullptr;
      if (optional.present) result["artifacts"][name] = {{"path", optional.path}, {"bytes", optional.bytes}, {"sha256", optional.sha256}, {"closed", true}};
    }
    result["inputs"]["dol_actual_sha1"] = disc_dol_sha1();
    result["inputs"]["dol_verified"] = disc_dol_verified();
    uint64_t commands = 0, draws = 0, vertices = 0; uint32_t copies = 0;
    gx::stats(&commands, &draws, &vertices, &copies);
    result["counters"] = {{"retraces", retrace_count()}, {"guest_timebase", cpu ? cpu->tb : 0},
      {"gx", {{"commands", commands}, {"draws", draws}, {"vertices", vertices}, {"efb_copies", copies}}},
      {"disc", {{"reads", g_disc_reads}, {"bytes", g_disc_bytes}}},
      {"audio", {{"ai_dma_frames", audio_pushed_frames()}, {"output_ok", headless_audio_output_ok()}}},
      {"slippi", {{"exi_commands", slippi::commands_seen()}, {"replays_written", slippi::replays_written()}, {"gct_load_address", slippi::gct_load_address()}}},
      {"aot", aot_snapshot()}};
    const auto recording = slippi::recording_events();
    result["recording"] = {{"commands", recording.commands}, {"game_start", recording.game_start},
      {"pre_frame", recording.pre_frame}, {"post_frame", recording.post_frame}, {"game_end", recording.game_end},
      {"has_frames", recording.has_frames}, {"min_frame", recording.min_frame}, {"max_frame", recording.max_frame},
      {"match_input_started", recording.match_input_started}};
    const auto scenes = scene_trace_snapshot();
    Json timeline = Json::array();
    for (size_t i = 0; i < scenes.size(); ++i) {
      const auto& scene = scenes.events()[i];
      timeline.push_back({{"retrace", scene.retrace}, {"mode_byte", scene.mode_byte},
                         {"state_byte", scene.state_byte}, {"combined", scene.combined()}});
    }
    result["scenes"] = {{"mode_address", SCENE_MODE_ADDRESS}, {"state_address", SCENE_STATE_ADDRESS},
      {"encoding", "0xSSMM: state byte high, mode byte low"}, {"transitions", scenes.transitions()},
      {"unrecorded", scenes.unrecorded()}, {"timeline", timeline}};
    if (result["acceptance"]["expect_scene"].get<bool>())
      result["acceptance"]["scene_observed"] = scenes.saw(result["acceptance"]["expected_combined"].get<uint16_t>());
    finished_ = write_artifact(fs::path(cache_) / "result.json", result, error);
    return finished_;
  } catch (const std::exception& e) { error = std::string("cannot finalize run evidence: ") + e.what(); return false; }
}
}  // namespace host
