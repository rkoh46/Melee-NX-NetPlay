// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_login.h"
#include "slippi_http.h"
#include <nlohmann/json.hpp>
#include "json_safe.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace slippi::login {
namespace {
using json = nlohmann::json;
// The Slippi project's Firebase web key. Firebase web keys identify the project rather than
// authenticate a caller (the Launcher ships the same one); access is governed server-side.
constexpr const char* kFirebaseKey = "AIzaSyAuQqc_wgqcUu3FqrICEPZ9Av_hPxMR_i4";
constexpr const char* kIdentityToolkit = "https://identitytoolkit.googleapis.com/v1/accounts:";
constexpr const char* kGraphQL = "https://internal.slippi.gg/graphql";
constexpr const char* kUserAgent = "Melee-NX-NetPlay";
constexpr const char* kUserQuery =
    "query getUserKeyQuery($fbUid: String) { getUser(fbUid: $fbUid) { fbUid displayName connectCode { code } private { playKey } } "
    "getLatestDolphin { version } }";
constexpr const char* kProfileQuery =
    "query ($fbUid: String) { getUser(fbUid: $fbUid) { fbUid displayName connectCode { code } rankedNetplayProfile { id ratingOrdinal ratingUpdateCount wins losses "
    "dailyGlobalPlacement dailyRegionalPlacement continent characters { character gameCount } } } }";
constexpr const char* kSecureToken = "https://securetoken.googleapis.com/v1/token?key=";

std::string firebase_message(const std::string& code) {
  if (code.rfind("INVALID_LOGIN_CREDENTIALS", 0) == 0 || code.rfind("INVALID_PASSWORD", 0) == 0 || code.rfind("EMAIL_NOT_FOUND", 0) == 0)
    return "Wrong email or password.";
  if (code.rfind("USER_DISABLED", 0) == 0) return "This account has been disabled.";
  if (code.rfind("TOO_MANY_ATTEMPTS", 0) == 0) return "Too many attempts; try again later.";
  if (code.rfind("INVALID_EMAIL", 0) == 0) return "That is not a valid email address.";
  return "Sign-in failed: " + code;
}

bool post_json(const std::string& url, const std::string& headers, const json& body, json& reply, std::string& error) {
  int status = 0;
  std::string response, transport;
  if (!report::http_request("POST", url, headers, body.dump(), kUserAgent, &status, &response, &transport)) {
    error = "No connection: " + transport;
    return false;
  }
  try { reply = json::parse(response); } catch (const std::exception&) { reply = json::object(); }
  if (status < 200 || status >= 300) {
    std::string code;
    if (auto it = reply.find("error"); it != reply.end() && it->is_object()) code = jget(*it, "message", "");
    error = code.empty() ? "Server error (" + std::to_string(status) + ")" : code;
    return false;
  }
  return true;
}
}  // namespace

static bool sign_in_impl(const std::string& email, const std::string& password, Account& out, Session* session, std::string& error);
bool sign_in(const std::string& email, const std::string& password, Account& out, std::string& error) { return sign_in_impl(email, password, out, nullptr, error); }
bool sign_in_session(const std::string& email, const std::string& password, Account& out, Session& session, std::string& error) {
  return sign_in_impl(email, password, out, &session, error);
}
static bool sign_in_impl(const std::string& email, const std::string& password, Account& out, Session* session, std::string& error) {
  json reply;
  const std::string sign_in_url = std::string(kIdentityToolkit) + "signInWithPassword?key=" + kFirebaseKey;
  if (!post_json(sign_in_url, "Content-Type: application/json", {{"email", email}, {"password", password}, {"returnSecureToken", true}}, reply, error)) {
    if (error.rfind("No connection", 0) != 0 && error.rfind("Server error", 0) != 0) error = firebase_message(error);
    return false;
  }
  const std::string token = jget(reply, "idToken", ""), uid = jget(reply, "localId", "");
  if (token.empty() || uid.empty()) { error = "Sign-in failed: no session token."; return false; }
  if (session) { session->email = email; session->refresh_token = jget(reply, "refreshToken", ""); session->uid = uid; }

  json user;
  if (!post_json(kGraphQL, "Content-Type: application/json\r\nAuthorization: Bearer " + token,
                 {{"query", kUserQuery}, {"variables", {{"fbUid", uid}}}}, user, error)) {
    error = "Signed in, but Slippi's account service did not answer: " + error;
    return false;
  }
  auto object_at = [](const json& j, const char* key) -> const json* {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
  };
  const json* data = object_at(user, "data");
  const json* u = data ? object_at(*data, "getUser") : nullptr;
  if (!u) { error = "Signed in, but this account has no Slippi profile yet. Finish setting it up at slippi.gg."; return false; }
  out.uid = uid;
  out.display_name = jget(*u, "displayName", "");
  if (const json* code = object_at(*u, "connectCode")) out.connect_code = jget(*code, "code", "");
  if (const json* priv = object_at(*u, "private")) out.play_key = jget(*priv, "playKey", "");
  if (const json* dolphin = object_at(*data, "getLatestDolphin")) out.latest_version = jget(*dolphin, "version", "");
  if (!out.valid()) { error = "This account has no connect code yet. Pick one at slippi.gg, then sign in again."; return false; }
  return true;
}

bool refresh_id_token(const Session& session, std::string& id_token, std::string& error) {
  if (session.refresh_token.empty()) { error = "No saved session."; return false; }
  int status = 0; std::string response, transport;
  const std::string body = "grant_type=refresh_token&refresh_token=" + session.refresh_token;
  if (!report::http_request("POST", std::string(kSecureToken) + kFirebaseKey, "Content-Type: application/x-www-form-urlencoded", body, kUserAgent, &status, &response, &transport)) {
    error = "No connection: " + transport; return false;
  }
  json reply;
  try { reply = json::parse(response); } catch (const std::exception&) { reply = json::object(); }
  if (status < 200 || status >= 300) { error = "Saved session expired; sign in again."; return false; }
  id_token = jget(reply, "id_token", "");
  if (id_token.empty()) { error = "Saved session expired; sign in again."; return false; }
  return true;
}

bool fetch_profile(const std::string& id_token, const std::string& uid, Profile& out, std::string& error) {
  json user;
  if (!post_json(kGraphQL, "Content-Type: application/json\r\nAuthorization: Bearer " + id_token,
                 {{"query", kProfileQuery}, {"variables", {{"fbUid", uid}}}}, user, error)) return false;
  auto object_at = [](const json& j, const char* key) -> const json* {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
  };
  const json* data = object_at(user, "data");
  const json* u = data ? object_at(*data, "getUser") : nullptr;
  if (!u) { error = "No profile."; return false; }
  out = Profile();
  out.display_name = jget(*u, "displayName", "");
  if (const json* code = object_at(*u, "connectCode")) out.connect_code = jget(*code, "code", "");
  if (const json* r = object_at(*u, "rankedNetplayProfile")) {
    out.ranked = true;
    out.rating = jget(*r, "ratingOrdinal", 0.0f);
    out.rating_updates = jget(*r, "ratingUpdateCount", 0);
    out.wins = jget(*r, "wins", 0); out.losses = jget(*r, "losses", 0);
    out.global_placement = jget(*r, "dailyGlobalPlacement", 0); out.regional_placement = jget(*r, "dailyRegionalPlacement", 0);
    out.continent = jget(*r, "continent", "");
    auto it = r->find("characters");
    if (it != r->end() && it->is_array())
      for (const json& c : *it) { CharacterUsage cu; cu.character = jget(c, "character", -1); cu.games = jget(c, "gameCount", 0); out.characters.push_back(cu); }
    std::sort(out.characters.begin(), out.characters.end(), [](const CharacterUsage& a, const CharacterUsage& b) { return a.games > b.games; });
  }
  return true;
}

std::string rank_name(float r, int updates, int placement) {
  if (updates < 5) return "Pending";
  if (r >= 2350.0f && placement >= 1 && placement <= 300) return "Grandmaster";
  static const struct { float floor; const char* name; } tiers[] = {
      {2350.0f, "Master III"}, {2275.0f, "Master II"}, {2191.75f, "Master I"}, {2136.28f, "Diamond III"}, {2073.67f, "Diamond II"}, {2003.92f, "Diamond I"},
      {1927.03f, "Platinum III"}, {1843.0f, "Platinum II"}, {1751.83f, "Platinum I"}, {1653.52f, "Gold III"}, {1548.07f, "Gold II"}, {1435.48f, "Gold I"},
      {1315.75f, "Silver III"}, {1188.88f, "Silver II"}, {1054.87f, "Silver I"}, {913.72f, "Bronze III"}, {765.43f, "Bronze II"}, {0.0f, "Bronze I"}};
  for (const auto& t : tiers) if (r >= t.floor) return t.name;
  return "Bronze I";
}
const char* character_name(int id) {
  static const char* const names[] = {"Captain Falcon", "Donkey Kong", "Fox", "Mr. Game & Watch", "Kirby", "Bowser", "Link", "Luigi", "Mario", "Marth", "Mewtwo",
                                      "Ness", "Peach", "Pikachu", "Ice Climbers", "Jigglypuff", "Samus", "Yoshi", "Zelda", "Sheik", "Falco", "Young Link",
                                      "Dr. Mario", "Roy", "Pichu", "Ganondorf"};
  return id >= 0 && id < (int)(sizeof names / sizeof names[0]) ? names[id] : "Unknown";
}
const char* stage_name(int id) {
  switch (id) {
    case 2: return "Fountain of Dreams"; case 3: return "Pokémon Stadium"; case 4: return "Princess Peach's Castle"; case 5: return "Kongo Jungle";
    case 6: return "Brinstar"; case 7: return "Corneria"; case 8: return "Yoshi's Story"; case 9: return "Onett"; case 10: return "Mute City";
    case 11: return "Rainbow Cruise"; case 12: return "Jungle Japes"; case 13: return "Great Bay"; case 14: return "Hyrule Temple"; case 15: return "Brinstar Depths";
    case 16: return "Yoshi's Island"; case 17: return "Green Greens"; case 18: return "Fourside"; case 19: return "Mushroom Kingdom I"; case 20: return "Mushroom Kingdom II";
    case 22: return "Venom"; case 23: return "Poké Floats"; case 24: return "Big Blue"; case 25: return "Icicle Mountain"; case 26: return "Icetop";
    case 27: return "Flat Zone"; case 28: return "Dream Land"; case 29: return "Yoshi's Island (N64)"; case 30: return "Kongo Jungle (N64)";
    case 31: return "Battlefield"; case 32: return "Final Destination"; default: return "Stage";
  }
}

bool read_session(const std::string& dir, Session& out) {
  std::ifstream f(dir + "/session.json");
  if (!f) return false;
  try { json j = json::parse(f); out.email = jget(j, "email", ""); out.refresh_token = jget(j, "refreshToken", ""); out.uid = jget(j, "uid", ""); }
  catch (const std::exception&) { return false; }
  return !out.refresh_token.empty();
}
bool write_session(const std::string& dir, const Session& s) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream f(dir + "/session.json", std::ios::trunc);
  if (!f) return false;
  json j = {{"email", s.email}, {"refreshToken", s.refresh_token}, {"uid", s.uid}};
  f << j.dump(2) << "\n";
  f.close();
  return (bool)f;
}
void remove_session(const std::string& dir) { std::error_code ec; std::filesystem::remove(dir + "/session.json", ec); }

bool send_password_reset(const std::string& email, std::string& error) {
  json reply;
  const std::string url = std::string(kIdentityToolkit) + "sendOobCode?key=" + kFirebaseKey;
  if (!post_json(url, "Content-Type: application/json", {{"requestType", "PASSWORD_RESET"}, {"email", email}}, reply, error)) {
    if (error.rfind("No connection", 0) != 0 && error.rfind("Server error", 0) != 0) error = firebase_message(error);
    return false;
  }
  return true;
}

bool read_user_file(const std::string& dir, Account& out) {
  std::ifstream f(dir + "/user.json");
  if (!f) return false;
  try {
    json j = json::parse(f);
    out.uid = jget(j, "uid", ""); out.play_key = jget(j, "playKey", ""); out.connect_code = jget(j, "connectCode", "");
    out.display_name = jget(j, "displayName", ""); out.latest_version = jget(j, "latestVersion", "");
  } catch (const std::exception&) { return false; }
  return out.valid();
}

bool write_user_file(const std::string& dir, const Account& a, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  json j = {{"uid", a.uid}, {"playKey", a.play_key}, {"connectCode", a.connect_code}, {"displayName", a.display_name}, {"latestVersion", a.latest_version}};
  std::ofstream f(dir + "/user.json", std::ios::trunc);
  if (!f) { error = "Cannot write " + dir + "/user.json"; return false; }
  f << j.dump(2) << "\n";
  f.close();
  if (!f) { error = "Cannot write " + dir + "/user.json"; return false; }
  return true;
}

void remove_user_file(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove(dir + "/user.json", ec);
}
}  // namespace slippi::login
