// Clang/libnx thread glue. Clang reads TLS through tpidr_el0 while libnx keeps
// its base behind __aarch64_read_tp(); the two are synchronized before static
// constructors and at every worker's entry. Threads created through pthread
// (std::thread) get an 8 MiB stack, and detached threads are reaped by a joiner
// because libnx's pthread_detach returns ENOSYS. exit/abort/_exit skip static
// destruction and service teardown, which would invalidate handles workers may
// still hold, and stop the process after the registered flush hook has run.
//
// Adapted from Melee-NX's clang_tls_switch.c (KawaiiBunga, GPL-3.0-or-later),
// itself adapted from KartPad-NX. Link with
//   -Wl,--wrap=pthread_create -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_join
//   -Wl,--wrap=exit -Wl,--wrap=abort -Wl,--wrap=_exit
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include <unistd.h>

#include "switch_threads.h"

#define MIN_THREAD_STACK ((size_t)8 * 1024 * 1024)

extern void* __aarch64_read_tp(void);

void switch_sync_clang_tls(void) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(__aarch64_read_tp()) : "memory");
}

__attribute__((constructor(101))) static void sync_clang_tls_main(void) { switch_sync_clang_tls(); }

extern int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern int __real_pthread_join(pthread_t, void**);

static int threads_created;
static size_t stack_bytes;

typedef struct ThreadRec {
    pthread_t id;
    void* stack_buf;
    int finished;
    int detached;
    struct ThreadRec* next;
    struct ThreadRec* reap_next;
} ThreadRec;

static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reap_cond = PTHREAD_COND_INITIALIZER;
static ThreadRec* registry;
static ThreadRec* reap_head;
static ThreadRec* reap_tail;
static int reaper_running;

static ThreadRec* find_locked(pthread_t id) {
    for (ThreadRec* r = registry; r != NULL; r = r->next)
        if (pthread_equal(r->id, id)) return r;
    return NULL;
}

static void unlink_locked(ThreadRec* rec) {
    ThreadRec** link = &registry;
    while (*link != NULL) {
        if (*link == rec) { *link = rec->next; return; }
        link = &(*link)->next;
    }
}

static void push_reap_locked(ThreadRec* rec) {
    rec->reap_next = NULL;
    if (reap_tail) reap_tail->reap_next = rec; else reap_head = rec;
    reap_tail = rec;
    pthread_cond_signal(&reap_cond);
}

static void* reaper_main(void* unused) {
    (void)unused;
    switch_sync_clang_tls();
    for (;;) {
        pthread_mutex_lock(&thread_mutex);
        while (reap_head == NULL) pthread_cond_wait(&reap_cond, &thread_mutex);
        ThreadRec* rec = reap_head;
        reap_head = rec->reap_next;
        if (reap_head == NULL) reap_tail = NULL;
        unlink_locked(rec);
        pthread_mutex_unlock(&thread_mutex);
        __real_pthread_join(rec->id, NULL);
        free(rec->stack_buf);
        free(rec);
    }
    return NULL;
}

int __wrap_pthread_detach(pthread_t thread) {
    pthread_mutex_lock(&thread_mutex);
    if (!reaper_running) {
        pthread_t reaper;
        if (__real_pthread_create(&reaper, NULL, reaper_main, NULL) == 0) reaper_running = 1;
    }
    ThreadRec* rec = find_locked(thread);
    int rc = 0;
    if (rec == NULL) rc = ESRCH;
    else if (rec->detached) rc = EINVAL;
    else {
        rec->detached = 1;
        if (rec->finished) push_reap_locked(rec);
    }
    pthread_mutex_unlock(&thread_mutex);
    return rc;
}

int __wrap_pthread_join(pthread_t thread, void** retval) {
    int rc = __real_pthread_join(thread, retval);
    pthread_mutex_lock(&thread_mutex);
    ThreadRec* rec = find_locked(thread);
    if (rec != NULL && rec->detached) rec = NULL;
    if (rec != NULL) unlink_locked(rec);
    pthread_mutex_unlock(&thread_mutex);
    if (rec != NULL) { free(rec->stack_buf); free(rec); }
    return rc;
}

typedef struct {
    void* (*start)(void*);
    void* arg;
    ThreadRec* rec;
} TrampolineCtx;

// Cores the wrapped pthreads may run on (0 = the kernel's default placement). The game thread
// owns core 2 at the highest application priority; a helper thread placed there by the
// scheduler waits behind it for seconds, so the renderer's helpers are confined to cores 0-1.
static uint64_t pthread_core_mask = 0;
static int pthread_core_preferred = -1;
void switch_set_pthread_cores(int preferred, uint64_t mask) {
    pthread_core_preferred = preferred;
    pthread_core_mask = mask;
}

static void* thread_trampoline(void* raw) {
    switch_sync_clang_tls();
    if (pthread_core_mask) svcSetThreadCoreMask(CUR_THREAD_HANDLE, pthread_core_preferred, pthread_core_mask);
    TrampolineCtx ctx = *(TrampolineCtx*)raw;
    free(raw);
    void* result = ctx.start(ctx.arg);
    if (ctx.rec != NULL) {
        pthread_mutex_lock(&thread_mutex);
        ctx.rec->finished = 1;
        if (ctx.rec->detached) push_reap_locked(ctx.rec);
        pthread_mutex_unlock(&thread_mutex);
    }
    return result;
}

static switch_exit_hook_fn exit_hook;
void switch_set_exit_hook(switch_exit_hook_fn hook) { exit_hook = hook; }

__attribute__((noreturn)) static void fast_exit(void) {
    switch_exit_hook_fn hook = exit_hook;
    exit_hook = NULL;   // a crash inside the hook must not run it again
    if (hook) hook();
    fflush(NULL);
    fsync(STDOUT_FILENO);
    fsync(STDERR_FILENO);
    svcExitProcess();
    __builtin_unreachable();
}

// A normal exit must go back through libnx so nx-hbloader regains control and returns to the
// Homebrew Menu; svcExitProcess from an NRO ends hbloader itself, which the system reports as
// "The software was closed because an error occurred". abort/_exit keep the fast path.
// exit() (Quit to HOME Menu, or the end of a measured run) asks the system to close the
// whole application, as a game quitting to the HOME menu does: the process ends with every thread
// in it, and no error is reported. Two earlier routes failed: svcExitProcess from the NRO is
// reported as "The software was closed because an error occurred", and returning to hbloader
// left the renderer, audio and writer threads running inside the same process, corrupting the
// heap of whatever hbloader loaded next (every second launch crashed in malloc at startup).
__attribute__((noreturn)) void __wrap_exit(int code) {
    (void)code;
    switch_exit_hook_fn hook = exit_hook;
    exit_hook = NULL;
    if (hook) hook();
    fflush(NULL);
    // An application (launched over a game title) closes with ISelfController::Exit, the call a
    // retail game makes; a library applet (launched from the Album) with RequestExitToSelf. Either
    // way the system ends the process with all its threads and returns to the HOME menu cleanly.
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    if (appletGetAppletType() == AppletType_Application) {
        appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
        rc = serviceDispatch(appletGetServiceSession_SelfController(), 0);
    } else {
        rc = appletRequestExitToSelf();
    }
    if (R_SUCCEEDED(rc)) {
        for (;;) svcSleepThread(1000000000LL);   // the system terminates the process
    }
    svcExitProcess();
    __builtin_unreachable();
}
__attribute__((noreturn)) void __wrap__exit(int code) { (void)code; fast_exit(); }
__attribute__((noreturn)) void __wrap_abort(void) { fast_exit(); }

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    TrampolineCtx* ctx = (TrampolineCtx*)malloc(sizeof(*ctx));
    if (!ctx) return EAGAIN;
    ThreadRec* rec = (ThreadRec*)calloc(1, sizeof(*rec));
    if (!rec) { free(ctx); return EAGAIN; }
    ctx->start = start; ctx->arg = arg; ctx->rec = rec;

    const size_t want_size = MIN_THREAD_STACK;
    int honour_attr = 0;
    if (attr != NULL) {
        size_t current = 0;
        pthread_attr_getstacksize(attr, &current);
        honour_attr = (current >= MIN_THREAD_STACK);
    }
    pthread_attr_t local_attr;
    const pthread_attr_t* use_attr = attr;
    if (!honour_attr) {
        void* stack_buf = memalign(4096, want_size);
        if (!stack_buf) {
            fprintf(stderr, "pthread_create: memalign(%zu KiB) failed after %d threads (%zu MiB of stacks)\n",
                    want_size / 1024, threads_created, stack_bytes / (1024 * 1024));
            free(rec); free(ctx);
            return EAGAIN;
        }
        rec->stack_buf = stack_buf;
        if (pthread_attr_init(&local_attr) != 0) { free(stack_buf); free(rec); free(ctx); return EAGAIN; }
        pthread_attr_setstack(&local_attr, stack_buf, want_size);
        use_attr = &local_attr;
    }
    pthread_mutex_lock(&thread_mutex);
    int rc = __real_pthread_create(thread, use_attr, thread_trampoline, ctx);
    if (rc == 0) {
        rec->id = *thread;
        rec->next = registry;
        registry = rec;
        threads_created++;
        stack_bytes += rec->stack_buf ? want_size : 0;
    }
    pthread_mutex_unlock(&thread_mutex);
    if (!honour_attr) pthread_attr_destroy(&local_attr);
    if (rc != 0) {
        fprintf(stderr, "pthread_create: rc=%d after %d threads (%zu MiB of stacks)\n", rc, threads_created,
                stack_bytes / (1024 * 1024));
        free(rec->stack_buf); free(rec); free(ctx);
    }
    return rc;
}

// Pins the calling thread to one core at the given priority (the GX worker uses core 0, at the
// simulation thread's priority so Aurora's floating workers cannot starve it).
void switch_pin_current_thread(int core, int priority) {
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ull << core);
    svcSetThreadPriority(CUR_THREAD_HANDLE, priority);
}

// Running time of the calling thread on every core so far, in ms (13.0.0+; older systems: the
// deprecated info type). Used by the frame log to tell work from preemption.
double switch_thread_cpu_ms(void) {
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX)) &&
        R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, UINT64_MAX)))
        return 0.0;
    return (double)ticks * 1000.0 / 19200000.0;
}

// Sets the calling thread's priority (higher numbers run later). Background cleanup threads.
void switch_set_current_thread_priority(int priority) {
    svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)priority);
}
