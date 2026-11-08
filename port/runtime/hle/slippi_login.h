// Native Slippi sign-in: the same Firebase email/password flow the Slippi Launcher uses,
// then the play key from Slippi's GraphQL backend, written as user.json for the game.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>

namespace slippi::login {
struct Account {
  std::string uid, display_name, connect_code, play_key, latest_version;
  bool valid() const { return !uid.empty() && !play_key.empty() && !connect_code.empty(); }
};
// Ranked profile from Slippi's backend (getUser with the signed-in token).
struct CharacterUsage { int character = -1; int games = 0; };   // external character id
struct Profile {
  bool ranked = false;                // has a ranked profile
  float rating = 0.0f;                // ratingOrdinal
  int rating_updates = 0;             // sets played that changed the rating
  int wins = 0, losses = 0;
  int global_placement = 0, regional_placement = 0;   // daily placements, 0 = unranked
  std::string continent;
  std::vector<CharacterUsage> characters;
  std::string display_name, connect_code;
};
// Slippi rank tier for a rating; placement 1..300 and 5 or more sets promote to Grandmaster.
std::string rank_name(float rating, int rating_updates, int global_placement);
const char* character_name(int external_id);
const char* stage_name(int stage_id);

// Saved session (email + Firebase refresh token) in <dir>/session.json: keeps the app signed in.
struct Session { std::string email, refresh_token, uid; };
bool read_session(const std::string& dir, Session& out);
bool write_session(const std::string& dir, const Session& session);
void remove_session(const std::string& dir);
// Exchanges the refresh token for a fresh ID token (an hour of validity).
bool refresh_id_token(const Session& session, std::string& id_token, std::string& error);
bool fetch_profile(const std::string& id_token, const std::string& uid, Profile& out, std::string& error);

// Blocking network calls; run off the UI thread. `error` is user-facing.
bool sign_in(const std::string& email, const std::string& password, Account& out, std::string& error);
// Same as sign_in and also returns the Firebase refresh token for `Session`.
bool sign_in_session(const std::string& email, const std::string& password, Account& out, Session& session, std::string& error);
bool send_password_reset(const std::string& email, std::string& error);
bool read_user_file(const std::string& dir, Account& out);
bool write_user_file(const std::string& dir, const Account& account, std::string& error);
void remove_user_file(const std::string& dir);
}  // namespace slippi::login
