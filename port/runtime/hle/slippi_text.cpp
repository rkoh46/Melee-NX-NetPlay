// Host text adapter for the existing Slippi EXI string format. Windows uses its
// CP932 converter; macOS/Linux use iconv's matching Windows-31J code page.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_net.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
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
