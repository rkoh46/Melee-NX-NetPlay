// The download melee-unlocked's phillip_ai.cpp asks for, for the frontend's build of it (features/proai.cpp): the app's
// libcurl and TLS, an HTTPS GET into a temporary file, its SHA-256 checked before it takes the policy's name. Not
// melee-unlocked's updater.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace host::updater {
bool fetch_verified(const std::string& url, const std::string& sha256, const std::string& dest_utf8, std::string* why);
// Upstream's own content release is not a source here: only Hugging Face's vladfi/phillip-models (phillip_ai.cpp's
// other allowed prefix), so this names the same place.
constexpr const char* kContentBase = "https://huggingface.co/vladfi/phillip-models/resolve/";
}  // namespace host::updater
