// Slippi sign-in screen for the Switch, drawn through Aurora's ImGui pass
// before the game boots. Email and password come from the console's software
// keyboard; the Firebase and Slippi calls are slippi_login's. Writes user.json
// (connect code, play key) and session.json (refresh token) into `user_dir`.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>

namespace host {
class MetalFrontend;
enum class SignInResult { SignedIn, PlayOffline, Quit };
// Blocks on the calling (simulation) thread until the player signs in, chooses
// to play offline, or closes the app. `frontend` must be attached and running.
SignInResult switch_signin_run(MetalFrontend& frontend, const std::string& user_dir, std::string& account_name,
                               std::string& connect_code);
// A full-screen notice before the game boots (a newer Slippi release than this build implements):
// `headline` in the yellow header, `lines` below it. Blocks until A (continue) or + (quit); returns
// false when the player quits.
enum class NoticeChoice { A, B, Quit };
// The same screen as a question: `hint` names the buttons; B is only a choice when `allow_b`.
NoticeChoice switch_prompt_run(MetalFrontend& frontend, const std::string& headline, const std::vector<std::string>& lines,
                               const char* hint, bool allow_b);
// One frame of the same screen with a progress bar (0..1); call it in a loop while work runs on
// another thread. False when the app is being closed.
bool switch_progress_frame(MetalFrontend& frontend, const std::string& headline, const std::vector<std::string>& lines, float fraction);
bool switch_notice_run(MetalFrontend& frontend, const std::string& headline, const std::vector<std::string>& lines);
}  // namespace host
