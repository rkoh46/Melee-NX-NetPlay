// A headless melee-unlocked lobby peer for test-lobby-linux.sh: melee-unlocked's own port/app/launcher_lobby_p2p.cpp compiled
// WITHOUT this repository's patches/0142 (as its Windows launcher with the Slippi layer builds it: both match transports in its
// hello), on the frontend's POSIX shim. It stands in for a Melee Unlocked PC in the lobby: it joins with that PC's profile
// fields (Source Port 1.0.5), lists who it sees with their status and stocks, and answers nothing (requests to it expire).
//   lobby_peer DIRECTORY BOOTSTRAP SECONDS NAME CODE
// Prints "peer: ..." lines: the roster each time it changes. Tests only.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher_lobby_p2p.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace fe::mu_lang { std::string by_key(const std::string&) { return {}; } }   // English only

int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: lobby_peer DIRECTORY BOOTSTRAP SECONDS NAME CODE\n"); return 2; }
  const int seconds = std::atoi(argv[3]);
  try {
    launcher::lobby::PeerLobby lobby(argv[1], argv[2]);
    nlohmann::json profile = {{"name", argv[4]}, {"code", argv[5]}, {"location", "PC stand-in"}, {"build", MU_PEER_BUILD},
                              {"mains", {9, 2, 20}}, {"ready", true}, {"open", {"vanilla"}}};
    lobby.join(profile);
    std::printf("peer: online as %s (%s), build %s, id %s\n", argv[4], argv[5], MU_PEER_BUILD, lobby.id().substr(0, 8).c_str());
    std::fflush(stdout);
    std::string last;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
      lobby.tick();
      const auto state = lobby.state();
      std::string now;
      for (const auto& p : state.value("players", nlohmann::json::array()))
        now += "[" + p.value("name", std::string("?")) + " " + p.value("code", std::string()) + " " + p.value("build", std::string()) + " " +
               p.value("status", std::string()) + " stocks " + p.value("stocks", nlohmann::json::array()).dump() + "] ";
      if (now != last) { std::printf("peer: %zu player(s): %s\n", state.value("players", nlohmann::json::array()).size(), now.c_str()); std::fflush(stdout); last = now; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    lobby.command("leave");
    for (int i = 0; i < 5; ++i) { lobby.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  } catch (const std::exception& e) {
    std::printf("peer: %s\n", e.what());
    return 1;
  }
  return 0;
}
