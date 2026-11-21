// Resolves the `slp:<disc file>:<anchor>...` asset links of Slippi's game files against the player's disc.
// C++ port of the `hsd` crate of project-slippi/slippi-rust-extensions (GPL-2.0, commit 934f373).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slippi::hsd {
enum class LinkStatus { Resolved, NoLinks, Failed };

struct LinkResult {
  LinkStatus status = LinkStatus::NoLinks;
  std::vector<std::string> disc_files;  // disc files read, in order
  std::vector<std::string> warnings;    // what the reference logs as warnings
  std::string error;                    // why status is Failed
  size_t links = 0;                     // number of slp: references in the archive
};

// read_disc(name, out) returns false when the disc has no such file.
using DiscReader = std::function<bool(const std::string& name, std::vector<uint8_t>& out)>;

// Mirrors slprs_gamefile_resolve. On Resolved, `contents` is replaced by the serialised resolved
// archive. On NoLinks/Failed it is left untouched.
LinkResult resolve_links(std::vector<uint8_t>& contents, const DiscReader& read_disc);
}  // namespace slippi::hsd
