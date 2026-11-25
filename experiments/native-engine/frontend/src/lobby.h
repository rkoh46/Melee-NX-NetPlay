// melee-unlocked's Multiplayer Lobby on the Switch app (services program): its serverless peer lobby, compiled from the fetched
// tree (gen/lobby/launcher_lobby_p2p.cpp with patches/0142), on a worker thread of its own, and the hand-over of an accepted
// match to the game as Slippi Direct. The home screen's Multiplayer Lobby page (mu_home.cpp) draws it.
//
// Nothing here runs on the simulation thread but two cheap calls (the presence it reads, the match it hands over): the DHT, the
// sockets, the crypto, the file writes and the name lookups are all on the lobby's own thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace fe::lobby {
// The player's lobby profile (upstream's lobby-profile.json, in the app's Lobby folder): what the Profile tab edits.
struct Profile {
  std::string name, location;
  std::vector<int> mains;            // one to three character select ids (0..25), the first is the match's fighter
};
// The character select ids' names, in upstream's order (the lobby's `characters` list).
const char* character_name(int id);
constexpr int kCharacters = 26;

// Called once by the home screen: the Lobby folder, the Slippi account (name and connect code, empty without one) and whether this
// console can play a match now (the disc found, an account). Starts the lobby's thread, which stays idle until go_online().
void start(const std::string& directory, const std::string& account_name, const std::string& account_code, bool ready);
bool started();
Profile profile();
void set_profile(const Profile& p);  // saved at once; re-announced when online
void go_online();                    // Go Online: the DHT, then the public lobby
void leave();                        // Leave: off the public roster (friends still see this player while the app runs)
// One of upstream's lobby actions (request, accept, cancel, chat, friend, friend_accept, friend_decline, unfriend, available),
// queued for the lobby's thread. Its errors come back as the snapshot's notice.
void command(const std::string& action, const nlohmann::json& data = nlohmann::json::object());

// What the page shows, copied under a lock (render thread): upstream's state() plus
//   "phase":  "offline" | "starting" | "online" | "error"
//   "error":  why the lobby could not start (upstream's own text)
//   "notice": the latest line for the player (a refusal, a delivery, an error of an action), "notice_seq"
//   "pings":  {id: ms} measured with the players a request is open with
//   "ready", "account": this console's side
nlohmann::json snapshot();

// An accepted match, taken once by the home screen: the opponent's connect code and name, this player's fighter.
struct Match { std::string request, code, name; int character = 2; };
bool take_match(Match* out);
// The home screen hands the match to the game (the app starts with Slippi's menus): the game goes to Training as the fighter and
// searches Direct for the code from there (upstream's static recomp does the same with its practice matchmaking).
void hand_to_game(const Match& m);
}  // namespace fe::lobby
