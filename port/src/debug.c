/* Debug features that work on the per-frame pad snapshot:
 *   - input recording and replay (AE2_RECORD=file / AE2_REPLAY=file). Together with
 *     deterministic mode, a replay reproduces a play session frame for frame. Both
 *     switch deterministic mode on. A recording also stores the memory card contents
 *     it started from, and a replay restores them into a scratch folder.
 *   - a test bot (AE2_BOT=poll) that chases the nearest monkey and swings the club,
 *     using the game's per-frame collision sphere list.
 *   - memory pokes (AE2_POKE="poll:addr=value[/size],...;...") for experiments.
 */
#include "rt.h"
#include <unistd.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

extern const char *g_save_dir;
int host_is_headless(void);

/* ------------------------------------------------------------------ memory card snapshot */
typedef struct { char path[256]; uint8_t *data; uint32_t size; int is_dir; } McFile;
static McFile mcf[512]; static int nmcf;
static void mc_scan(const char *root, const char *rel) {
    char full[512]; snprintf(full, sizeof full, "%s/%s", root, rel);
    DIR *d = opendir(full); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nmcf < 512) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r2[256]; snprintf(r2, sizeof r2, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        char f2[512]; snprintf(f2, sizeof f2, "%s/%s", root, r2);
        struct stat st; if (stat(f2, &st)) continue;
        McFile *m = &mcf[nmcf++]; snprintf(m->path, sizeof m->path, "%s", r2);
        if (S_ISDIR(st.st_mode)) { m->is_dir = 1; mc_scan(root, r2); continue; }
        m->size = (uint32_t)st.st_size; m->data = malloc(m->size + 1);
        FILE *f = fopen(f2, "rb"); if (f) { if (fread(m->data, 1, m->size, f) != m->size) m->size = 0; fclose(f); }
    }
    closedir(d);
}
static void mkdirs(const char *p) {
    char t[512]; snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}

/* ------------------------------------------------------------------ record / replay */
#define REC_MAGIC "AE2REC02"
typedef struct { uint16_t buttons; uint8_t an[4]; uint16_t pad; } RecFrame;
static FILE *rec_f;
static RecFrame *rep; static uint32_t nrep; static int rep_on;
static uint32_t rep_flags;

static void rec_start(const char *path) {
    rec_f = fopen(path, "wb");
    if (!rec_f) { WARN("[debug] cannot write recording %s\n", path); return; }
    nmcf = 0; mc_scan(g_save_dir, "");
    uint32_t flags = getenv("AE2_FPU_NEAREST") ? 1 : 0;
    fwrite(REC_MAGIC, 1, 8, rec_f); fwrite(&flags, 4, 1, rec_f);
    uint32_t n = (uint32_t)nmcf; fwrite(&n, 4, 1, rec_f);
    for (int i = 0; i < nmcf; i++) {
        uint16_t l = (uint16_t)strlen(mcf[i].path); uint8_t dir = (uint8_t)mcf[i].is_dir;
        fwrite(&l, 2, 1, rec_f); fwrite(mcf[i].path, 1, l, rec_f); fwrite(&dir, 1, 1, rec_f);
        fwrite(&mcf[i].size, 4, 1, rec_f); if (mcf[i].size) fwrite(mcf[i].data, 1, mcf[i].size, rec_f);
    }
    fflush(rec_f);
    INFO("[debug] recording input to %s (memory card: %d entries)\n", path, nmcf);
}
static void rep_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { WARN("[debug] cannot open recording %s\n", path); return; }
    char mg[8]; uint32_t n = 0;
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, REC_MAGIC, 8)) { WARN("[debug] %s is not a recording\n", path); fclose(f); return; }
    if (fread(&rep_flags, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1) { fclose(f); return; }
    /* restore the memory card into a scratch folder */
    static char dir[512]; snprintf(dir, sizeof dir, "%s.memcard", path);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t l; uint8_t isdir; uint32_t sz; char p[300];
        if (fread(&l, 2, 1, f) != 1 || l >= 256 || fread(p, 1, l, f) != l) break;
        p[l] = 0;
        if (fread(&isdir, 1, 1, f) != 1 || fread(&sz, 4, 1, f) != 1) break;
        char full[900]; snprintf(full, sizeof full, "%s/%s", dir, p);
        if (isdir) { mkdirs(full); continue; }
        char *sl = strrchr(full, '/'); if (sl) { *sl = 0; mkdirs(full); *sl = '/'; }
        uint8_t *buf = malloc(sz + 1);
        if (sz && fread(buf, 1, sz, f) != sz) { free(buf); break; }
        FILE *o = fopen(full, "wb"); if (o) { fwrite(buf, 1, sz, o); fclose(o); }
        free(buf);
    }
    mkdirs(dir);
    g_save_dir = dir;
    long pos = ftell(f); fseek(f, 0, SEEK_END); long end = ftell(f); fseek(f, pos, SEEK_SET);
    nrep = (uint32_t)((end - pos) / sizeof(RecFrame));
    rep = malloc((nrep + 1) * sizeof(RecFrame));
    nrep = (uint32_t)fread(rep, sizeof(RecFrame), nrep, f);
    fclose(f);
    rep_on = 1;
    INFO("[debug] replaying %u frames from %s (memory card restored to %s)\n", nrep, path, dir);
    if ((rep_flags & 1) != (getenv("AE2_FPU_NEAREST") != NULL))
        WARN("[debug] the recording was made with AE2_FPU_NEAREST %s: the replay may diverge\n", (rep_flags & 1) ? "set" : "unset");
}

/* called by main() before anything else starts */
void debug_init(void) {
    const char *r = getenv("AE2_REPLAY"), *w = getenv("AE2_RECORD");
    if (r) rep_load(r);
    if (w) rec_start(w);
}

/* ------------------------------------------------------------------ pokes */
typedef struct { uint32_t poll, addr, val; int size; } Poke;
static Poke pokes[128]; static int npokes = -1;
static void pokes_parse(void) {
    npokes = 0;
    const char *e = getenv("AE2_POKE"); if (!e) return;
    char buf[4096]; snprintf(buf, sizeof buf, "%s", e);
    char *sv1;
    for (char *grp = strtok_r(buf, ";", &sv1); grp; grp = strtok_r(NULL, ";", &sv1)) {
        char *colon = strchr(grp, ':'); if (!colon) continue;
        uint32_t poll = (uint32_t)strtoul(grp, NULL, 10);
        char *sv2;
        for (char *it = strtok_r(colon + 1, ",", &sv2); it && npokes < 128; it = strtok_r(NULL, ",", &sv2)) {
            Poke *p = &pokes[npokes]; p->poll = poll; p->size = 4;
            char *eq = strchr(it, '='); if (!eq) continue;
            p->addr = (uint32_t)strtoul(it, NULL, 16);
            p->val = (uint32_t)strtoul(eq + 1, NULL, 16);
            char *sl = strchr(eq, '/'); if (sl) p->size = atoi(sl + 1);
            npokes++;
        }
    }
}
static void pokes_run(uint32_t poll) {
    if (npokes < 0) pokes_parse();
    for (int i = 0; i < npokes; i++) if (pokes[i].poll == poll) {
        Poke *p = &pokes[i];
        if (p->size == 1) MW8(p->addr, (uint8_t)p->val); else if (p->size == 2) MW16(p->addr, (uint16_t)p->val); else MW32(p->addr, p->val);
        INFO("[debug] poll %u: poke %08x = %x (%d bytes)\n", poll, p->addr, p->val, p->size);
    }
}

/* ------------------------------------------------------------------ test bot */
/* The game keeps a per-frame list of collision spheres (stride 0x150): +0x30 centre,
 * +0x44 kind (0x20: Spike's body, 0x21: a monkey's body), +0x58 owner actor. */
#define SPH_BASE MR32(0x5376c0u)   /* the list is on the heap; this global points at it */
#define SPH_STRIDE 0x150u
static float rf(uint32_t a) { uint32_t u = MR32(a); float f; memcpy(&f, &u, 4); return f; }
typedef struct { float x, z, y; uint32_t owner; int ok; } Ent;
static int find_ent(uint32_t kind, Ent *out, float nx, float nz) {
    float best = 1e30f; int found = 0;
    for (int i = 0; i < 48; i++) {
        uint32_t b = SPH_BASE + i * SPH_STRIDE;
        if ((MR32(b + 0x44) & 0xff) != kind) continue;
        float x = rf(b + 0x30), y = rf(b + 0x34), z = rf(b + 0x38);
        if (!(fabsf(x) < 1e5f && fabsf(z) < 1e5f)) continue;
        float d = (x - nx) * (x - nx) + (z - nz) * (z - nz);
        if (d < best) { best = d; out->x = x; out->y = y; out->z = z; out->owner = MR32(b + 0x58); found = 1; }
    }
    out->ok = found;
    return found;
}
static uint32_t bot_start; static FILE *bot_log; static uint32_t bot_watch;
/* stick -> velocity map, learned from samples: v = M s (least squares, decaying) */
static double Sss[2][2], Svs[2][2];
static void bot_frame(uint32_t poll, uint16_t *buttons, uint8_t an[4]) {
    static Ent prev; static int have_prev; static float sx = 0, sy = 0; static uint32_t t;
    static int swing_t;
    Ent sp, mk;
    if (!find_ent(0x20, &sp, 0, 0)) { have_prev = 0; return; }
    t++;
    /* learn the stick -> movement map from the last frame */
    if (have_prev && (sx || sy)) {
        double vx = sp.x - prev.x, vz = sp.z - prev.z;
        if (vx * vx + vz * vz < 100) {
            for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { Sss[i][j] *= 0.97; Svs[i][j] *= 0.97; }
            double s[2] = {sx, sy}, v[2] = {vx, vz};
            for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { Sss[i][j] += s[i] * s[j]; Svs[i][j] += v[i] * s[j]; }
        }
    }
    prev = sp; have_prev = 1;
    float nsx, nsy;
    int have_mk = find_ent(0x21, &mk, sp.x, sp.z);
    double det = Sss[0][0] * Sss[1][1] - Sss[0][1] * Sss[1][0];
    if (t < 20) { nsx = 0; nsy = -1; }                 /* probe: forward, then right */
    else if (t < 40) { nsx = 1; nsy = 0; }
    else if (!have_mk || fabs(det) < 1e-6) { nsx = 0; nsy = 0; }
    else {
        /* M = Svs * inv(Sss);  s = inv(M) d */
        double inv[2][2] = {{Sss[1][1] / det, -Sss[0][1] / det}, {-Sss[1][0] / det, Sss[0][0] / det}};
        double M[2][2];
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) M[i][j] = Svs[i][0] * inv[0][j] + Svs[i][1] * inv[1][j];
        double md = M[0][0] * M[1][1] - M[0][1] * M[1][0];
        double dx = mk.x - sp.x, dz = mk.z - sp.z, dist = sqrt(dx * dx + dz * dz);
        double s0 = (M[1][1] * dx - M[0][1] * dz) / md, s1 = (-M[1][0] * dx + M[0][0] * dz) / md;
        double l = sqrt(s0 * s0 + s1 * s1); if (l > 0) { s0 /= l; s1 /= l; }
        if (dist > 10) { nsx = (float)s0; nsy = (float)s1; } else { nsx = (float)s0 * 0.3f; nsy = (float)s1 * 0.3f; }
        /* swing toward the monkey when it is in reach */
        if (dist < 18 && swing_t <= 0) swing_t = 10;
        if (swing_t > 6) { an[0] = (uint8_t)(0x80 + (int)(s0 * 127)); an[1] = (uint8_t)(0x80 + (int)(s1 * 127)); }
        if (swing_t > 0) swing_t--;
        if (bot_log) fprintf(bot_log, "%u spike %.1f %.1f %.1f monkey %.1f %.1f %.1f owner %08x dist %.1f stick %.2f %.2f swing %d\n",
                             poll, sp.x, sp.y, sp.z, mk.x, mk.y, mk.z, mk.owner, dist, nsx, nsy, swing_t);
        if (!bot_watch && dist < 30) bot_watch = mk.owner;
    }
    if (bot_log && (t < 60 || !have_mk)) fprintf(bot_log, "%u t=%u spike %.1f %.1f %.1f (xf %.1f %.1f %.1f) mk=%d det=%g stick %.2f %.2f an %02x%02x%02x%02x\n", poll, t, sp.x, sp.y, sp.z, rf(0xd279a0+0x30), rf(0xd279a0+0x34), rf(0xd279a0+0x38), have_mk, det, nsx, nsy, an[0], an[1], an[2], an[3]);
    sx = nsx; sy = nsy;
    an[2] = (uint8_t)(0x80 + (int)(nsx * 127)); an[3] = (uint8_t)(0x80 + (int)(nsy * 127));
    (void)buttons;
}
/* AE2_WATCHOBJ=addr,bytes[:file]: dump that memory every frame (binary, prefixed by the poll) */
static void watch_frame(uint32_t poll) {
    static int init; static uint32_t wa, wn; static FILE *wf;
    if (!init) { init = 1; const char *e = getenv("AE2_WATCHOBJ"); if (e) { sscanf(e, "%x,%u", &wa, &wn); const char *c = strchr(e, ':'); wf = fopen(c ? c + 1 : "watch.bin", "wb"); } }
    uint32_t a = wa ? wa : bot_watch;
    if (!wf || !a) return;
    if (!wn) wn = 0x400;
    fwrite(&poll, 4, 1, wf); fwrite(&a, 4, 1, wf);
    for (uint32_t i = 0; i < wn; i += 4) { uint32_t v = MR32(a + i); fwrite(&v, 4, 1, wf); }
}

/* ------------------------------------------------------------------ per frame */
__attribute__((weak)) void debug_menu_frame(uint32_t poll, uint16_t *buttons, uint8_t an[4]) { (void)poll; (void)buttons; (void)an; }
void debug_pad_frame(uint32_t poll, uint16_t *buttons, uint8_t an[4]) {
    static int init; static long exit_at = -2;
    if (exit_at == -2) exit_at = getenv("AE2_EXIT_AT") ? atol(getenv("AE2_EXIT_AT")) : -1;   /* benchmarks: quit at that game frame */
    if (exit_at >= 0 && poll >= (uint32_t)exit_at) { void prof_report(void); prof_report(); void vu1_print_stats(void); vu1_print_stats(); fflush(NULL); _exit(0); }
    if (!init) {
        init = 1;
        const char *b = getenv("AE2_BOT"); if (b) bot_start = (uint32_t)atoi(b);
        const char *l = getenv("AE2_BOTLOG"); if (l) bot_log = fopen(l, "w");
    }
    pokes_run(poll);
    { const char *e = getenv("AE2_HEXDUMP");   /* "poll,addr,len;..." prints guest memory at those polls */
      static uint32_t last; if (e && poll != last) { last = poll; char buf[512]; snprintf(buf, sizeof buf, "%s", e);
        for (char *sv, *t = strtok_r(buf, ";", &sv); t; t = strtok_r(NULL, ";", &sv)) { unsigned p, a, n; if (sscanf(t, "%u,%x,%x", &p, &a, &n) == 3 && p == poll) {
          for (unsigned o = 0; o < n; o += 32) { INFO("[hex] %u %08x:", poll, a + o); for (unsigned k = 0; k < 32 && o + k < n; k += 4) INFO(" %08x", MR32(a + o + k)); INFO("\n"); } } } } }
    if (rep_on) {
        if (poll >= 1 && poll <= nrep) { RecFrame *r = &rep[poll - 1]; *buttons = r->buttons; memcpy(an, r->an, 4); }
        else if (poll == nrep + 1) INFO("[debug] replay finished at frame %u; live input from here\n", poll);
    }
    if (bot_start && poll >= bot_start) bot_frame(poll, buttons, an);
    debug_menu_frame(poll, buttons, an);
    if (rec_f) {
        RecFrame r = {*buttons, {an[0], an[1], an[2], an[3]}, 0};
        fwrite(&r, sizeof r, 1, rec_f);
        if ((poll & 63) == 0) fflush(rec_f);
    }
    watch_frame(poll);
}

/* ------------------------------------------------------------------ FP operand check (CHECKS=1 builds) */
#ifdef AE2_FPCHECK
uint32_t g_last_fn;
__thread uint32_t g_fstack[256]; __thread int g_fdepth;
void rt_fpcheck(const char *op, float a, float b) {
    uint32_t ua, ub; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
    int ba = (ua & 0x7f800000) == 0x7f800000, bb = (ub & 0x7f800000) == 0x7f800000;
    if (!ba && !bb) return;
    static struct { uint32_t fn; const char *op; uint32_t n; } seen[4096]; static int nseen;
    extern uint32_t g_pad_polls;
    for (int i = 0; i < nseen; i++) if (seen[i].fn == g_last_fn && seen[i].op == op) { if (++seen[i].n % 1000) return; goto log; }
    if (nseen < 4096) { seen[nseen].fn = g_last_fn; seen[nseen].op = op; seen[nseen].n = 1; nseen++; }
log:
    fprintf(stderr, "[fpcheck] frame %u: %s with %08x, %08x in function %08x\n", g_pad_polls, op, ua, ub, g_last_fn);
}
#endif
#ifdef AE2_FPCHECK
uint32_t g_ww_lo = 0xffffffffu, g_ww_hi;
static void __attribute__((constructor)) ww_init(void) {
    const char *e = getenv("AE2_WWATCH"); unsigned a, n;
    if (e && sscanf(e, "%x,%x", &a, &n) == 2) { g_ww_lo = a; g_ww_hi = a + n; }
}
uint32_t g_rw_lo = 0xffffffffu, g_rw_hi;
static uint32_t rw_base, rw_len, rw_stride, rw_off, rw_ptr;
/* AE2_RWATCH=addr,len[,stride,off] or AE2_RWATCH=*ptr,len,... (the range starts at the
 * address stored at ptr, read again every frame) */
void rt_rwatch_frame(void) {
    static int init; if (!init) { init = 1; const char *e = getenv("AE2_RWATCH"); if (!e) return;
        if (*e == '*') { rw_ptr = 1; e++; }
        sscanf(e, "%x,%x,%x,%x", &rw_base, &rw_len, &rw_stride, &rw_off); }
    if (!rw_len) return;
    uint32_t b = rw_base; if (rw_ptr) { uint32_t lo = g_rw_lo; g_rw_lo = 0xffffffffu; b = MR32(rw_base); g_rw_lo = lo; }
    g_rw_lo = b; g_rw_hi = b + rw_len;
}
void rt_rwatch(uint32_t a, int n) {
    if (rw_stride && ((a - g_rw_lo) % rw_stride + (uint32_t)n <= rw_off || (a - g_rw_lo) % rw_stride >= rw_off + 4)) return;
    static struct { uint32_t fn, caller, n; } seen[1024]; static int nseen;
    uint32_t fn = g_last_fn, caller = g_fdepth >= 2 && g_fdepth <= 256 ? g_fstack[g_fdepth - 2] : 0;
    for (int i = 0; i < nseen; i++) if (seen[i].fn == fn && seen[i].caller == caller) { seen[i].n++; return; }
    if (nseen < 1024) { seen[nseen].fn = fn; seen[nseen].caller = caller; seen[nseen].n = 1; nseen++; }
    extern uint32_t g_pad_polls; char st[400] = ""; int k = 0;
    for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 12; d--) if (d < 256) k += snprintf(st + k, sizeof st - (size_t)k, " <%06x", g_fstack[d]);
    fprintf(stderr, "[rwatch] frame %u: read%d %08x (+%x) in%s\n", g_pad_polls, n * 8, a, rw_stride ? (a - g_rw_lo) % rw_stride : a - g_rw_lo, st);
}
void rt_wwatch(uint32_t a, int n, uint64_t v) {
    extern uint32_t g_pad_polls; static int nl; static long from = -1;
    if (from < 0) { const char *e = getenv("AE2_WWATCH_FROM"); from = e ? atol(e) : 0; }
    if (g_pad_polls < (uint32_t)from) return;
    if (nl++ < 2000) {
        char st[400] = ""; int k = 0;
        for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 24; d--) if (d < 256) k += snprintf(st + k, sizeof st - (size_t)k, " <%08x", g_fstack[d]);
        fprintf(stderr, "[wwatch] frame %u: write%d %08x = %llx in function %08x stack%s\n", g_pad_polls, n * 8, a, (unsigned long long)v, g_last_fn, st);
    }
}
#endif
