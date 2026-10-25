/* VIF0/VIF1: command stream decoding, UNPACK (all formats, masking, modes,
 * skip/fill cycles), MPG uploads, double-buffer bookkeeping, DIRECT -> GIF. */
#include "rt.h"

void vu1_exec(uint32_t addr);
extern uint32_t g_vif1_top, g_vif1_itop;

typedef struct {
    int unit;
    /* registers */
    uint32_t cl, wl, mode, mask, row[4], col[4];
    uint32_t base, ofst, tops, top, itops, itop, dbf;
    uint32_t mark;
    /* command in progress */
    uint32_t code;
    int cmd;
    uint32_t need;            /* words still required */
    uint32_t got;
    uint32_t buf[4096 + 8];   /* collected data words */
    uint32_t direct_left;     /* qwords remaining for DIRECT */
    uint32_t dqw[4]; int dqn;
} Vif;

static Vif vifs[2];
static uint64_t vif_cmd_count[128];

static uint8_t *vu_data(int unit) { return unit ? g_vu1mem : g_vu0mem; }
static uint32_t vu_qmask(int unit) { return unit ? 0x3ff : 0xff; }

static const uint8_t vsize_tab[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

static inline void write_comp(Vif *v, uint32_t *dst, int comp, uint32_t data, int domask, int cl) {
    int n = 0;
    if (domask) n = (v->mask >> ((cl > 3 ? 3 : cl) * 8 + comp * 2)) & 3;
    switch (n) {
    case 0:
        switch (v->mode) {
        case 1: dst[comp] = data + v->row[comp]; break;
        case 2: v->row[comp] += data; dst[comp] = v->row[comp]; break;
        case 3: v->row[comp] = data; dst[comp] = data; break;
        default: dst[comp] = data; break;
        }
        break;
    case 1: dst[comp] = v->row[comp]; break;
    case 2: dst[comp] = v->col[cl > 3 ? 3 : cl]; break;
    default: break;
    }
}

static uint32_t unpack_words(Vif *v, int cmd, uint32_t num) {
    uint32_t vs = vsize_tab[cmd & 15];
    if (!vs) return 0;
    uint32_t n;
    if (v->wl <= v->cl || v->wl == 0) n = num;
    else n = v->cl * (num / v->wl) + (num % v->wl < v->cl ? num % v->wl : v->cl);
    return (n * vs + 3) / 4;
}

/* one element of an UNPACK, widened to four 32-bit words; fmt = cmd & 15 is a constant
 * at every call site below, so each format compiles to straight-line code */
static inline __attribute__((always_inline)) void unpack_elem(int fmt, int usn, const uint8_t *src, uint32_t e[4]) {
    uint16_t h[4]; int k;
    switch (fmt) {
    case 0x0: memcpy(e, src, 4); e[1] = e[2] = e[3] = e[0]; break;
    case 0x1: memcpy(h, src, 2); e[0] = e[1] = e[2] = e[3] = usn ? h[0] : (uint32_t)(int32_t)(int16_t)h[0]; break;
    case 0x2: e[0] = e[1] = e[2] = e[3] = usn ? src[0] : (uint32_t)(int32_t)(int8_t)src[0]; break;
    case 0x4: memcpy(e, src, 8); e[2] = e[0]; e[3] = e[1]; break;
    case 0x5: memcpy(h, src, 4); for (k = 0; k < 2; k++) e[k] = usn ? h[k] : (uint32_t)(int32_t)(int16_t)h[k]; e[2] = e[0]; e[3] = e[1]; break;
    case 0x6: for (k = 0; k < 2; k++) e[k] = usn ? src[k] : (uint32_t)(int32_t)(int8_t)src[k]; e[2] = e[0]; e[3] = e[1]; break;
    /* V3 reads four components (hardware behaviour), like V4 */
    case 0x8: case 0xc: memcpy(e, src, 16); break;
    case 0x9: case 0xd: memcpy(h, src, 8); for (k = 0; k < 4; k++) e[k] = usn ? h[k] : (uint32_t)(int32_t)(int16_t)h[k]; break;
    case 0xa: case 0xe: for (k = 0; k < 4; k++) e[k] = usn ? src[k] : (uint32_t)(int32_t)(int8_t)src[k]; break;
    }
}
/* no filling, MODE 0 or 1 (offset by ROW), any MASK: act[row][comp] is the mask code */
static inline __attribute__((always_inline)) void unpack_loop(Vif *v, int fmt, int usn, int masked, int offset, uint8_t *mem, uint32_t qmask,
                                                               uint32_t addr, const uint8_t *src, uint32_t num, uint32_t wl, uint32_t clc) {
    uint32_t vs = vsize_tab[fmt], cl = 0, add[4] = {0, 0, 0, 0};
    uint8_t act[4][4];
    if (offset) memcpy(add, v->row, 16);
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) act[r][k] = masked ? (v->mask >> (r * 8 + k * 2)) & 3 : 0;
    for (uint32_t i = 0; i < num; i++) {
        uint32_t *dst = (uint32_t *)(mem + (addr & qmask) * 16);
        uint32_t e[4];
        unpack_elem(fmt, usn, src, e);
        if (!masked) {
            if (offset) for (int k = 0; k < 4; k++) e[k] += add[k];
            memcpy(dst, e, 16);
        } else {
            const uint8_t *a = act[cl > 3 ? 3 : cl];
            for (int k = 0; k < 4; k++)
                switch (a[k]) {
                case 0: dst[k] = e[k] + add[k]; break;
                case 1: dst[k] = v->row[k]; break;
                case 2: dst[k] = v->col[cl > 3 ? 3 : cl]; break;
                default: break;
                }
        }
        addr++; src += vs;
        if (++cl >= wl) { addr += (clc - wl); cl = 0; }
    }
}
#define UNPACK_FORMATS(X) X(0x0) X(0x1) X(0x2) X(0x4) X(0x5) X(0x6) X(0x8) X(0x9) X(0xa) X(0xc) X(0xd) X(0xe)

static void do_unpack(Vif *v) {
    int cmd = v->cmd;
    { static int nl; static int skip = -1; if (skip == -1) skip = getenv("AE2_UNPK") ? atoi(getenv("AE2_UNPK")) : -2;
      if (skip >= 0 && nl < skip + 40) { if (nl >= skip)
        fprintf(stderr, "[unpk] cmd=%02x addr=%03x num=%d cl=%d wl=%d mode=%d mask=%08x row=%08x,%08x,%08x,%08x tops=%03x data0=%08x %08x %08x %08x\n",
            cmd, v->code & 0x3ff | ((v->code >> 15 & 1) << 15), (v->code >> 16) & 0xff, v->cl, v->wl, v->mode, v->mask, v->row[0], v->row[1], v->row[2], v->row[3], v->tops,
            v->buf[0], v->buf[1], v->buf[2], v->buf[3]);
        nl++; } }
    uint32_t imm = v->code & 0xffff;
    uint32_t num = (v->code >> 16) & 0xff; if (!num) num = 256;
    int vn = (cmd >> 2) & 3, vl = cmd & 3;
    int domask = (cmd >> 4) & 1;
    int usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
    uint32_t addr = imm & 0x3ff;
    if (flg && v->unit == 1) addr += v->tops;
    uint32_t qmask = vu_qmask(v->unit);
    uint8_t *mem = vu_data(v->unit);
    const uint8_t *src = (const uint8_t *)v->buf;
    uint32_t vs = vsize_tab[cmd & 15];
    int isfill = v->cl < v->wl;
    uint32_t wl = v->wl ? v->wl : 1, clc = v->cl;
    uint32_t cl = 0;
    int mode_save = v->mode;
    if (vn == 3 && vl == 3) v->mode = 0;  /* V4-5 ignores MODE */
    /* the common cases (no filling; no row arithmetic or MODE 1 offset; any mask): one
     * specialised loop per format and variant */
    if (!isfill && v->mode <= 1 && ((0x7777 >> (cmd & 15)) & 1)) {   /* every format but V4-5 (and the invalid ones) */
        int masked = domask && v->mask != 0, offset = v->mode == 1, var = masked * 2 + offset;
        switch ((cmd & 15) * 4 + var) {
#define UNPACK_CASE(F) \
        case F * 4 + 0: unpack_loop(v, F, usn, 0, 0, mem, qmask, addr, src, num, wl, clc); break; \
        case F * 4 + 1: unpack_loop(v, F, usn, 0, 1, mem, qmask, addr, src, num, wl, clc); break; \
        case F * 4 + 2: unpack_loop(v, F, usn, 1, 0, mem, qmask, addr, src, num, wl, clc); break; \
        case F * 4 + 3: unpack_loop(v, F, usn, 1, 1, mem, qmask, addr, src, num, wl, clc); break;
        UNPACK_FORMATS(UNPACK_CASE)
#undef UNPACK_CASE
        }
        v->mode = mode_save;
        return;
    }
    for (uint32_t i = 0; i < num; i++) {
        uint32_t *dst = (uint32_t *)(mem + (addr & qmask) * 16);
        uint32_t e[4] = {0, 0, 0, 0};
        int nc = vn + 1;
        if (vn == 3 && vl == 3) {
            uint16_t d; memcpy(&d, src, 2);
            e[0] = (d & 0x1f) << 3; e[1] = (d & 0x3e0) >> 2; e[2] = (d & 0x7c00) >> 7; e[3] = (d & 0x8000) >> 8;
            nc = 4;
        } else {
            int esz = 4 >> vl;   /* bytes per component */
            int ncr = (vn == 2) ? 4 : nc;  /* V3 reads 4 components (hardware behaviour) */
            for (int k = 0; k < ncr; k++) {
                const uint8_t *p = src + k * esz;
                uint32_t x;
                if (esz == 4) memcpy(&x, p, 4);
                else if (esz == 2) { uint16_t h; memcpy(&h, p, 2); x = usn ? h : (uint32_t)(int32_t)(int16_t)h; }
                else x = usn ? p[0] : (uint32_t)(int32_t)(int8_t)p[0];
                e[k] = x;
            }
            if (vn == 0) e[1] = e[2] = e[3] = e[0];
            else if (vn == 1) { e[2] = e[0]; e[3] = e[1]; }
        }
        for (int k = 0; k < 4; k++) write_comp(v, dst, k, e[k], domask, (int)cl);
        addr++;
        cl++;
        if (isfill) {
            if (cl <= clc) src += vs;
            else if (cl == wl) cl = 0;
            if (cl == wl) cl = 0;
        } else {
            src += vs;
            if (cl >= wl) { addr += (clc - wl); cl = 0; }
        }
    }
    v->mode = mode_save;
}

static void vif_mscal(Vif *v, uint32_t addr) {
    if (v->unit == 1) {
        v->top = v->tops;
        g_vif1_top = v->top;
        g_vif1_itop = v->itops;
        v->itop = v->itops;
        if (v->dbf) { v->tops = v->base; v->dbf = 0; }
        else { v->tops = v->base + v->ofst; v->dbf = 1; }
        if (addr == 0xffffffffu) { void vu1_continue(void); vu1_continue(); }
        else vu1_exec(addr);
    } else {
        Ctx dummy; memset(&dummy, 0, sizeof dummy);
        (void)dummy;
        /* VU0 micro via VIF0: run on the shared VU0 state is uncommon; ignore */
    }
}

static uint32_t last_mscal_addr[2];

static void vif_exec_code(Vif *v) {
    uint32_t code = v->code;
    int cmd = (code >> 24) & 0x7f;
    uint32_t imm = code & 0xffff;
    v->cmd = cmd;
    vif_cmd_count[cmd]++;
    v->need = 0; v->got = 0;
    switch (cmd) {
    case 0x00: break;
    case 0x01: v->cl = imm & 0xff; v->wl = (imm >> 8) & 0xff; break;
    case 0x02: v->ofst = imm & 0x3ff; v->dbf = 0; v->tops = v->base; break;
    case 0x03: v->base = imm & 0x3ff; break;
    case 0x04: v->itops = imm & 0x3ff; break;
    case 0x05: v->mode = imm & 3; break;
    case 0x06: break;  /* MSKPATH3 */
    case 0x07: v->mark = imm; break;
    case 0x10: case 0x11: case 0x13: break;  /* flushes: synchronous model */
    case 0x14: case 0x15: last_mscal_addr[v->unit] = imm * 8;
        { static uint32_t seen[64]; static int ns; int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == (imm * 8 | (v->unit << 20))) f = 1;
          if (!f && ns < 64) { seen[ns++] = imm * 8 | (v->unit << 20); INFO("[vif%d] new MSCAL entry %04x\n", v->unit, imm * 8); } }
        vif_mscal(v, imm * 8); break;
    case 0x17: vif_mscal(v, 0xffffffffu); break;   /* MSCNT: continue from TPC */
    case 0x20: v->need = 1; break;
    case 0x30: case 0x31: v->need = 4; break;
    case 0x4A: { uint32_t num = (code >> 16) & 0xff; if (!num) num = 256; v->need = num * 2; break; }
    case 0x50: case 0x51: v->direct_left = imm ? imm : 65536; v->dqn = 0; break;
    default:
        if (cmd >= 0x60) {
            uint32_t num = (code >> 16) & 0xff; if (!num) num = 256;
            v->need = unpack_words(v, cmd, num);
            if (!v->need) do_unpack(v);
        } else {
            static int n; if (n++ < 20) WARN("[vif%d] unknown command %02x (%08x)\n", v->unit, cmd, code);
        }
        break;
    }
    if (code & 0x80000000u) kernel_raise_intc(v->unit ? 5 : 4);
}

static void vif_finish_data(Vif *v) {
    switch (v->cmd) {
    case 0x20: v->mask = v->buf[0]; break;
    case 0x30: memcpy(v->row, v->buf, 16); break;
    case 0x31: memcpy(v->col, v->buf, 16); break;
    case 0x4A: {
        uint32_t addr = (v->code & 0xffff) * 8;
        uint8_t *code = v->unit ? g_vu1code : g_vu0code;
        uint32_t cm = v->unit ? 0x3fff : 0xfff;
        { extern uint32_t g_vucode_version[2]; int changed = 0;
          for (uint32_t i = 0; i < v->need; i++) {
              uint8_t *dst = code + ((addr + i * 4) & cm);
              if (memcmp(dst, &v->buf[i], 4)) { memcpy(dst, &v->buf[i], 4); changed = 1; }
          }
          if (changed) g_vucode_version[v->unit]++; }
        break;
    }
    default: if (v->cmd >= 0x60) do_unpack(v); break;
    }
    v->need = 0;
}

static void vif_word(Vif *v, uint32_t w) {
    if (v->direct_left) {
        v->dqw[v->dqn++] = w;
        if (v->dqn == 4) {
            gif_transfer((const uint8_t *)v->dqw, 1, 2);
            v->dqn = 0;
            v->direct_left--;
        }
        return;
    }
    if (v->need) {
        if (v->got < 4096) v->buf[v->got] = w;
        v->got++;
        if (v->got >= v->need) { v->buf[v->got < 4096 ? v->got : 4095] = 0; vif_finish_data(v); }
        return;
    }
    v->code = w;
    vif_exec_code(v);
}

void vif_transfer(int unit, const uint8_t *data, uint32_t qwc) {
    Vif *v = &vifs[unit & 1];
    const uint32_t *w = (const uint32_t *)data;
    uint32_t nw = qwc * 4;
    for (uint32_t i = 0; i < nw;) {
        /* bulk paths: DIRECT/DIRECTHL qwords and command payloads are copied in runs */
        if (v->direct_left && v->dqn == 0 && nw - i >= 4) {
            uint32_t k = (nw - i) / 4; if (k > v->direct_left) k = v->direct_left;
            gif_transfer((const uint8_t *)(w + i), k, 2);
            v->direct_left -= k; i += k * 4;
            continue;
        }
        if (!v->direct_left && v->need && v->got < v->need) {
            uint32_t k = v->need - v->got; if (k > nw - i) k = nw - i;
            if (v->got < 4096) { uint32_t c = 4096 - v->got < k ? 4096 - v->got : k; memcpy(&v->buf[v->got], w + i, c * 4); }
            v->got += k; i += k;
            if (v->got >= v->need) { v->buf[v->got < 4096 ? v->got : 4095] = 0; vif_finish_data(v); }
            continue;
        }
        vif_word(v, w[i++]);
    }
}

void vif_print_stats(void) {
    for (int i = 0; i < 128; i++) if (vif_cmd_count[i]) fprintf(stderr, "[vifstat] cmd %02x: %llu\n", i, (unsigned long long)vif_cmd_count[i]);
}
void vif1_reset_now(void) {
    Vif *v = &vifs[1];
    v->need = 0; v->direct_left = 0; v->dqn = 0;
}

void vif_init(void) {
    memset(vifs, 0, sizeof vifs);
    vifs[0].unit = 0; vifs[1].unit = 1;
    vifs[0].cl = vifs[0].wl = vifs[1].cl = vifs[1].wl = 1;
}
