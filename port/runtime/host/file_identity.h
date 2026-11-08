// Streaming file identity for reproducible native launch artifacts.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include "sha256.h"
namespace host {
bool file_sha1(const std::string& path, std::string& digest, std::string& error);
bool file_sha256(const std::string& path, std::string& digest, std::string& error);
bool executable_path(std::string& path, std::string& error);
struct FileArtifact { bool present = false; std::string path, sha256; uint64_t bytes = 0; };
// An unclosed artifact is deliberately not read or identified. Closed artifacts
// must be regular files directly inside the supplied isolated directory.
bool closed_file_artifact(const std::string& directory, const std::string& path, bool closed,
                          FileArtifact& artifact, std::string& error);
}
