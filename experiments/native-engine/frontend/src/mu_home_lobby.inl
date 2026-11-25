// The home screen's Multiplayer Lobby page (included by mu_home.cpp, inside its namespace): melee-unlocked's Multiplayer Lobby tab
// (port/app/launcher_lobby.cpp and launcher_lobby_ui.inl) on a controller and the launcher's 720 x 480 layout. Its parts:
//   Go Online / Leave (upstream's GO_ONLINE), the network state under the title;
//   tabs: Players (its roster card: avatar, name, Game Build, status / location / ping, mains, stocks in a match; Send match request
//   for Slippi Direct, Peer-to-peer match for melee-unlocked's own, Private chat, Add friend), Lobby chat (the public chat and a tab
//   per private room, upstream's room tabs), Friends (and friend requests), Match history (its W/L record and games), Your profile
//   (name, the Slippi code read from the account, location, three mains, auto reject invites, the request sound and its volume);
//   the request bar (Accept / Decline, or Cancel, with the time left and the measured ping), and the status line (its notices).
// Upstream's empty states and their words: "Find your next match", "Ready for a few games?", "Friends, one click away.", "Match
// history". Added for the console: "Looking for players" while the lobby network has found nobody yet, and the reason when the
// lobby cannot start. The request chime is upstream's (660 Hz then 880 Hz, 0.3 s), played through SDL's audio output.
// SPDX-License-Identifier: GPL-3.0-or-later

using LobbyJson = nlohmann::json;

enum LobbyItem { LI_GO, LI_TABS, LI_PLAYER, LI_REQUEST, LI_P2P, LI_PM, LI_ADD_FRIEND, LI_ROOM, LI_WRITE, LI_PM_ACCEPT, LI_PM_DECLINE,
                 LI_PM_CLOSE, LI_FRIEND, LI_FRIEND_A, LI_FRIEND_B, LI_FRIEND_CODE, LI_NAME, LI_LOCATION, LI_MAIN, LI_AUTO_REJECT,
                 LI_SOUND, LI_VOLUME, LI_ACCEPT, LI_DECLINE, LI_CANCEL };
struct LobbyFocus { int kind; int index; std::string label; };
constexpr int kLobbyTabCount = 5;
constexpr const char* kLobbyTabs[kLobbyTabCount] = {"Players", "Lobby chat", "Friends", "Match history", "Your profile"};

// Account name and code from user.json (never its key), for the profile.
void account_parts(const std::string& user_dir, std::string* name, std::string* code) {
  std::ifstream in(user_dir + "/user.json");
  std::stringstream ss; ss << in.rdbuf();
  const LobbyJson j = LobbyJson::parse(ss.str(), nullptr, false);
  *name = j.is_object() ? j.value("displayName", std::string()) : std::string();
  *code = j.is_object() ? j.value("connectCode", std::string()) : std::string();
}

// Upstream's request_chime: two tones, 660 Hz then 880 Hz, 0.3 s, at the player's volume, on its own thread.
void request_chime(int volume) {
  if (volume <= 0) return;
  std::thread([volume] {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { say("home: lobby request sound: no audio output (%s)", SDL_GetError()); return; }
    constexpr int rate = 22050, frames = rate * 3 / 10;
    std::vector<int16_t> samples(frames);
    for (int i = 0; i < frames; ++i) {
      const double time = double(i) / rate, frequency = i < frames / 2 ? 660.0 : 880.0;
      const double envelope = std::min(1.0, time * 35.0) * std::min(1.0, (frames - i) / 1800.0);
      samples[size_t(i)] = int16_t(std::sin(time * frequency * 6.283185307179586) * envelope * 7500 * volume / 100);
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, rate};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) { say("home: lobby request sound: the audio output did not open (%s)", SDL_GetError()); return; }
    SDL_AudioSpec device{}; int device_frames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &device_frames);
    SDL_PutAudioStreamData(stream, samples.data(), int(samples.size() * sizeof samples[0]));
    SDL_FlushAudioStream(stream);
    SDL_ResumeAudioStreamDevice(stream);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    SDL_DestroyAudioStream(stream);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    say("home: lobby request sound played (volume %d%%; output %d Hz, %d channels, format %04X)", volume, device.freq, device.channels, unsigned(device.format));
  }).detach();
}

const LobbyJson* pending_request(const LobbyJson& snap, const std::string& self, bool* incoming) {
  for (const auto& r : snap.value("requests", LobbyJson::array())) {
    const std::string state = r.value("state", std::string());
    if (state != "pending" && state != "accepted") continue;
    *incoming = r.value("to", std::string()) == self;
    static LobbyJson keep; keep = r;
    return &keep;
  }
  return nullptr;
}
LobbyJson lobby_friends(const LobbyJson& snap) {
  LobbyJson list = LobbyJson::array();
  for (const auto& f : snap.value("friend_requests", LobbyJson::array())) { LobbyJson x = f; x["incoming"] = true; list.push_back(x); }
  for (const auto& f : snap.value("friends", LobbyJson::array())) list.push_back(f);
  return list;
}
// The chat tab's rooms: the public chat ("") and each private room, as upstream's room tabs list them.
std::vector<std::string> lobby_rooms(const LobbyJson& snap) {
  std::vector<std::string> rooms{""};
  for (const auto& r : snap.value("private", LobbyJson::array())) rooms.push_back(r.value("id", std::string()));
  return rooms;
}
const LobbyJson* lobby_room(const LobbyJson& snap, const std::string& id) {
  static LobbyJson keep;
  for (const auto& r : snap.value("private", LobbyJson::array())) if (r.value("id", std::string()) == id) { keep = r; return &keep; }
  return nullptr;
}

std::vector<LobbyFocus> lobby_items(const Home& h, const LobbyJson& snap) {
  std::vector<LobbyFocus> v;
  const bool online = snap.value("phase", std::string()) == "online";
  v.push_back({LI_GO, 0, online ? "Leave" : "Go Online"});
  const std::string self = snap.count("self") ? snap["self"].value("id", std::string()) : std::string();
  bool incoming = false;
  if (const LobbyJson* r = pending_request(snap, self, &incoming); r && r->value("state", std::string()) == "pending") {
    if (incoming) { v.push_back({LI_ACCEPT, 0, "Accept"}); v.push_back({LI_DECLINE, 0, "Decline"}); }
    else v.push_back({LI_CANCEL, 0, "Cancel request"});
  }
  v.push_back({LI_TABS, 0, std::string("Tab ") + kLobbyTabs[h.lobby_tab]});
  if (h.lobby_tab == 0) {
    const auto players = snap.value("players", LobbyJson::array());
    // A player's row: Left / Right pick the action under the list (Send match request, Peer-to-peer match, Private chat, Add friend),
    // A does it for that player (upstream: select a player, then press one of the buttons under the list).
    for (size_t i = 0; i < players.size(); ++i) v.push_back({LI_PLAYER, int(i), "Player " + players[i].value("name", std::string("?"))});
  } else if (h.lobby_tab == 1) {
    v.push_back({LI_ROOM, 0, "Room"});
    const LobbyJson* room = h.lobby_room.empty() ? nullptr : lobby_room(snap, h.lobby_room);
    const std::string state = room ? room->value("state", std::string()) : std::string("public");
    if (state == "incoming") { v.push_back({LI_PM_ACCEPT, 0, "Accept private chat"}); v.push_back({LI_PM_DECLINE, 0, "Decline private chat"}); }
    else if (state == "outgoing") v.push_back({LI_PM_CLOSE, 0, "Cancel private chat"});
    else {
      v.push_back({LI_WRITE, 0, "Write a message"});
      if (room) v.push_back({LI_PM_CLOSE, 0, "Close private chat"});
    }
  } else if (h.lobby_tab == 2) {
    const LobbyJson list = lobby_friends(snap);
    for (size_t i = 0; i < list.size(); ++i) v.push_back({LI_FRIEND, int(i), "Friend " + list[i].value("name", std::string("?"))});
    if (!list.empty()) {
      const bool inc = list[size_t(std::clamp(h.lobby_friend, 0, int(list.size()) - 1))].value("incoming", false);
      v.push_back({LI_FRIEND_A, 0, inc ? "Accept friend" : "Invite to match"});
      v.push_back({LI_FRIEND_B, 0, inc ? "Decline friend" : "Remove friend"});
    }
    v.push_back({LI_FRIEND_CODE, 0, "Add friend by code"});
  } else if (h.lobby_tab == 4) {
    v.push_back({LI_NAME, 0, "Name"});
    v.push_back({LI_LOCATION, 0, "Location"});
    for (int i = 0; i < 3; ++i) v.push_back({LI_MAIN, i, "Main " + std::to_string(i + 1)});
    v.push_back({LI_AUTO_REJECT, 0, "Auto reject invites"});
    v.push_back({LI_SOUND, 0, "Request sound"});
    v.push_back({LI_VOLUME, 0, "Volume"});
  }
  return v;
}

// A card of the roster or the friends list (upstream's draw_control for PLAYERS / FRIENDS).
void lobby_card(ImDrawList* d, const Layout& L, const Fonts& f, float x, float y, float w, float hh, const LobbyJson& p,
                bool selected, bool focused, const LobbyJson& pings) {
  round_rect(d, L.p(x, y), L.p(x + w, y + hh), L.u(8), selected ? C_NAV_ON : C_FIELD, selected ? C_NAV_ON : C_FIELD,
             focused ? C_ACC_HI : selected ? C_FIELD_BORDER : NO_FILL);
  const auto mains = p.value("mains", LobbyJson::array());
  const int main = !mains.empty() && mains[0].is_number_integer() ? mains[0].get<int>() : -1;
  // The avatar: the first main's initials on the accent (upstream draws the main's stock icon from the game; the console has
  // no extracted icons before the game starts).
  const ImVec2 c = L.p(x + 22, y + hh * 0.5f);
  d->AddCircleFilled(c, L.u(14), col(launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .55)), 24);
  std::string initials = main >= 0 ? std::string(lobby::character_name(main)).substr(0, 2) : p.value("name", std::string("?")).substr(0, 1);
  text(d, f.bold, L.u(12), L.p(x + 8, y), L.p(x + 36, y + hh), C_TEXT, initials, 1);
  std::string name = p.value("name", std::string("Player"));
  std::string version = p.value("build", std::string()), kind;
  if (const size_t colon = version.find(':'); colon != std::string::npos) { kind = version.substr(colon + 1); version.resize(colon); }
  if (!version.empty() && version[0] != 'v' && version[0] != 'V') version = "v" + version;
  if (kind == "source") version += "  Source Port"; else if (kind == "recomp") version += "  Static Recomp";
  text(d, f.bold, L.u(13), L.p(x + 44, y + 4), L.p(x + w - 130, y + 22), C_TEXT, name);
  text(d, f.body, L.u(10), L.p(x + w - 128, y + 4), L.p(x + w - 10, y + 22), launcher::theme::glow(), version, 2);
  std::string detail = p.value("incoming", false) ? "Friend request" : p.value("status", std::string("Online"));
  if (detail == "Online" && !p.value("ready", true)) detail = "Setup unfinished";
  if (p.count("online") && !p.value("online", true)) detail = "Offline";
  const std::string location = p.value("location", std::string());
  if (!location.empty()) detail += "  /  " + location;
  if (pings.is_object() && pings.count(p.value("id", std::string()))) detail += "  /  " + std::to_string(pings[p.value("id", std::string())].get<int>()) + " ms";
  text(d, f.body, L.u(11), L.p(x + 44, y + 21), L.p(x + w - 10, y + 36), C_DIM, detail);
  std::string third;
  const auto stocks = p.value("stocks", LobbyJson::array());
  if (!stocks.empty()) { third = "Stocks:"; for (const auto& s : stocks) if (s.is_number_integer()) third += " " + std::to_string(s.get<int>()); }
  else if (selected && !p.value("code", std::string()).empty()) {
    // Upstream's selected card: their record (W / L, from their profile) and code; and this console's own record against them.
    const auto mine = lobby::record(p.value("code", std::string()));
    third = "W " + std::to_string(p.value("wins", 0)) + "  /  L " + std::to_string(p.value("losses", 0)) + "   " + p.value("code", std::string()) +
            "   You vs them: " + std::to_string(mine.first) + " - " + std::to_string(mine.second);
  } else for (const auto& m : mains) if (m.is_number_integer()) third += (third.empty() ? "" : " / ") + std::string(lobby::character_name(m.get<int>()));
  text(d, f.body, L.u(10), L.p(x + 44, y + 35), L.p(x + w - 10, y + hh - 2), selected && stocks.empty() ? launcher::theme::glow() : C_FAINT, third);
}

// Upstream's empty_state: a badge, a heading and a line under it.
void lobby_empty(ImDrawList* d, const Layout& L, const Fonts& f, float x, float y, float w, const std::string& heading,
                 const std::string& detail, bool social) {
  round_rect(d, L.p(x, y), L.p(x + 40, y + 40), L.u(14), RGB(32, 35, 59), RGB(32, 35, 59), NO_FILL);
  const ImU32 glow = col(launcher::theme::glow());
  if (social) { d->AddCircle(L.p(x + 20, y + 14), L.u(6), glow, 16, L.u(1.6f)); d->AddRect(L.p(x + 9, y + 23), L.p(x + 31, y + 33), glow, L.u(5), 0, L.u(1.6f)); }
  else { d->AddRect(L.p(x + 9, y + 10), L.p(x + 31, y + 27), glow, L.u(4), 0, L.u(1.6f)); d->AddLine(L.p(x + 14, y + 27), L.p(x + 14, y + 33), glow, L.u(1.6f)); d->AddLine(L.p(x + 14, y + 33), L.p(x + 20, y + 27), glow, L.u(1.6f)); }
  text(d, f.bold, L.u(17), L.p(x, y + 50), L.p(x + w, y + 74), C_TEXT, heading);
  text(d, f.body, L.u(12), L.p(x, y + 78), L.p(x + w, y + 120), C_DIM, detail, 0, true);
}

// Chat lines, newest at the bottom: the name in the accent's glow, the time, the text (upstream's CHATLOG item).
void lobby_messages(ImDrawList* d, const Layout& L, const Fonts& f, float ix, float iw, float top, float bottom, const LobbyJson& messages) {
  const int rows = std::max(1, int((bottom - top) / 34));
  const int first = std::max(0, int(messages.size()) - rows);
  for (int i = first, n = 0; i < int(messages.size()); ++i, ++n) {
    const auto& m = messages[size_t(i)];
    const float y = top + n * 34;
    text(d, f.bold, L.u(11), L.p(ix + 4, y), L.p(ix + iw - 70, y + 15), launcher::theme::glow(), m.value("name", std::string("Player")));
    const long long wall = m.value("wall_time", 0LL);
    if (wall > 1000000000) {
      std::time_t seconds = std::time_t(wall); std::tm local{}; char stamp[16] = {};
      localtime_r(&seconds, &local); std::strftime(stamp, sizeof stamp, "%I:%M %p", &local);
      text(d, f.body, L.u(10), L.p(ix + iw - 70, y), L.p(ix + iw - 4, y + 15), C_FAINT, stamp, 2);
    }
    text(d, f.body, L.u(12), L.p(ix + 4, y + 15), L.p(ix + iw - 4, y + 32), C_TEXT, m.value("text", std::string()));
  }
}

void paint_lobby(ImDrawList* d, const Layout& L, const Fonts& f, Home& h) {
  const LobbyJson snap = lobby::snapshot();
  const std::vector<LobbyFocus> items = lobby_items(h, snap);
  if (!h.rail) h.focus = std::clamp(h.focus, 0, int(items.size()) - 1);   // on the rail h.focus is the page nav's
  const LobbyFocus* fo = h.rail ? nullptr : &items[size_t(h.focus)];
  const auto is = [&](int kind, int index = 0) { return fo && fo->kind == kind && fo->index == index; };
  const std::string phase = snap.value("phase", std::string("offline"));
  const bool online = phase == "online";
  const auto players = snap.value("players", LobbyJson::array());
  const LobbyJson pings = snap.value("pings", LobbyJson::object());
  text(d, f.bold, L.u(10), L.p(CX, 26), L.p(CX + CW - 140, 44), C_FAINT, "MULTIPLAYER LOBBY");
  // The network state: a dot and one line (upstream's ONLINE_HINT and status).
  std::string line; COLORREF dotc = C_FAINT, ink = C_DIM;
  if (phase == "error") { line = snap.value("error", std::string("The lobby could not start.")); dotc = C_BAD; ink = C_BAD; }
  else if (phase == "starting") { line = "Connecting to the lobby network..."; dotc = C_WARN; }
  else if (phase == "held") { line = "In a match: the lobby is closed until it ends."; dotc = C_WARN; }
  else if (online && players.empty()) { line = "Online. Looking for players on the lobby network..."; dotc = C_WARN; }
  else if (online) { line = "Online. " + std::to_string(players.size()) + (players.size() == 1 ? " player" : " players") + " in the lobby."; dotc = C_OK; }
  else line = "Offline. Go Online to show your name in other players' lobbies.";
  dot(d, L, CX, 50, dotc);
  text(d, f.body, L.u(11), L.p(CX + 14, 45), L.p(CX + CW, 63), ink, line);
  button(d, L, f, CX + CW - 130, 18, 130, 26, online ? "Leave" : "Go Online", !online, is(LI_GO), phase == "starting");
  // Tabs (upstream's TAB_CHAT .. TAB_PROFILE).
  const float tw = (CW - (kLobbyTabCount - 1) * 5) / float(kLobbyTabCount);
  const auto rooms_unread = [&] {
    for (const auto& r : snap.value("private", LobbyJson::array())) if (r.value("state", std::string()) == "incoming") return true;
    return false;
  }();
  for (int i = 0; i < kLobbyTabCount; ++i) {
    const float x = CX + i * (tw + 5);
    const bool on = h.lobby_tab == i;
    round_rect(d, L.p(x, 68), L.p(x + tw, 92), L.u(7), on ? C_ACC_HI : C_BTN, on ? C_ACC_LO : C_BTN, on ? NO_FILL : C_BTN_BORDER);
    if (is(LI_TABS) && on) round_rect(d, L.p(x + 2, 70), L.p(x + tw - 2, 90), L.u(5), NO_FILL, NO_FILL, C_PLAY_TEXT);
    std::string label = kLobbyTabs[i];
    if (i == 2 && !snap.value("friend_requests", LobbyJson::array()).empty()) label += " (" + std::to_string(snap["friend_requests"].size()) + ")";
    text(d, f.bold, L.u(11), L.p(x, 68), L.p(x + tw, 92), on ? C_PLAY_TEXT : C_DIM, label, 1);
    if (i == 1 && rooms_unread) d->AddCircleFilled(L.p(x + tw - 9, 75), L.u(3), col(launcher::theme::glow()), 12);   // upstream's unread dot
  }
  // The card.
  bool incoming = false;
  const std::string self = snap.count("self") ? snap["self"].value("id", std::string()) : std::string();
  const LobbyJson* request = pending_request(snap, self, &incoming);
  const float card_top = 98, card_bottom = request ? 392 : 430;
  const COLORREF C_CARD = RGB(0x14, 0x1C, 0x2C), C_CARD_BORDER = RGB(0x27, 0x31, 0x4A);
  round_rect(d, L.p(CX, card_top), L.p(CX + CW, card_bottom), L.u(10), C_CARD, C_CARD, C_CARD_BORDER);
  const float ix = CX + 10, iw = CW - 20;
  if (h.lobby_tab == 0) {
    if (players.empty()) {
      if (phase == "error") lobby_empty(d, L, f, ix + 8, card_top + 16, iw - 16, "The lobby could not start", snap.value("error", std::string()), false);
      else if (online || phase == "starting") {
        const LobbyJson net = snap.value("network", LobbyJson::object());
        lobby_empty(d, L, f, ix + 8, card_top + 16, iw - 16, "Looking for players",
                    "Players on the same version appear here when the lobby network finds them. This can take up to a minute.", true);
        if (!net.empty())
          text(d, f.body, L.u(10), L.p(ix + 8, card_bottom - 24), L.p(ix + iw, card_bottom - 8), C_FAINT,
               "Lobby network: " + std::to_string(net.value("dht_nodes", 0)) + " nodes, " + std::to_string(net.value("candidates", 0)) + " candidates");
      } else lobby_empty(d, L, f, ix + 8, card_top + 16, iw - 16, "Find your next match", "Go online to discover players. Their location and mains appear here.", true);
    } else {
      h.lobby_player = std::clamp(h.lobby_player, 0, int(players.size()) - 1);
      const int visible = request ? 4 : 5; const float row = 50;
      if (h.lobby_player < h.lobby_scroll) h.lobby_scroll = h.lobby_player;
      if (h.lobby_player >= h.lobby_scroll + visible) h.lobby_scroll = h.lobby_player - visible + 1;
      for (int i = 0; i < visible && h.lobby_scroll + i < int(players.size()); ++i) {
        const int k = h.lobby_scroll + i;
        lobby_card(d, L, f, ix, card_top + 8 + i * (row + 4), iw, row, players[size_t(k)], k == h.lobby_player, is(LI_PLAYER, k), pings);
      }
      const bool p2p_ok = players[size_t(h.lobby_player)].value("p2p", false);
      const float by = card_bottom - 36;
      const bool on_row = fo && fo->kind == LI_PLAYER;
      button(d, L, f, ix, by, 150, 28, "Send match request", true, on_row && h.lobby_action == 0, !online || request != nullptr);
      button(d, L, f, ix + 156, by, 132, 28, "Peer-to-peer match", false, on_row && h.lobby_action == 1, !online || request != nullptr || !p2p_ok);
      button(d, L, f, ix + 294, by, 92, 28, "Private chat", false, on_row && h.lobby_action == 2, !online);
      button(d, L, f, ix + 392, by, iw - 392, 28, "Add friend", false, on_row && h.lobby_action == 3, !online);
      text(d, f.body, L.u(10), L.p(ix, by - 16), L.p(ix + iw, by - 2), C_FAINT, "On a player: Left / Right pick the action, A does it.");
    }
  } else if (h.lobby_tab == 1) {
    // Upstream's room tabs: the public chat, then each private room (its state; a dot for a request to answer).
    const std::vector<std::string> rooms = lobby_rooms(snap);
    if (std::find(rooms.begin(), rooms.end(), h.lobby_room) == rooms.end()) h.lobby_room.clear();
    float rx = ix;
    for (const auto& id : rooms) {
      const LobbyJson* r = id.empty() ? nullptr : lobby_room(snap, id);
      const std::string label = r ? r->value("name", std::string("?")) : std::string("Public");
      const float rw = std::min(140.0f, f.bold->CalcTextSizeA(L.u(11), FLT_MAX, 0, label.c_str()).x / L.s + 28);
      if (rx + rw > ix + iw) break;
      const bool front = id == h.lobby_room;
      round_rect(d, L.p(rx, card_top + 8), L.p(rx + rw, card_top + 30), L.u(7), front ? launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .45) : RGB(33, 43, 66),
                 front ? launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .45) : RGB(33, 43, 66), front && is(LI_ROOM) ? C_PLAY_TEXT : NO_FILL);
      text(d, f.bold, L.u(11), L.p(rx, card_top + 8), L.p(rx + rw, card_top + 30), front ? C_TEXT : C_DIM, label, 1);
      if (r && r->value("state", std::string()) == "incoming") d->AddCircleFilled(L.p(rx + rw - 7, card_top + 13), L.u(3), col(launcher::theme::glow()), 12);
      rx += rw + 6;
    }
    const LobbyJson* room = h.lobby_room.empty() ? nullptr : lobby_room(snap, h.lobby_room);
    const float mtop = card_top + 38, mbottom = card_bottom - 44;
    if (!room) {
      const auto messages = snap.value("messages", LobbyJson::array());
      if (messages.empty()) lobby_empty(d, L, f, ix + 8, mtop + 6, iw - 16, "Ready for a few games?", "Join the lobby, say hello, and send a match request when you're ready.", false);
      else lobby_messages(d, L, f, ix, iw, mtop, mbottom, messages);
      button(d, L, f, ix, card_bottom - 36, 200, 28, "Write a message", true, is(LI_WRITE), !online);
    } else {
      const std::string state = room->value("state", std::string()), name = room->value("name", std::string("?"));
      const auto messages = room->value("messages", LobbyJson::array());
      if (state == "incoming") {
        lobby_empty(d, L, f, ix + 8, mtop + 6, iw - 16, name + " wants to chat privately", "Only the two of you see a private chat. It ends when either of you closes it.", true);
        button(d, L, f, ix, card_bottom - 36, 150, 28, "Accept", true, is(LI_PM_ACCEPT), false);
        button(d, L, f, ix + 156, card_bottom - 36, 120, 28, "Decline", false, is(LI_PM_DECLINE), false);
      } else if (state == "outgoing") {
        lobby_empty(d, L, f, ix + 8, mtop + 6, iw - 16, "Waiting for " + name, "The private chat opens when they accept (" + std::to_string(room->value("remaining", 0)) + " s left).", true);
        button(d, L, f, ix, card_bottom - 36, 150, 28, "Cancel", false, is(LI_PM_CLOSE), false);
      } else {
        if (messages.empty()) text(d, f.body, L.u(12), L.p(ix + 8, mtop + 8), L.p(ix + iw - 8, mtop + 60), C_DIM,
                                   "A private chat with " + name + ". Only the two of you see it.", 0, true);
        else lobby_messages(d, L, f, ix, iw, mtop, mbottom, messages);
        if (state == "closed") text(d, f.body, L.u(11), L.p(ix + 220, card_bottom - 36), L.p(ix + iw, card_bottom - 8), C_FAINT, name + " closed this chat.");
        button(d, L, f, ix, card_bottom - 36, 200, 28, "Write a message", true, is(LI_WRITE), state != "open");
        button(d, L, f, ix + iw - 130, card_bottom - 36, 130, 28, "Close", false, is(LI_PM_CLOSE), false);
      }
    }
  } else if (h.lobby_tab == 2) {
    const LobbyJson list = lobby_friends(snap);
    if (list.empty()) lobby_empty(d, L, f, ix + 8, card_top + 16, iw - 16, "Friends, one click away.", "Add a player to see when they're online and ready for another game.", true);
    else {
      h.lobby_friend = std::clamp(h.lobby_friend, 0, int(list.size()) - 1);
      const int visible = request ? 4 : 5;
      const int top = std::clamp(h.lobby_friend - visible + 1, 0, std::max(0, int(list.size()) - visible));
      for (int i = 0; i < visible && top + i < int(list.size()); ++i)
        lobby_card(d, L, f, ix, card_top + 8 + i * 54, iw, 50, list[size_t(top + i)], top + i == h.lobby_friend, is(LI_FRIEND, top + i), pings);
      const bool inc = list[size_t(h.lobby_friend)].value("incoming", false);
      button(d, L, f, ix, card_bottom - 36, 140, 28, inc ? "Accept" : "Invite to Match", true, is(LI_FRIEND_A), !inc && !online && !list[size_t(h.lobby_friend)].value("online", false));
      button(d, L, f, ix + 148, card_bottom - 36, 120, 28, inc ? "Decline" : "Remove Friend", false, is(LI_FRIEND_B), false);
    }
    button(d, L, f, ix + iw - 150, card_bottom - 36, 150, 28, "Add friend by code", false, is(LI_FRIEND_CODE), !online);
  } else if (h.lobby_tab == 3) {
    // Upstream's Match history page: the record over all games, then the games, newest first.
    const LobbyJson games = lobby::history();
    const auto total = lobby::record();
    if (games.empty()) lobby_empty(d, L, f, ix + 8, card_top + 16, iw - 16, "Match history", "Completed lobby games appear here, with your wins and losses.", false);
    else {
      text(d, f.bold, L.u(13), L.p(ix + 4, card_top + 8), L.p(ix + iw, card_top + 30), C_TEXT,
           "Record: " + std::to_string(total.first) + " wins, " + std::to_string(total.second) + " losses   (" + std::to_string(games.size()) + " games)");
      const int rows = int((card_bottom - card_top - 44) / 28);
      for (int i = 0; i < rows && i < int(games.size()); ++i) {
        const auto& g = games[size_t(i)];
        const float y = card_top + 36 + i * 28;
        const std::string result = g.value("result", std::string());
        round_rect(d, L.p(ix, y), L.p(ix + iw, y + 24), L.u(6), C_FIELD, C_FIELD, NO_FILL);
        text(d, f.bold, L.u(11), L.p(ix + 8, y), L.p(ix + 80, y + 24), result == "win" ? C_OK : result == "loss" ? C_BAD : C_FAINT,
             result == "win" ? "Win" : result == "loss" ? "Loss" : "Incomplete");
        text(d, f.body, L.u(11), L.p(ix + 84, y), L.p(ix + iw - 150, y + 24), C_TEXT,
             g.value("opponent", std::string("Unknown")) + " (" + g.value("code", std::string()) + ")" +
             (g.value("transport", std::string()) == "p2p" ? "  peer to peer" : "  Slippi Direct"));
        text(d, f.body, L.u(10), L.p(ix + iw - 150, y), L.p(ix + iw - 8, y + 24), C_FAINT, g.value("when", std::string()), 2);
      }
    }
  } else {
    const lobby::Profile p = lobby::profile();
    const std::string account = snap.value("account", h.lobby_code);
    struct Row { int kind, index; std::string label, value; bool edit; };
    std::vector<Row> rows = {{LI_NAME, 0, "Display name", p.name.empty() ? h.lobby_name + "  (your Slippi name)" : p.name, true},
                             {-1, 0, "Slippi code", account.empty() ? "No Slippi account: sign in on the Play page" : account + "  (from your Slippi account)", false},
                             {LI_LOCATION, 0, "Location", p.location.empty() ? "(optional)" : p.location, true}};
    for (int i = 0; i < 3; ++i)
      rows.push_back({LI_MAIN, i, i == 0 ? "Main (your fighter)" : "Main " + std::to_string(i + 1),
                      i < int(p.mains.size()) ? std::string(lobby::character_name(p.mains[size_t(i)])) : std::string("None"), true});
    rows.push_back({LI_AUTO_REJECT, 0, "Auto reject invites", p.auto_reject ? "On" : "Off", true});
    rows.push_back({LI_SOUND, 0, "Request sound", p.request_sound ? "On" : "Off", true});
    rows.push_back({LI_VOLUME, 0, "Volume", std::to_string(p.request_volume) + "%", true});
    for (size_t i = 0; i < rows.size(); ++i) {
      const float y = card_top + 8 + float(i) * 34;
      const bool focused = rows[i].kind >= 0 && is(rows[i].kind, rows[i].index);
      round_rect(d, L.p(ix, y), L.p(ix + iw, y + 30), L.u(7), C_FIELD, C_FIELD, focused ? C_ACC_HI : C_FIELD_BORDER);
      text(d, f.bold, L.u(10), L.p(ix + 10, y), L.p(ix + 150, y + 30), C_FAINT, rows[i].label);
      std::string value = rows[i].value;
      if ((rows[i].kind == LI_MAIN || rows[i].kind == LI_VOLUME) && focused) value = "<  " + value + "  >";
      text(d, f.body, L.u(12), L.p(ix + 150, y), L.p(ix + iw - 10, y + 30), rows[i].edit ? C_TEXT : C_DIM, value);
    }
    text(d, f.body, L.u(10), L.p(ix, card_bottom - 22), L.p(ix + iw, card_bottom - 4), C_FAINT,
         "Self-reported. Your first main is your fighter in a lobby match. A edits or switches, Left / Right change a main or the volume.");
  }
  // The request bar.
  if (request) {
    const float y = 398;
    round_rect(d, L.p(CX, y), L.p(CX + CW, y + 38), L.u(9), launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .7),
               launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .7), C_ACC_LO);
    const LobbyJson who = request->value("peer", LobbyJson::object());
    const std::string name = who.value("name", std::string("Player")), state = request->value("state", std::string());
    const bool p2p = request->value("transport", std::string()) == "p2p";
    std::string what;
    if (state == "accepted") what = p2p ? "Match accepted. Starting the peer-to-peer match against " + name + "..." : "Match accepted. Starting Slippi Direct against " + name + "...";
    else if (incoming) what = name + (p2p ? " wants a peer-to-peer match" : " wants to play");
    else what = (p2p ? "Peer-to-peer match request sent to " : "Match request sent to ") + name;
    if (state == "pending") {
      what += "   " + std::to_string(request->value("remaining", 0)) + " s";
      const std::string other = incoming ? request->value("from", std::string()) : request->value("to", std::string());
      if (pings.count(other)) what += "   " + std::to_string(pings[other].get<int>()) + " ms";
      else if (incoming) what += "   Ping unavailable";
    }
    const float right = state != "pending" ? 0 : incoming ? 214 : 110;
    text(d, f.bold, L.u(12), L.p(CX + 12, y), L.p(CX + CW - 12 - right, y + 38), C_TEXT, what);
    if (state == "pending" && incoming) {
      button(d, L, f, CX + CW - 210, y + 6, 100, 26, "Accept", true, is(LI_ACCEPT), false);
      button(d, L, f, CX + CW - 104, y + 6, 96, 26, "Decline", false, is(LI_DECLINE), false);
    } else if (state == "pending") button(d, L, f, CX + CW - 104, y + 6, 96, 26, "Cancel", false, is(LI_CANCEL), false);
  }
  // The status line: the lobby's latest notice (upstream's STATUS), else the controls.
  const std::string notice = snap.value("notice", std::string());
  text(d, f.body, L.u(11), L.p(CX, 444), L.p(CX + CW, 462), notice.empty() ? C_FAINT : C_DIM,
       notice.empty() ? "A  Select     B  Pages     Left / Right  Tabs" : notice);
}

void lobby_edit(Home& h, const char* header, const char* guide, std::string& value) {
  std::string text = value;
  if (software_keyboard(header, guide, text)) value = text;
  h.pad_reset = true;   // the keyboard's own buttons are not the page's
}

void lobby_activate(Home& h, const LobbyFocus& fo) {
  const LobbyJson snap = lobby::snapshot();
  const bool online = snap.value("phase", std::string()) == "online";
  const auto players = snap.value("players", LobbyJson::array());
  const std::string self = snap.count("self") ? snap["self"].value("id", std::string()) : std::string();
  bool incoming = false;
  const LobbyJson* request = pending_request(snap, self, &incoming);
  const auto picked = [&]() -> LobbyJson { return players.empty() ? LobbyJson::object() : players[size_t(std::clamp(h.lobby_player, 0, int(players.size()) - 1))]; };
  switch (fo.kind) {
    case LI_GO:
      if (online) { lobby::leave(); say("home: lobby Leave"); }
      else if (snap.value("phase", std::string()) != "starting") { lobby::go_online(); say("home: lobby Go Online"); }
      break;
    case LI_TABS: h.lobby_tab = (h.lobby_tab + 1) % kLobbyTabCount; say("home: lobby tab %s", kLobbyTabs[h.lobby_tab]); break;
    case LI_PLAYER: {   // A on a player: the action picked with Left / Right, for them
      h.lobby_player = fo.index;
      static const int kinds[4] = {LI_REQUEST, LI_P2P, LI_PM, LI_ADD_FRIEND};
      static const char* const names[4] = {"Send match request", "Peer-to-peer match", "Private chat", "Add friend"};
      lobby_activate(h, LobbyFocus{kinds[std::clamp(h.lobby_action, 0, 3)], 0, names[std::clamp(h.lobby_action, 0, 3)]});
      break;
    }
    case LI_REQUEST:
    case LI_P2P:
      if (online && !players.empty() && !request) {
        const LobbyJson p = picked();
        LobbyJson data = {{"target", p.value("id", std::string())}, {"mode", "vanilla"}};
        if (fo.kind == LI_P2P) data["transport"] = "p2p";   // upstream's kP2pTransport
        lobby::command("request", data);
        say("home: lobby %s to %s", fo.kind == LI_P2P ? "peer-to-peer match request" : "match request", p.value("name", std::string("?")).c_str());
      }
      break;
    case LI_PM:
      if (online && !players.empty()) {
        const LobbyJson p = picked();
        lobby::command("pm_request", {{"target", p.value("id", std::string())}});
        say("home: lobby private chat request to %s", p.value("name", std::string("?")).c_str());
      }
      break;
    case LI_ADD_FRIEND:
      if (online && !players.empty()) {
        const LobbyJson p = picked();
        lobby::command("friend", {{"target", p.value("id", std::string())}});
        say("home: lobby friend request to %s", p.value("name", std::string("?")).c_str());
      }
      break;
    case LI_ROOM: {   // A steps to the next room, as Right does
      const auto rooms = lobby_rooms(snap);
      const auto it = std::find(rooms.begin(), rooms.end(), h.lobby_room);
      h.lobby_room = rooms[size_t(((it == rooms.end() ? 0 : it - rooms.begin()) + 1) % int(rooms.size()))];
      break;
    }
    case LI_WRITE:
      if (online) {
        std::string line;
        lobby_edit(h, h.lobby_room.empty() ? "Lobby chat" : "Private chat", "A message (up to 300 characters)", line);
        if (!line.empty() && h.lobby_room.empty()) { lobby::command("chat", {{"text", line}}); say("home: lobby chat message (%zu bytes)", line.size()); }
        else if (!line.empty()) { lobby::command("pm", {{"room", h.lobby_room}, {"text", line}}); say("home: lobby private message (%zu bytes)", line.size()); }
      }
      break;
    case LI_PM_ACCEPT: lobby::command("pm_accept", {{"room", h.lobby_room}}); say("home: lobby private chat accepted"); break;
    case LI_PM_DECLINE: lobby::command("pm_decline", {{"room", h.lobby_room}}); say("home: lobby private chat declined"); break;
    case LI_PM_CLOSE: lobby::command("pm_close", {{"room", h.lobby_room}}); say("home: lobby private chat closed"); h.lobby_room.clear(); break;
    case LI_FRIEND_A: case LI_FRIEND_B: {
      const LobbyJson list = lobby_friends(snap);
      if (list.empty()) break;
      const auto& fr = list[size_t(std::clamp(h.lobby_friend, 0, int(list.size()) - 1))];
      const std::string id = fr.value("id", std::string());
      if (fr.value("incoming", false)) lobby::command(fo.kind == LI_FRIEND_A ? "friend_accept" : "friend_decline", {{"target", id}});
      else if (fo.kind == LI_FRIEND_A) { if (!request) lobby::command("request", {{"target", id}, {"mode", "vanilla"}}); }
      else lobby::command("unfriend", {{"target", id}});
      say("home: lobby %s %s", fo.label.c_str(), fr.value("name", std::string("?")).c_str());
      break;
    }
    case LI_FRIEND_CODE: {
      std::string code;
      lobby_edit(h, "Add friend", "Their Slippi code, like ABCD#123", code);
      if (!code.empty()) { lobby::command("friend_code", {{"code", code}}); say("home: lobby add friend by code"); }
      break;
    }
    case LI_NAME: case LI_LOCATION: {
      lobby::Profile p = lobby::profile();
      if (fo.kind == LI_NAME) lobby_edit(h, "Display name", "Shown to other players in the lobby (up to 32 characters)", p.name);
      else lobby_edit(h, "Location", "Self-reported, shown to other players (optional)", p.location);
      if (p.name.size() > 32) p.name.resize(32);
      if (p.location.size() > 48) p.location.resize(48);
      lobby::set_profile(p);
      say("home: lobby profile %s changed", fo.kind == LI_NAME ? "name" : "location");
      break;
    }
    case LI_AUTO_REJECT: case LI_SOUND: {
      lobby::Profile p = lobby::profile();
      if (fo.kind == LI_AUTO_REJECT) p.auto_reject = !p.auto_reject; else p.request_sound = !p.request_sound;
      lobby::set_profile(p);
      say("home: lobby %s %s", fo.kind == LI_AUTO_REJECT ? "auto reject invites" : "request sound", (fo.kind == LI_AUTO_REJECT ? p.auto_reject : p.request_sound) ? "on" : "off");
      break;
    }
    case LI_MAIN: case LI_VOLUME: break;   // Left / Right
    case LI_ACCEPT: case LI_DECLINE: case LI_CANCEL:
      if (request) {
        lobby::command(fo.kind == LI_ACCEPT ? "accept" : "cancel", {{"request", request->value("id", std::string())}});
        say("home: lobby %s", fo.label.c_str());
      }
      break;
  }
}

// Left / Right on a main: the next character the other two mains do not already have (Main 2 and 3 may be None).
void lobby_main_step(int slot, int dir) {
  lobby::Profile p = lobby::profile();
  std::vector<int> m = p.mains;
  if (m.empty()) m.push_back(2);
  if (slot > int(m.size())) slot = int(m.size());   // past the first empty slot: that slot
  const int lowest = slot == 0 ? 0 : -1;             // -1: None (only for the second and third)
  int value = slot < int(m.size()) ? m[size_t(slot)] : -1;
  for (int step = 0; step < lobby::kCharacters + 1; ++step) {
    value += dir;
    if (value >= lobby::kCharacters) value = lowest;
    if (value < lowest) value = lobby::kCharacters - 1;
    if (value == -1) break;
    bool taken = false;
    for (size_t i = 0; i < m.size(); ++i) if (int(i) != slot && m[i] == value) taken = true;
    if (!taken) break;
  }
  if (value < 0) { if (slot < int(m.size())) m.erase(m.begin() + slot); }
  else if (slot < int(m.size())) m[size_t(slot)] = value;
  else m.push_back(value);
  p.mains = m;
  lobby::set_profile(p);
  say("home: lobby main %d %s", slot + 1, value < 0 ? "None" : lobby::character_name(value));
}

// One frame of the page's controller: returns true when it handled Left (so the home screen does not go back to the rail).
// Also watches for new requests whatever page shows: the chime (upstream's request_chime) and, on this page, Accept takes the focus.
bool lobby_input(Home& h, const Edges& e, bool on_page) {
  const LobbyJson snap = lobby::snapshot();
  const std::vector<LobbyFocus> items = lobby_items(h, snap);
  const int n = int(items.size());
  {
    bool incoming = false;
    const std::string self = snap.count("self") ? snap["self"].value("id", std::string()) : std::string();
    const LobbyJson* r = pending_request(snap, self, &incoming);
    const std::string rid = r && incoming && r->value("state", std::string()) == "pending" ? r->value("id", std::string()) : std::string();
    if (!rid.empty() && rid != h.lobby_seen_request) {
      h.lobby_seen_request = rid;
      const lobby::Profile p = lobby::profile();
      if (!p.auto_reject) {
        if (p.request_sound) request_chime(p.request_volume);
        if (on_page) { for (int i = 0; i < n; ++i) if (items[size_t(i)].kind == LI_ACCEPT) { h.focus = i; h.rail = false; } }
        say("home: lobby request from %s%s: Accept focused", r->value("peer", LobbyJson::object()).value("name", std::string("?")).c_str(),
            r->value("transport", std::string()) == "p2p" ? " (peer to peer)" : "");
      }
    }
    // A private chat request: its room is put in front on the chat tab (upstream: a notice and the room's tab with a dot).
    for (const auto& room : snap.value("private", LobbyJson::array())) {
      const std::string id = room.value("id", std::string());
      if (room.value("state", std::string()) != "incoming" || h.lobby_rooms_seen.count(id)) continue;
      h.lobby_rooms_seen.insert(id); h.lobby_room = id;
      say("home: lobby private chat request from %s", room.value("name", std::string("?")).c_str());
    }
  }
  if (!on_page) return false;
  h.focus = std::clamp(h.focus, 0, n - 1);
  if (e.up) h.focus = std::max(0, h.focus - 1);
  if (e.down) h.focus = std::min(n - 1, h.focus + 1);
  const LobbyFocus& fo = items[size_t(h.focus)];
  if (fo.kind == LI_PLAYER) h.lobby_player = fo.index;
  if (fo.kind == LI_FRIEND) h.lobby_friend = fo.index;
  if (fo.label != h.lobby_focus_said) { h.lobby_focus_said = fo.label; say("home: lobby focus %s", fo.label.c_str()); }
  bool took_left = false;
  if (fo.kind == LI_TABS && (e.left || e.right)) {
    h.lobby_tab = (h.lobby_tab + (e.left ? kLobbyTabCount - 1 : 1)) % kLobbyTabCount;
    say("home: lobby tab %s", kLobbyTabs[h.lobby_tab]);
    took_left = e.left;
  } else if (fo.kind == LI_PLAYER && (e.right || (e.left && h.lobby_action > 0))) {
    static const char* const names[4] = {"Send match request", "Peer-to-peer match", "Private chat", "Add friend"};
    h.lobby_action = std::clamp(h.lobby_action + (e.left ? -1 : 1), 0, 3);
    say("home: lobby action %s", names[h.lobby_action]);
    took_left = e.left;
  } else if (fo.kind == LI_MAIN && (e.left || e.right)) {
    lobby_main_step(fo.index, e.left ? -1 : 1);
    took_left = e.left;
  } else if (fo.kind == LI_VOLUME && (e.left || e.right)) {
    lobby::Profile p = lobby::profile();
    p.request_volume = std::clamp(p.request_volume + (e.left ? -5 : 5), 0, 100);
    lobby::set_profile(p);
    say("home: lobby request volume %d%%", p.request_volume);
    took_left = e.left;
  } else if (fo.kind == LI_ROOM && (e.left || e.right)) {
    const auto rooms = lobby_rooms(snap);
    const auto it = std::find(rooms.begin(), rooms.end(), h.lobby_room);
    const int at = it == rooms.end() ? 0 : int(it - rooms.begin()), count = int(rooms.size());
    h.lobby_room = rooms[size_t(((at + (e.left ? -1 : 1)) % count + count) % count)];
    say("home: lobby room %s", h.lobby_room.empty() ? "Public" : "private");
    took_left = e.left;
  }
  if (e.a) lobby_activate(h, fo);
  if (e.a && fo.kind == LI_MAIN) lobby_main_step(fo.index, 1);
  return took_left;
}
