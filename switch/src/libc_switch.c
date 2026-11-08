// Small libc bridges for libraries that assume a process model. Horizon
// homebrew cannot spawn processes: Dear ImGui's default "open in shell"
// helper (built into Aurora's ImGui pass) links waitpid/execvp and never
// succeeds here.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

pid_t waitpid(pid_t pid, int* status, int options) {
    (void)pid; (void)status; (void)options;
    errno = ENOSYS;
    return -1;
}

int execvp(const char* file, char* const argv[]) {
    (void)file; (void)argv;
    errno = ENOSYS;
    return -1;
}
