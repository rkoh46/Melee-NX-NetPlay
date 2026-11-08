// See switch_threads.c.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Point Clang's TLS register at libnx's thread base. Required at the start of
// any thread not created through pthread (libnx threadCreate).
void switch_sync_clang_tls(void);
// Runs once from exit/abort/_exit before svcExitProcess; flush logs here.
typedef void (*switch_exit_hook_fn)(void);
void switch_set_exit_hook(switch_exit_hook_fn hook);
#ifdef __cplusplus
}
#endif
