/* Platform layer: see platform.h. */
#if defined(__APPLE__)
#define _XOPEN_SOURCE 700   /* ucontext */
#define _DARWIN_C_SOURCE
#endif
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
/* ------------------------------------------------------------------ Windows */
#include <windows.h>
#include <io.h>

void *plat_reserve(size_t size) {
    /* committed pages are zero-filled on first touch, like MAP_NORESERVE memory */
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void plat_protect(void *p, size_t size, int writable) {
    DWORD old; VirtualProtect(p, size, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
}
static int (*fault_handler)(void *);
static LONG CALLBACK veh(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1
        && fault_handler && fault_handler((void *)r->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
void plat_set_write_fault_handler(int (*handler)(void *)) {
    if (!fault_handler) AddVectoredExceptionHandler(1, veh);
    fault_handler = handler;
}
int plat_ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
void plat_init(void) {
    timeBeginPeriod(1);   /* 1 ms sleeps for frame pacing (the default is 15.6 ms) */
    SetConsoleOutputCP(CP_UTF8);
}
static int has_console(void) { DWORD pids[2]; return GetConsoleProcessList(pids, 2) > 1; }
void plat_fatal_box(const char *msg) {
    if (!has_console()) MessageBoxA(NULL, msg, "Ape Escape 2", MB_OK | MB_ICONERROR);
}

struct Coro { LPVOID fiber, back; void (*fn)(void); };
static __thread LPVOID thread_fiber;
static VOID CALLBACK fiber_entry(LPVOID arg) { ((Coro *)arg)->fn(); }
Coro *coro_new(void (*fn)(void), size_t stack) {
    Coro *c = calloc(1, sizeof *c); c->fn = fn;
    c->fiber = CreateFiber(stack, fiber_entry, c);
    return c;
}
void coro_free(Coro *c) { if (c) { DeleteFiber(c->fiber); free(c); } }
void coro_resume(Coro *c) {
    /* whichever thread resumes the coroutine gets it back when it yields */
    if (!thread_fiber) thread_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    c->back = thread_fiber;
    SwitchToFiber(c->fiber);
}
void coro_yield(Coro *c) { SwitchToFiber(c->back); }

#else
/* ------------------------------------------------------------------ Linux, macOS */
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
void *plat_reserve(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void plat_protect(void *p, size_t size, int writable) {
    mprotect(p, size, writable ? PROT_READ | PROT_WRITE : PROT_READ);
}
static int (*fault_handler)(void *);
static struct sigaction old_segv, old_bus;
static void on_fault(int sig, siginfo_t *si, void *uc) {
    if (fault_handler && fault_handler(si->si_addr)) return;
    /* not ours: the default action (or whatever handler was there) */
    struct sigaction *o = sig == SIGBUS ? &old_bus : &old_segv;
    if (o->sa_flags & SA_SIGINFO) { if (o->sa_sigaction) { o->sa_sigaction(sig, si, uc); return; } }
    else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) { o->sa_handler(sig); return; }
    signal(sig, SIG_DFL);
}
void plat_set_write_fault_handler(int (*handler)(void *)) {
    if (!fault_handler) {
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &old_segv);
#ifdef __APPLE__
        sigaction(SIGBUS, &sa, &old_bus);   /* macOS reports writes to read-only pages as SIGBUS */
#endif
    }
    fault_handler = handler;
}
int plat_ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
void plat_init(void) {}
void plat_fatal_box(const char *msg) { (void)msg; }

struct Coro { ucontext_t ctx, back; char *stack; };
Coro *coro_new(void (*fn)(void), size_t stack) {
    Coro *c = calloc(1, sizeof *c);
    c->stack = malloc(stack);
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp = c->stack; c->ctx.uc_stack.ss_size = stack; c->ctx.uc_link = NULL;
    makecontext(&c->ctx, fn, 0);
    return c;
}
void coro_free(Coro *c) { if (c) { free(c->stack); free(c); } }
void coro_resume(Coro *c) { swapcontext(&c->back, &c->ctx); }
void coro_yield(Coro *c) { swapcontext(&c->ctx, &c->back); }
#endif

const char *plat_default_game_dir(void) {
    static char path[2048];
    char *base = SDL_GetBasePath();
    if (!base) return NULL;
    snprintf(path, sizeof path, "%sgame", base);
    SDL_free(base);
    char probe[2100]; snprintf(probe, sizeof probe, "%s/SYSTEM.CNF", path);
    FILE *f = fopen(probe, "rb");
    if (!f) return NULL;
    fclose(f);
    return path;
}
