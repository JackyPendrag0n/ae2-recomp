/* Host implementations of the IOP RPC servers the game binds to. */
#include "rt.h"
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

/* ======================================================== host file lookup */
/* Case-insensitive resolution of a relative path under the game directory. */
static int resolve_ci(const char *rel, char *out, size_t outsz) {
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", g_game_dir);
    const char *p = rel;
    while (*p == '/' || *p == '\\') p++;
    while (*p) {
        char comp[256]; int n = 0;
        while (*p && *p != '/' && *p != '\\' && n < 255) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/' || *p == '\\') p++;
        DIR *d = opendir(cur);
        if (!d) return 0;
        struct dirent *e; int found = 0;
        while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) { found = 1; break; }
        if (found) { size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", e->d_name); }
        closedir(d);
        if (!found) return 0;
    }
    snprintf(out, outsz, "%s", cur);
    return 1;
}
FILE *host_open_game_file(const char *rel, const char *mode) {
    char path[1024];
    if (!resolve_ci(rel, path, sizeof path)) return NULL;
    return fopen(path, mode);
}

/* ======================================================== CD/DVD image model */
typedef struct { char path[96]; uint32_t lsn, count, size; FILE *fp; int missing; } DiscFile;
static DiscFile *dfiles;
static int ndfiles;
static uint32_t next_synth_lsn = 1000000;

static int cmp_lsn(const void *a, const void *b) {
    const DiscFile *x = a, *y = b;
    return x->lsn < y->lsn ? -1 : x->lsn > y->lsn;
}

static DiscFile *add_file(const char *path, uint32_t lsn, uint32_t count) {
    dfiles = realloc(dfiles, sizeof(DiscFile) * (ndfiles + 1));
    DiscFile *f = &dfiles[ndfiles++];
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->lsn = lsn; f->count = count;
    char full[1024];
    if (resolve_ci(path, full, sizeof full)) {
        struct stat st;
        if (!stat(full, &st)) f->size = (uint32_t)st.st_size;
    } else f->missing = 1;
    return f;
}

void cdvd_init(void) {
    FILE *fp = host_open_game_file("SARU2.MM", "rb");
    if (!fp) rt_fatal("SARU2.MM not found in game directory '%s'", g_game_dir);
    static uint8_t mm[65536];
    size_t n = fread(mm, 1, sizeof mm, fp);
    fclose(fp);
    int missing = 0;
    for (size_t i = 0; i + 0x50 <= n; ) {
        if (mm[i] == '/') {
            char name[64]; memcpy(name, mm + i, 60); name[60] = 0;
            uint32_t v[5]; memcpy(v, mm + i + 0x3c, 20);
            const char *rel = name;
            if (!strncasecmp(rel, "/APE2_ENG/", 10)) rel += 10;
            DiscFile *f = add_file(rel, v[0], v[2]);
            if (f->missing) missing++;
            i += 0x50;
        } else i += 4;
    }
    add_file("SYSTEM.CNF", next_synth_lsn, 1); next_synth_lsn += 16;
    add_file("SCES_508.85", next_synth_lsn, 4400 / 2 + 2200); next_synth_lsn += 4400;
    qsort(dfiles, ndfiles, sizeof(DiscFile), cmp_lsn);
    INFO("[cdvd] %d files in disc table, %d not present (movies/streams)\n", ndfiles, missing);
}

static DiscFile *file_for_lsn(uint32_t lsn) {
    int lo = 0, hi = ndfiles - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (dfiles[mid].lsn <= lsn) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return NULL;
    DiscFile *f = &dfiles[best];
    if (lsn >= f->lsn + f->count) return NULL;
    return f;
}

static int warned_missing;
int cdvd_read_sectors(uint32_t lsn, uint32_t count, void *dst) {
    uint8_t *d = dst;
    for (uint32_t i = 0; i < count; i++, d += 2048) {
        DiscFile *f = file_for_lsn(lsn + i);
        memset(d, 0, 2048);
        if (!f) continue;
        if (f->missing) {
            if (warned_missing++ < 20) WARN("[cdvd] read from missing file %s\n", f->path);
            continue;
        }
        if (!f->fp) f->fp = host_open_game_file(f->path, "rb");
        if (!f->fp) continue;
        fseek(f->fp, (long)(lsn + i - f->lsn) * 2048, SEEK_SET);
        size_t got = fread(d, 1, 2048, f->fp);
        (void)got;
    }
    return 1;
}

int cdvd_search(const char *name, uint32_t *lsn, uint32_t *size) {
    char norm[256]; int j = 0;
    const char *p = name;
    while (*p == '\\' || *p == '/') p++;
    for (; *p && *p != ';' && j < 255; p++) norm[j++] = (*p == '\\') ? '/' : toupper((unsigned char)*p);
    norm[j] = 0;
    if (!strncmp(norm, "APE2_ENG/", 9)) memmove(norm, norm + 9, strlen(norm + 9) + 1);
    for (int i = 0; i < ndfiles; i++) {
        if (!strcasecmp(dfiles[i].path, norm)) {
            *lsn = dfiles[i].lsn;
            *size = dfiles[i].size ? dfiles[i].size : dfiles[i].count * 2048;
            return !dfiles[i].missing || 1;
        }
    }
    WARN("[cdvd] search: '%s' not found\n", name);
    return 0;
}

/* a stage file (STAGE/.../<code>.BIN) is in the disc table: the scene table also lists
 * areas that never shipped (BEA_B1), and loading one searches for an empty name forever */
int cdvd_stage_on_disc(const char *code) {
    char want[32]; snprintf(want, sizeof want, "/%s.BIN", code);
    size_t wl = strlen(want);
    for (int i = 0; i < ndfiles; i++) {
        size_t l = strlen(dfiles[i].path);
        if (!strncasecmp(dfiles[i].path, "STAGE/", 6) && l > wl && !strcasecmp(dfiles[i].path + l - wl, want)) return 1;
    }
    return 0;
}

/* ======================================================== servers */
static uint32_t result_area;   /* scratch IOP memory for replies */
#define RES(off) (*(uint32_t *)IOP(result_area + (off)))

static uint32_t srv_cd_init(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)fno; (void)buf; (void)size; (void)rs;
    memset(IOP(result_area), 0, 64);
    RES(0) = 1;
    return result_area;
}
static uint32_t srv_cd_diskready(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)fno; (void)buf; (void)size; (void)rs;
    memset(IOP(result_area), 0, 64);
    RES(0) = 2;  /* SCECdComplete */
    return result_area;
}
static uint32_t srv_cd_scmd(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)buf; (void)size; (void)rs;
    memset(IOP(result_area), 0, 0x420);
    switch (fno) {
    case 1: { /* ReadClock: retres, then sceCdCLOCK {stat, sec, min, hour, pad, day, month, year} BCD */
        RES(0) = 1;
        uint8_t *ck = IOP(result_area + 4);
        ck[0] = 0; ck[1] = 0x00; ck[2] = 0x30; ck[3] = 0x12; ck[4] = 0; ck[5] = 0x15; ck[6] = 0x10; ck[7] = 0x02;
        break;
    }
    case 3: RES(0) = 0x14; break;   /* disk type: PS2 DVD */
    case 4: RES(0) = 0; break;      /* GetError: none */
    case 5: RES(0) = 1; RES(4) = 0; break;  /* TrayReq */
    case 12: RES(0) = 0x0A; break;  /* status: paused */
    default: RES(0) = 1; break;
    }
    TRACE("[cdvd] S cmd %d\n", fno);
    return result_area;
}
static uint32_t srv_cd_search(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)fno; (void)rs;
    uint8_t *in = IOP(buf);
    char path[257];
    int has_attr = (size == 0x128 || size == 0x12c);
    const char *ppath = (const char *)in + 32 + (has_attr ? 4 : 0);
    memcpy(path, ppath, 256); path[256] = 0;
    uint32_t eedest = *(uint32_t *)(in + 32 + (has_attr ? 4 : 0) + 256);
    uint32_t lsn = 0, fsize = 0;
    int ok = cdvd_search(path, &lsn, &fsize);
    INFO("[cdvd] search '%s' -> %s lsn=%u size=%u\n", path, ok ? "ok" : "MISSING", lsn, fsize);
    /* sceCdlFILE: lsn, size, name[16], date[8] */
    uint8_t fp[36] = {0};
    memcpy(fp, &lsn, 4); memcpy(fp + 4, &fsize, 4);
    const char *base = strrchr(path, '\\'); base = base ? base + 1 : path;
    strncpy((char *)fp + 8, base, 15);
    if (eedest) gwrite(eedest, fp, has_attr ? 36 : 32);
    memset(IOP(result_area), 0, 16);
    RES(0) = ok;
    return result_area;
}
static uint32_t srv_cd_ncmd(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)size; (void)rs;
    uint32_t *in = (uint32_t *)IOP(buf);
    memset(IOP(result_area), 0, 64);
    RES(0) = 1;
    switch (fno) {
    case 1: case 19: { /* ReadEE: lbn, sectors, paddr, rmode, eeremaindest, eedest */
        uint32_t lbn = in[0], nsec = in[1], dst = in[2], remain = in[4], posdest = in[5];
        uint8_t *tmp = malloc((size_t)nsec * 2048 + 1);
        cdvd_read_sectors(lbn, nsec, tmp);
        gwrite(dst, tmp, nsec * 2048);
        free(tmp);
        if (remain) { uint32_t z[36] = {0}; gwrite(remain, z, 144); }
        if (posdest) { uint32_t pos = nsec * 2048; gwrite(posdest, &pos, 4); }
        TRACE("[cdvd] read lsn=%u n=%u -> %08x\n", lbn, nsec, dst);
        break;
    }
    case 4: { uint8_t toc[0x810] = {0}; gwrite(in[0], toc, sizeof toc); break; }
    case 9: { /* CD streaming (sceCdSt*): {lbn, nsectors, ee_buf, cmd, mode} */
        static uint32_t st_lsn, st_bufmax = 64; static int st_active;
        uint32_t lbn = in[0], n = in[1], dst = in[2], cmd = in[3];
        { static int nl, tr = -1; if (tr < 0) tr = getenv("AE2_STTRACE") != NULL; if (tr && nl < 300) { nl++; INFO("[cdst] cmd %u lbn %u n %u dst %08x mode %u (frame %u)\n", cmd, lbn, n, dst, in[4], g_frame); } }
        switch (cmd) {
        case 1: st_lsn = lbn; st_active = 1; RES(0) = 1; INFO("[cdvd] stream start lsn=%u\n", lbn); break;
        case 2: {
            if (!st_active) { RES(0) = 0; break; }
            if (n > 512) n = 512;
            uint8_t *tmp = malloc((size_t)n * 2048);
            cdvd_read_sectors(st_lsn, n, tmp);
            gwrite(dst, tmp, n * 2048);
            free(tmp);
            st_lsn += n;
            RES(0) = n;
            break;
        }
        case 3: st_active = 0; RES(0) = 1; break;
        case 4: case 9: st_lsn = lbn; RES(0) = 1; break;
        case 5: st_bufmax = lbn ? lbn : 64; RES(0) = 1; break;
        case 6: RES(0) = st_active ? st_bufmax : 0; break;
        default: RES(0) = 1; break;
        }
        break;
    }
    case 5: case 6: case 7: case 8: break;
    case 14: RES(0) = 2; break;
    default: WARN("[cdvd] N cmd %d unimplemented\n", fno); break;
    }
    return result_area;
}

/* ---- loadfile (sceSifLoadModule etc.) */
static int next_modid = 10;
static uint32_t srv_loadfile(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)size; (void)rs;
    char path[256] = {0};
    memcpy(path, IOP(buf + 8), 252);
    INFO("[iop] loadfile fno=%d size=%d '%s' raw:", fno, size, path);
    for (int i = 0; i < 8; i++) INFO(" %08x", *(uint32_t *)IOP(buf + i * 4));
    INFO("\n");
    memset(IOP(result_area), 0, 16);
    RES(0) = next_modid++;  /* module id / result */
    RES(4) = 0;             /* module's own return value */
    return result_area;
}

/* ---- IOP heap */
static uint32_t srv_iopheap(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)size; (void)rs;
    uint32_t *in = (uint32_t *)IOP(buf);
    memset(IOP(result_area), 0, 16);
    switch (fno) {
    case 1: RES(0) = iop_alloc(in[0]); TRACE("[iop] heap alloc %x -> %x\n", in[0], RES(0)); break;
    case 2: iop_free(in[0]); RES(0) = 0; break;
    case 3: {
        char path[256] = {0}; memcpy(path, IOP(buf + 4), 252);
        INFO("[iop] load to heap %08x '%s'\n", in[0], path);
        RES(0) = 0; break;
    }
    default: RES(0) = 0; break;
    }
    return result_area;
}

/* ---- IOPMISC.IRX ('imsc'): IOP memory helpers */
static uint32_t srv_iopmisc(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)size; (void)rs;
    uint32_t *in = (uint32_t *)IOP(buf);
    switch (fno) {
    case 0: RES(0) = 1; break;                       /* free */
    case 1: RES(0) = 0x100000; break;                /* total free */
    case 2: RES(0) = 0x0C0000; break;                /* max free block */
    case 3: memmove(IOP(in[0]), IOP(in[1]), in[2] & 0x1fffff); RES(0) = in[0]; break;  /* memcpy */
    default: RES(0) = 0; break;
    }
    TRACE("[iopmisc] fno=%d\n", fno);
    return result_area;
}

/* ---- generic stub: logs once per (sid,fno) and replies with zeros */
static uint32_t srv_stub(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)buf; (void)rs;
    if (sid == 0x80000900 || sid == 0x8000091b) {
        static int nd; if (nd++ < 30) { uint32_t *w = (uint32_t *)IOP(buf); INFO("[dbc] sid=%x fno=%x size=%d:", sid, fno, size);
            for (int i = 0; i < 12; i++) INFO(" %08x", w[i]); INFO("\n"); }
    }
    static uint32_t seen[512]; static int nseen;
    uint32_t key = (sid << 8) ^ (uint32_t)fno;
    int s = 0; for (int i = 0; i < nseen; i++) if (seen[i] == key) { s = 1; break; }
    if (!s && nseen < 512) { seen[nseen++] = key; INFO("[iop] stub rpc sid=%08x fno=%d size=%d\n", sid, fno, size); }
    memset(IOP(result_area), 0, 0x400);
    if (sid == 0x80000900) {
        uint32_t *in = (uint32_t *)IOP(buf);
        if (fno == (int)0x80000963) RES(0) = 0x200;            /* dbcman version 2.0 */
        if (fno == (int)0x80000901) { memcpy(IOP(result_area), in, 0x30); RES(0x24) = in[2]; }  /* socket id = port */
        if (fno == (int)0x80000904) RES(0) = 1;
    }
    return result_area;
}

void iop_services_init(void) {
    result_area = iop_alloc(0x2000);
    cdvd_init();
    iop_register_server(0x80000592, srv_cd_init, 0x40);
    iop_register_server(0x80000593, srv_cd_scmd, 0x420);
    iop_register_server(0x80000595, srv_cd_ncmd, 0x420);
    iop_register_server(0x80000596, srv_cd_ncmd, 0x420);
    iop_register_server(0x80000597, srv_cd_search, 0x140);
    iop_register_server(0x8000059A, srv_cd_diskready, 0x40);
    iop_register_server(0x8000059C, srv_cd_diskready, 0x40);
    iop_register_server(0x80000006, srv_loadfile, 0x400);
    iop_register_server(0x80000003, srv_iopheap, 0x400);
    iop_register_server(0x80000001, srv_stub, 0x1000);
    { void iop_memcard_init(void); iop_memcard_init(); }
    iop_register_server(0x63736d69, srv_iopmisc, 0x100);      /* 'imsc' IOPMISC.IRX */
    { void iop_sound_init(void); iop_sound_init(); }          /* 'sndn' SNDN2DRV.IRX */
    static const uint32_t misc[] = {0x80000080, 0x800000ff, 0x80000011, 0x80000012, 0x80000013,
        0x80000900, 0x80000901, 0x80000902, 0x80000903, 0x80000904, 0x80000915, 0x80000916, 0x80000917,
        0x80000918, 0x80000919, 0x8000091a, 0x8000091b, 0x80004000, 0x80008000, 0x80000100, 0x80000101};
    for (unsigned i = 0; i < sizeof misc / sizeof misc[0]; i++) iop_register_server(misc[i], srv_stub, 0x2000);
}
