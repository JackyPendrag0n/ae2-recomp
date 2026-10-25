/* HLE of the EE kernel: threads, semaphores, interrupt handlers, alarms, syscalls.
 *
 * Each EE thread runs on its own host pthread. A global mutex (g_big) is the
 * "CPU": exactly one EE thread holds it and executes at a time. Context
 * switches happen only at syscalls and at polling points (backward branches,
 * hardware register reads), mirroring the EE's single core.
 */
#include "rt.h"
#include <time.h>
#include <unistd.h>
#include <errno.h>

pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;
volatile int g_quit;
uint32_t g_frame;
int g_in_intr;

#define MAX_THREADS 256
#define MAX_SEMAS 256
#define MAX_HANDLERS 64
#define MAX_ALARMS 64

enum { TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_WAITSUSPEND = 12, TS_DORMANT = 16 };
enum { TW_NONE = 0, TW_SLEEP = 1, TW_SEMA = 2 };

typedef struct {
    int used, status, prio, init_prio;
    uint32_t entry, stack, stack_size, gp, attr, option;
    int wait_type, wait_id, wakeup_count, suspend_count;
    int wait_result;
    uint64_t seq;
    Ctx *ctx;
    pthread_t th;
    pthread_cond_t cv;
    int gen;
    int host_started;
} Thread;

typedef struct { int used, count, max, init; uint32_t attr, option; int waiters; } Sema;
typedef struct { int used, kind, cause, enabled; uint32_t fn, arg, gp; } Handler;  /* kind 0 intc 1 dmac */
typedef struct { int used; uint64_t due; uint32_t fn, arg, gp, ticks; } Alarm;

static Thread threads[MAX_THREADS];
static Sema semas[MAX_SEMAS];
static Handler handlers[MAX_HANDLERS];
static Alarm alarms[MAX_ALARMS];
static int g_cur = -1;
static uint64_t seq_ctr = 1;
static uint32_t intc_mask, dmac_mask;
static uint32_t pending_intc, pending_dmac;
static uint64_t next_vblank_us;
static uint32_t vsync_flag_addr, vsync_csr_addr;
static uint32_t gs_imr = 0x7f00;
static uint32_t heap_start, heap_end;
static int dispatch_disabled;

#define INTR_STACK_TOP 0x000FFF00u
/* vblank period: PAL 50 Hz, or 60 Hz when chosen in Options > Graphics (the game
 * logic then runs 20% faster, like the NTSC versions); FMVs always use 50 Hz, the
 * rate their pictures and sound are timed for */
uint32_t g_vblank_us = 20000u;
extern uint32_t g_fmv_vsync;
static inline uint32_t vblank_period(void) {
    if (g_vblank_us == 20000u || (g_fmv_vsync && g_frame - g_fmv_vsync < 8)) return 20000u;
    return g_vblank_us;
}
#define VBLANK_US vblank_period()
#define HSYNC_US 64u

uint32_t kernel_gs_imr(void) { return gs_imr; }

/* AE2_TURBO=1 (testing): when the guest is idle, skip the clock forward to the next
 * event instead of sleeping, so headless runs go as fast as the host allows */
static int turbo = -1;
static uint64_t time_skew;
/* AE2_DETERMINISTIC=1 (testing): the clock is virtual. It advances by a fixed step per
 * scheduler poll of the EE code and jumps over idle time, and audio is mixed on the
 * EE thread, so a scripted run plays out identically every time. */
int g_det = -1;
static uint64_t vtime = 1000000, det_step = 10;
static inline int det_on(void) {
    if (g_det < 0) {
        const char *e = getenv("AE2_DETERMINISTIC");
        g_det = e != NULL || getenv("AE2_RECORD") || getenv("AE2_REPLAY");
        if (e && atoi(e) > 1) det_step = (uint64_t)atoi(e);
    }
    return g_det;
}
int rt_deterministic(void) { return det_on(); }
/* deterministic mode with a window: keep the virtual clock from running ahead of
 * real time, so the game plays at its normal speed */
static void det_pace(void) {
    static uint64_t wall0, v0;
    extern int g_headless;
    if (g_headless || turbo) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t wall = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000, v = vtime;
    if (!wall0) { wall0 = wall; v0 = v; return; }
    uint64_t target = wall0 + (v - v0);
    if (wall > target + 100000) { wall0 = wall - (v - v0); return; }   /* host fell behind: don't race to catch up */
    if (target > wall) {
        pthread_mutex_unlock(&g_big);
        usleep((useconds_t)(target - wall));
        pthread_mutex_lock(&g_big);
    }
}
uint64_t host_usec(void) {
    if (det_on()) return __atomic_load_n(&vtime, __ATOMIC_RELAXED);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000 + __atomic_load_n(&time_skew, __ATOMIC_RELAXED);
}

Ctx *ctx_new(void) {
    Ctx *c = aligned_alloc(64, (sizeof(Ctx) + 63) & ~(size_t)63);   /* C11 (and macOS) want a multiple of the alignment */
    memset(c, 0, sizeof(Ctx));
    c->vf[0].f[3] = 1.0f;
    c->budget = 1000;
    c->fcr31 = 0x01000001;
    c->cop0_status = 0x70030c13;
    return c;
}

/* ------------------------------------------------------------ scheduling */
static int pick_next(void) {
    int best = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (!t->used || !(t->status == TS_RUN || t->status == TS_READY)) continue;
        if (best < 0) { best = i; continue; }
        Thread *b = &threads[best];
        if (t->prio < b->prio) best = i;
        else if (t->prio == b->prio) {
            uint64_t ts = (t->status == TS_RUN) ? 0 : t->seq;
            uint64_t bs = (b->status == TS_RUN) ? 0 : b->seq;
            if (ts < bs) best = i;
        }
    }
    return best;
}

static void *thread_host_main(void *arg);

static void start_host_thread(Thread *t) {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    t->host_started = 1;
    if (pthread_create(&t->th, &at, thread_host_main, (void *)(intptr_t)(t - threads)))
        rt_fatal("pthread_create failed");
    pthread_detach(t->th);
    pthread_attr_destroy(&at);
}

static void idle_wait(void);

/* Wait (holding g_big) until this host thread's EE thread is scheduled again. */
static void wait_until_scheduled(int me, int gen) {
    Thread *t = &threads[me];
    while (g_cur != me || t->gen != gen) {
        if (t->gen != gen) { pthread_mutex_unlock(&g_big); pthread_exit(NULL); }
        pthread_cond_wait(&t->cv, &g_big);
    }
}

/* Hand the CPU to `next` (which must be runnable). */
static void switch_to(int next, int me_runnable) {
    int me = g_cur;
    if (next == me) { threads[me].status = TS_RUN; return; }
    { extern int g_ghost, g_ghost_switched; if (g_ghost && g_ghost_switched++ < 5) { WARN("[p2] thread switch %d -> %d during the second-view pass\n", me, next); } }
    if (me >= 0 && me_runnable && threads[me].status == TS_RUN) threads[me].status = TS_READY;
    g_cur = next;
    threads[next].status = TS_RUN;
    if (!threads[next].host_started) start_host_thread(&threads[next]);
    else pthread_cond_signal(&threads[next].cv);
}

/* Re-evaluate who should run. Called by the running thread at safe points. */
static void reschedule(void) {
    if (g_in_intr || dispatch_disabled) return;
    int me = g_cur;
    int gen = threads[me].gen;
    for (;;) {
        int next = pick_next();
        if (next < 0) { idle_wait(); continue; }
        if (next == me) { threads[me].status = TS_RUN; return; }
        switch_to(next, 1);
        wait_until_scheduled(me, gen);
        /* we may have been woken to run */
        if (threads[me].status == TS_RUN) return;
    }
}

/* Current thread blocks (status already set to WAIT) until woken. */
static void block_current(void) {
    int me = g_cur;
    int gen = threads[me].gen;
    for (;;) {
        int next = pick_next();
        if (next < 0) { idle_wait(); continue; }
        if (next == me) { threads[me].status = TS_RUN; return; }
        switch_to(next, 0);
        wait_until_scheduled(me, gen);
        if (threads[me].status == TS_RUN) return;
    }
}

static void exit_current(void) __attribute__((noreturn));
static void exit_current(void) {
    int me = g_cur;
    Thread *t = &threads[me];
    t->status = TS_DORMANT;
    t->host_started = 0;
    t->gen++;
    for (;;) {
        int next = pick_next();
        if (next < 0) { idle_wait(); continue; }
        g_cur = next;
        threads[next].status = TS_RUN;
        if (!threads[next].host_started) start_host_thread(&threads[next]);
        else pthread_cond_signal(&threads[next].cv);
        break;
    }
    pthread_mutex_unlock(&g_big);
    pthread_exit(NULL);
}

/* The EE FPU and VUs round toward zero and flush denormals (PCSX2's default
 * "chop" mode). Applied to every host thread that runs guest code.
 * AE2_FPU_NEAREST=1 restores the host's round-to-nearest for comparison (denormals are
 * still flushed: the VU clamps in vu_impl.h and vu0.h rely on it). */
#include <xmmintrin.h>
static void guest_fpu_mode(void) {
    static int nearest = -1;
    if (nearest < 0) nearest = getenv("AE2_FPU_NEAREST") != NULL;
    _mm_setcsr(0x1f80 | (nearest ? 0 : 0x6000) /* RC=toward zero */ | 0x8000 /* FTZ */ | 0x0040 /* DAZ */);
#if defined(__aarch64__) || defined(__arm64__)
    /* sse2neon's _mm_setcsr sets only the rounding mode: flush-to-zero (ARM FZ, inputs and outputs) separately */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
}
/* for host threads that run guest work only part of the time (the GS thread runs VU1):
 * switch to the guest mode and back */
uint64_t rt_fpu_enter_guest(void) {
    uint64_t saved;
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
#else
    saved = _mm_getcsr();
#endif
    guest_fpu_mode();
    return saved;
}
void rt_fpu_leave_guest(uint64_t saved) {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(saved));
#else
    _mm_setcsr((unsigned)saved);
#endif
}

static void *thread_host_main(void *arg) {
    guest_fpu_mode();
    int id = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_big);
    Thread *t = &threads[id];
    int gen = t->gen;
    wait_until_scheduled(id, gen);
    rt_call(t->ctx, t->entry);
    INFO("[kernel] thread %d returned from entry\n", id);
    exit_current();
}

static void make_ready(int id) {
    Thread *t = &threads[id];
    t->wait_type = TW_NONE;
    if (t->status == TS_WAIT) t->status = TS_READY;
    else if (t->status == TS_WAITSUSPEND) t->status = TS_SUSPEND;
    t->seq = seq_ctr++;
}

/* ------------------------------------------------------------ interrupts */
static Ctx *intr_ctx[8];

static void run_guest_handler(uint32_t fn, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2) {
    int depth = g_in_intr;
    if (depth >= 8) return;
    if (!intr_ctx[depth]) intr_ctx[depth] = ctx_new();
    Ctx *ic = intr_ctx[depth];
    g_in_intr++;

    ic->r[4].sd[0] = (int32_t)a0; ic->r[5].sd[0] = (int32_t)a1; ic->r[6].sd[0] = (int32_t)a2;
    ic->r[28].sd[0] = (int32_t)gp;
    ic->r[29].sd[0] = (int32_t)(INTR_STACK_TOP - depth * 0x4000);
    ic->r[31].sd[0] = 0;
    ic->tid = g_cur;
    ic->cop0_status = 0x70030c12;
    rt_call(ic, fn);
    g_in_intr--;
}

void kernel_raise_intc(int cause) { __atomic_or_fetch(&pending_intc, 1u << cause, __ATOMIC_SEQ_CST); }
void kernel_raise_dmac(int ch) { pending_dmac |= 1u << ch; }

void kernel_deliver_pending(void) {
    if (g_in_intr) return;
    /* p2.c's second-view pass: interrupts wait until it is over (their handlers would
     * write to the pass's memory, which is thrown away, and their signals would be lost) */
    { extern int g_ghost; if (g_ghost) return; }
    for (int guard = 0; guard < 16 && (pending_intc | pending_dmac); guard++) {
        uint32_t pi = __atomic_load_n(&pending_intc, __ATOMIC_SEQ_CST) & intc_mask; __atomic_and_fetch(&pending_intc, ~pi, __ATOMIC_SEQ_CST);
        uint32_t pd = pending_dmac & dmac_mask; pending_dmac &= ~pd;
        if (!pi && !pd) break;
        for (int cause = 0; cause < 15; cause++) {
            if (!(pi & (1u << cause))) continue;
            for (int h = 0; h < MAX_HANDLERS; h++)
                if (handlers[h].used && handlers[h].kind == 0 && handlers[h].cause == cause && handlers[h].enabled)
                    run_guest_handler(handlers[h].fn, handlers[h].gp, cause, handlers[h].arg, 0);
        }
        for (int ch = 0; ch < 16; ch++) {
            if (!(pd & (1u << ch))) continue;
            for (int h = 0; h < MAX_HANDLERS; h++)
                if (handlers[h].used && handlers[h].kind == 1 && handlers[h].cause == ch && handlers[h].enabled)
                    run_guest_handler(handlers[h].fn, handlers[h].gp, ch, handlers[h].arg, 0);
        }
        sif_poll();
    }
}

static void watch_dump(void) {
    static int init; static uint32_t addrs[16]; static int n;
    if (!init) { init = 1; const char *e = getenv("AE2_WATCH");
        while (e && *e && n < 16) { addrs[n++] = (uint32_t)strtoul(e, (char **)&e, 16); while (*e == ',') e++; } }
    if (!n || (g_frame % 25)) return;
    fprintf(stderr, "[watch] frame %u:", g_frame);
    for (int i = 0; i < n; i++) fprintf(stderr, " %08x=%08x", addrs[i], MR32(addrs[i]));
    fprintf(stderr, "\n");
}
static void do_vblank(void) {
    g_frame++;
    watch_dump();
    hw_vblank(1);
    if (vsync_flag_addr) MW32(vsync_flag_addr, 1);
    if (vsync_csr_addr) MW64(vsync_csr_addr, gs_read_priv64(0x12001000));
    kernel_raise_intc(2);
    kernel_deliver_pending();
    gs_vsync();
    hw_vblank(0);
    kernel_raise_intc(3);
    kernel_deliver_pending();
}

void spu2_pump(void);
void sched_check_timers(void) {
    uint64_t now = host_usec();
    if (g_det > 0) spu2_pump();
    if (!next_vblank_us) next_vblank_us = now + VBLANK_US;
    if (now >= next_vblank_us) {
        next_vblank_us += VBLANK_US;
        if (now > next_vblank_us + 5 * VBLANK_US) next_vblank_us = now + VBLANK_US;
        do_vblank();
    }
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (alarms[i].used && now >= alarms[i].due) {
            Alarm a = alarms[i];
            alarms[i].used = 0;
            uint32_t r;
            g_in_intr++; g_in_intr--;  /* keep symmetry */
            run_guest_handler(a.fn, a.gp, i + 1, a.ticks, a.arg);
            r = (uint32_t)intr_ctx[0]->r[2].uw[0];
            (void)r;
        }
    }
    sif_poll();
    kernel_deliver_pending();
}

uint64_t g_idle_us;   /* wall time the guest spent with nothing to run (frame pacing) */
static void idle_wait(void) {
    /* No runnable thread: sleep until the next timed event and fire it. */
    uint64_t now = host_usec();
    uint64_t due = next_vblank_us ? next_vblank_us : now + VBLANK_US;
    for (int i = 0; i < MAX_ALARMS; i++) if (alarms[i].used && alarms[i].due < due) due = alarms[i].due;
    if (turbo < 0) turbo = getenv("AE2_TURBO") != NULL;
    if (turbo < 0) turbo = getenv("AE2_TURBO") != NULL;
    if (det_on() && due > now) { __atomic_store_n(&vtime, due, __ATOMIC_RELAXED); now = due; det_pace(); }
    if (turbo && due > now) { __atomic_add_fetch(&time_skew, due - now, __ATOMIC_RELAXED); now = due; }
    if (due > now) {
        pthread_mutex_unlock(&g_big);
        usleep((useconds_t)(due - now > 20000 ? 20000 : due - now));
        pthread_mutex_lock(&g_big);
        g_idle_us += host_usec() - now;
    }
    if (g_quit) { pthread_mutex_unlock(&g_big); exit(0); }
    sched_check_timers();
}

void rt_poll(Ctx *c) {
    c->budget = 4000;
    if (g_det > 0) __atomic_add_fetch(&vtime, det_step, __ATOMIC_RELAXED);
    if (g_in_intr || !(c->cop0_status & 0x10000u)) return;
    extern int g_ghost; if (g_ghost) return;   /* p2.c's second-view pass: no thread switches or interrupts */
    sched_check_timers();
    reschedule();
}

/* ------------------------------------------------------------ helpers */
static int alloc_thread(void) {
    for (int i = 1; i < MAX_THREADS; i++) if (!threads[i].used) return i;
    return -1;
}

static void sema_wake_one(int sid) {
    /* wake longest-waiting thread on this sema */
    int best = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (t->used && (t->status & TS_WAIT) && t->wait_type == TW_SEMA && t->wait_id == sid)
            if (best < 0 || t->seq < threads[best].seq) best = i;
    }
    if (best >= 0) { threads[best].wait_result = sid; semas[sid].waiters--; make_ready(best); }
    else semas[sid].count++;
}

static uint32_t ARG(Ctx *c, int n) { return c->r[4 + n].uw[0]; }
#define RET(v) (c->r[2].sd[0] = (int64_t)(int32_t)(v))

/* ------------------------------------------------------------ syscalls */
static int ghost_waits_skipped;
void rt_syscall(Ctx *c) {
    int32_t num = (int32_t)c->r[3].uw[0];
    int n = num < 0 ? -num : num;
    int isr = num < 0;
    Thread *cur = &threads[g_cur];
    TRACE("[sys] %d (%x %x %x %x) tid=%d\n", num, ARG(c,0), ARG(c,1), ARG(c,2), ARG(c,3), g_cur);
    /* p2.c's second-view pass runs alone and its memory is thrown away afterwards, so it
     * must leave the kernel as it found it: semaphores, wake-ups and alarms are left
     * alone and nothing blocks (a wait there would run the game's other threads on the
     * pass's memory: the sound and streaming threads, the hint phone's voice) */
    { extern int g_ghost;
      if (g_ghost) switch (n) {
        case 0x42: case 0x43: case 0x44: case 0x45: case 0x46:   /* Signal/Wait/PollSema */
        case 0x33: case 0x34: case 0x2D: case 0x2E:               /* WakeupThread, ReleaseWaitThread */
        case 0x37: case 0x38: case 0x39: case 0x3A:               /* Suspend/ResumeThread */
        case 0x19: case 0x1F: case 0xFE: case 0xFF:               /* ReleaseAlarm */
            if (ghost_waits_skipped++ < 5) WARN("[p2] syscall 0x%x skipped in the second-view pass\n", n);
            RET(ARG(c, 0)); return;
        case 0x18: case 0x1E: case 0xFC: case 0xFD:               /* SetAlarm */
            if (ghost_waits_skipped++ < 5) WARN("[p2] syscall 0x%x skipped in the second-view pass\n", n);
            RET(0); return;
      } }
    switch (n) {
    case 0x02: gs_set_crt(ARG(c, 0), ARG(c, 1), ARG(c, 2)); RET(0); break;
    case 0x04: case 0x06: case 0x07: case 0x7B:
        INFO("[sys] program exit (%d)\n", num); g_quit = 1; pthread_mutex_unlock(&g_big); exit(0);
    case 0x0D: case 0x0E: case 0x0F: RET(0); break;
    case 0x10: case 0x12: { /* AddIntcHandler / AddDmacHandler */
        int i;
        for (i = 0; i < MAX_HANDLERS && handlers[i].used; i++);
        if (i == MAX_HANDLERS) { RET(-1); break; }
        handlers[i] = (Handler){1, n == 0x12, (int)ARG(c, 0), 1, ARG(c, 1), ARG(c, 3), c->r[28].uw[0]};
        INFO("[sys] Add%sHandler(%d, %08x) -> %d\n", n == 0x12 ? "Dmac" : "Intc", ARG(c, 0), ARG(c, 1), i + 1);
        RET(i + 1); break;
    }
    case 0x11: case 0x13: {
        int id = ARG(c, 1) - 1;
        if (id >= 0 && id < MAX_HANDLERS) handlers[id].used = 0;
        RET(0); break;
    }
    case 0x14: case 0x1A: RET((intc_mask >> ARG(c, 0)) & 1 ? 0 : 1); intc_mask |= 1u << ARG(c, 0); break;
    case 0x15: case 0x1B: RET((intc_mask >> ARG(c, 0)) & 1); intc_mask &= ~(1u << ARG(c, 0)); break;
    case 0x16: case 0x1C: RET((dmac_mask >> ARG(c, 0)) & 1 ? 0 : 1); dmac_mask |= 1u << ARG(c, 0); break;
    case 0x17: case 0x1D: RET((dmac_mask >> ARG(c, 0)) & 1); dmac_mask &= ~(1u << ARG(c, 0)); break;
    case 0x18: case 0x1E: case 0xFC: case 0xFD: { /* SetAlarm(ticks, handler, arg) */
        int i;
        for (i = 0; i < MAX_ALARMS && alarms[i].used; i++);
        if (i == MAX_ALARMS) { RET(-1); break; }
        alarms[i] = (Alarm){1, host_usec() + (uint64_t)ARG(c, 0) * HSYNC_US, ARG(c, 1), ARG(c, 2), c->r[28].uw[0], ARG(c, 0)};
        RET(i + 1); break;
    }
    case 0x19: case 0x1F: case 0xFE: case 0xFF: {
        int id = ARG(c, 0) - 1;
        if (id >= 0 && id < MAX_ALARMS) alarms[id].used = 0;
        RET(0); break;
    }
    case 0x20: { /* CreateThread(ee_thread_t*) */
        uint32_t p = ARG(c, 0);
        int id = alloc_thread();
        if (id < 0) { RET(-1); break; }
        Thread *t = &threads[id];
        int gen = t->gen;
        memset(t, 0, sizeof *t);
        t->gen = gen + 1;
        pthread_cond_init(&t->cv, NULL);
        t->used = 1; t->status = TS_DORMANT;
        t->entry = MR32(p + 4); t->stack = MR32(p + 8); t->stack_size = MR32(p + 12);
        t->gp = MR32(p + 16); t->init_prio = t->prio = MR32(p + 20);
        t->attr = MR32(p + 28); t->option = MR32(p + 32);
        t->ctx = ctx_new();
        INFO("[sys] CreateThread entry=%08x stack=%08x+%x prio=%d -> %d\n", t->entry, t->stack, t->stack_size, t->prio, id);
        RET(id); break;
    }
    case 0x21: { int id = ARG(c, 0); if (id > 0 && id < MAX_THREADS && threads[id].status == TS_DORMANT) { threads[id].used = 0; RET(0); } else RET(-1); break; }
    case 0x22: { /* StartThread(id, arg) */
        int id = ARG(c, 0);
        if (id <= 0 || id >= MAX_THREADS || !threads[id].used) { RET(-1); break; }
        Thread *t = &threads[id];
        Ctx *tc = t->ctx;
        memset(tc->r, 0, sizeof tc->r);
        tc->r[4].sd[0] = (int32_t)ARG(c, 1);
        tc->r[28].sd[0] = (int32_t)t->gp;
        tc->r[29].sd[0] = (int32_t)((t->stack + t->stack_size - 0x2a0) & ~15u);
        tc->r[30].sd[0] = tc->r[29].sd[0];
        tc->tid = id;
        t->status = TS_READY; t->prio = t->init_prio; t->seq = seq_ctr++;
        t->wakeup_count = 0; t->host_started = 0;
        RET(0);
        reschedule();
        break;
    }
    case 0x23: exit_current();
    case 0x24: threads[g_cur].used = 0; exit_current();
    case 0x25: case 0x26: { /* TerminateThread */
        int id = ARG(c, 0);
        if (id <= 0 || id >= MAX_THREADS || id == g_cur) { RET(-1); break; }
        Thread *t = &threads[id];
        if (t->status & TS_WAIT && t->wait_type == TW_SEMA) semas[t->wait_id].waiters--;
        t->status = TS_DORMANT; t->gen++; t->host_started = 0;
        pthread_cond_signal(&t->cv);
        RET(0); break;
    }
    case 0x27: dispatch_disabled = 1; RET(0); break;
    case 0x28: dispatch_disabled = 0; RET(0); reschedule(); break;
    case 0x29: case 0x2A: { /* ChangeThreadPriority */
        int id = ARG(c, 0); if (id == 0) id = g_cur;
        if (id <= 0 || id >= MAX_THREADS || !threads[id].used) { RET(-1); break; }
        int old = threads[id].prio;
        threads[id].prio = ARG(c, 1);
        RET(old);
        if (!isr) reschedule();
        break;
    }
    case 0x2B: case 0x2C: { /* RotateThreadReadyQueue(prio) */
        int prio = ARG(c, 0);
        if (!isr && cur->prio == prio) {
            cur->status = TS_READY; cur->seq = seq_ctr++;
            RET(0);
            block_current();
        } else {
            /* rotate: move earliest ready thread of that prio to the end */
            int best = -1;
            for (int i = 0; i < MAX_THREADS; i++)
                if (threads[i].used && threads[i].status == TS_READY && threads[i].prio == prio)
                    if (best < 0 || threads[i].seq < threads[best].seq) best = i;
            if (best >= 0) threads[best].seq = seq_ctr++;
            RET(0);
        }
        break;
    }
    case 0x2D: case 0x2E: { /* ReleaseWaitThread */
        int id = ARG(c, 0);
        if (id > 0 && id < MAX_THREADS && (threads[id].status & TS_WAIT)) {
            if (threads[id].wait_type == TW_SEMA) semas[threads[id].wait_id].waiters--;
            threads[id].wait_result = -418; make_ready(id);
            RET(0); if (!isr) reschedule();
        } else RET(-1);
        break;
    }
    case 0x2F: RET(g_cur); break;
    case 0x30: case 0x31: { /* ReferThreadStatus(id, info) */
        int id = ARG(c, 0); if (id == 0) id = g_cur;
        uint32_t p = ARG(c, 1);
        if (id <= 0 || id >= MAX_THREADS || !threads[id].used) { RET(-1); break; }
        Thread *t = &threads[id];
        if (p) {
            uint32_t v[13] = {t->status, t->entry, t->stack, t->stack_size, t->gp, t->init_prio, t->prio,
                              t->attr, t->option, t->wait_type, t->wait_id, t->wakeup_count, 0};
            for (int i = 0; i < 13; i++) MW32(p + i * 4, v[i]);
        }
        RET(t->status); break;
    }
    case 0x32: /* SleepThread */
        if (cur->wakeup_count > 0) { cur->wakeup_count--; RET(g_cur); break; }
        /* p2.c's second-view pass runs alone (its memory is thrown away afterwards): the
         * stage frame's wait for the end of the display frame (0x19d1e0) returns at once */
        { extern int g_ghost; if (g_ghost) { RET(g_cur); break; } }
        cur->status = TS_WAIT; cur->wait_type = TW_SLEEP; cur->seq = seq_ctr++;
        TRACE("[sys] tid %d sleeps\n", g_cur);
        RET(g_cur);
        block_current();
        break;
    case 0x33: case 0x34: { /* WakeupThread */
        int id = ARG(c, 0);
        if (id <= 0 || id >= MAX_THREADS || !threads[id].used) { RET(-1); break; }
        Thread *t = &threads[id];
        if ((t->status & TS_WAIT) && t->wait_type == TW_SLEEP) make_ready(id);
        else t->wakeup_count++;
        RET(0);
        if (!isr) reschedule();
        break;
    }
    case 0x35: case 0x36: {
        int id = ARG(c, 0); if (id == 0) id = g_cur;
        if (id <= 0 || id >= MAX_THREADS) { RET(-1); break; }
        RET(threads[id].wakeup_count); threads[id].wakeup_count = 0; break;
    }
    case 0x37: case 0x38: { /* SuspendThread */
        int id = ARG(c, 0);
        if (id <= 0 || id >= MAX_THREADS || !threads[id].used || id == g_cur) { RET(-1); break; }
        Thread *t = &threads[id];
        t->suspend_count++;
        if (t->status == TS_READY) t->status = TS_SUSPEND;
        else if (t->status == TS_WAIT) t->status = TS_WAITSUSPEND;
        RET(0); break;
    }
    case 0x39: case 0x3A: { /* ResumeThread */
        int id = ARG(c, 0);
        if (id <= 0 || id >= MAX_THREADS || !threads[id].used) { RET(-1); break; }
        Thread *t = &threads[id];
        if (t->suspend_count > 0 && --t->suspend_count == 0) {
            if (t->status == TS_SUSPEND) { t->status = TS_READY; t->seq = seq_ctr++; }
            else if (t->status == TS_WAITSUSPEND) t->status = TS_WAIT;
        }
        RET(0);
        if (!isr) reschedule();
        break;
    }
    case 0x3C: { /* SetupThread(gp, stack, size, args, root) */
        uint32_t stack = ARG(c, 1), size = ARG(c, 2), args = ARG(c, 3);
        uint32_t top = (stack == 0xffffffffu) ? 0x02000000u : stack + size;
        /* the main thread's stack sits at the top of the 32 MB (crt0 passes 0x1fa0000,
         * 0x60000); with more RAM it moves to the new top, and the heap below grows */
        if (top == 0x02000000u) top = g_ram_size;
        if (args) {
            const char *a0 = "cdrom0:\\SCES_508.85;1";
            MW32(args, 1);
            MW32(args + 4, args + 0x40);
            MW32(args + 8, 0);
            gwrite(args + 0x40, a0, (uint32_t)strlen(a0) + 1);
        }
        cur->stack = top - size; cur->stack_size = size; cur->gp = ARG(c, 0);
        RET(top - 0x2a0);
        break;
    }
    case 0x3D: heap_start = ARG(c, 0); heap_end = (ARG(c, 1) == 0xffffffffu) ? cur->stack : heap_start + ARG(c, 1); RET(heap_start); break;
    case 0x3E: RET(heap_end); break;
    case 0x40: { /* CreateSema(ee_sema_t*) */
        uint32_t p = ARG(c, 0);
        int i;
        for (i = 1; i < MAX_SEMAS && semas[i].used; i++);
        if (i == MAX_SEMAS) { RET(-1); break; }
        semas[i] = (Sema){1, (int)MR32(p + 8), (int)MR32(p + 4), (int)MR32(p + 8), MR32(p + 16), MR32(p + 20), 0};
        TRACE("[sys] CreateSema init=%d max=%d -> %d\n", semas[i].count, semas[i].max, i);
        RET(i); break;
    }
    case 0x41: case 0x49: { /* DeleteSema */
        int s = ARG(c, 0);
        if (s <= 0 || s >= MAX_SEMAS || !semas[s].used) { RET(-1); break; }
        for (int i = 0; i < MAX_THREADS; i++)
            if (threads[i].used && (threads[i].status & TS_WAIT) && threads[i].wait_type == TW_SEMA && threads[i].wait_id == s) {
                threads[i].wait_result = -425; make_ready(i);
            }
        semas[s].used = 0;
        RET(s);
        if (!isr) reschedule();
        break;
    }
    case 0x42: case 0x43: { /* SignalSema */
        int s = ARG(c, 0);
        if (s <= 0 || s >= MAX_SEMAS || !semas[s].used) { RET(-1); break; }
        sema_wake_one(s);
        RET(s);
        if (!isr) reschedule();
        break;
    }
    case 0x44: { /* WaitSema */
        int s = ARG(c, 0);
        if (s <= 0 || s >= MAX_SEMAS || !semas[s].used) { RET(-1); break; }
        if (semas[s].count > 0) { semas[s].count--; RET(s); break; }
        cur->status = TS_WAIT; cur->wait_type = TW_SEMA; cur->wait_id = s; cur->seq = seq_ctr++;
        cur->wait_result = s;
        TRACE("[sys] tid %d blocks on sema %d\n", g_cur, s);
        semas[s].waiters++;
        block_current();
        RET(cur->wait_result);
        break;
    }
    case 0x45: case 0x46: { /* PollSema */
        int s = ARG(c, 0);
        if (s <= 0 || s >= MAX_SEMAS || !semas[s].used) { RET(-1); break; }
        if (semas[s].count > 0) { semas[s].count--; RET(s); } else RET(-419);
        break;
    }
    case 0x47: case 0x48: { /* ReferSemaStatus */
        int s = ARG(c, 0); uint32_t p = ARG(c, 1);
        if (s <= 0 || s >= MAX_SEMAS || !semas[s].used) { RET(-1); break; }
        MW32(p, semas[s].count); MW32(p + 4, semas[s].max); MW32(p + 8, semas[s].init);
        MW32(p + 12, semas[s].waiters); MW32(p + 16, semas[s].attr); MW32(p + 20, semas[s].option);
        RET(s); break;
    }
    case 0x4A: RET(0); break;
    case 0x4B: { /* GetOsdConfigParam: English, 4:3, RGB */
        uint32_t p = ARG(c, 0);
        MW32(p, (1u << 16) /* language english */);
        RET(0); break;
    }
    case 0x5A: RET(0); break;  /* QueryBootMode */
    case 0x5B: RET(0); break;
    case 0x56: RET(0); break;
    case 0x64: case 0x68: RET(0); break;  /* FlushCache */
    case 0x6B: RET(0); break;
    case 0x70: RET(gs_imr); break;
    case 0x71: { uint32_t old = gs_imr; gs_imr = ARG(c, 0); RET(old); break; }
    case 0x73: vsync_flag_addr = ARG(c, 0); vsync_csr_addr = ARG(c, 1); RET(0); break;
    case 0x74: RET(0); break;  /* SetSyscall */
    case 0x76: RET(-1); break; /* SifDmaStat: done */
    case 0x77: RET(sif_set_dma(ARG(c, 0), ARG(c, 1))); break;
    case 0x78: RET(0); break;  /* SifSetDChain */
    case 0x79: sif_set_reg(ARG(c, 0), ARG(c, 1)); RET(0); break;
    case 0x7A: RET(sif_get_reg(ARG(c, 0))); break;
    case 0x7C: RET(0); break;  /* Deci2Call */
    case 0x7F: RET(g_ram_size); break;
    case 0x82: case 0x83: RET(0); break;
    default:
        WARN("[sys] unimplemented syscall %d (0x%x) at %08x\n", num, n, c->pc);
        RET(0);
        break;
    }
    if (!g_in_intr && (c->cop0_status & 0x10000u)) kernel_deliver_pending();
}

void kernel_init(void) {
    for (int i = 0; i < MAX_THREADS; i++) pthread_cond_init(&threads[i].cv, NULL);
}

void kernel_start_main(uint32_t entry) {
    guest_fpu_mode();
    Thread *t = &threads[1];
    t->used = 1; t->status = TS_RUN; t->prio = t->init_prio = 1;
    t->ctx = ctx_new(); t->ctx->tid = 1;
    t->host_started = 1;
    t->th = pthread_self();
    g_cur = 1;
    pthread_mutex_lock(&g_big);
    next_vblank_us = host_usec() + VBLANK_US;
    rt_call(t->ctx, entry);
    INFO("[kernel] main returned\n");
    exit_current();
}

/* ------------------------------------------------------------ misc CPU hooks */
static uint32_t cop0_count_base;
uint32_t rt_mfc0(Ctx *c, int r) {
    if (r == 9) return (uint32_t)(host_usec() * 294) - cop0_count_base;  /* 294.912 MHz */
    if (r == 12) return c->cop0_status;
    if (r == 15) return 0x2e20;  /* PRId */
    return 0;
}
void rt_mtc0(Ctx *c, int r, uint32_t v) { if (r == 12) c->cop0_status = v; if (r == 9) cop0_count_base = (uint32_t)(host_usec() * 294) - v; }
void rt_eret(Ctx *c) { WARN("[cpu] eret at %08x\n", c->pc); }
int rt_cpcond0(Ctx *c) { (void)c; return 1; }
void rt_break(Ctx *c, uint32_t pc) { (void)c; WARN("[cpu] break at %08x\n", pc); }
