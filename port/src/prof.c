/* Built-in sampling profiler (AE2_PROF=1): SIGPROF every 1 ms of CPU time,
 * records the interrupted PC; on exit prints the hottest functions. */
#if defined(__linux__) && defined(__x86_64__)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <stdint.h>

#define NS (1 << 22)
static uintptr_t *samples;
static volatile int nsamp;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    int i = __sync_fetch_and_add(&nsamp, 1);
    if (i < NS) samples[i] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
}

typedef struct { const char *name; int n; } Ent;
static int cmp(const void *a, const void *b) { return ((const Ent *)b)->n - ((const Ent *)a)->n; }

void prof_report(void) {
    static int done;
    if (!samples || done++) return;
    struct itimerval it = {0}; setitimer(ITIMER_PROF, &it, NULL);
    int n = nsamp < NS ? nsamp : NS;
    Ent *e = calloc(65536, sizeof(Ent)); int ne = 0;
    for (int i = 0; i < n; i++) {
        Dl_info di; const char *nm = "?";
        if (dladdr((void *)samples[i], &di) && di.dli_sname) nm = di.dli_sname;
        int j; for (j = 0; j < ne && e[j].name != nm && strcmp(e[j].name, nm); j++);
        if (j == ne && ne < 65536) { e[ne].name = nm; e[ne].n = 0; ne++; }
        if (j < 65536) e[j].n++;
    }
    qsort(e, ne, sizeof(Ent), cmp);
    int gen = 0; for (int j = 0; j < ne; j++) if (!strncmp(e[j].name, "f_", 2)) gen += e[j].n;
    fprintf(stderr, "[prof] %d samples; recompiled EE code total %.1f%%\n", n, 100.0 * gen / (n ? n : 1));
    for (int j = 0; j < ne && j < 40; j++) fprintf(stderr, "[prof] %6.2f%% %s\n", 100.0 * e[j].n / n, e[j].name);
    /* raw offsets of unresolved samples for offline symbolisation (nm) */
    const char *rf = getenv("AE2_PROF_RAW");
    if (rf) {
        FILE *f = fopen(rf, "w");
        Dl_info di; dladdr((void *)prof_report, &di);
        for (int i = 0; f && i < n; i++) fprintf(f, "%lx\n", (unsigned long)(samples[i] - (uintptr_t)di.dli_fbase));
        if (f) fclose(f);
    }
}

void prof_init(void) {
    if (!getenv("AE2_PROF")) return;
    samples = malloc(sizeof(uintptr_t) * NS);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    atexit(prof_report);   /* closing the window ends with exit() */
}
#else
/* the sampling profiler reads the interrupted PC from an x86-64 Linux ucontext: elsewhere it is off */
#include <stdio.h>
#include <stdlib.h>
void prof_report(void) {}
void prof_init(void) { if (getenv("AE2_PROF")) fprintf(stderr, "[prof] AE2_PROF is only available on x86-64 Linux\n"); }
#endif
