// A headless melee-unlocked lobby peer for test-lobby-linux.sh: melee-unlocked's own port/app/launcher_lobby_p2p.cpp, as fetched,
// on the frontend's POSIX shim. It stands in for a Melee Unlocked PC in the lobby (its Windows launcher with the Slippi layer: both
// match transports in its hello): it joins with that PC's profile fields (Source Port 1.0.5), lists who it sees with their status
// and stocks, and does what the test asks through a command file, one line each:
//   request NAME           a Slippi Direct match request to the player called NAME
//   p2p NAME               a peer-to-peer match request (melee-unlocked's own match) to NAME
// An accepted peer-to-peer match starts its game as melee-unlocked's launcher does, with p2p_arguments(...): here the native
// engine's headless runner (PEER_GAME, PEER_ISO, PEER_OUT), its own UDP port (upstream's test switch MELEE_P2P_SEPARATE_PORT=1,
// so this lobby stays up), generated inputs (--net-inputs 3). Tests only.
//   lobby_peer DIRECTORY BOOTSTRAP SECONDS NAME CODE [COMMAND_FILE]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher_lobby_p2p.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace fe::mu_lang { std::string by_key(const std::string&) { return {}; } }   // English only

namespace {
std::vector<std::string> split_arguments(const std::string& line) {
  std::vector<std::string> out; std::string word; bool quoted = false, any = false;
  for (char c : line) {
    if (c == '"') { quoted = !quoted; any = true; continue; }
    if (c == ' ' && !quoted) { if (any) out.push_back(word); word.clear(); any = false; continue; }
    word += c; any = true;
  }
  if (any) out.push_back(word);
  return out;
}
pid_t start_game(const std::vector<std::string>& p2p) {
  const char* game = std::getenv("PEER_GAME"); const char* iso = std::getenv("PEER_ISO"); const char* out = std::getenv("PEER_OUT");
  if (!game || !iso || !out) { std::printf("peer: no game to start (PEER_GAME, PEER_ISO, PEER_OUT)\n"); return -1; }
  std::vector<std::string> args = {game, "--iso", iso, "--out", out, "--frames", std::getenv("PEER_FRAMES") ? std::getenv("PEER_FRAMES") : "20000", "--net-inputs", "3"};
  for (const auto& a : p2p) args.push_back(a);
  std::string shown;
  for (const auto& a : args) shown += " " + a;
  std::printf("peer: starting the game:%s\n", shown.c_str()); std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const std::string log = std::string(out) + "/console.log";
    if (FILE* f = std::freopen(log.c_str(), "w", stdout)) { (void)f; dup2(fileno(stdout), 2); }
    execv(game, argv.data());
    _exit(127);
  }
  return pid;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: lobby_peer DIRECTORY BOOTSTRAP SECONDS NAME CODE [COMMAND_FILE]\n"); return 2; }
  const int seconds = std::atoi(argv[3]);
  const std::string commands = argc > 6 ? argv[6] : "";
  setenv("MELEE_P2P_SEPARATE_PORT", "1", 1);
  try {
    launcher::lobby::PeerLobby lobby(argv[1], argv[2]);
    nlohmann::json profile = {{"name", argv[4]}, {"code", argv[5]}, {"location", "PC stand-in"}, {"build", MU_PEER_BUILD},
                              {"mains", {9, 2, 20}}, {"ready", true}, {"open", {"vanilla"}}};
    lobby.join(profile);
    std::printf("peer: online as %s (%s), build %s, id %s\n", argv[4], argv[5], MU_PEER_BUILD, lobby.id().substr(0, 8).c_str());
    std::fflush(stdout);
    std::string last; size_t done = 0; unsigned shown_notice = 0;
    std::set<std::string> logged;
    pid_t game = -1;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
      lobby.presence({{"status", game > 0 ? "In game" : "Online"}, {"stocks", nlohmann::json::array()}});
      lobby.tick();
      const auto state = lobby.state();
      std::string now;
      for (const auto& p : state.value("players", nlohmann::json::array()))
        now += "[" + p.value("name", std::string("?")) + " " + p.value("code", std::string()) + " " + p.value("build", std::string()) + " " +
               p.value("status", std::string()) + " stocks " + p.value("stocks", nlohmann::json::array()).dump() + " p2p " +
               (p.value("p2p", false) ? "yes" : "no") + "] ";
      if (now != last) { std::printf("peer: %zu player(s): %s\n", state.value("players", nlohmann::json::array()).size(), now.c_str()); std::fflush(stdout); last = now; }
      if (state.value("notice_seq", 0u) != shown_notice) {
        shown_notice = state.value("notice_seq", 0u);
        std::printf("peer: notice: %s\n", state.value("notice", std::string()).c_str()); std::fflush(stdout);
      }
      for (const auto& r : state.value("requests", nlohmann::json::array())) {
        const std::string key = r.value("id", std::string()) + r.value("state", std::string());
        if (logged.insert(key).second) { std::printf("peer: request %s to %s: %s\n", r.value("id", std::string()).substr(0, 8).c_str(),
                                                     r["peer"].value("name", std::string("?")).c_str(), r.value("state", std::string()).c_str()); std::fflush(stdout); }
      }
      // The test's commands, each line once.
      if (!commands.empty()) {
        std::ifstream in(commands); std::string line; size_t n = 0;
        while (std::getline(in, line)) {
          if (n++ < done) continue;
          ++done;
          const size_t space = line.find(' ');
          const std::string verb = line.substr(0, space), name = space == std::string::npos ? "" : line.substr(space + 1);
          std::string target;
          for (const auto& p : state.value("players", nlohmann::json::array())) if (p.value("name", std::string()) == name) target = p.value("id", std::string());
          try {
            if (target.empty()) throw std::runtime_error("no player called " + name);
            nlohmann::json data = {{"target", target}, {"mode", "vanilla"}};
            if (verb == "p2p") data["transport"] = launcher::lobby::kP2pTransport;
            lobby.command("request", data);
            std::printf("peer: %s request sent to %s\n", verb == "p2p" ? "peer-to-peer" : "Slippi Direct", name.c_str());
          } catch (const std::exception& e) { std::printf("peer: %s %s: %s\n", verb.c_str(), name.c_str(), e.what()); }
          std::fflush(stdout);
        }
      }
      nlohmann::json launch;
      while (lobby.take_launch(launch)) {
        if (!launch.count("p2p")) { std::printf("peer: a Slippi Direct match was accepted (no game here)\n"); continue; }
        const std::string out = std::getenv("PEER_OUT") ? std::getenv("PEER_OUT") : ".";
        const std::string line = launcher::lobby::p2p_arguments(launch["p2p"], std::string(argv[1]) + "/lobby-peer-identity.json", out + "/p2p-result.json");
        std::printf("peer: peer-to-peer match accepted against %s: %s\n", launch.value("name", std::string("?")).c_str(), line.empty() ? "SETUP REFUSED" : "starting the game");
        std::fflush(stdout);
        if (!line.empty() && game < 0) game = start_game(split_arguments(line));
      }
      if (game > 0) {
        int status = 0;
        if (waitpid(game, &status, WNOHANG) == game) {
          std::printf("peer: the game ended (%d)\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1); std::fflush(stdout);
          game = 0;
          lobby.command("available");
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    lobby.command("leave");
    for (int i = 0; i < 5; ++i) { lobby.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    if (game > 0) { kill(game, SIGTERM); waitpid(game, nullptr, 0); }
  } catch (const std::exception& e) {
    std::printf("peer: %s\n", e.what());
    return 1;
  }
  return 0;
}
