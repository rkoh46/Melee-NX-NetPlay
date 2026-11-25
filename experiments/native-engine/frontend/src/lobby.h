// melee-unlocked's Multiplayer Lobby on the Switch app (services program): its serverless peer lobby, compiled from the fetched
// tree (gen/lobby/launcher_lobby_p2p.cpp, as fetched), on a worker thread of its own, the hand-over of an accepted match to the
// game (Slippi Direct, or melee-unlocked's own peer-to-peer match) and the match history. The home screen's Multiplayer Lobby page (mu_home.cpp) draws it.
//
// Nothing here runs on the simulation thread but two cheap calls (the presence it reads, the match it hands over): the DHT, the
// sockets, the crypto, the file writes and the name lookups are all on the lobby's own thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace fe::lobby {
// The player's lobby profile (upstream's lobby-profile.json, in the app's Lobby folder): what the Profile tab edits.
struct Profile {
  std::string name, location;
  std::vector<int> mains;            // one to three character select ids (0..25), the first is the match's fighter
  bool auto_reject = false;          // upstream's "Auto reject invites"
  bool request_sound = true;         // upstream's request chime, and its volume (percent)
  int request_volume = 65;
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

// An accepted match, taken once by the home screen: the opponent's connect code and name, this player's fighter; a peer-to-peer
// match carries the game's arguments for it (upstream's p2p_arguments) and its result file.
struct Match {
  std::string request, code, name, build;
  int character = 2;
  bool p2p = false;
  std::vector<std::string> args;
  std::string result_file;
};
bool take_match(Match* out);
// The home screen hands the match to the game (the app starts with Slippi's menus): the game goes to Training as the fighter and
// searches Direct for the code from there (upstream's static recomp does the same with its practice matchmaking).
// A peer-to-peer match first closes the lobby (the game takes over its UDP port), as upstream's launcher does.
void hand_to_game(const Match& m);
// play_main.cpp: the game arguments of a peer-to-peer lobby match (once), and after a run whether it was a lobby match that is
// over: the app then goes back to its home screen, and the lobby opens again.
bool session_args(std::vector<std::string>* args);
bool take_return_home();
// The home screen: true once after a lobby match brought the app back (it opens on the lobby page).
bool returning();
// The local match history (upstream's lobby-history.json, newest first) and the wins and losses in it, all or against one code.
nlohmann::json history();
std::pair<int, int> record(const std::string& code = {});
}  // namespace fe::lobby
