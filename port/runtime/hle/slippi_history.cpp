// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_history.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace slippi::history {
namespace {
uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

bool parse(const std::string& path, Game& g) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  // {U\3raw[$U#l<len>  then events
  const uint8_t magic[] = {'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l'};
  if (d.size() < 16 || std::memcmp(d.data(), magic, sizeof magic) != 0) return false;
  size_t pos = 15, end = std::min(d.size(), (size_t)15 + be32(&d[11]));
  if (end == (size_t)15) end = d.size();   // still being written: length 0
  if (pos >= d.size() || d[pos] != 0x35) return false;
  const uint8_t sizes_len = d[pos + 1];
  uint16_t size_of[256] = {0};
  for (size_t i = pos + 2; i + 2 < pos + 1 + sizes_len; i += 3) size_of[d[i]] = be16(&d[i + 1]);
  pos += 1 + sizes_len;
  g.path = path;
  g.players.assign(4, Player());
  for (int i = 0; i < 4; ++i) g.players[i].port = i + 1;
  bool have_start = false;
  int lras = -1;
  while (pos < end) {
    const uint8_t cmd = d[pos];
    const size_t len = size_of[cmd];
    if (!len || pos + 1 + len > d.size()) break;
    const uint8_t* p = &d[pos + 1];
    if (cmd == 0x36 && len >= 0x140) {   // game start
      have_start = true;
      g.stage = be16(p + 0x12);   // spec offsets count the command byte; p starts after it
      for (int i = 0; i < 4; ++i) {
        Player& pl = g.players[i];
        pl.character = p[0x64 + 0x24 * i];
        pl.type = p[0x65 + 0x24 * i];
        if (len >= 0x221 + 0x0A * 4) {
          char name[32] = {0}; std::memcpy(name, p + 0x1A4 + 0x1F * i, 31); pl.name = name;
          char code[11] = {0}; std::memcpy(code, p + 0x220 + 0x0A * i, 10); pl.code = code;
          for (char& c : pl.code) if (c == (char)0x81) c = '#';   // Shift-JIS fullwidth '#' lead byte
          pl.code.erase(std::remove_if(pl.code.begin(), pl.code.end(), [](char c) { return c == (char)0x94; }), pl.code.end());
          if (!pl.code.empty()) g.online = true;
        }
      }
    } else if (cmd == 0x38 && len >= 0x22) {   // post-frame update: stocks remaining
      const int idx = p[4];
      if (idx >= 0 && idx < 4) { g.players[idx].stocks = p[0x20]; g.last_frame = (int32_t)be32(p); }
    } else if (cmd == 0x39 && len >= 2) {   // game end
      if (p[0] == 7 && len >= 2) lras = (int8_t)p[1];
    }
    pos += 1 + len;
  }
  if (!have_start) return false;
  int best = -1, best_stocks = -1;
  for (int i = 0; i < 4; ++i) {
    const Player& pl = g.players[i];
    if (pl.type == 3 || i == lras) continue;
    if (pl.stocks > best_stocks) { best_stocks = pl.stocks; best = i; }
  }
  g.winner_port = best >= 0 ? best + 1 : -1;
  const std::string base = std::filesystem::path(path).stem().string();   // Game_20260913T223606
  if (base.size() >= 20 && base.rfind("Game_", 0) == 0) {
    const std::string t = base.substr(5);
    g.started_at = t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2) + " " + t.substr(9, 2) + ":" + t.substr(11, 2);
  }
  return true;
}
}  // namespace

std::vector<Game> recent_games(const std::string& dir, size_t limit) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) if (e.is_regular_file(ec) && e.path().extension() == ".slp") files.push_back(e.path());
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename().string() > b.filename().string(); });
  std::vector<Game> games;
  for (const auto& path : files) {
    if (games.size() >= limit) break;
    Game g;
    if (parse(path.string(), g) && g.last_frame > 60) games.push_back(std::move(g));
  }
  return games;
}
}  // namespace slippi::history
