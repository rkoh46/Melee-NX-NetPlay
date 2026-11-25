// The home screen's Multiplayer Lobby page (included by mu_home.cpp, inside its namespace): melee-unlocked's Multiplayer Lobby tab
// (port/app/launcher_lobby.cpp and launcher_lobby_ui.inl) on a controller and the launcher's 720 x 480 layout. Its parts:
//   Go Online / Leave (upstream's GO_ONLINE), the network state under the title;
//   tabs: Players (its roster card: avatar, name, Game Build, status / location / ping, mains, stocks in a match), Lobby chat,
//   Friends (and friend requests), Your profile (name, the Slippi code read from the account, location, three mains);
//   the request bar (Accept / Decline, or Cancel, with the time left and the measured ping), and the status line (its notices).
// Upstream's empty states and their words: "Find your next match", "Ready for a few games?", "Friends, one click away.". Added for
// the console: "Looking for players" while the lobby network has found nobody yet, and the reason when the lobby cannot start.
// Left out: private chat rooms, the match history and its W/L, auto reject and the request sound (see FEATURES.md).
// SPDX-License-Identifier: GPL-3.0-or-later

using LobbyJson = nlohmann::json;

enum LobbyItem { LI_GO, LI_TABS, LI_PLAYER, LI_REQUEST, LI_ADD_FRIEND, LI_WRITE, LI_FRIEND, LI_FRIEND_A, LI_FRIEND_B, LI_FRIEND_CODE,
                 LI_NAME, LI_LOCATION, LI_MAIN, LI_ACCEPT, LI_DECLINE, LI_CANCEL };
struct LobbyFocus { int kind; int index; std::string label; };
constexpr const char* kLobbyTabs[4] = {"Players", "Lobby chat", "Friends", "Your profile"};

// Account name and code from user.json (never its key), for the profile.
void account_parts(const std::string& user_dir, std::string* name, std::string* code) {
  std::ifstream in(user_dir + "/user.json");
  std::stringstream ss; ss << in.rdbuf();
  const LobbyJson j = LobbyJson::parse(ss.str(), nullptr, false);
  *name = j.is_object() ? j.value("displayName", std::string()) : std::string();
  *code = j.is_object() ? j.value("connectCode", std::string()) : std::string();
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
    for (size_t i = 0; i < players.size(); ++i) v.push_back({LI_PLAYER, int(i), "Player " + players[i].value("name", std::string("?"))});
    if (!players.empty()) { v.push_back({LI_REQUEST, 0, "Send match request"}); v.push_back({LI_ADD_FRIEND, 0, "Add friend"}); }
  } else if (h.lobby_tab == 1) {
    v.push_back({LI_WRITE, 0, "Write a message"});
  } else if (h.lobby_tab == 2) {
    const LobbyJson list = lobby_friends(snap);
    for (size_t i = 0; i < list.size(); ++i) v.push_back({LI_FRIEND, int(i), "Friend " + list[i].value("name", std::string("?"))});
    if (!list.empty()) {
      const bool inc = list[size_t(std::clamp(h.lobby_friend, 0, int(list.size()) - 1))].value("incoming", false);
      v.push_back({LI_FRIEND_A, 0, inc ? "Accept friend" : "Invite to match"});
      v.push_back({LI_FRIEND_B, 0, inc ? "Decline friend" : "Remove friend"});
    }
    v.push_back({LI_FRIEND_CODE, 0, "Add friend by code"});
  } else {
    v.push_back({LI_NAME, 0, "Name"});
    v.push_back({LI_LOCATION, 0, "Location"});
    for (int i = 0; i < 3; ++i) v.push_back({LI_MAIN, i, "Main " + std::to_string(i + 1)});
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
  else if (selected && !p.value("code", std::string()).empty()) third = p.value("code", std::string());
  else for (const auto& m : mains) if (m.is_number_integer()) third += (third.empty() ? "" : " / ") + std::string(lobby::character_name(m.get<int>()));
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
  else if (online && players.empty()) { line = "Online. Looking for players on the lobby network..."; dotc = C_WARN; }
  else if (online) { line = "Online. " + std::to_string(players.size()) + (players.size() == 1 ? " player" : " players") + " in the lobby."; dotc = C_OK; }
  else line = "Offline. Go Online to show your name in other players' lobbies.";
  dot(d, L, CX, 50, dotc);
  text(d, f.body, L.u(11), L.p(CX + 14, 45), L.p(CX + CW, 63), ink, line);
  button(d, L, f, CX + CW - 130, 18, 130, 26, online ? "Leave" : "Go Online", !online, is(LI_GO), phase == "starting");
  // Tabs (upstream's TAB_CHAT .. TAB_PROFILE).
  const float tw = (CW - 3 * 6) / 4.0f;
  for (int i = 0; i < 4; ++i) {
    const float x = CX + i * (tw + 6);
    const bool on = h.lobby_tab == i;
    round_rect(d, L.p(x, 68), L.p(x + tw, 92), L.u(7), on ? C_ACC_HI : C_BTN, on ? C_ACC_LO : C_BTN, on ? NO_FILL : C_BTN_BORDER);
    if (is(LI_TABS) && on) round_rect(d, L.p(x + 2, 70), L.p(x + tw - 2, 90), L.u(5), NO_FILL, NO_FILL, C_PLAY_TEXT);
    std::string label = kLobbyTabs[i];
    if (i == 2 && !snap.value("friend_requests", LobbyJson::array()).empty()) label += " (" + std::to_string(snap["friend_requests"].size()) + ")";
    text(d, f.bold, L.u(11), L.p(x, 68), L.p(x + tw, 92), on ? C_PLAY_TEXT : C_DIM, label, 1);
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
      const float by = card_bottom - 36;
      button(d, L, f, ix, by, 200, 28, "Send match request", true, is(LI_REQUEST), !online || request != nullptr);
      button(d, L, f, ix + 208, by, 120, 28, "Add friend", false, is(LI_ADD_FRIEND), !online);
    }
  } else if (h.lobby_tab == 1) {
    const auto messages = snap.value("messages", LobbyJson::array());
    if (messages.empty()) lobby_empty(d, L, f, ix + 8, card_top + 16, iw - 16, "Ready for a few games?", "Join the lobby, say hello, and send a match request when you're ready.", false);
    else {
      const int rows = int((card_bottom - card_top - 52) / 34);
      const int first = std::max(0, int(messages.size()) - rows);
      for (int i = first, n = 0; i < int(messages.size()); ++i, ++n) {
        const auto& m = messages[size_t(i)];
        const float y = card_top + 8 + n * 34;
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
    button(d, L, f, ix, card_bottom - 36, 200, 28, "Write a message", true, is(LI_WRITE), !online);
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
    for (size_t i = 0; i < rows.size(); ++i) {
      const float y = card_top + 10 + float(i) * 40;
      const bool focused = rows[i].kind >= 0 && is(rows[i].kind, rows[i].index);
      round_rect(d, L.p(ix, y), L.p(ix + iw, y + 34), L.u(7), C_FIELD, C_FIELD, focused ? C_ACC_HI : C_FIELD_BORDER);
      text(d, f.bold, L.u(10), L.p(ix + 10, y), L.p(ix + 150, y + 34), C_FAINT, rows[i].label);
      std::string value = rows[i].value;
      if (rows[i].kind == LI_MAIN && focused) value = "<  " + value + "  >";
      text(d, f.body, L.u(12), L.p(ix + 150, y), L.p(ix + iw - 10, y + 34), rows[i].edit ? C_TEXT : C_DIM, value);
    }
    text(d, f.body, L.u(10), L.p(ix, card_bottom - 40), L.p(ix + iw, card_bottom - 6), C_FAINT,
         "Name and location are self-reported. Your first main is your fighter in a lobby match. A edits a field (the keyboard), Left / Right change a main.", 0, true);
  }
  // The request bar.
  if (request) {
    const float y = 398;
    round_rect(d, L.p(CX, y), L.p(CX + CW, y + 38), L.u(9), launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .7),
               launcher::theme::mix(launcher::theme::accent, RGB(12, 18, 33), .7), C_ACC_LO);
    const LobbyJson who = request->value("peer", LobbyJson::object());
    const std::string name = who.value("name", std::string("Player")), state = request->value("state", std::string());
    std::string what;
    if (state == "accepted") what = "Match accepted. Starting Slippi Direct against " + name + "...";
    else if (incoming) what = name + " wants to play";
    else what = "Match request sent to " + name;
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
  switch (fo.kind) {
    case LI_GO:
      if (online) { lobby::leave(); say("home: lobby Leave"); }
      else if (snap.value("phase", std::string()) != "starting") { lobby::go_online(); say("home: lobby Go Online"); }
      break;
    case LI_TABS: h.lobby_tab = (h.lobby_tab + 1) % 4; say("home: lobby tab %s", kLobbyTabs[h.lobby_tab]); break;
    case LI_PLAYER:   // A on a player: Send match request to them (the buttons under the list act on the last one picked)
    case LI_REQUEST:
      if (fo.kind == LI_PLAYER) h.lobby_player = fo.index;
      if (online && !players.empty() && !request) {
        const auto& p = players[size_t(std::clamp(h.lobby_player, 0, int(players.size()) - 1))];
        lobby::command("request", {{"target", p.value("id", std::string())}, {"mode", "vanilla"}});
        say("home: lobby match request to %s", p.value("name", std::string("?")).c_str());
      }
      break;
    case LI_ADD_FRIEND:
      if (online && !players.empty()) {
        const auto& p = players[size_t(std::clamp(h.lobby_player, 0, int(players.size()) - 1))];
        lobby::command("friend", {{"target", p.value("id", std::string())}});
        say("home: lobby friend request to %s", p.value("name", std::string("?")).c_str());
      }
      break;
    case LI_WRITE:
      if (online) {
        std::string line;
        lobby_edit(h, "Lobby chat", "A message for everyone in the lobby (up to 300 characters)", line);
        if (!line.empty()) { lobby::command("chat", {{"text", line}}); say("home: lobby chat message (%zu bytes)", line.size()); }
      }
      break;
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
    case LI_MAIN: break;   // Left / Right
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
bool lobby_input(Home& h, const Edges& e) {
  const LobbyJson snap = lobby::snapshot();
  const std::vector<LobbyFocus> items = lobby_items(h, snap);
  const int n = int(items.size());
  // An incoming request takes the focus once, to Accept (upstream shows Accept and Decline at once beside the chat).
  {
    bool incoming = false;
    const std::string self = snap.count("self") ? snap["self"].value("id", std::string()) : std::string();
    const LobbyJson* r = pending_request(snap, self, &incoming);
    const std::string rid = r && incoming && r->value("state", std::string()) == "pending" ? r->value("id", std::string()) : std::string();
    if (!rid.empty() && rid != h.lobby_seen_request) {
      h.lobby_seen_request = rid;
      for (int i = 0; i < n; ++i) if (items[size_t(i)].kind == LI_ACCEPT) { h.focus = i; h.rail = false; }
      say("home: lobby request from %s: Accept focused", r->value("peer", LobbyJson::object()).value("name", std::string("?")).c_str());
    }
  }
  h.focus = std::clamp(h.focus, 0, n - 1);
  if (e.up) h.focus = std::max(0, h.focus - 1);
  if (e.down) h.focus = std::min(n - 1, h.focus + 1);
  const LobbyFocus& fo = items[size_t(h.focus)];
  if (fo.kind == LI_PLAYER) h.lobby_player = fo.index;
  if (fo.kind == LI_FRIEND) h.lobby_friend = fo.index;
  if (fo.label != h.lobby_focus_said) { h.lobby_focus_said = fo.label; say("home: lobby focus %s", fo.label.c_str()); }
  bool took_left = false;
  if (fo.kind == LI_TABS && (e.left || e.right)) {
    h.lobby_tab = (h.lobby_tab + (e.left ? 3 : 1)) % 4;
    say("home: lobby tab %s", kLobbyTabs[h.lobby_tab]);
    took_left = e.left;
  } else if (fo.kind == LI_MAIN && (e.left || e.right)) {
    lobby_main_step(fo.index, e.left ? -1 : 1);
    took_left = e.left;
  }
  if (e.a) lobby_activate(h, fo);
  if (e.a && fo.kind == LI_MAIN) lobby_main_step(fo.index, 1);
  return took_left;
}
