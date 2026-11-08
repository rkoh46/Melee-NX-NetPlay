// Melee Unlocked Launcher: an optional Win32 client with a Play page (ISO, Slippi account, version
// and self-update) and a Build tab (drop the ISO: verify it, run the source build when this is a
// checkout, precompile the pipeline library, remember the path). Starts melee_port.exe with the
// release settings. Plain Win32 so it has no dependencies beyond the OS.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include "updater.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace {
enum { ID_TABS = 100, ID_ISO_EDIT, ID_BROWSE, ID_PLAY, ID_SLIPPI_TEXT, ID_SLIPPI_GET, ID_VERSION_TEXT, ID_UPDATE, ID_HINT,
       ID_BUILD_TEXT, ID_DROP, ID_BUILD, ID_LOG, ID_TIMER = 1 };
const UINT WM_APP_LOG = WM_APP + 1;      // lParam: heap std::string* to append to the log
const UINT WM_APP_BUILD_DONE = WM_APP + 2;
const UINT WM_APP_GAME_DONE = WM_APP + 3;

HWND g_main, g_tabs, g_play[8], g_build[4];
HWND g_iso_edit, g_play_btn, g_slippi_text, g_slippi_btn, g_version_text, g_update_btn, g_log, g_build_btn, g_drop;
HFONT g_font, g_font_big, g_font_mono;
std::string g_dir, g_iso, g_game_exe;
std::atomic<bool> g_building{false}, g_playing{false};
bool g_slippi_missing = false;
int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring widen(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w; }
std::string narrow(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n ? n - 1 : 0, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); return s; }
bool file_exists(const std::string& p) { DWORD a = GetFileAttributesW(widen(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
void set_text(HWND h, const std::string& s) { SetWindowTextW(h, widen(s).c_str()); }

void log_line(const char* fmt, ...) {
  char buf[4096]; va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  PostMessageW(g_main, WM_APP_LOG, 0, (LPARAM) new std::string(std::string(buf) + "\r\n"));
}

std::string repo_root();   // defined below; load_ini looks for a disc beside a source checkout
std::string ini_path() { return g_dir + "\\launcher.ini"; }
// A second copy outside the game folder, so the ISO path survives an update, a re-extracted zip
// or a second copy of the game, and so the launcher knows the disc the game itself was last
// started with (melee_port records it there too).
std::string shared_ini_path() {
  char* local = nullptr; size_t n = 0;
  if (_dupenv_s(&local, &n, "LOCALAPPDATA") != 0 || !local) return "";
  std::string dir = std::string(local) + "\\MeleeUnlocked";
  free(local);
  CreateDirectoryW(widen(dir).c_str(), nullptr);
  return dir + "\\launcher.ini";
}
std::string read_iso_from(const std::string& path) {
  std::ifstream f(path); std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("iso=", 0) == 0) return line.substr(4);
  }
  return "";
}
void load_ini() {
  g_iso = read_iso_from(ini_path());
  if (g_iso.empty() || !file_exists(g_iso)) {
    std::string remembered = read_iso_from(shared_ini_path());
    if (!remembered.empty() && file_exists(remembered)) g_iso = remembered;
  }
  if (g_iso.empty() && file_exists(g_dir + "\\melee.iso")) g_iso = g_dir + "\\melee.iso";
  std::string root = repo_root();
  if (g_iso.empty() && !root.empty() && file_exists(root + "\\melee.iso")) g_iso = root + "\\melee.iso";
  if (!g_iso.empty() && !file_exists(g_iso)) g_iso.clear();
}
void save_ini() {
  { std::ofstream f(ini_path()); f << "iso=" << g_iso << "\n"; }
  const std::string shared = shared_ini_path();
  if (!shared.empty()) { std::ofstream f(shared); f << "iso=" << g_iso << "\n"; }
}

// Header check: game id GALE01 at offset 0, revision byte 2 at offset 7 (NTSC 1.02).
bool verify_iso(const std::string& path, std::string* why) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *why = "cannot open the file"; return false; }
  char hdr[8]{}; f.read(hdr, 8);
  if (std::string(hdr, 6) != "GALE01") { *why = "not a Melee NTSC disc (game id " + std::string(hdr, 6) + ")"; return false; }
  if (hdr[7] != 2) { *why = "Melee NTSC 1.0" + std::to_string((int)hdr[7]) + "; version 1.02 is required"; return false; }
  f.seekg(0, std::ios::end);
  auto size = (unsigned long long)f.tellg();
  if (size < 1000000000ull) { *why = "file is too small to be a full disc image (" + std::to_string(size / 1000000) + " MB)"; return false; }
  return true;
}

// Slippi account: this folder's User\Slippi\user.json, else the Slippi Launcher's login.
std::string slippi_account_line() {
  std::string paths[2] = {g_dir + "\\User\\Slippi\\user.json", ""};
  char* appdata = nullptr; size_t n = 0;
  if (_dupenv_s(&appdata, &n, "APPDATA") == 0 && appdata) { paths[1] = std::string(appdata) + "\\Slippi Launcher\\netplay\\User\\Slippi\\user.json"; free(appdata); }
  for (auto& p : paths) {
    if (p.empty() || !file_exists(p)) continue;
    std::ifstream f(p); auto j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_object() && j.count("connectCode")) return "Slippi account: " + j.value("displayName", std::string("?")) + " (" + j["connectCode"].get<std::string>() + ")";
  }
  return "Slippi online needs an account: install the Slippi Launcher and log in once.";
}

// Runs a command line with stdout/stderr piped into the log. Returns the exit code.
DWORD run_logged(std::string cmd, const std::string& cwd) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return 1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) { CloseHandle(rd); CloseHandle(wr); log_line("cannot start: %s", cmd.c_str()); return 1; }
  CloseHandle(wr);
  std::string acc; char buf[4096]; DWORD got = 0;
  while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) {
    acc.append(buf, got);
    size_t nl;
    while ((nl = acc.find('\n')) != std::string::npos) { std::string line = acc.substr(0, nl); if (!line.empty() && line.back() == '\r') line.pop_back(); log_line("%s", line.c_str()); acc.erase(0, nl + 1); }
  }
  if (!acc.empty()) log_line("%s", acc.c_str());
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  return code;
}

// A source checkout has build.bat at the repo root; the launcher then lives in build-review\port\Release.
std::string repo_root() {
  std::string d = g_dir;
  for (int i = 0; i < 4; ++i) { if (file_exists(d + "\\build.bat") && file_exists(d + "\\tools\\extract_dol.py")) return d; auto p = d.find_last_of("\\/"); if (p == std::string::npos) break; d.resize(p); }
  return "";
}
std::string game_exe() {
  if (file_exists(g_dir + "\\melee_port.exe")) return g_dir + "\\melee_port.exe";
  std::string root = repo_root();
  if (!root.empty()) return root + "\\build-review\\port\\Release\\melee_port.exe";
  return g_dir + "\\melee_port.exe";
}
// Working directory for the game: the release folder (Sys next to the launcher) or the repo root
// of a source checkout (its defaults, port/slippi_sys and shadercache, are relative paths).
std::string work_dir() {
  if (file_exists(g_dir + "\\Sys\\codehandler.bin")) return g_dir;
  std::string root = repo_root();
  return root.empty() ? g_dir : root;
}
std::string game_args() {
  std::string base = g_dir;
  std::string a = " --iso \"" + g_iso + "\" --threaded-renderer --fps unlocked --frame-mode authored --scale auto --volume 70";
  if (file_exists(g_dir + "\\Sys\\codehandler.bin"))
    a += " --sys-dir \"" + base + "\\Sys\" --user-dir \"" + base + "\\User\\Slippi\" --replay-dir \"" + base + "\\Replays\" --card-dir \"" + base + "\\User\\GC\\CardA\"";
  return a;
}

void build_thread() {
  std::string why;
  log_line("Checking %s", g_iso.c_str());
  if (!verify_iso(g_iso, &why)) { log_line("Rejected: %s.", why.c_str()); PostMessageW(g_main, WM_APP_BUILD_DONE, 1, 0); return; }
  log_line("Melee NTSC 1.02 disc image: OK");
  save_ini();
  g_game_exe = game_exe();
  if (!file_exists(g_game_exe)) {
    std::string root = repo_root();
    if (root.empty()) { log_line("melee_port.exe is missing next to this launcher and this is not a source checkout."); PostMessageW(g_main, WM_APP_BUILD_DONE, 1, 0); return; }
    log_line("Source checkout at %s: running build.bat (20 to 40 minutes the first time)", root.c_str());
    DWORD code = run_logged("cmd /c \"\"" + root + "\\build.bat\" \"" + g_iso + "\"\" <nul", root);
    if (code != 0 || !file_exists(g_game_exe)) { log_line("Build failed (exit code %lu).", code); PostMessageW(g_main, WM_APP_BUILD_DONE, 1, 0); return; }
  }
  log_line("Precompiling the graphics pipelines for this GPU (this runs once, about 15 to 30 seconds)");
  std::string cwd = work_dir();
  DWORD code = run_logged("\"" + g_game_exe + "\"" + game_args() + " --hidden --frames 30 --volume 0 --log-file launcher_build.log", cwd);
  if (code != 0) log_line("The game exited with code %lu during the pipeline precompile; see launcher_build.log.", code);
  log_line("Done. Press Play.");
  PostMessageW(g_main, WM_APP_BUILD_DONE, code, 0);
}

void start_build() {
  if (g_building || g_iso.empty()) return;
  g_building = true;
  EnableWindow(g_build_btn, FALSE); EnableWindow(g_play_btn, FALSE);
  SetWindowTextW(g_log, L"");
  std::thread(build_thread).detach();
}

void start_game() {
  if (g_playing || g_iso.empty()) return;
  g_game_exe = game_exe();
  if (!file_exists(g_game_exe)) { TabCtrl_SetCurSel(g_tabs, 1); PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(ID_TABS, 0), 0); start_build(); return; }
  std::string cwd = work_dir();
  std::string cmd = "\"" + g_game_exe + "\"" + game_args();
  STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) { MessageBoxW(g_main, L"Could not start melee_port.exe", L"Melee Unlocked Launcher", MB_ICONERROR); return; }
  CloseHandle(pi.hThread);
  g_playing = true;
  EnableWindow(g_play_btn, FALSE);
  set_text(g_play_btn, "Running");
  ShowWindow(g_main, SW_MINIMIZE);
  std::thread([h = pi.hProcess] { WaitForSingleObject(h, INFINITE); CloseHandle(h); PostMessageW(g_main, WM_APP_GAME_DONE, 0, 0); }).detach();
}

void set_iso(const std::string& path) { g_iso = path; set_text(g_iso_edit, g_iso); EnableWindow(g_play_btn, !g_iso.empty() && !g_playing && !g_building); EnableWindow(g_build_btn, !g_iso.empty() && !g_building); }

void browse() {
  wchar_t file[MAX_PATH]{};
  OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_main; ofn.lpstrFilter = L"GameCube disc image (*.iso;*.gcm)\0*.iso;*.gcm\0All files\0*.*\0"; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
  if (GetOpenFileNameW(&ofn)) { set_iso(narrow(file)); save_ini(); }
}

void show_tab(int idx) {
  for (HWND h : g_play) if (h) ShowWindow(h, idx == 0 ? SW_SHOW : SW_HIDE);
  if (idx == 0 && !g_slippi_missing) ShowWindow(g_slippi_btn, SW_HIDE);
  for (HWND h : g_build) if (h) ShowWindow(h, idx == 1 ? SW_SHOW : SW_HIDE);
}

bool g_update_prompted = false;
void refresh_updater() {
  using host::updater::State;
  auto st = host::updater::state();
  // One yes/no prompt per launch when a newer release exists. Nothing installs without a Yes.
  if (st == State::UpdateAvailable && !g_update_prompted) {
    g_update_prompted = true;
    std::string text = "Melee Unlocked " + host::updater::latest_version() + " is available (you have " MELEE_PORT_VERSION ").\n\nUpdate now? The game folder is updated in place; settings, saves and replays are kept.\n\nNo keeps this version; the Update button stays on the Play page.";
    if (MessageBoxW(g_main, widen(text).c_str(), L"Melee Unlocked Launcher", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) host::updater::download_and_install();
    st = host::updater::state();
  }
  std::string line = "Version " MELEE_PORT_VERSION ". " + host::updater::message();
  if (st == State::Checking) line = "Version " MELEE_PORT_VERSION ". Checking for updates...";
  set_text(g_version_text, line);
  ShowWindow(g_update_btn, (st == State::UpdateAvailable || st == State::Failed) && TabCtrl_GetCurSel(g_tabs) == 0 ? SW_SHOW : SW_HIDE);
  set_text(g_update_btn, st == State::Failed ? "Retry" : "Update and restart");
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr, DWORD ex = 0) {
  HWND hw = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_main, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(hw, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
  return hw;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_main = hwnd;
      g_tabs = make(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, 8, 8, 604, 400, ID_TABS);
      TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = (LPWSTR)L"  Play  "; TabCtrl_InsertItem(g_tabs, 0, &it); it.pszText = (LPWSTR)L"  Build  "; TabCtrl_InsertItem(g_tabs, 1, &it);
      // Play page
      int i = 0;
      g_play[i++] = make(L"STATIC", L"Melee NTSC 1.02 ISO", 0, 24, 48, 300, 18, 0);
      g_play[i++] = g_iso_edit = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 24, 68, 480, 24, ID_ISO_EDIT);
      g_play[i++] = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 512, 67, 84, 26, ID_BROWSE);
      g_play[i++] = g_play_btn = make(L"BUTTON", L"PLAY", BS_PUSHBUTTON, 24, 110, 572, 64, ID_PLAY, g_font_big);
      g_play[i++] = g_slippi_text = make(L"STATIC", L"", 0, 24, 196, 420, 40, ID_SLIPPI_TEXT);
      g_play[i++] = g_slippi_btn = make(L"BUTTON", L"Get Slippi Launcher", BS_PUSHBUTTON, 452, 194, 144, 26, ID_SLIPPI_GET);
      g_play[i++] = g_version_text = make(L"STATIC", L"", 0, 24, 250, 420, 40, ID_VERSION_TEXT);
      g_play[i++] = g_update_btn = make(L"BUTTON", L"Update and restart", BS_PUSHBUTTON, 452, 248, 144, 26, ID_UPDATE);
      make(L"STATIC", L"In game: F1 or Z + Start opens the PC settings (fullscreen, frame rate, resolution, DLSS, anti-aliasing, widescreen, audio).\nA GameCube adapter is used automatically when it has the WinUSB driver (the Slippi Launcher installs it). Keyboard: arrows, IJKL, Z X C V, Enter, Q W E.",
           0, 24, 300, 572, 96, ID_HINT);
      g_play[7] = GetDlgItem(hwnd, ID_HINT);
      // Build page
      g_build[0] = make(L"STATIC", L"Drop your Melee NTSC 1.02 ISO here", SS_CENTER | SS_CENTERIMAGE | WS_BORDER, 24, 48, 572, 70, ID_DROP, g_font_big);
      g_drop = g_build[0];
      g_build[1] = make(L"STATIC", L"Checks the disc, builds the game from source when this is a checkout, precompiles the graphics pipelines for your GPU and remembers the path. The ISO itself is never copied.", 0, 24, 126, 572, 40, ID_BUILD_TEXT);
      g_build[2] = g_build_btn = make(L"BUTTON", L"Build", BS_PUSHBUTTON, 24, 170, 120, 28, ID_BUILD);
      g_build[3] = g_log = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 24, 206, 572, 190, ID_LOG, g_font_mono);
      show_tab(0);
      load_ini();
      set_iso(g_iso);
      if (!g_iso.empty()) save_ini();   // remember wherever it came from
      set_text(g_slippi_text, slippi_account_line());
      g_slippi_missing = slippi_account_line().rfind("Slippi account:", 0) != 0;
      ShowWindow(g_slippi_btn, g_slippi_missing ? SW_SHOW : SW_HIDE);
      host::updater::check(MELEE_PORT_VERSION);
      refresh_updater();
      SetTimer(hwnd, ID_TIMER, 500, nullptr);
      DragAcceptFiles(hwnd, TRUE);
      return 0;
    }
    case WM_NOTIFY:
      if (((LPNMHDR)lp)->idFrom == ID_TABS && ((LPNMHDR)lp)->code == TCN_SELCHANGE) { show_tab(TabCtrl_GetCurSel(g_tabs)); refresh_updater(); }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case ID_BROWSE: browse(); break;
        case ID_PLAY: start_game(); break;
        case ID_BUILD: start_build(); break;
        case ID_UPDATE:
          if (host::updater::state() == host::updater::State::Failed) host::updater::check(MELEE_PORT_VERSION);
          else host::updater::download_and_install();
          refresh_updater();
          break;
        case ID_SLIPPI_GET: ShellExecuteW(hwnd, L"open", L"https://slippi.gg/downloads", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_TABS: show_tab(TabCtrl_GetCurSel(g_tabs)); refresh_updater(); break;
      }
      return 0;
    case WM_DROPFILES: {
      wchar_t file[MAX_PATH]{};
      if (DragQueryFileW((HDROP)wp, 0, file, MAX_PATH)) { set_iso(narrow(file)); TabCtrl_SetCurSel(g_tabs, 1); show_tab(1); refresh_updater(); start_build(); }
      DragFinish((HDROP)wp);
      return 0;
    }
    case WM_APP_LOG: {
      auto* s = (std::string*)lp;
      int len = GetWindowTextLengthW(g_log);
      SendMessageW(g_log, EM_SETSEL, len, len);
      SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)widen(*s).c_str());
      delete s;
      return 0;
    }
    case WM_APP_BUILD_DONE:
      g_building = false;
      set_iso(g_iso);
      if (wp == 0) { TabCtrl_SetCurSel(g_tabs, 0); show_tab(0); refresh_updater(); }
      return 0;
    case WM_APP_GAME_DONE:
      g_playing = false;
      set_text(g_play_btn, "PLAY");
      set_iso(g_iso);
      ShowWindow(hwnd, SW_RESTORE);
      return 0;
    case WM_TIMER: refresh_updater(); return 0;
    case WM_CTLCOLORSTATIC: {
      HDC dc = (HDC)wp; SetBkMode(dc, TRANSPARENT);
      if ((HWND)lp == g_drop) { SetBkColor(dc, RGB(236, 240, 246)); static HBRUSH b = CreateSolidBrush(RGB(236, 240, 246)); return (LRESULT)b; }
      return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DESTROY: KillTimer(hwnd, ID_TIMER); host::updater::shutdown(); PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

// The updater expects these from the host runtime.
namespace host {
void log(const char* fmt, ...) { char buf[2048]; va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); OutputDebugStringA(buf); OutputDebugStringA("\n"); }
void request_exit(int) { PostMessageW(g_main, WM_CLOSE, 0, 0); }
}  // namespace host

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
  wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g_dir = narrow(exe); g_dir.resize(g_dir.find_last_of("\\/"));
  SetCurrentDirectoryW(widen(g_dir).c_str());
  g_dpi = (int)GetDpiForSystem();
  NONCLIENTMETRICSW ncm{sizeof ncm}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
  ncm.lfMessageFont.lfHeight = -S(12); g_font = CreateFontIndirectW(&ncm.lfMessageFont);
  ncm.lfMessageFont.lfHeight = -S(20); ncm.lfMessageFont.lfWeight = FW_BOLD; g_font_big = CreateFontIndirectW(&ncm.lfMessageFont);
  g_font_mono = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  WNDCLASSW wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = inst; wc.lpszClassName = L"MeleeUnlockedLauncher"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  RegisterClassW(&wc);
  RECT r{0, 0, S(620), S(416)}; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
  std::wstring title = widen(std::string("Melee Unlocked Launcher ") + MELEE_PORT_VERSION);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  ShowWindow(hwnd, show);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) { if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
  return 0;
}
