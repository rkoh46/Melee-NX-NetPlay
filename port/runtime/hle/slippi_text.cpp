// Host text adapter for the existing Slippi EXI string format. Windows uses its
// CP932 converter; macOS/Linux use iconv's matching Windows-31J code page; the
// Switch (newlib, no iconv) has a built-in converter for the characters Melee's
// font can show: ASCII, full-width ASCII, hiragana, katakana and the common
// punctuation. Anything else, kanji included, becomes '?'.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_net.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__SWITCH__)
#include <cstdint>
#include <unordered_map>
#else
#include <iconv.h>
#endif

namespace slippi {
#if defined(_WIN32)
static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
static std::string wide_to_cp(const std::wstring& w, UINT cp) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(n, 0);
  WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
std::string utf8_to_shiftjis(const std::string& s) { return wide_to_cp(utf8_to_wide(s), 932); }
std::string shiftjis_to_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(932, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(n, 0);
  MultiByteToWideChar(932, 0, s.data(), (int)s.size(), w.data(), n);
  return wide_to_cp(w, CP_UTF8);
}
#elif defined(__SWITCH__)
namespace {
// Code point <-> CP932 for the ranges the game's font covers.
bool cp932_from_codepoint(uint32_t cp, uint16_t& out) {
  if (cp < 0x80) { out = uint16_t(cp); return true; }
  if (cp == 0xA5) { out = 0x5C; return true; }                      // yen sign is the backslash cell
  if (cp == 0x203E) { out = 0x7E; return true; }                    // overline is the tilde cell
  if (cp >= 0x3041 && cp <= 0x3093) { out = uint16_t(0x829F + (cp - 0x3041)); return true; }   // hiragana
  if (cp >= 0x30A1 && cp <= 0x30F6) {                                // katakana, skipping the 0x837F gap
    uint16_t code = uint16_t(0x8340 + (cp - 0x30A1));
    if (code >= 0x837F) ++code;
    out = code; return true;
  }
  if (cp >= 0xFF01 && cp <= 0xFF5E) {                               // full-width ASCII: JIS row 3 / 0x8140 block
    static const std::unordered_map<uint32_t, uint16_t> punct = {
      {0xFF01, 0x8149}, {0xFF02, 0x8168}, {0xFF03, 0x8194}, {0xFF04, 0x8190}, {0xFF05, 0x8193}, {0xFF06, 0x8195},
      {0xFF07, 0x8166}, {0xFF08, 0x8169}, {0xFF09, 0x816A}, {0xFF0A, 0x8196}, {0xFF0B, 0x817B}, {0xFF0C, 0x8143},
      {0xFF0D, 0x817C}, {0xFF0E, 0x8144}, {0xFF0F, 0x815E}, {0xFF1A, 0x8146}, {0xFF1B, 0x8147}, {0xFF1C, 0x8183},
      {0xFF1D, 0x8181}, {0xFF1E, 0x8184}, {0xFF1F, 0x8148}, {0xFF20, 0x8197}, {0xFF3B, 0x816D}, {0xFF3C, 0x815F},
      {0xFF3D, 0x816E}, {0xFF3E, 0x814F}, {0xFF3F, 0x8151}, {0xFF40, 0x814D}, {0xFF5B, 0x816F}, {0xFF5C, 0x8162},
      {0xFF5D, 0x8170}, {0xFF5E, 0x8160}};
    if (cp >= 0xFF10 && cp <= 0xFF19) { out = uint16_t(0x824F + (cp - 0xFF10)); return true; }   // digits
    if (cp >= 0xFF21 && cp <= 0xFF3A) { out = uint16_t(0x8260 + (cp - 0xFF21)); return true; }   // A-Z
    if (cp >= 0xFF41 && cp <= 0xFF5A) { out = uint16_t(0x8281 + (cp - 0xFF41)); return true; }   // a-z
    auto it = punct.find(cp);
    if (it != punct.end()) { out = it->second; return true; }
    return false;
  }
  static const std::unordered_map<uint32_t, uint16_t> symbols = {
    {0x3000, 0x8140}, {0x3001, 0x8141}, {0x3002, 0x8142}, {0x30FC, 0x815B}, {0x309B, 0x814A}, {0x309C, 0x814B},
    {0x300C, 0x8175}, {0x300D, 0x8176}, {0x300E, 0x8177}, {0x300F, 0x8178}, {0x2026, 0x8163}, {0x2605, 0x819A},
    {0x2606, 0x8199}, {0x25CB, 0x819B}, {0x25CF, 0x819C}, {0x25B3, 0x81A2}, {0x25A1, 0x81A0}, {0x00D7, 0x817E},
    {0x00F7, 0x8180}, {0x00B0, 0x818B}, {0x2192, 0x81A8}, {0x2190, 0x81A9}, {0x2191, 0x81AA}, {0x2193, 0x81AB}};
  auto it = symbols.find(cp);
  if (it == symbols.end()) return false;
  out = it->second; return true;
}
bool codepoint_from_cp932(uint16_t code, uint32_t& cp) {
  if (code < 0x80) { cp = code; return true; }
  if (code >= 0x829F && code <= 0x82F1) { cp = 0x3041 + (code - 0x829F); return true; }
  if (code >= 0x8340 && code <= 0x8396 && code != 0x837F) { cp = 0x30A1 + (code - 0x8340) - (code > 0x837F ? 1 : 0); return true; }
  if (code >= 0x824F && code <= 0x8258) { cp = 0xFF10 + (code - 0x824F); return true; }
  if (code >= 0x8260 && code <= 0x8279) { cp = 0xFF21 + (code - 0x8260); return true; }
  if (code >= 0x8281 && code <= 0x829A) { cp = 0xFF41 + (code - 0x8281); return true; }
  for (uint32_t probe : {0x3000u, 0x3001u, 0x3002u, 0x30FCu, 0x300Cu, 0x300Du, 0x2026u, 0x2605u, 0x2606u, 0x25CBu, 0x25CFu,
                         0xFF01u, 0xFF02u, 0xFF03u, 0xFF04u, 0xFF05u, 0xFF06u, 0xFF07u, 0xFF08u, 0xFF09u, 0xFF0Au, 0xFF0Bu,
                         0xFF0Cu, 0xFF0Du, 0xFF0Eu, 0xFF0Fu, 0xFF1Au, 0xFF1Bu, 0xFF1Cu, 0xFF1Du, 0xFF1Eu, 0xFF1Fu, 0xFF20u,
                         0xFF3Bu, 0xFF3Cu, 0xFF3Du, 0xFF3Eu, 0xFF3Fu, 0xFF40u, 0xFF5Bu, 0xFF5Cu, 0xFF5Du, 0xFF5Eu}) {
    uint16_t back;
    if (cp932_from_codepoint(probe, back) && back == code) { cp = probe; return true; }
  }
  return false;
}
void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out.push_back(char(cp));
  else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
  else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
}
}  // namespace
std::string utf8_to_shiftjis(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const auto lead = static_cast<unsigned char>(s[i]);
    size_t width = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 1;
    if (i + width > s.size()) width = 1;
    uint32_t cp = lead;
    if (width > 1) {
      cp = lead & (0xFF >> (width + 1));
      for (size_t k = 1; k < width; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    uint16_t code;
    if (cp932_from_codepoint(cp, code)) {
      if (code > 0xFF) out.push_back(char(code >> 8));
      out.push_back(char(code & 0xFF));
    } else out.push_back('?');   // like Win32 and the iconv path: one '?' per unrepresentable character
    i += width;
  }
  return out;
}
std::string shiftjis_to_utf8(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    const bool lead = (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
    uint16_t code = b;
    size_t width = 1;
    if (lead && i + 1 < s.size()) { code = uint16_t(b << 8 | static_cast<unsigned char>(s[i + 1])); width = 2; }
    else if (b >= 0xA1 && b <= 0xDF) { append_utf8(out, 0xFF61 + (b - 0xA1)); ++i; continue; }   // half-width katakana
    uint32_t cp;
    if (codepoint_from_cp932(code, cp)) append_utf8(out, cp); else out.push_back('?');
    i += width;
  }
  return out;
}
#else
static std::string convert_codepage(const std::string& input, bool from_utf8) {
  if (input.empty()) return {};
  iconv_t converter = iconv_open(from_utf8 ? "CP932" : "UTF-8", from_utf8 ? "UTF-8" : "CP932");
  if (converter == (iconv_t)-1) throw std::runtime_error("CP932 conversion is unavailable");
  std::string result;
  char* source = const_cast<char*>(input.data()); // iconv changes the cursor, never input bytes
  size_t remaining = input.size();
  while (remaining) {
    char buffer[256];
    char* output = buffer;
    size_t available = sizeof(buffer);
    const size_t converted = iconv(converter, &source, &remaining, &output, &available);
    result.append(buffer, static_cast<size_t>(output - buffer));
    if (converted != static_cast<size_t>(-1)) break;
    if (errno == E2BIG) continue;
    if (errno != EILSEQ && errno != EINVAL) {
      iconv_close(converter);
      throw std::runtime_error("CP932 conversion failed");
    }
    // Win32 substitutes unrepresentable characters. Skip one complete UTF-8
    // sequence (or one malformed source byte) so an emoji becomes one '?'.
    size_t skip = 1;
    if (from_utf8 && errno == EILSEQ) {
      const auto lead = static_cast<unsigned char>(*source);
      size_t width = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 :
                     lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 1;
      if (width <= remaining && std::all_of(source + 1, source + width,
            [](unsigned char c) { return (c & 0xC0) == 0x80; })) skip = width;
    }
    result.push_back('?');
    source += skip;
    remaining -= skip;
  }
  iconv_close(converter);
  return result;
}
std::string utf8_to_shiftjis(const std::string& s) { return convert_codepage(s, true); }
std::string shiftjis_to_utf8(const std::string& s) { return convert_codepage(s, false); }
#endif

std::string truncate_length_char(const std::string& input, int length) {
  // Count code points, not bytes (UTF8ToUTF32 / resize / UTF32toUTF8 in Dolphin).
  std::string out;
  int count = 0;
  for (size_t i = 0; i < input.size() && count < length; ) {
    unsigned char c = (unsigned char)input[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    out.append(input, i, len);
    i += len; ++count;
  }
  return out;
}
static void convert_narrow_special_shiftjis(std::string& input) {
  static const std::unordered_map<char, uint16_t> table = {
      {'!', 0x8149}, {'"', 0x8168}, {'#', 0x8194}, {'$', 0x8190}, {'%', 0x8193}, {'&', 0x8195}, {'\'', 0x8166}, {'(', 0x8169},
      {')', 0x816a}, {'*', 0x8196}, {'+', 0x817b}, {',', 0x8143}, {'-', 0x817c}, {'.', 0x8144}, {'/', 0x815e}, {':', 0x8146},
      {';', 0x8147}, {'<', 0x8183}, {'=', 0x8181}, {'>', 0x8184}, {'?', 0x8148}, {'@', 0x8197}, {'[', 0x816d}, {'\\', 0x815f},
      {']', 0x816e}, {'^', 0x814f}, {'_', 0x8151}, {'`', 0x814d}, {'{', 0x816f}, {'|', 0x8162}, {'}', 0x8170}, {'~', 0x8160},
  };
  size_t pos = 0;
  while (pos < input.size()) {
    char c = input[pos];
    if ((unsigned char)c & 0x80) { pos += 2; continue; }
    auto it = table.find(c);
    if (it == table.end()) { ++pos; continue; }
    input.erase(pos, 1);
    input.insert(input.begin() + pos, (char)(it->second & 0xFF));
    input.insert(input.begin() + pos, (char)(it->second >> 8));
    pos += 2;
  }
}
std::string convert_string_for_game(const std::string& input, int length) {
  if (length <= 0) return std::string(1, '\0');
  std::string sj = utf8_to_shiftjis(truncate_length_char(input, length));
  convert_narrow_special_shiftjis(sj);
  sj.resize(static_cast<size_t>(length) * 2 + 1);
  return sj;
}
std::string convert_connect_code_for_game(const std::string& input) {
  std::string code;
  for (char c : input) { if (c == '#') { code += (char)0x81; code += (char)0x94; } else code += c; }
  code.resize(8 + 2);
  return code;
}
}  // namespace slippi
