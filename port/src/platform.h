/* The few things that differ between Linux, macOS and Windows: reserving EE RAM,
 * page protection with a write-fault hook (2-player split screen), coroutines (IPU),
 * CPU count and start-up tweaks. Everything else is plain C11 + pthreads + SDL2. */
#pragma once
#include <stddef.h>

void *plat_reserve(size_t size);                    /* zeroed, read/write; NULL on failure */
void plat_protect(void *p, size_t size, int writable);
/* handler(addr) is called on a write fault; it returns 1 if it dealt with it (and made
 * the page writable), 0 to pass the fault on */
void plat_set_write_fault_handler(int (*handler)(void *addr));
int plat_ncpu(void);
void plat_init(void);                               /* timer resolution, console output */
const char *plat_default_game_dir(void);            /* "game" next to the executable, if present */
void plat_fatal_box(const char *msg);               /* a message box where there is no console */

typedef struct Coro Coro;
Coro *coro_new(void (*fn)(void), size_t stack);     /* fn must never return */
void coro_free(Coro *c);
void coro_resume(Coro *c);                          /* run c until it yields */
void coro_yield(Coro *c);                           /* from inside c: back to the resumer */
