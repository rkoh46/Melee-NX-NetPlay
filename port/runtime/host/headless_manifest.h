// Structured evidence for a single isolated native run.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "headless_options.h"

namespace host {
class HeadlessManifest {
public:
  bool begin(const HeadlessOptions& options, int argc, const char* const* argv, std::string& error);
  bool finish(int code, const std::string& cause, const std::string& detail, bool orderly_shutdown, bool producers_joined, std::string& error);
private:
  std::string cache_, initial_;
  bool finished_ = false;
};
}
