// RetroArch shader presets over the game picture (melee-unlocked's gx/user_shader.cpp; settings.ini shaderpreset). Upstream hands
// a .slangp preset to librashader (Rust), which this stack does not have; here the same presets run on what the console has:
//   - the preset: melee-unlocked's own preset helpers, ported from its user_shader.cpp (the Shaders folder's listing, a single
//     .slang taken as a preset of one pass, the first missing file a preset or shader names, its #include walk), and a .slangp
//     reader with RetroArch's keys (#reference, shaders, shaderN, filter_linearN, wrap_modeN, scale_type[_x|_y]N, scale[_x|_y]N,
//     float_framebufferN, srgb_framebufferN, aliasN, frame_count_modN, textures with NAME, NAME_linear, NAME_wrap_mode, and
//     parameter values);
//   - the shaders: each pass's slang file read with its #includes, split at #pragma stage, its #pragma parameter, name and format
//     lines taken out, its push constant block turned into a uniform block (WebGPU has none) and its samplers given bindings of
//     their own (texture at 2 + 2k, Dawn's SPIR-V reader puts the sampler it splits off at 3 + 2k), compiled by glslang to SPIR-V
//     (Vulkan rules) with glslang's reflection for the uniforms' offsets;
//   - the passes: run in Aurora's present pass (frontend/patches/0134) through Dawn's SPIR-V reader, with RetroArch's semantics:
//     MVP, SourceSize, OriginalSize, OutputSize, FinalViewportSize, FrameCount (frame_count_mod), FrameDirection, Rotation, the
//     sub-frame values, OriginalHistoryN and its size, PassOutputN, PassFeedbackN, aliases and their feedback, LUT textures and
//     their sizes, and the parameters (the preset's values over the shaders' defaults).
// Compiling runs on a thread of its own; the GPU objects are made on Aurora's render worker the first frame after. Presentation
// only: the game never sees any of it. Off unless a preset is chosen.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "user_shader.h"
#include "gfx_live.h"
#include "video_bg.h"   // load_picture: LUT pictures (PNG, JPG, BMP) through FFmpeg where the build has it

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <webgpu/gpu.hpp>   // Aurora's lib/: the device, the queue, TextureWithSampler, set_present_filter (patches/0134)

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#ifdef __SWITCH__
// glslang's OSDependent part is built only for Windows and Unix hosts; this is its one function (never called here).
namespace glslang { void OS_DumpMemoryCounters() {} }
#endif

namespace fe::shaders {
namespace {
namespace wg = aurora::webgpu;
using Clock = std::chrono::steady_clock;

std::string shader_root() { return fe::gfx::folder() + "/Shaders"; }

// ---- melee-unlocked's preset helpers (port/runtime/gx/user_shader.cpp), off Windows ----
bool is_file(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
std::string read_text(const std::string& path) {
  std::string text;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return text;
  char chunk[4096];
  for (size_t n; text.size() < (1u << 20) && (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) text.append(chunk, n);
  std::fclose(f);
  return text;
}
std::string trimmed(const std::string& s, const char* drop = " \t\r\"") {
  const size_t a = s.find_first_not_of(drop), b = s.find_last_not_of(drop);
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// Calls `each` with every line of `text`, leading blanks removed.
template <class F> void for_lines(const std::string& text, F each) {
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const size_t first = text.find_first_not_of(" \t\r", at);
    if (first != std::string::npos && first < end) each(text.substr(first, end - first));
    at = end + 1;
  }
}
std::string parent_of(const std::string& path) { const size_t s = path.find_last_of('/'); return s == std::string::npos ? std::string(".") : path.substr(0, s); }
// A path named relative to a folder ("../x" steps up), as the preset's own folder resolves it; absolute names stay.
std::string join(const std::string& folder, const std::string& named) {
  if (!named.empty() && named[0] == '/') return named;
  std::vector<std::string> parts;
  auto push = [&](const std::string& s) {
    for (size_t at = 0; at <= s.size();) {
      size_t end = s.find_first_of("/\\", at);
      if (end == std::string::npos) end = s.size();
      const std::string p = s.substr(at, end - at);
      if (p == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else parts.push_back(p); }
      else if (!p.empty() && p != ".") parts.push_back(p);
      at = end + 1;
    }
  };
  push(folder); push(named);
  std::string out = folder.size() && folder[0] == '/' ? "/" : "";
  for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
  return out;
}
// The first file a shader pulls in with #include (followed into the files those name) that is not there, as the line wrote it.
// `budget` bounds the files read, against a pack that includes in a ring.
std::string missing_include(const std::string& shader, int depth, int* budget) {
  if (depth > 16 || --*budget < 0) return {};
  std::string missing;
  for_lines(read_text(shader), [&](const std::string& line) {
    if (!missing.empty() || line.rfind("#include", 0) != 0) return;
    const size_t open = line.find('"'), close = line.find('"', open == std::string::npos ? 0 : open + 1);
    if (open == std::string::npos || close == std::string::npos) return;
    const std::string named = line.substr(open + 1, close - open - 1);
    const std::string target = join(parent_of(shader), named);
    if (!is_file(target)) missing = named;
    else missing = missing_include(target, depth + 1, budget);
  });
  return missing;
}
// A preset holds no shader code: it lists shader files, the pictures they sample and sometimes another preset it builds on, all
// named relative to its own folder. This names the first file that is not there, as the preset or shader wrote it. Empty when
// every file named exists (or the preset cannot be read).
std::string first_missing_file(const std::string& preset, int depth = 0) {
  const std::string text = read_text(preset);
  const std::string folder = parent_of(preset);
  std::vector<std::string> textures;   // the names its "textures" line lists; each has a line of its own
  for_lines(text, [&](const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || trimmed(line.substr(0, eq)) != "textures") return;
    const std::string list = trimmed(line.substr(eq + 1));
    for (size_t at = 0; at <= list.size();) {
      size_t end = list.find(';', at);
      if (end == std::string::npos) end = list.size();
      const std::string name = trimmed(list.substr(at, end - at));
      if (!name.empty()) textures.push_back(name);
      at = end + 1;
    }
  });
  std::string missing;
  int budget = 600;
  for_lines(text, [&](const std::string& line) {
    if (!missing.empty()) return;
    std::string named;
    bool reference = false, shader = false;
    if (line.rfind("#reference", 0) == 0) { named = trimmed(line.substr(10)); reference = true; }
    else {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) return;
      const std::string key = trimmed(line.substr(0, eq));
      shader = key.size() > 6 && key.rfind("shader", 0) == 0 &&
               std::all_of(key.begin() + 6, key.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; });
      if (!shader && std::find(textures.begin(), textures.end(), key) == textures.end()) return;
      named = trimmed(line.substr(eq + 1));
    }
    if (named.empty()) return;
    const std::string target = join(folder, named);
    if (!is_file(target)) missing = named;
    else if (reference && depth < 8) missing = first_missing_file(target, depth + 1);
    else if (shader) missing = missing_include(target, 0, &budget);
  });
  return missing;
}
std::string say_missing(const std::string& missing) {
  return "this needs the file " + missing + ", which is not in the Shaders folder. Copy the whole shader pack with its folders, not single files out of it";
}

std::mutex g_status_mutex;
std::string g_status = "Off";
void set_status(const std::string& s) { std::lock_guard<std::mutex> lock(g_status_mutex); g_status = s; }

std::mutex g_presets_mutex;
std::vector<std::string> g_presets;
bool g_presets_listed = false;
void list_into(const std::string& dir, const std::string& rel, int depth, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    const std::string path = dir + "/" + name, r = rel.empty() ? name : rel + "/" + name;
    if (fe::gfx::is_dir(path)) { if (depth < 8 && out.size() < 4000) list_into(path, r, depth + 1, out); continue; }
    std::string ext;
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) for (char c : name.substr(dot)) ext += char(std::tolower((unsigned char)c));
    if (ext == ".slangp" || (ext == ".slang" && depth == 0)) out.push_back(r);
  }
  closedir(d);
}

// ---- the preset ----
enum class Scale { Unset, Source, Viewport, Absolute };
enum class Wrap { Border, Edge, Repeat, Mirror };
struct PassDesc {
  std::string path, alias;
  int linear = -1;              // filter_linearN: -1 not given (linear), 0 nearest, 1 linear
  Wrap wrap = Wrap::Border;     // RetroArch's default is clamp to border
  Scale type_x = Scale::Unset, type_y = Scale::Unset;
  float scale_x = 1.f, scale_y = 1.f;
  bool float_fb = false, srgb_fb = false, mipmap = false;
  uint32_t frame_count_mod = 0;
};
struct LutDesc { std::string name, path; bool linear = false, mipmap = false; Wrap wrap = Wrap::Border; };
struct Preset { std::vector<PassDesc> passes; std::vector<LutDesc> luts; std::map<std::string, float> params; };

Wrap wrap_of(const std::string& v) {
  if (v == "clamp_to_edge") return Wrap::Edge;
  if (v == "repeat") return Wrap::Repeat;
  if (v == "mirrored_repeat") return Wrap::Mirror;
  return Wrap::Border;
}
Scale scale_of(const std::string& v) {
  if (v == "source") return Scale::Source;
  if (v == "viewport") return Scale::Viewport;
  if (v == "absolute") return Scale::Absolute;
  return Scale::Unset;
}
bool truthy(const std::string& v) { return v == "true" || v == "1"; }

// key = value lines of a preset and every preset it #references (theirs first, so this file's values win). Paths are made
// absolute against the file that names them.
void read_preset_values(const std::string& file, std::map<std::string, std::string>& kv, int depth, std::string& error) {
  if (depth > 8) { error = "presets #reference each other too deeply"; return; }
  const std::string text = read_text(file);
  if (text.empty()) { error = "cannot read " + file; return; }
  const std::string folder = parent_of(file);
  std::map<std::string, std::string> own;
  std::vector<std::string> textures;
  for_lines(text, [&](const std::string& raw) {
    if (!error.empty()) return;
    if (raw.rfind("#reference", 0) == 0) { read_preset_values(join(folder, trimmed(raw.substr(10))), kv, depth + 1, error); return; }
    if (raw[0] == '#' || raw.rfind("//", 0) == 0) return;
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) return;
    std::string value = raw.substr(eq + 1);
    if (value.find('"') == std::string::npos) { const size_t c = value.find('#'); if (c != std::string::npos) value.resize(c); }
    own[trimmed(raw.substr(0, eq), " \t\r")] = trimmed(value);
  });
  // File names become absolute here: shaderN and the pictures the textures line lists.
  if (own.count("textures")) {
    const std::string list = own["textures"];
    for (size_t at = 0; at <= list.size();) {
      size_t end = list.find(';', at);
      if (end == std::string::npos) end = list.size();
      const std::string n = trimmed(list.substr(at, end - at));
      if (!n.empty()) textures.push_back(n);
      at = end + 1;
    }
  }
  for (auto& [k, v] : own) {
    const bool shader = k.size() > 6 && k.rfind("shader", 0) == 0 && std::all_of(k.begin() + 6, k.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; });
    if (shader || std::find(textures.begin(), textures.end(), k) != textures.end()) v = join(folder, v);
    kv[k] = v;
  }
  if (own.count("textures") && kv.count("textures") && depth > 0) {}   // a referencing preset's own list wins (set above)
}
bool parse_preset(const std::string& file, Preset& out, std::string& error) {
  std::map<std::string, std::string> kv;
  read_preset_values(file, kv, 0, error);
  if (!error.empty()) return false;
  const int count = kv.count("shaders") ? std::atoi(kv["shaders"].c_str()) : 0;
  if (count <= 0 || count > 32) { error = "the preset names no shaders (shaders = N)"; return false; }
  std::set<std::string> known{"shaders", "textures", "feedback_pass"};
  auto get = [&](const std::string& k) -> const std::string* { known.insert(k); auto it = kv.find(k); return it == kv.end() ? nullptr : &it->second; };
  for (int i = 0; i < count; ++i) {
    const std::string n = std::to_string(i);
    PassDesc p;
    if (const auto* v = get("shader" + n)) p.path = *v; else { error = "shader" + n + " is missing from the preset"; return false; }
    if (const auto* v = get("alias" + n)) p.alias = *v;
    if (const auto* v = get("filter_linear" + n)) p.linear = truthy(*v) ? 1 : 0;
    if (const auto* v = get("wrap_mode" + n)) p.wrap = wrap_of(*v);
    if (const auto* v = get("scale_type" + n)) p.type_x = p.type_y = scale_of(*v);
    if (const auto* v = get("scale_type_x" + n)) p.type_x = scale_of(*v);
    if (const auto* v = get("scale_type_y" + n)) p.type_y = scale_of(*v);
    if (const auto* v = get("scale" + n)) p.scale_x = p.scale_y = float(std::atof(v->c_str()));
    if (const auto* v = get("scale_x" + n)) p.scale_x = float(std::atof(v->c_str()));
    if (const auto* v = get("scale_y" + n)) p.scale_y = float(std::atof(v->c_str()));
    if (const auto* v = get("float_framebuffer" + n)) p.float_fb = truthy(*v);
    if (const auto* v = get("srgb_framebuffer" + n)) p.srgb_fb = truthy(*v);
    if (const auto* v = get("mipmap_input" + n)) p.mipmap = truthy(*v);
    if (const auto* v = get("frame_count_mod" + n)) p.frame_count_mod = uint32_t(std::max(0, std::atoi(v->c_str())));
    out.passes.push_back(p);
  }
  if (const auto* v = get("textures")) {
    for (size_t at = 0; at <= v->size();) {
      size_t end = v->find(';', at);
      if (end == std::string::npos) end = v->size();
      const std::string name = trimmed(v->substr(at, end - at));
      at = end + 1;
      if (name.empty()) continue;
      LutDesc l;
      l.name = name;
      if (const auto* p = get(name)) l.path = *p; else { error = "the texture " + name + " has no file in the preset"; return false; }
      if (const auto* p = get(name + "_linear")) l.linear = truthy(*p);
      if (const auto* p = get(name + "_mipmap")) l.mipmap = truthy(*p);
      if (const auto* p = get(name + "_wrap_mode")) l.wrap = wrap_of(*p);
      out.luts.push_back(l);
    }
  }
  // What is left with a number for a value is a parameter (the "parameters" line only lists them).
  for (const auto& [k, v] : kv) {
    if (known.count(k) || k == "parameters" || v.empty()) continue;
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    if (end && *end == 0) out.params[k] = f;
  }
  return true;
}

// ---- the shaders ----
struct Param { std::string name, desc; float def = 0, min = 0, max = 0, step = 0; };
struct Source { std::string vertex, fragment, name, format; std::vector<Param> params; std::vector<std::string> samplers; };

bool load_slang(const std::string& file, std::string& out, int depth, std::string& error) {
  if (depth > 16) { error = "#include goes too deep in " + file; return false; }
  const std::string text = read_text(file);
  if (text.empty()) { error = "cannot read " + file; return false; }
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trimmed(line, " \t");
    if (t.rfind("#include", 0) == 0) {
      const size_t open = t.find('"'), close = t.find('"', open == std::string::npos ? 0 : open + 1);
      if (open == std::string::npos || close == std::string::npos) { error = "bad #include in " + file; return false; }
      if (!load_slang(join(parent_of(file), t.substr(open + 1, close - open - 1)), out, depth + 1, error)) return false;
    } else {
      out += line; out += '\n';
    }
    at = end + 1;
  }
  return true;
}
// Words of a #pragma parameter line: NAME "description" default min max [step].
bool parse_param(const std::string& rest, Param& p) {
  const size_t q1 = rest.find('"'), q2 = rest.find('"', q1 == std::string::npos ? 0 : q1 + 1);
  if (q1 == std::string::npos || q2 == std::string::npos) return false;
  p.name = trimmed(rest.substr(0, q1), " \t");
  p.desc = rest.substr(q1 + 1, q2 - q1 - 1);
  float v[4] = {0, 0, 0, 0};
  const int n = std::sscanf(rest.c_str() + q2 + 1, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
  if (p.name.empty() || n < 3) return false;
  p.def = v[0]; p.min = v[1]; p.max = v[2]; p.step = n >= 4 ? v[3] : 0;
  return true;
}
bool split_source(const std::string& file, Source& out, std::string& error) {
  std::string all;
  if (!load_slang(file, all, 0, error)) return false;
  std::string shared, stage[2];
  int which = -1;   // -1 both, 0 vertex, 1 fragment
  std::map<std::string, int> sampler_index;
  size_t at = 0;
  while (at < all.size()) {
    size_t end = all.find('\n', at);
    if (end == std::string::npos) end = all.size();
    std::string line = all.substr(at, end - at);
    at = end + 1;
    const std::string t = trimmed(line, " \t");
    if (t.rfind("#pragma", 0) == 0) {
      std::string rest = trimmed(t.substr(7), " \t");
      if (rest.rfind("stage", 0) == 0) {
        const std::string s = trimmed(rest.substr(5), " \t");
        which = s == "vertex" ? 0 : s == "fragment" ? 1 : -1;
        if (which < 0) { error = "unknown #pragma stage " + s + " in " + file; return false; }
        line.clear();
      } else if (rest.rfind("parameter", 0) == 0) {
        Param p;
        if (parse_param(trimmed(rest.substr(9), " \t"), p) && std::none_of(out.params.begin(), out.params.end(), [&](const Param& q) { return q.name == p.name; }))
          out.params.push_back(p);
        line.clear();
      } else if (rest.rfind("name", 0) == 0) {
        out.name = trimmed(rest.substr(4), " \t");
        line.clear();
      } else if (rest.rfind("format", 0) == 0) {
        out.format = trimmed(rest.substr(6), " \t");
        line.clear();
      }
    }
    // layout(push_constant) uniform -> a uniform block at binding 1 (WebGPU has no push constants).
    for (size_t p = line.find("push_constant"); p != std::string::npos; p = line.find("push_constant", p + 1)) {
      const size_t open = line.rfind("layout", p), close = line.find(')', p);
      if (open == std::string::npos || close == std::string::npos) break;
      line = line.substr(0, open) + "layout(set = 0, binding = 1, std140)" + line.substr(close + 1);
      break;
    }
    // uniform sampler2D NAME; -> its own binding: texture 2 + 2k (the split sampler lands at 3 + 2k).
    const size_t s2d = line.find("sampler2D");
    if (s2d != std::string::npos && line.find("uniform") != std::string::npos && line.find("uniform") < s2d) {
      size_t n0 = s2d + 9;
      while (n0 < line.size() && std::isspace((unsigned char)line[n0])) ++n0;
      size_t n1 = n0;
      while (n1 < line.size() && (std::isalnum((unsigned char)line[n1]) || line[n1] == '_')) ++n1;
      const std::string name = line.substr(n0, n1 - n0);
      if (!name.empty()) {
        auto it = sampler_index.find(name);
        if (it == sampler_index.end()) { it = sampler_index.emplace(name, int(out.samplers.size())).first; out.samplers.push_back(name); }
        const size_t uni = line.find("uniform");
        line = "layout(set = 0, binding = " + std::to_string(2 + 2 * it->second) + ") " + line.substr(uni);
      }
    }
    if (which < 0) shared += line + "\n";
    else stage[which] += line + "\n";
  }
  if (stage[0].empty() || stage[1].empty()) { error = file + " has no vertex or no fragment stage (#pragma stage)"; return false; }
  out.vertex = shared + stage[0];
  out.fragment = shared + stage[1];
  return true;
}

// One uniform the pass reads: which block (0 the UBO at binding 0, 1 the push block at binding 1), where, and what it is.
struct Uniform { std::string name; int block = 0; uint32_t offset = 0; };
struct Compiled {
  std::vector<uint32_t> vertex, fragment;
  std::vector<Uniform> uniforms;
  uint32_t block_size[2] = {0, 0};
  bool has_block[2] = {false, false};
};
std::once_flag g_glslang_once;
bool compile(const Source& src, const std::string& file, Compiled& out, std::string& error) {
  std::call_once(g_glslang_once, [] { glslang::InitializeProcess(); });
  const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  glslang::TShader vs(EShLangVertex), fs(EShLangFragment);
  const char* vtext = src.vertex.c_str();
  const char* ftext = src.fragment.c_str();
  vs.setStrings(&vtext, 1); fs.setStrings(&ftext, 1);
  for (glslang::TShader* sh : {&vs, &fs}) {
    sh->setEnvInput(glslang::EShSourceGlsl, sh == &vs ? EShLangVertex : EShLangFragment, glslang::EShClientVulkan, 100);
    sh->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    sh->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (!sh->parse(GetDefaultResources(), 450, false, messages)) {
      error = file + " (" + (sh == &vs ? "vertex" : "fragment") + "): " + trimmed(sh->getInfoLog(), " \t\r\n");
      return false;
    }
  }
  glslang::TProgram program;
  program.addShader(&vs); program.addShader(&fs);
  if (!program.link(messages)) { error = file + ": " + trimmed(program.getInfoLog(), " \t\r\n"); return false; }
  program.buildReflection(EShReflectionDefault);
  for (int b = 0; b < program.getNumUniformBlocks(); ++b) {
    const auto& block = program.getUniformBlock(b);
    const int binding = block.getBinding();
    if (binding == 0 || binding == 1) { out.has_block[binding] = true; out.block_size[binding] = uint32_t(std::max(block.size, 16)); }
  }
  for (int i = 0; i < program.getNumUniformVariables(); ++i) {
    const auto& u = program.getUniform(i);
    if (u.index < 0 || u.index >= program.getNumUniformBlocks()) continue;
    const int binding = program.getUniformBlock(u.index).getBinding();
    if (binding != 0 && binding != 1) continue;
    std::string name = u.name;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(dot + 1);
    out.uniforms.push_back({name, binding, uint32_t(u.offset)});
  }
  glslang::SpvOptions options;
  options.disableOptimizer = true;
  glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), out.vertex, &options);
  glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), out.fragment, &options);
  if (out.vertex.empty() || out.fragment.empty()) { error = file + ": no SPIR-V came out"; return false; }
  return true;
}

// Everything the compile thread makes for one preset: CPU side only.
struct Lut { LutDesc desc; uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };
struct BuiltPass { PassDesc desc; Source source; Compiled code; };
struct Built {
  std::string preset;
  std::vector<BuiltPass> passes;
  std::vector<Lut> luts;
  std::map<std::string, float> params;   // every parameter's value: the shaders' defaults, then the preset's
  unsigned history = 0;                   // the deepest OriginalHistoryN any pass samples
};

std::shared_ptr<Built> build(const std::string& name, std::string& error) {
  std::string file = shader_root() + "/" + name;
  if (!is_file(file)) { error = name + " is not in the Shaders folder"; return nullptr; }
  Preset preset;
  const bool single = file.size() > 6 && file.compare(file.size() - 6, 6, ".slang") == 0;
  if (single) {
    // A single .slang file is a whole shader of one pass (upstream's one_pass_preset), here without a file written for it.
    int budget = 600;
    const std::string missing = missing_include(file, 0, &budget);
    if (!missing.empty()) { error = say_missing(missing); return nullptr; }
    PassDesc p; p.path = file; preset.passes.push_back(p);
  } else {
    const std::string missing = first_missing_file(file);
    if (!missing.empty()) { error = say_missing(missing); return nullptr; }
    if (!parse_preset(file, preset, error)) return nullptr;
  }
  auto out = std::make_shared<Built>();
  out->preset = name;
  for (const auto& p : preset.passes) {
    BuiltPass bp;
    bp.desc = p;
    if (!split_source(p.path, bp.source, error)) return nullptr;
    if (bp.desc.alias.empty()) bp.desc.alias = bp.source.name;
    if (!compile(bp.source, p.path.substr(p.path.find_last_of('/') + 1), bp.code, error)) return nullptr;
    for (const auto& prm : bp.source.params) if (!out->params.count(prm.name)) out->params[prm.name] = prm.def;
    for (const auto& s : bp.source.samplers)
      if (s.rfind("OriginalHistory", 0) == 0) out->history = std::max(out->history, unsigned(std::atoi(s.c_str() + 15)));
    out->passes.push_back(std::move(bp));
  }
  for (const auto& [k, v] : preset.params) if (out->params.count(k)) out->params[k] = v;
  for (const auto& l : preset.luts) {
    Lut lut; lut.desc = l;
    if (!&fe::video_bg::load_picture || !fe::video_bg::load_picture(l.path, 4096, 4096, &lut.width, &lut.height, lut.rgba)) {
      error = "the picture " + l.path.substr(l.path.find_last_of('/') + 1) + " (" + l.name + ") cannot be read";
      return nullptr;
    }
    out->luts.push_back(std::move(lut));
  }
  out->history = std::min(out->history, 16u);
  return out;
}

// ---- the GPU side (Aurora's render worker) ----
struct Target { wgpu::Texture texture; wgpu::TextureView view; uint32_t width = 0, height = 0; };
struct GpuPass {
  wgpu::RenderPipeline pipeline;
  wgpu::BindGroupLayout layout;
  wgpu::Buffer block[2];
  std::vector<uint8_t> data[2];
  wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;
  Target out[2];          // out[cur] is written this frame; out[1 - cur] is last frame's (feedback)
  bool feedback = false;
  wgpu::Sampler sampler;  // how this pass samples its inputs
  uint32_t width = 0, height = 0;
};
struct Chain {
  std::shared_ptr<Built> built;
  std::vector<GpuPass> passes;
  std::vector<Target> luts;
  std::vector<wgpu::Sampler> lut_samplers;
  std::vector<Target> history;   // ring of past Originals, history[(frame - k) % n]
  wgpu::RenderPipeline blit;     // copies the picture into the history ring
  wgpu::BindGroupLayout blit_layout;
  wgpu::TextureFormat blit_format = wgpu::TextureFormat::RGBA8Unorm;
  wgpu::Buffer vertices;
  Target black;
  wgpu::Sampler blit_sampler;
  wg::TextureWithSampler result;
  uint64_t frame = 0;
  unsigned cur = 0;
};

std::mutex g_chain_mutex;                // the worker's chain and what the compile thread hands over
std::shared_ptr<Built> g_pending;        // compiled, waiting for the worker
bool g_drop = false;                     // the worker drops its chain (preset off or replaced)
std::unique_ptr<Chain> g_chain;          // worker only
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_passes_running{0};
std::atomic<uint64_t> g_frames{0};

wgpu::TextureFormat format_of(const PassDesc& d, const std::string& pragma) {
  if (pragma == "R8G8B8A8_SRGB") return wgpu::TextureFormat::RGBA8UnormSrgb;
  if (pragma == "R16G16B16A16_SFLOAT" || pragma == "R32G32B32A32_SFLOAT") return wgpu::TextureFormat::RGBA16Float;   // 32-bit float is not filterable here
  if (pragma == "R8_UNORM") return wgpu::TextureFormat::R8Unorm;
  if (pragma == "R8G8_UNORM") return wgpu::TextureFormat::RG8Unorm;
  if (pragma == "R16_SFLOAT") return wgpu::TextureFormat::R16Float;
  if (pragma == "R16G16_SFLOAT") return wgpu::TextureFormat::RG16Float;
  if (pragma == "A2B10G10R10_UNORM_PACK32") return wgpu::TextureFormat::RGB10A2Unorm;
  if (d.float_fb) return wgpu::TextureFormat::RGBA16Float;
  if (d.srgb_fb) return wgpu::TextureFormat::RGBA8UnormSrgb;
  return wgpu::TextureFormat::RGBA8Unorm;
}
wgpu::AddressMode address_of(Wrap w) {
  switch (w) {
    case Wrap::Edge: return wgpu::AddressMode::ClampToEdge;
    case Wrap::Repeat: return wgpu::AddressMode::Repeat;
    case Wrap::Mirror: return wgpu::AddressMode::MirrorRepeat;
    default: return wgpu::AddressMode::ClampToEdge;   // WebGPU has no border colour: the edge stands in for it
  }
}
wgpu::Sampler make_sampler(bool linear, Wrap wrap) {
  const wgpu::AddressMode a = address_of(wrap);
  const wgpu::FilterMode f = linear ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  const wgpu::SamplerDescriptor d{.label = "shader preset sampler", .addressModeU = a, .addressModeV = a, .addressModeW = a,
                                  .magFilter = f, .minFilter = f, .mipmapFilter = wgpu::MipmapFilterMode::Nearest};
  return wg::g_device.CreateSampler(&d);
}
Target make_target(uint32_t w, uint32_t h, wgpu::TextureFormat format, const char* label) {
  Target t;
  t.width = std::max(1u, w); t.height = std::max(1u, h);
  const wgpu::TextureDescriptor d{.label = label,
                                  .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
                                  .dimension = wgpu::TextureDimension::e2D, .size = {t.width, t.height, 1}, .format = format,
                                  .mipLevelCount = 1, .sampleCount = 1};
  t.texture = wg::g_device.CreateTexture(&d);
  t.view = t.texture.CreateView();
  return t;
}
void upload(const Target& t, const uint8_t* rgba) {
  const wgpu::TexelCopyTextureInfo dst{.texture = t.texture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = t.width * 4, .rowsPerImage = t.height};
  const wgpu::Extent3D size{t.width, t.height, 1};
  wg::g_queue.WriteTexture(&dst, rgba, size_t(t.width) * t.height * 4, &layout, &size);
}
wgpu::ShaderModule spirv_module(const std::vector<uint32_t>& words, const char* label) {
  wgpu::ShaderSourceSPIRV spirv;
  spirv.codeSize = uint32_t(words.size());
  spirv.code = words.data();
  const wgpu::ShaderModuleDescriptor d{.nextInChain = &spirv, .label = label};
  return wg::g_device.CreateShaderModule(&d);
}
// The pipeline objects of a chain, made inside an error scope: a shader Dawn's SPIR-V reader refuses, or a pipeline it cannot
// make, is reported and the preset stays off.
constexpr const char* kBlitWgsl = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
struct Out { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> Out {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  var o: Out; o.pos = vec4f(p * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0); o.uv = p; return o;
}
@fragment fn fs(in: Out) -> @location(0) vec4f { return textureSample(src, smp, in.uv); }
)";
std::string make_chain(Chain& c) {
  const auto& b = *c.built;
  wg::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
  // Four corners, as RetroArch's: Position in 0..1 and TexCoord with v = 0 at the top.
  const float quad[4 * 6] = {0, 0, 0, 1, 0, 0,   1, 0, 0, 1, 1, 0,   0, 1, 0, 1, 0, 1,   1, 1, 0, 1, 1, 1};
  const wgpu::BufferDescriptor vd{.label = "shader preset quad", .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst, .size = sizeof quad};
  c.vertices = wg::g_device.CreateBuffer(&vd);
  wg::g_queue.WriteBuffer(c.vertices, 0, quad, sizeof quad);
  const uint8_t zero[4] = {0, 0, 0, 255};
  c.black = make_target(1, 1, wgpu::TextureFormat::RGBA8Unorm, "shader preset black");
  upload(c.black, zero);
  c.blit_sampler = make_sampler(false, Wrap::Edge);
  for (const auto& l : b.luts) {
    Target t = make_target(l.width, l.height, wgpu::TextureFormat::RGBA8Unorm, "shader preset LUT");
    upload(t, l.rgba.data());
    c.luts.push_back(t);
    c.lut_samplers.push_back(make_sampler(l.desc.linear, l.desc.wrap));
  }
  if (b.history) {
    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = kBlitWgsl;
    const wgpu::ShaderModuleDescriptor md{.nextInChain = &wgsl, .label = "shader preset history copy"};
    const wgpu::ShaderModule m = wg::g_device.CreateShaderModule(&md);
    const wgpu::ColorTargetState target{.format = c.blit_format};
    const wgpu::FragmentState fragment{.module = m, .entryPoint = "fs", .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor pd{.label = "shader preset history copy", .vertex = {.module = m, .entryPoint = "vs"},
                                            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList}, .fragment = &fragment};
    c.blit = wg::g_device.CreateRenderPipeline(&pd);
    c.blit_layout = c.blit.GetBindGroupLayout(0);
  }
  // Which passes are read back a frame later (PassFeedbackN or <alias>Feedback).
  std::set<std::string> fed;
  for (const auto& p : b.passes) for (const auto& s : p.source.samplers) fed.insert(s);
  for (size_t i = 0; i < b.passes.size(); ++i) {
    const BuiltPass& bp = b.passes[i];
    GpuPass g;
    g.format = format_of(bp.desc, bp.source.format);
    g.feedback = fed.count("PassFeedback" + std::to_string(i)) || (!bp.desc.alias.empty() && fed.count(bp.desc.alias + "Feedback"));
    g.sampler = make_sampler(bp.desc.linear != 0, bp.desc.wrap);
    std::vector<wgpu::BindGroupLayoutEntry> entries;
    for (int k = 0; k < 2; ++k) {
      if (!bp.code.has_block[k]) continue;
      g.data[k].assign((bp.code.block_size[k] + 15) & ~15u, 0);
      const wgpu::BufferDescriptor bd{.label = "shader preset uniforms", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = g.data[k].size()};
      g.block[k] = wg::g_device.CreateBuffer(&bd);
      entries.push_back({.binding = uint32_t(k), .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
                         .buffer = {.type = wgpu::BufferBindingType::Uniform}});
    }
    for (size_t s = 0; s < bp.source.samplers.size(); ++s) {
      entries.push_back({.binding = uint32_t(2 + 2 * s), .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
                         .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D}});
      entries.push_back({.binding = uint32_t(3 + 2 * s), .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
                         .sampler = {.type = wgpu::SamplerBindingType::Filtering}});
    }
    const wgpu::BindGroupLayoutDescriptor ld{.label = "shader preset pass", .entryCount = entries.size(), .entries = entries.data()};
    g.layout = wg::g_device.CreateBindGroupLayout(&ld);
    const wgpu::PipelineLayoutDescriptor pld{.label = "shader preset pass", .bindGroupLayoutCount = 1, .bindGroupLayouts = &g.layout};
    const wgpu::PipelineLayout pl = wg::g_device.CreatePipelineLayout(&pld);
    const wgpu::ShaderModule vm = spirv_module(bp.code.vertex, "shader preset vertex");
    const wgpu::ShaderModule fm = spirv_module(bp.code.fragment, "shader preset fragment");
    const std::array<wgpu::VertexAttribute, 2> attrs{wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 0},
                                                     wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x2, .offset = 16, .shaderLocation = 1}};
    const wgpu::VertexBufferLayout vbl{.stepMode = wgpu::VertexStepMode::Vertex, .arrayStride = 24, .attributeCount = attrs.size(), .attributes = attrs.data()};
    const wgpu::ColorTargetState target{.format = g.format};
    const wgpu::FragmentState fragment{.module = fm, .entryPoint = "main", .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor pd{.label = "shader preset pass", .layout = pl,
                                            .vertex = {.module = vm, .entryPoint = "main", .bufferCount = 1, .buffers = &vbl},
                                            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleStrip}, .fragment = &fragment};
    g.pipeline = wg::g_device.CreateRenderPipeline(&pd);
    c.passes.push_back(std::move(g));
  }
  static std::string scope_error;   // worker only
  scope_error.clear();
  const auto future = wg::g_device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
      [](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
        if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError) scope_error = std::string(message.data, message.length);
      });
  wg::g_instance.WaitAny(future, 10000000000ull);
  return scope_error;
}

// RetroArch's size semantic: width, height, 1/width, 1/height.
void put_size(std::vector<uint8_t>& data, uint32_t at, uint32_t w, uint32_t h) {
  const float v[4] = {float(w), float(h), 1.f / float(std::max(1u, w)), 1.f / float(std::max(1u, h))};
  if (at + sizeof v <= data.size()) std::memcpy(data.data() + at, v, sizeof v);
}
template <class T> void put(std::vector<uint8_t>& data, uint32_t at, T v) { if (at + sizeof v <= data.size()) std::memcpy(data.data() + at, &v, sizeof v); }

uint32_t scaled(Scale type, float scale, uint32_t source, uint32_t viewport) {
  switch (type) {
    case Scale::Viewport: return uint32_t(std::lround(float(viewport) * scale));
    case Scale::Absolute: return uint32_t(std::lround(scale));
    default: return uint32_t(std::lround(float(source) * scale));
  }
}

const wg::TextureWithSampler* run(const wgpu::CommandEncoder& encoder, const wg::TextureWithSampler& source, uint32_t vw, uint32_t vh) {
  {
    std::lock_guard<std::mutex> lock(g_chain_mutex);
    if (g_drop) { g_chain.reset(); g_drop = false; g_passes_running.store(0); }
    if (g_pending) {
      auto chain = std::make_unique<Chain>();
      chain->built = std::move(g_pending);
      const std::string error = make_chain(*chain);
      if (!error.empty()) {
        set_status("the GPU refused it: " + error.substr(0, 300));
        say("shaders: %s is off: the GPU refused it: %s", chain->built->preset.c_str(), error.c_str());
        g_chain.reset();
      } else {
        say("shaders: %s is running (%zu passes)", chain->built->preset.c_str(), chain->passes.size());
        set_status("Running: " + std::to_string(chain->passes.size()) + (chain->passes.size() == 1 ? " pass" : " passes"));
        g_passes_running.store(uint32_t(chain->passes.size()));
        g_chain = std::move(chain);
      }
    }
  }
  if (!g_chain) return nullptr;
  Chain& c = *g_chain;
  const Built& b = *c.built;
  const uint32_t ow = source.size.width, oh = source.size.height;
  // The Original this frame: the picture itself, and a copy into the history ring when a pass looks back.
  wgpu::TextureView original = source.view;
  if (b.history) {
    if (c.history.size() != b.history + 1 || c.history[0].width != ow || c.history[0].height != oh) {
      c.history.clear();
      for (unsigned i = 0; i <= b.history; ++i) c.history.push_back(make_target(ow, oh, c.blit_format, "shader preset history"));
    }
    const Target& slot = c.history[c.frame % c.history.size()];
    const std::array e{wgpu::BindGroupEntry{.binding = 0, .textureView = source.view}, wgpu::BindGroupEntry{.binding = 1, .sampler = c.blit_sampler}};
    const wgpu::BindGroupDescriptor bd{.layout = c.blit_layout, .entryCount = e.size(), .entries = e.data()};
    const wgpu::BindGroup group = wg::g_device.CreateBindGroup(&bd);
    const wgpu::RenderPassColorAttachment at{.view = slot.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor rp{.label = "shader preset history", .colorAttachmentCount = 1, .colorAttachments = &at};
    const auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(c.blit); pass.SetBindGroup(0, group); pass.Draw(3); pass.End();
  }
  auto history = [&](unsigned k, uint32_t* w, uint32_t* h) -> wgpu::TextureView {   // k frames ago (k >= 1)
    *w = ow; *h = oh;
    if (c.history.empty() || k >= c.history.size() || c.frame < k) return original;
    return c.history[(c.frame - k) % c.history.size()].view;
  };
  uint32_t sw = ow, sh = oh;
  wgpu::TextureView src = original;
  for (size_t i = 0; i < c.passes.size(); ++i) {
    GpuPass& g = c.passes[i];
    const BuiltPass& bp = b.passes[i];
    const bool last = i + 1 == c.passes.size();
    const Scale tx = bp.desc.type_x == Scale::Unset ? (last ? Scale::Viewport : Scale::Source) : bp.desc.type_x;
    const Scale ty = bp.desc.type_y == Scale::Unset ? (last ? Scale::Viewport : Scale::Source) : bp.desc.type_y;
    const uint32_t w = std::clamp(scaled(tx, bp.desc.scale_x, sw, vw), 1u, 8192u), h = std::clamp(scaled(ty, bp.desc.scale_y, sh, vh), 1u, 8192u);
    const unsigned slot = g.feedback ? c.cur : 0;
    for (unsigned k = 0; k < (g.feedback ? 2u : 1u); ++k)
      if (g.out[k].width != w || g.out[k].height != h) g.out[k] = make_target(w, h, g.format, "shader preset pass");
    g.width = w; g.height = h;
    // Uniforms.
    for (const Uniform& u : bp.code.uniforms) {
      auto& d = g.data[u.block];
      const std::string& n = u.name;
      if (n == "MVP") {
        // RetroArch's orthographic matrix with y turned over: WebGPU's clip space has +y up, Vulkan's (which the shaders assume) down.
        const float m[16] = {2, 0, 0, 0,   0, -2, 0, 0,   0, 0, 1, 0,   -1, 1, 0, 1};
        if (u.offset + sizeof m <= d.size()) std::memcpy(d.data() + u.offset, m, sizeof m);
      } else if (n == "OutputSize") put_size(d, u.offset, w, h);
      else if (n == "SourceSize") put_size(d, u.offset, sw, sh);
      else if (n == "OriginalSize") put_size(d, u.offset, ow, oh);
      else if (n == "FinalViewportSize") put_size(d, u.offset, vw, vh);
      else if (n == "FrameCount") put<uint32_t>(d, u.offset, uint32_t(bp.desc.frame_count_mod ? c.frame % bp.desc.frame_count_mod : c.frame));
      else if (n == "FrameDirection") put<int32_t>(d, u.offset, 1);
      else if (n == "Rotation") put<uint32_t>(d, u.offset, 0);
      else if (n == "TotalSubFrames" || n == "CurrentSubFrame") put<uint32_t>(d, u.offset, 1);
      else if (n == "OriginalAspect") put<float>(d, u.offset, float(ow) / float(std::max(1u, oh)));
      else if (n == "OriginalFPS") put<float>(d, u.offset, 60.f);
      else if (n == "FrameTimeDelta") put<uint32_t>(d, u.offset, 16667);
      else if (n.rfind("OriginalHistorySize", 0) == 0) { uint32_t hw, hh; history(unsigned(std::atoi(n.c_str() + 19)), &hw, &hh); put_size(d, u.offset, hw, hh); }
      else if (n.rfind("PassOutputSize", 0) == 0) { const size_t k = size_t(std::atoi(n.c_str() + 14)); if (k < c.passes.size()) put_size(d, u.offset, c.passes[k].width, c.passes[k].height); }
      else if (n.rfind("PassFeedbackSize", 0) == 0) { const size_t k = size_t(std::atoi(n.c_str() + 16)); if (k < c.passes.size()) put_size(d, u.offset, c.passes[k].width, c.passes[k].height); }
      else if (const auto it = b.params.find(n); it != b.params.end()) put<float>(d, u.offset, it->second);
      else {
        for (size_t k = 0; k < b.luts.size(); ++k) if (n == b.luts[k].desc.name + "Size") put_size(d, u.offset, b.luts[k].width, b.luts[k].height);
        for (size_t k = 0; k < c.passes.size(); ++k) {
          const std::string& alias = b.passes[k].desc.alias;
          if (!alias.empty() && (n == alias + "Size" || n == alias + "FeedbackSize")) put_size(d, u.offset, c.passes[k].width, c.passes[k].height);
        }
      }
    }
    std::vector<wgpu::BindGroupEntry> entries;
    for (int k = 0; k < 2; ++k)
      if (g.block[k]) {
        wg::g_queue.WriteBuffer(g.block[k], 0, g.data[k].data(), g.data[k].size());
        entries.push_back({.binding = uint32_t(k), .buffer = g.block[k], .size = g.data[k].size()});
      }
    for (size_t s = 0; s < bp.source.samplers.size(); ++s) {
      const std::string& n = bp.source.samplers[s];
      wgpu::TextureView view = c.black.view;
      wgpu::Sampler smp = g.sampler;
      uint32_t dummy_w, dummy_h;
      auto pass_by_alias = [&](const std::string& name, bool feedback) -> int {
        for (size_t k = 0; k < b.passes.size(); ++k) if (!b.passes[k].desc.alias.empty() && b.passes[k].desc.alias + (feedback ? "Feedback" : "") == name) return int(k);
        return -1;
      };
      auto feedback_view = [&](size_t k) { const GpuPass& p = c.passes[k]; return p.feedback && p.out[1 - c.cur].view ? p.out[1 - c.cur].view : c.black.view; };
      if (n == "Original" || n == "OriginalHistory0") view = original;
      else if (n == "Source") view = src;
      else if (n.rfind("OriginalHistory", 0) == 0) view = history(unsigned(std::atoi(n.c_str() + 15)), &dummy_w, &dummy_h);
      else if (n.rfind("PassOutput", 0) == 0) { const size_t k = size_t(std::atoi(n.c_str() + 10)); if (k < i) view = c.passes[k].out[c.passes[k].feedback ? c.cur : 0].view; }
      else if (n.rfind("PassFeedback", 0) == 0) { const size_t k = size_t(std::atoi(n.c_str() + 12)); if (k < c.passes.size()) view = feedback_view(k); }
      else if (const int k = pass_by_alias(n, false); k >= 0) { if (size_t(k) < i) view = c.passes[k].out[c.passes[k].feedback ? c.cur : 0].view; }
      else if (const int k = pass_by_alias(n, true); k >= 0) view = feedback_view(size_t(k));
      else for (size_t k = 0; k < b.luts.size(); ++k) if (b.luts[k].desc.name == n) { view = c.luts[k].view; smp = c.lut_samplers[k]; }
      entries.push_back({.binding = uint32_t(2 + 2 * s), .textureView = view});
      entries.push_back({.binding = uint32_t(3 + 2 * s), .sampler = smp});
    }
    const wgpu::BindGroupDescriptor bd{.layout = g.layout, .entryCount = entries.size(), .entries = entries.data()};
    const wgpu::BindGroup group = wg::g_device.CreateBindGroup(&bd);
    const wgpu::RenderPassColorAttachment at{.view = g.out[slot].view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
                                             .clearValue = {0, 0, 0, 1}};
    const wgpu::RenderPassDescriptor rp{.label = "shader preset pass", .colorAttachmentCount = 1, .colorAttachments = &at};
    const auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(g.pipeline);
    pass.SetBindGroup(0, group);
    pass.SetVertexBuffer(0, c.vertices);
    pass.SetViewport(0, 0, float(w), float(h), 0, 1);
    pass.Draw(4);
    pass.End();
    src = g.out[slot].view; sw = w; sh = h;
  }
  const GpuPass& final_pass = c.passes.back();
  const Target& out = final_pass.out[final_pass.feedback ? c.cur : 0];
  c.result.texture = out.texture; c.result.view = out.view; c.result.size = {out.width, out.height, 1}; c.result.format = final_pass.format;
  if (!c.result.sampler) c.result.sampler = make_sampler(true, Wrap::Edge);
  ++c.frame;
  c.cur ^= 1u;
  g_frames.fetch_add(1, std::memory_order_relaxed);
  return &c.result;
}

// The compile thread: one preset at a time, the newest choice wins.
struct Loader {
  std::thread thread;
  std::mutex mutex;
  std::string chosen, built_for;
  uint32_t generation = ~0u;
  bool busy = false;
};
Loader g_loader;

void start_build(const std::string& name) {
  if (g_loader.thread.joinable()) g_loader.thread.join();
  g_loader.busy = true;
  set_status("Compiling " + name + "...");
  g_loader.thread = std::thread([name] {
    const auto t0 = Clock::now();
    std::string error;
    auto built = build(name, error);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!built) {
      set_status(error);
      say("shaders: %s is off: %s", name.c_str(), error.c_str());
    } else {
      say("shaders: %s compiled to SPIR-V in %.0f ms (%zu passes, %zu textures, %zu parameters, history %u)", name.c_str(), ms,
          built->passes.size(), built->luts.size(), built->params.size(), built->history);
      std::lock_guard<std::mutex> lock(g_chain_mutex);
      g_pending = std::move(built);
    }
    std::lock_guard<std::mutex> lock(g_loader.mutex);
    g_loader.busy = false;
  });
}
}  // namespace

void frame() {
  auto& l = fe::gfx::live();
  const uint32_t gen = l.shader_generation.load(std::memory_order_relaxed);
  if (gen == g_loader.generation) return;
  {
    std::lock_guard<std::mutex> lock(g_loader.mutex);
    if (g_loader.busy) return;   // the next frame looks again once this compile is done
  }
  g_loader.generation = gen;
  std::string name;
  { std::lock_guard<std::mutex> lock(l.mutex); name = l.shader_preset; }
  if (name == g_loader.built_for) return;
  g_loader.built_for = name;
  {
    std::lock_guard<std::mutex> lock(g_chain_mutex);
    g_pending.reset(); g_drop = true;
  }
  if (name.empty()) { set_status("Off"); say("shaders: off"); return; }
  if (!g_installed.exchange(true)) wg::set_present_filter(&run);
  start_build(name);
}
std::vector<std::string> presets() {
  std::lock_guard<std::mutex> lock(g_presets_mutex);
  if (!g_presets_listed) {
    g_presets.clear();
    list_into(shader_root(), "", 0, g_presets);
    std::sort(g_presets.begin(), g_presets.end());
    g_presets_listed = true;
  }
  return g_presets;
}
void refresh_presets() { std::lock_guard<std::mutex> lock(g_presets_mutex); g_presets_listed = false; }
std::string status() {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  return g_status;
}
void shutdown() {
  if (g_loader.thread.joinable()) g_loader.thread.join();
  if (g_installed.exchange(false)) wg::set_present_filter(nullptr);
  if (g_frames.load()) say("shaders: %llu frames drawn through the preset", (unsigned long long)g_frames.load());
  // The chain is left to the process's end, not destroyed: the worker may still be drawing the frame in flight with it, and
  // after Aurora shuts down its objects must not be released against a device that is gone.
  std::lock_guard<std::mutex> lock(g_chain_mutex);
  (void)g_chain.release();
}
// gfx_render.cpp refers to this (MU_FE_SHADERS), so this object is linked: everything else reaches it through weak references.
extern const int shaders_linked = 1;
}  // namespace fe::shaders
