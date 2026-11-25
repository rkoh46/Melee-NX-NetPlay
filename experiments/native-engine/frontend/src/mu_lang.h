// melee-unlocked's launcher languages on the home screen (its port/app/launcher_lang.h rules, its lang/*.txt tables embedded
// by build.sh sync). Fixed English text is looked up by its wording ("@English = ..." lines); a line with no translation,
// and every line the Switch app adds itself, stays English. launcher.ini `language` (upstream's key), empty: English.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace fe::mu_lang {
struct Language { const char* code; const char* native; int script; };   // script: 0 Latin/Cyrillic, 1 Japanese, 2 Chinese, 3 Korean
const std::vector<Language>& languages();
bool set(const std::string& code);      // false: unknown (English kept)
std::string current();
const Language* find(const std::string& code);
std::string tx(const std::string& english);   // the current language's wording, or the English given
std::string by_key(const std::string& key);   // the current language's line for one of upstream's keys ("" for English)
}  // namespace fe::mu_lang
