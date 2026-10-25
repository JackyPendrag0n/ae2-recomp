/* Entry point, ELF loading, indirect-call dispatch and misc CPU hooks. */
#include "rt.h"
#include <unistd.h>
#include "platform.h"
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <elf.h>
#include <signal.h>

int g_log_level = 1;
const char *g_game_dir = ".";

void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    { char msg[1024]; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap); plat_fatal_box(msg); }
    exit(1);
}

/* ------------------------------------------------------------ dispatch */
static const FuncEntry *find_func(uint32_t a) {
    int lo = 0, hi = g_functable_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (g_functable[mid].start == a) return &g_functable[mid];
        if (g_functable[mid].start < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void rt_call(Ctx *c, uint32_t addr) {
    const FuncEntry *f = find_func(addr & 0x1fffffff);
    if (likely(f != NULL)) { f->fn(c); return; }
    static int nwarn;
    if (nwarn++ < 100) WARN("[cpu] call to unknown address %08x (ra=%08x) - add to recomp/extra_entries.txt\n", addr, c->r[31].uw[0]);
    FILE *uf = fopen("unknown_calls.txt", "a");
    if (uf) { fprintf(uf, "%08x\n", addr); fclose(uf); }
}

void rt_trace_pc(Ctx *c, uint32_t pc) {
    static uint32_t last[64]; static uint64_t cnt[64]; static int n;
    int i; for (i = 0; i < n; i++) if (last[i] == pc) break;
    if (i == n && n < 64) { last[n] = pc; cnt[n] = 0; n++; }
    if (i < 64 && (cnt[i]++ < 20 || (cnt[i] % 100) == 0))
        fprintf(stderr, "[trace] pc=%08x hit %llu v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x s0=%08x f0=%g f12=%g vr=%06x ra=%08x frame=%u\n", pc, (unsigned long long)cnt[i], c->r[2].uw[0], c->r[3].uw[0], c->r[4].uw[0], c->r[5].uw[0], c->r[6].uw[0], c->r[16].uw[0], c->f[0].f, c->f[12].f, c->vr & 0xffffff, c->r[31].uw[0], g_frame);
}
void rt_badop(Ctx *c, uint32_t pc, uint32_t w) {
    (void)c;
    static int n;
    if (n++ < 50) WARN("[cpu] bad opcode %08x at %08x\n", w, pc);
}

/* ------------------------------------------------------------ COP2 control */
uint32_t rt_cfc2(Ctx *c, int r) {
    if (r < 16) return c->vi[r] & 0xffff;
    if (r == 20) return 0x3f800000u | (c->vr & 0x7fffff);   /* R: the VRNEXT/VRGET random register */
    if (r == 22) { uint32_t u; memcpy(&u, &c->vq, 4); return u; }
    if (r == 29) return 0;  /* VPU-STAT: VU0/VU1 idle */
    return c->vi[r];
}
void rt_vu0_exec(Ctx *c, uint32_t addr);
void rt_ctc2(Ctx *c, int r, uint32_t v) {
    if (r == 0) return;
    if (r < 16) { c->vi[r] = v & 0xffff; return; }
    if (r == 20) { c->vr = v & 0x7fffff; return; }
    if (r == 16) { c->vi[16] = (c->vi[16] & 0x3f) | (v & 0xfc0); return; }   /* status: only the sticky bits are writable */
    if (r == 17 || r == 18) { c->vi[r] = v & (r == 17 ? 0xffff : 0xffffff); return; }
    if (r == 22) { memcpy(&c->vq, &v, 4); return; }
    if (r == 28) {  /* FBRST */
        if (v & 2) memset(g_vu0mem, 0, 0);  /* reset VU0: nothing to do */
        if (v & 0x200) vif1_reset();
        return;
    }
    c->vi[r] = v;
}
void rt_vcallms(Ctx *c, uint32_t addr) { rt_vu0_exec(c, addr); }

/* ------------------------------------------------------------ setjmp/longjmp */
typedef struct { uint32_t guest; jmp_buf jb; r128 regs[32]; int used; pthread_t th; } JmpSlot;
static JmpSlot jslots[64];
static JmpSlot *jslot(uint32_t g, int create) {
    pthread_t me = pthread_self();
    for (int i = 0; i < 64; i++) if (jslots[i].used && jslots[i].guest == g && pthread_equal(jslots[i].th, me)) return &jslots[i];
    if (!create) return NULL;
    for (int i = 0; i < 64; i++) if (!jslots[i].used) { jslots[i].used = 1; jslots[i].guest = g; jslots[i].th = me; return &jslots[i]; }
    static int rr; JmpSlot *s = &jslots[rr++ & 63]; s->guest = g; s->th = me; return s;
}
jmp_buf *rt_setjmp_buf(Ctx *c, uint32_t g) { (void)c; return &jslot(g, 1)->jb; }
void rt_setjmp_save(Ctx *c, uint32_t g) { memcpy(jslot(g, 1)->regs, c->r, sizeof c->r); }
void rt_longjmp(Ctx *c) {
    uint32_t g = c->r[4].uw[0];
    int val = (int)c->r[5].uw[0];
    JmpSlot *s = jslot(g, 0);
    if (!s) rt_fatal("longjmp to unknown buffer %08x", g);
    static const int keep[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
    for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; i++) c->r[keep[i]] = s->regs[keep[i]];
    c->r[2].sd[0] = val ? val : 1;
    longjmp(s->jb, 1);
}

/* ------------------------------------------------------------ ELF */
static uint32_t load_elf(const char *rel) {
    FILE *fp = host_open_game_file(rel, "rb");
    if (!fp) rt_fatal("cannot open %s in %s", rel, g_game_dir);
    Elf32_Ehdr eh;
    if (fread(&eh, sizeof eh, 1, fp) != 1) rt_fatal("short ELF");
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        fseek(fp, eh.e_phoff + i * eh.e_phentsize, SEEK_SET);
        if (fread(&ph, sizeof ph, 1, fp) != 1) rt_fatal("short phdr");
        if (ph.p_type != PT_LOAD) continue;
        fseek(fp, ph.p_offset, SEEK_SET);
        if (fread(g_ram + ph.p_vaddr, 1, ph.p_filesz, fp) != ph.p_filesz) rt_fatal("short segment");
        memset(g_ram + ph.p_vaddr + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);
        INFO("[elf] segment %08x filesz %x memsz %x\n", ph.p_vaddr, ph.p_filesz, ph.p_memsz);
    }
    fclose(fp);
    return eh.e_entry;
}

void prof_init(void); void prof_report(void);
static void on_signal(int s) { (void)s; g_quit = 1; { void vif_print_stats(void); vif_print_stats(); void vu1_print_stats(void); vu1_print_stats(); } prof_report(); _exit(0); }

int g_headless;
static void *guest_main(void *arg) { kernel_start_main(*(uint32_t *)arg); return NULL; }
int g_hw_request = 0;   /* renderer: 0 software, 1 GPU */
int g_auto_scale = 2;   /* GPU resolution picked from the screen height */
int main(int argc, char **argv) {
    int headless = 0, renderer_set = 0, scale_set = 0, dir_set = 0;
    extern int g_hw_scale;
    plat_init();
    { const char *r = getenv("AE2_RENDERER"); if (r) { g_hw_request = !strcmp(r, "gpu") || !strcmp(r, "hw"); renderer_set = 1; }
      const char *sc = getenv("AE2_SCALE"); if (sc) { g_hw_scale = atoi(sc); scale_set = 1; } }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_log_level = 2;
        else if (!strcmp(argv[i], "-vv")) g_log_level = 3;
        else if (!strcmp(argv[i], "-q")) g_log_level = 0;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--record")) {
            /* optional file name; default: ae2-YYYYMMDD-HHMMSS.ae2rec in the current folder */
            static char def[64];
            if (i + 1 < argc && strstr(argv[i + 1], ".ae2rec")) setenv("AE2_RECORD", argv[++i], 1);
            else { time_t t = time(NULL); strftime(def, sizeof def, "ae2-%Y%m%d-%H%M%S.ae2rec", localtime(&t)); setenv("AE2_RECORD", def, 1); }
        }
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) setenv("AE2_REPLAY", argv[++i], 1);
        else if (!strcmp(argv[i], "--widescreen")) { extern int g_force_widescreen; g_force_widescreen = 1; }
        else if (!strcmp(argv[i], "--gpu")) { g_hw_request = 1; renderer_set = 1; }
        else if (!strcmp(argv[i], "--sw") || !strcmp(argv[i], "--software")) { g_hw_request = 0; renderer_set = 1; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) { g_hw_scale = atoi(argv[++i]); scale_set = 1; if (!renderer_set) { g_hw_request = 1; renderer_set = 1; } }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: ae2 [options] <game folder>\n"
                   "  -v, -vv, -q        more / less logging\n"
                   "  --widescreen       force the game's 16:9 mode\n"
                   "  --gpu / --sw       GPU (OpenGL, the default when available) or software renderer\n"
                   "  --scale N          GPU internal resolution, 1-8 times the PS2's (2 = ~1080p, 4 = ~4K);\n"
                   "                     default: picked from your screen height\n"
                   "  --record [f.ae2rec] record input for a bug report (deterministic mode)\n"
                   "  --replay f.ae2rec  replay a recording\n"
                   "  --headless         no window (testing)\n"
                   "In game: F1 (or SELECT+START on a gamepad) opens the debug menu; F2 free camera, F3 no clip.\n");
            return 0;
        }
        else { g_game_dir = argv[i]; dir_set = 1; }
    }
    if (!dir_set) { const char *d = plat_default_game_dir(); if (d) g_game_dir = d; }   /* "game" next to the program */
    prof_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    /* EE RAM: the whole window up to the hardware registers is reserved (pages are only
     * backed once touched); the game is told it has g_ram_size */
    { const char *e = getenv("AE2_RAM_MB"); unsigned mb = e ? (unsigned)atoi(e) : 128;
      if (mb < 32) mb = 32; if (mb > 256) mb = 256;
      g_ram_size = mb == 256 ? EE_RAM_MAX : mb << 20;
      g_ram = plat_reserve(EE_RAM_MAX);
      if (!g_ram) rt_fatal("cannot reserve EE RAM");
      INFO("[main] EE RAM %u MB\n", g_ram_size >> 20); }
    uint32_t entry = load_elf("SCES_508.85");
    INFO("[main] entry %08x, %d recompiled functions\n", entry, g_functable_n);
    g_headless = headless;
    { void debug_init(void); debug_init(); }
    host_init(headless);
    /* default renderer: GPU when the window got an OpenGL 3.3 context, at the scale that
     * matches the screen (the PS2 image is 512 lines: 1080p -> 2x, 1440p -> 3x, 2160p -> 4x) */
    { extern int g_display_h; int host_use_gl(void);
      { int s = (g_display_h + 256) / 512; g_auto_scale = s < 2 ? 2 : s > 4 ? 4 : s; }
      /* Options > Graphics settings (ae2-settings.ini); command line and environment win */
      { int gfx_settings_load(int renderer_set, int scale_set); int got = gfx_settings_load(renderer_set, scale_set);
        if (got & 1) renderer_set = 1; if (got & 2) scale_set = 1; }
      if (!renderer_set && !headless && host_use_gl()) g_hw_request = 1;
      if (g_hw_request == 1 && !headless && !host_use_gl()) g_hw_request = 0;
      if (!scale_set) g_hw_scale = g_auto_scale;
      if (g_hw_scale < 1) g_hw_scale = 1; if (g_hw_scale > 8) g_hw_scale = 8;
      INFO("[main] renderer: %s", g_hw_request == 1 ? "GPU" : "software");
      if (g_hw_request == 1) INFO(" at %dx", g_hw_scale);
      INFO("\n"); }
    hw_init();
    gs_init();
    vu1_init();
    { void vif_init(void); vif_init(); }
    kernel_init();
    sif_init();
    if (headless) { kernel_start_main(entry); return 0; }
    /* the emulated EE runs on its own thread; the main thread owns the window */
    {
        void host_ui_loop(void);
        static uint32_t e; e = entry;
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
        pthread_t th; pthread_create(&th, &at, guest_main, &e);
        host_ui_loop();
    }
    return 0;
}

/* ---- AE2_FTRACE="from-to[,from-to...]:prefix": count the guest functions entered
 * during each window of game frames (pad polls) and write "<prefix>.<n>" (address count, by address). */
int g_ftrace;
static struct { uint32_t a; uint32_t n; } ft_tab[1 << 16];
static uint32_t ft_win[512][2]; static int ft_nwin = -1, ft_cur = -1; static char ft_prefix[256];
/* AE2_FWATCH=addr[,words]: on each entry of that function (inside a trace window),
 * log a0 and the first <words> words at a0 (as hex) */
static uint32_t fw_addr, fw_words = 64; static int fw_init;
/* AE2_FSEQ=file: also write every function entry (in a trace window) in order:
 * depth (check builds), address, a0, a1 */
static FILE *fseq_f; static int fseq_init;
void rt_ftrace(Ctx *c, uint32_t addr) {
    if (!fseq_init) { fseq_init = 1; const char *e = getenv("AE2_FSEQ"); if (e) fseq_f = fopen(e, "w"); }
    if (fseq_f) {
        int d = 0;
#ifdef AE2_FPCHECK
        extern __thread int g_fdepth; d = g_fdepth;
#endif
        fprintf(fseq_f, "%d %06x %x %x\n", d, addr, c->r[4].uw[0], c->r[5].uw[0]);
    }
    if (!fw_init) { fw_init = 1; const char *e = getenv("AE2_FWATCH"); if (e) sscanf(e, "%x,%u", &fw_addr, &fw_words); }
    if (addr == fw_addr) {
        extern uint32_t g_pad_polls;
        uint32_t a0 = c->r[4].uw[0];
        fprintf(stderr, "[fwatch] poll %u a0=%08x a1=%08x:", g_pad_polls, a0, c->r[5].uw[0]);
        for (uint32_t i = 0; i < fw_words; i++) fprintf(stderr, " %08x", MR32(a0 + i * 4));
#ifdef AE2_FPCHECK
        extern __thread uint32_t g_fstack[256]; extern __thread int g_fdepth;
        fprintf(stderr, " stack");
        for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 10; d--) if (d < 256) fprintf(stderr, " <%08x", g_fstack[d]);
#endif
        fprintf(stderr, "\n");
    }
    uint32_t h = (addr * 2654435761u) >> 16;
    for (;; h = (h + 1) & 0xffff) {
        if (ft_tab[h].a == addr) { ft_tab[h].n++; return; }
        if (!ft_tab[h].a) { ft_tab[h].a = addr; ft_tab[h].n = 1; return; }
    }
}
static int ft_cmp(const void *x, const void *y) { uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y; return a < b ? -1 : a > b; }
/* AE2_MEMDUMP=poll[,poll...]:prefix - write EE RAM to "<prefix>.<poll>" at those game frames */
static void memdump_frame(uint32_t frame) {
    static int init; static uint32_t at[64]; static int n; static char pre[256];
    if (!init) { init = 1; const char *e = getenv("AE2_MEMDUMP"); if (!e) return;
        const char *colon = strchr(e, ':'); snprintf(pre, sizeof pre, "%s", colon ? colon + 1 : "mem");
        while (*e && *e != ':' && n < 64) { at[n++] = (uint32_t)strtoul(e, (char **)&e, 10); if (*e == ',') e++; } }
    for (int i = 0; i < n; i++) if (at[i] == frame) {
        char path[300]; snprintf(path, sizeof path, "%s.%u", pre, frame);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(g_ram, 1, g_ram_size, f); fclose(f); }
    }
}
void rt_ftrace_frame(uint32_t frame) {
#ifdef AE2_FPCHECK
    { void rt_rwatch_frame(void); rt_rwatch_frame(); }
#endif
    memdump_frame(frame);
    if (ft_nwin < 0) {
        ft_nwin = 0;
        const char *e = getenv("AE2_FTRACE"); if (!e) return;
        const char *colon = strchr(e, ':'); snprintf(ft_prefix, sizeof ft_prefix, "%s", colon ? colon + 1 : "ftrace");
        while (*e && *e != ':' && ft_nwin < 512) {
            unsigned a, b, st = 0; int k = sscanf(e, "%u-%u/%u", &a, &b, &st); if (k < 2) break;
            if (k == 3 && st) { for (unsigned x = a; x < b && ft_nwin < 512; x += st) { ft_win[ft_nwin][0] = x; ft_win[ft_nwin][1] = x + st; ft_nwin++; } }
            else { ft_win[ft_nwin][0] = a; ft_win[ft_nwin][1] = b; ft_nwin++; }
            while (*e && *e != ',' && *e != ':') e++;
            if (*e == ',') e++;
        }
    }
    for (int i = 0; i < ft_nwin; i++) {
        if (frame == ft_win[i][1] && ft_cur == i) {
            g_ftrace = 0; ft_cur = -1;
            char path[300]; snprintf(path, sizeof path, "%s.%d", ft_prefix, i);
            FILE *f = fopen(path, "w"); if (!f) continue;
            static uint32_t tmp[1 << 17]; int n = 0;
            for (int h = 0; h < (1 << 16); h++) if (ft_tab[h].a) { tmp[n * 2] = ft_tab[h].a; tmp[n * 2 + 1] = ft_tab[h].n; n++; }
            qsort(tmp, n, 8, ft_cmp);
            for (int k = 0; k < n; k++) fprintf(f, "%08x %u\n", tmp[k * 2], tmp[k * 2 + 1]);
            fclose(f);
        }
    }
    for (int i = 0; i < ft_nwin; i++)
        if (frame == ft_win[i][0]) { memset(ft_tab, 0, sizeof ft_tab); ft_cur = i; g_ftrace = 1; }
}
