#include <time.h>
/* VU0/VU1 micro-mode interpreter.
 * Executes 64-bit instruction pairs (upper FMAC op + lower op) with the timing
 * details software-pipelined microcode relies on: FMAC result stalls, Q/P
 * latencies (reads see the old value until the divide completes), delayed
 * MAC/status/clip flags, branch delay slots and the E-bit end delay.
 */
#include "vu_impl.h"

static VU vu[2];
int g_vu_kick_capture;
/* kick capture (verification): address + VU1 memory snapshot per XGKICK */
typedef struct { uint32_t addr; uint8_t *mem; } KickRec;
static KickRec *kick_log; static int kick_n, kick_cap;
void vu_kick_record(uint32_t addr) {
    if (kick_n == kick_cap) { kick_cap = kick_cap ? kick_cap * 2 : 64; kick_log = realloc(kick_log, sizeof(KickRec) * kick_cap); for (int i = kick_n; i < kick_cap; i++) kick_log[i].mem = NULL; }
    if (!kick_log[kick_n].mem) kick_log[kick_n].mem = malloc(16384);
    kick_log[kick_n].addr = addr; memcpy(kick_log[kick_n].mem, g_vu1mem, 16384); kick_n++;
}
uint32_t g_vif1_top, g_vif1_itop;   /* set by VIF */
/* ------------------------------------------------------------------ upper */
typedef struct { int dest_reg; int to_acc; int dest; float v[4]; int valid; int clip; uint32_t clipv; } UpperRes;

static void exec_upper(VU *u, uint32_t w, UpperRes *o) {
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    int fn = w & 63;
    o->valid = 0; o->clip = 0; o->dest = dest; o->to_acc = 0; o->dest_reg = 0;
    float b[4], a[4];
    enum { OP_ADD, OP_SUB, OP_MUL, OP_MADD, OP_MSUB, OP_MAX, OP_MINI } op;
    int to_acc = 0;
    if (fn < 60) {
        if (fn < 28) {
            int base = fn & ~3, bc = fn & 3;
            stall_on(u, fs); stall_on(u, ft);
            float s = u->vf[ft].f[bc];
            b[0] = b[1] = b[2] = b[3] = s;
            switch (base) { case 0: op = OP_ADD; break; case 4: op = OP_SUB; break; case 8: op = OP_MADD; break;
                case 12: op = OP_MSUB; break; case 16: op = OP_MAX; break; case 20: op = OP_MINI; break; case 24: op = OP_MUL; break;
                default: return; }
        } else {
            stall_on(u, fs);
            switch (fn) {
            case 28: op = OP_MUL; b[0] = b[1] = b[2] = b[3] = getq(u); break;
            case 29: op = OP_MAX; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 30: op = OP_MUL; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 31: op = OP_MINI; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 32: op = OP_ADD; b[0] = b[1] = b[2] = b[3] = getq(u); break;
            case 33: op = OP_MADD; b[0] = b[1] = b[2] = b[3] = getq(u); break;
            case 34: op = OP_ADD; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 35: op = OP_MADD; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 36: op = OP_SUB; b[0] = b[1] = b[2] = b[3] = getq(u); break;
            case 37: op = OP_MSUB; b[0] = b[1] = b[2] = b[3] = getq(u); break;
            case 38: op = OP_SUB; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 39: op = OP_MSUB; b[0] = b[1] = b[2] = b[3] = u->i; break;
            case 40: case 41: case 42: case 43: case 44: case 45: case 47:
                stall_on(u, ft);
                memcpy(b, u->vf[ft].f, 16);
                op = fn == 40 ? OP_ADD : fn == 41 ? OP_MADD : fn == 42 ? OP_MUL : fn == 43 ? OP_MAX : fn == 44 ? OP_SUB : fn == 45 ? OP_MSUB : OP_MINI;
                break;
            case 46: { /* OPMSUB */
                stall_on(u, ft);
                float *s = u->vf[fs].f, *t = u->vf[ft].f;
                o->v[0] = vclampf(u->acc[0] - s[1] * t[2]);
                o->v[1] = vclampf(u->acc[1] - s[2] * t[0]);
                o->v[2] = vclampf(u->acc[2] - s[0] * t[1]);
                o->v[3] = 0;
                o->dest = 0xE; o->dest_reg = fd; o->valid = 1;
                return;
            }
            default: return;
            }
        }
        memcpy(a, u->vf[fs].f, 16);
        o->dest_reg = fd;
    } else {
        int op2 = (w & 3) | ((w >> 4) & 0x7c);
        if (op2 < 16 || (op2 >= 24 && op2 < 28)) {
            int base = op2 & ~3, bc = op2 & 3;
            stall_on(u, fs); stall_on(u, ft);
            float s = u->vf[ft].f[bc];
            b[0] = b[1] = b[2] = b[3] = s;
            op = base == 0 ? OP_ADD : base == 4 ? OP_SUB : base == 8 ? OP_MADD : base == 12 ? OP_MSUB : OP_MUL;
            to_acc = 1;
        } else if (op2 >= 16 && op2 < 24) {
            stall_on(u, fs);
            static const int fr[4] = {0, 4, 12, 15};
            int n = fr[op2 & 3];
            if (op2 < 20) {
                float sc = 1.0f / (float)(1 << n);
                for (int k = 0; k < 4; k++) o->v[k] = (float)u->vf[fs].sw[k] * sc;
            } else {
                float sc = (float)(1 << n);
                for (int k = 0; k < 4; k++) { int32_t iv = CVTWS(u->vf[fs].f[k] * sc); memcpy(&o->v[k], &iv, 4); }
            }
            o->dest_reg = ft; o->valid = 1;
            o->to_acc = 0;
            o->clip = 2;  /* marker: integer conversion, no flags */
            return;
        } else {
            switch (op2) {
            case 28: op = OP_MUL; b[0] = b[1] = b[2] = b[3] = getq(u); to_acc = 1; stall_on(u, fs); break;
            case 29: stall_on(u, fs); for (int k = 0; k < 4; k++) o->v[k] = fabsf(u->vf[fs].f[k]); o->dest_reg = ft; o->valid = 1; o->clip = 2; return;
            case 30: op = OP_MUL; b[0] = b[1] = b[2] = b[3] = u->i; to_acc = 1; stall_on(u, fs); break;
            case 31: { /* CLIP */
                stall_on(u, fs); stall_on(u, ft);
                float wv = fabsf(u->vf[ft].f[3]);
                float *s = u->vf[fs].f;
                uint32_t fl = 0;
                if (s[0] > +wv) fl |= 1; if (s[0] < -wv) fl |= 2;
                if (s[1] > +wv) fl |= 4; if (s[1] < -wv) fl |= 8;
                if (s[2] > +wv) fl |= 16; if (s[2] < -wv) fl |= 32;
                { static long nclip; static long start = -2; if (start == -2) start = getenv("AE2_CLIPTRACE") ? atol(getenv("AE2_CLIPTRACE")) : -1;
                  nclip++; if (start >= 0 && nclip > start && nclip < start + 40)
                    fprintf(stderr, "[clip] pc=%04x v=(%g %g %g %g) bits=%02x\n", u->pc, s[0], s[1], s[2], u->vf[ft].f[3], fl); }
                o->clip = 1; o->clipv = fl;
                return;
            }
            case 32: op = OP_ADD; b[0] = b[1] = b[2] = b[3] = getq(u); to_acc = 1; stall_on(u, fs); break;
            case 33: op = OP_MADD; b[0] = b[1] = b[2] = b[3] = getq(u); to_acc = 1; stall_on(u, fs); break;
            case 34: op = OP_ADD; b[0] = b[1] = b[2] = b[3] = u->i; to_acc = 1; stall_on(u, fs); break;
            case 35: op = OP_MADD; b[0] = b[1] = b[2] = b[3] = u->i; to_acc = 1; stall_on(u, fs); break;
            case 36: op = OP_SUB; b[0] = b[1] = b[2] = b[3] = getq(u); to_acc = 1; stall_on(u, fs); break;
            case 37: op = OP_MSUB; b[0] = b[1] = b[2] = b[3] = getq(u); to_acc = 1; stall_on(u, fs); break;
            case 38: op = OP_SUB; b[0] = b[1] = b[2] = b[3] = u->i; to_acc = 1; stall_on(u, fs); break;
            case 39: op = OP_MSUB; b[0] = b[1] = b[2] = b[3] = u->i; to_acc = 1; stall_on(u, fs); break;
            case 40: case 41: case 42: case 44: case 45:
                stall_on(u, fs); stall_on(u, ft);
                memcpy(b, u->vf[ft].f, 16);
                op = op2 == 40 ? OP_ADD : op2 == 41 ? OP_MADD : op2 == 42 ? OP_MUL : op2 == 44 ? OP_SUB : OP_MSUB;
                to_acc = 1;
                break;
            case 46: { /* OPMULA */
                stall_on(u, fs); stall_on(u, ft);
                float *s = u->vf[fs].f, *t = u->vf[ft].f;
                o->v[0] = vclampf(s[1] * t[2]); o->v[1] = vclampf(s[2] * t[0]); o->v[2] = vclampf(s[0] * t[1]); o->v[3] = 0;
                o->dest = 0xE; o->to_acc = 1; o->valid = 1;
                return;
            }
            case 47: return;  /* NOP */
            default: return;
            }
        }
        memcpy(a, u->vf[fs].f, 16);
    }
    for (int k = 0; k < 4; k++) {
        float r;
        switch (op) {
        case OP_ADD: r = a[k] + b[k]; break;
        case OP_SUB: r = a[k] - b[k]; break;
        case OP_MUL: r = a[k] * b[k]; break;
        case OP_MADD: r = u->acc[k] + a[k] * b[k]; break;
        case OP_MSUB: r = u->acc[k] - a[k] * b[k]; break;
        case OP_MAX: r = a[k] > b[k] ? a[k] : b[k]; break;
        default: r = a[k] < b[k] ? a[k] : b[k]; break;
        }
        o->v[k] = vclampf(r);
    }
    o->to_acc = to_acc;
    o->valid = 1;
    if (op == OP_MAX || op == OP_MINI) o->clip = 2;  /* MAX/MINI don't set flags */
}

static void commit_upper(VU *u, UpperRes *o, int lower_dest) {
    if (o->clip == 1) {
        u->clip_latest = ((u->clip_latest << 6) | o->clipv) & 0xffffff;
        uint32_t mac, st;
        flags_visible(u, &mac, &st, NULL);
        flags_push(u, mac, st & 0x3f, u->clip_latest);
        return;
    }
    if (!o->valid) return;
    if (o->to_acc) {
        for (int k = 0; k < 4; k++) if (o->dest & (8 >> k)) u->acc[k] = o->v[k];
    } else if (o->dest_reg && o->dest_reg != lower_dest) {
        for (int k = 0; k < 4; k++) if (o->dest & (8 >> k)) u->vf[o->dest_reg].f[k] = o->v[k];
        u->vf_ready[o->dest_reg] = u->cycle + 4;
    }
    if (o->clip != 2) fmac_flags(u, o->v, o->dest);
}

/* ------------------------------------------------------------------ lower */

/* returns: 0 normal, 1 branch taken (target in *tgt) */
static int exec_lower(VU *u, uint32_t w, uint32_t pc, uint32_t *tgt, int *ldest) {
    *ldest = 0;
    uint32_t op7 = w >> 25;
    int dest = (w >> 21) & 15, it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
    int ft = it, fs = is;
    int32_t imm11 = (int32_t)(w << 21) >> 21;
    uint32_t imm15 = ((w >> 10) & 0x7800) | (w & 0x7ff);
    it &= 31; is &= 31;
    switch (op7) {
    case 0x00: { /* LQ */
        r128 *m = vmem(u, VI(is & 15) + imm11);
        if (ft) { for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].uw[k] = m->uw[k]; }
        *ldest = ft;
        return 0;
    }
    case 0x01: { /* SQ */
        stall_on(u, fs);
        r128 *m = vmem(u, VI(it & 15) + imm11);
        for (int k = 0; k < 4; k++) if (dest & (8 >> k)) m->uw[k] = u->vf[fs].uw[k];
        return 0;
    }
    case 0x04: { /* ILW */
        r128 *m = vmem(u, VI(is & 15) + imm11);
        for (int k = 0; k < 4; k++) if (dest & (8 >> k)) { SETVI(it & 15, m->uw[k]); break; }
        return 0;
    }
    case 0x05: { /* ISW */
        r128 *m = vmem(u, VI(is & 15) + imm11);
        for (int k = 0; k < 4; k++) if (dest & (8 >> k)) m->uw[k] = VI(it & 15);
        return 0;
    }
    case 0x08: SETVI(it & 15, VI(is & 15) + imm15); return 0;
    case 0x09: SETVI(it & 15, VI(is & 15) - imm15); return 0;
    case 0x10: { uint32_t c; flags_visible(u, NULL, NULL, &c); SETVI(1, (c & 0xffffff) == (w & 0xffffff)); return 0; }
    case 0x11: u->clip_latest = w & 0xffffff; { uint32_t m, s; flags_visible(u, &m, &s, NULL); flags_push(u, m, s & 0x3f, u->clip_latest); u->fh[u->fhead].at = u->cycle; } return 0;
    case 0x12: { uint32_t c; flags_visible(u, NULL, NULL, &c); SETVI(1, (c & w & 0xffffff) != 0); return 0; }
    case 0x13: { uint32_t c; flags_visible(u, NULL, NULL, &c); SETVI(1, ((c | w) & 0xffffff) == 0xffffff); return 0; }
    case 0x14: { uint32_t s; flags_visible(u, NULL, &s, NULL); uint32_t imm12 = ((w >> 10) & 0x800) | (w & 0x7ff); SETVI(it & 15, (s & 0xfff) == imm12); return 0; }
    case 0x15: { uint32_t imm12 = ((w >> 10) & 0x800) | (w & 0x7ff); u->status_sticky = imm12 & 0xfc0; return 0; }
    case 0x16: { uint32_t s; flags_visible(u, NULL, &s, NULL); uint32_t imm12 = ((w >> 10) & 0x800) | (w & 0x7ff); SETVI(it & 15, s & imm12); return 0; }
    case 0x17: { uint32_t s; flags_visible(u, NULL, &s, NULL); uint32_t imm12 = ((w >> 10) & 0x800) | (w & 0x7ff); SETVI(it & 15, (s | imm12) & 0xfff); return 0; }
    case 0x18: { uint32_t m; flags_visible(u, &m, NULL, NULL); SETVI(it & 15, (m & 0xffff) == VI(is & 15)); return 0; }
    case 0x1A: { uint32_t m; flags_visible(u, &m, NULL, NULL); SETVI(it & 15, m & VI(is & 15)); return 0; }
    case 0x1B: { uint32_t m; flags_visible(u, &m, NULL, NULL); SETVI(it & 15, (m | VI(is & 15)) & 0xffff); return 0; }
    case 0x1C: { uint32_t c; flags_visible(u, NULL, NULL, &c); SETVI(it & 15, c & 0xfff); return 0; }
    case 0x20: *tgt = pc + 8 + imm11 * 8; return 1;
    case 0x21: SETVI(it & 15, (pc + 16) / 8); *tgt = pc + 8 + imm11 * 8; return 1;
    case 0x24: *tgt = VI(is & 15) * 8; return 1;
    case 0x25: { uint32_t t = VI(is & 15) * 8; SETVI(it & 15, (pc + 16) / 8); *tgt = t; return 1; }
    case 0x28: if (VI(it & 15) == VI(is & 15)) { *tgt = pc + 8 + imm11 * 8; return 1; } return 0;
    case 0x29: if (VI(it & 15) != VI(is & 15)) { *tgt = pc + 8 + imm11 * 8; return 1; } return 0;
    case 0x2C: if ((int16_t)VI(is & 15) < 0) { *tgt = pc + 8 + imm11 * 8; return 1; } return 0;
    case 0x2D: if ((int16_t)VI(is & 15) > 0) { *tgt = pc + 8 + imm11 * 8; return 1; } return 0;
    case 0x2E: if ((int16_t)VI(is & 15) <= 0) { *tgt = pc + 8 + imm11 * 8; return 1; } return 0;
    case 0x2F: if ((int16_t)VI(is & 15) >= 0) { *tgt = pc + 8 + imm11 * 8; return 1; } return 0;
    case 0x40: break;
    default: return 0;
    }
    /* lower op class */
    int fn = w & 63;
    int i_t = it & 15, i_s = is & 15, i_d = id & 15;
    switch (fn) {
    case 0x30: SETVI(i_d, VI(i_s) + VI(i_t)); return 0;
    case 0x31: SETVI(i_d, VI(i_s) - VI(i_t)); return 0;
    case 0x32: { int32_t im = (int32_t)((w >> 6) & 31); if (im & 16) im -= 32; SETVI(i_t, VI(i_s) + im); return 0; }
    case 0x34: SETVI(i_d, VI(i_s) & VI(i_t)); return 0;
    case 0x35: SETVI(i_d, VI(i_s) | VI(i_t)); return 0;
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: break;
    default: return 0;
    }
    int op2 = (w & 3) | ((w >> 4) & 0x7c);
    int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    switch (op2) {
    case 0x30: stall_on(u, fs); if (ft) { for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].uw[k] = u->vf[fs].uw[k]; } *ldest = ft; return 0;
    case 0x31: { stall_on(u, fs); r128 s = u->vf[fs]; if (ft) { uint32_t t[4] = {s.uw[1], s.uw[2], s.uw[3], s.uw[0]};
                 for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].uw[k] = t[k]; } *ldest = ft; return 0; }
    case 0x34: { r128 *m = vmem(u, VI(i_s)); if (ft) { for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].uw[k] = m->uw[k]; }
                 SETVI(i_s, VI(i_s) + 1); *ldest = ft; return 0; }
    case 0x35: { stall_on(u, fs); r128 *m = vmem(u, VI(i_t)); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) m->uw[k] = u->vf[fs].uw[k];
                 SETVI(i_t, VI(i_t) + 1); return 0; }
    case 0x36: { SETVI(i_s, VI(i_s) - 1); r128 *m = vmem(u, VI(i_s)); if (ft) { for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].uw[k] = m->uw[k]; }
                 *ldest = ft; return 0; }
    case 0x37: { stall_on(u, fs); SETVI(i_t, VI(i_t) - 1); r128 *m = vmem(u, VI(i_t)); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) m->uw[k] = u->vf[fs].uw[k]; return 0; }
    case 0x38: case 0x39: case 0x3A: {  /* DIV / SQRT / RSQRT */
        stall_on(u, fs); stall_on(u, ft);
        if (u->cycle < u->q_done) u->cycle = u->q_done;
        getq(u);
        float a = u->vf[fs].f[fsf], b = u->vf[ft].f[ftf], r;
        int lat;
        if (op2 == 0x38) { r = FDIV(a, b); lat = 7; }
        else if (op2 == 0x39) { r = sqrtf(fabsf(b)); lat = 7; }
        else { r = FRSQRT(a, b); lat = 13; }
        u->q_new = r; u->q_done = u->cycle + lat;
        return 0;
    }
    case 0x3B: if (u->cycle < u->q_done) u->cycle = u->q_done; getq(u); return 0;
    case 0x3C: stall_on(u, fs); SETVI(i_t, u->vf[fs].uw[fsf]); return 0;
    case 0x3D: if (ft) { for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].sw[k] = (int16_t)VI(i_s); } *ldest = ft; return 0;
    case 0x3E: { r128 *m = vmem(u, VI(i_s)); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) { SETVI(i_t, m->uw[k]); break; } return 0; }
    case 0x3F: { r128 *m = vmem(u, VI(i_s)); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) m->uw[k] = VI(i_t); return 0; }
    case 0x40: { uint32_t x = u->r; int b = ((x >> 4) ^ (x >> 22)) & 1; u->r = ((x << 1) | b) & 0x7fffff; }
               /* fallthrough */
    case 0x41: if (ft) { uint32_t v = 0x3f800000 | (u->r & 0x7fffff); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].uw[k] = v; } *ldest = ft; return 0;
    case 0x42: u->r = u->vf[fs].uw[fsf] & 0x7fffff; return 0;
    case 0x43: u->r = (u->r ^ u->vf[fs].uw[fsf]) & 0x7fffff; return 0;
    case 0x64: if (ft) { float p = getp(u); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) u->vf[ft].f[k] = p; } *ldest = ft; return 0;
    case 0x68: SETVI(i_t, g_vif1_top); return 0;
    case 0x69: SETVI(i_t, g_vif1_itop); return 0;
    case 0x6C: vu_xgkick(u, VI(i_s)); return 0;
    /* EFU */
    case 0x70: { stall_on(u, fs); float *s = u->vf[fs].f; efu(u, s[0] * s[0] + s[1] * s[1] + s[2] * s[2], 11); return 0; }
    case 0x71: { stall_on(u, fs); float *s = u->vf[fs].f; efu(u, FDIV(1.0f, s[0] * s[0] + s[1] * s[1] + s[2] * s[2]), 18); return 0; }
    case 0x72: { stall_on(u, fs); float *s = u->vf[fs].f; efu(u, sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]), 18); return 0; }
    case 0x73: { stall_on(u, fs); float *s = u->vf[fs].f; float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]); efu(u, FDIV(1.0f, l), 24); return 0; }
    case 0x74: { stall_on(u, fs); float *s = u->vf[fs].f; efu(u, atan2f(s[1], s[0]), 54); return 0; }
    case 0x75: { stall_on(u, fs); float *s = u->vf[fs].f; efu(u, atan2f(s[2], s[0]), 54); return 0; }
    case 0x76: { stall_on(u, fs); float *s = u->vf[fs].f; efu(u, s[0] + s[1] + s[2] + s[3], 12); return 0; }
    case 0x78: stall_on(u, fs); efu(u, sqrtf(fabsf(u->vf[fs].f[fsf])), 12); return 0;
    case 0x79: stall_on(u, fs); efu(u, FDIV(1.0f, sqrtf(fabsf(u->vf[fs].f[fsf]))), 18); return 0;
    case 0x7A: stall_on(u, fs); efu(u, FDIV(1.0f, u->vf[fs].f[fsf]), 12); return 0;
    case 0x7B: if (u->cycle < u->p_done) u->cycle = u->p_done; getp(u); return 0;
    case 0x7C: stall_on(u, fs); efu(u, sinf(u->vf[fs].f[fsf]), 29); return 0;
    case 0x7D: stall_on(u, fs); efu(u, atanf(u->vf[fs].f[fsf]), 54); return 0;
    case 0x7E: stall_on(u, fs); efu(u, expf(-u->vf[fs].f[fsf]), 44); return 0;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ run loop */
static uint64_t vu_instr_total[2];

static void vu_run(VU *u, uint32_t start) {
    u->pc = start & u->codemask;
    int ebit_pending = 0;
    int branch_pending = 0; uint32_t branch_target = 0;
    u->vf[0].f[0] = u->vf[0].f[1] = u->vf[0].f[2] = 0; u->vf[0].f[3] = 1.0f;
    for (uint32_t guard = 0; guard < 4000000; guard++) {
        uint32_t pc = u->pc;
        uint32_t lower, upper;
        memcpy(&lower, u->code + (pc & u->codemask), 4);
        memcpy(&upper, u->code + ((pc + 4) & u->codemask), 4);
        u->cycle++;
        vu_instr_total[u->unit]++;
        int ibit = (upper >> 31) & 1, ebit = (upper >> 30) & 1;
        UpperRes ur;
        if (!ibit && (lower >> 25) == 0x40 && (lower & 0x3c) == 0x3c) {
            /* lower-pipe stalls hold the whole pair: apply them before the upper reads Q/P */
            int op2 = (lower & 3) | ((lower >> 4) & 0x7c);
            if (op2 == 0x3B || op2 == 0x38 || op2 == 0x39 || op2 == 0x3A) { if (u->cycle < u->q_done) u->cycle = u->q_done; }
            else if (op2 == 0x7B) { if (u->cycle < u->p_done) u->cycle = u->p_done; }
        }
        exec_upper(u, upper, &ur);
        uint32_t tgt = 0; int ldest = 0, taken = 0;
        if (!ibit) taken = exec_lower(u, lower, pc, &tgt, &ldest);
        else memcpy(&u->i, &lower, 4); /* upper reads the old I */
        commit_upper(u, &ur, ldest);
        if (ldest) u->vf_ready[ldest] = u->cycle + 1;
        u->vf[0].uw[0] = u->vf[0].uw[1] = u->vf[0].uw[2] = 0; u->vf[0].f[3] = 1.0f;
        u->pc = (pc + 8) & u->codemask;
        if (branch_pending) { u->pc = branch_target & u->codemask; branch_pending = 0; }
        if (taken) { branch_pending = 1; branch_target = tgt; }
        if (ebit_pending) break;
        if (ebit) ebit_pending = 1;
    }
    u->tpc = u->pc;   /* MSCNT continues from here */
    /* let pending Q/P complete for macro-mode readers */
    u->q = u->q_new; u->p = u->p_new;
}

/* ------------------------------------------------------------------ recompiled VU1 programs
 * A "program" is the set of instruction pairs statically reachable from an
 * entry point; it is identified by a hash of their addresses and contents,
 * so the same microcode uploaded again (or elsewhere in memory alongside
 * other programs) still matches. tools/vu_recomp.py turns captured programs
 * (AE2_VUCAPTURE=dir) into C (gen/vu1_progs.c). */
extern const VuProg g_vu1_progs[], g_vu0_progs[];
extern const int g_vu1_nprogs, g_vu0_nprogs;
uint32_t g_vucode_version[2] = {1, 1};
static uint32_t vp_ver[2][2048];
static VuProgFn vp_fn[2][2048];
/* hand-written versions of VU1 programs (vu1_native.c), found by the same entry and hash */
typedef int (*VuNativeFn)(VU *u);
VuNativeFn vu1_native_find(uint32_t entry, uint64_t hash);
static VuNativeFn vn_fn[2048];
static uint64_t vp_hits, vp_misses, vp_fallback, vu0_calls, vu0_compiled;

static int vu_is_branch(uint32_t lower) { uint32_t op = lower >> 25; return op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2f && op != 0x2a && op != 0x2b); }

/* mark: bit0 included, bit1 successors followed. Returns number of pairs. */
static int vu_reach(const uint8_t *code, uint32_t cmask, uint32_t entry, uint8_t *mark) {
    uint32_t npairs = (cmask + 1) / 8;
    memset(mark, 0, npairs);
    static uint32_t stk[8192]; int sp = 0, n = 0;
    stk[sp++] = (entry & cmask) | 0x80000000u;    /* bit31: follow successors */
    while (sp) {
        uint32_t e = stk[--sp];
        uint32_t pc = e & cmask; int follow = (e >> 31) & 1;
        uint32_t idx = pc >> 3;
        if (!(mark[idx] & 1)) { mark[idx] |= 1; n++; }
        if (!follow || (mark[idx] & 2)) continue;
        mark[idx] |= 2;
        uint32_t lower, upper; memcpy(&lower, code + pc, 4); memcpy(&upper, code + ((pc + 4) & cmask), 4);
        int ibit = upper >> 31, ebit = (upper >> 30) & 1;
#define PUSH(a, f) do { if (sp < 8190) stk[sp++] = ((a) & cmask) | ((f) ? 0x80000000u : 0); } while (0)
        if (ebit) { PUSH(pc + 8, 0); continue; }
        if (!ibit && vu_is_branch(lower)) {
            uint32_t op = lower >> 25;
            int32_t imm11 = (int32_t)(lower << 21) >> 21;
            uint32_t tgt = pc + 8 + imm11 * 8;
            PUSH(pc + 8, 1);
            if (op != 0x24 && op != 0x25) PUSH(tgt, 1);
            if (op != 0x20 && op != 0x24) PUSH(pc + 16, 1);
        } else PUSH(pc + 8, 1);
#undef PUSH
    }
    return n;
}
static uint64_t vu_prog_hash(const uint8_t *code, uint32_t cmask, uint32_t entry, const uint8_t *mark) {
    uint64_t h = 1469598103934665603ull ^ (entry & cmask);
    for (uint32_t i = 0; i < (cmask + 1) / 8; i++) if (mark[i] & 1) {
        uint32_t w[3] = {i, 0, 0}; memcpy(&w[1], code + i * 8, 8);
        for (int k = 0; k < 3; k++) { h ^= w[k]; h *= 1099511628211ull; h ^= h >> 29; }
    }
    return h;
}
static void vu_capture(int unit, uint32_t entry, uint64_t h, const uint8_t *mark) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("AE2_VUCAPTURE"); }
    if (!dir) return;
    const uint8_t *code = unit ? g_vu1code : g_vu0code;
    uint32_t npairs = unit ? 2048 : 512;
    char path[512]; snprintf(path, sizeof path, "%s/vu%d_%04x_%016llx.txt", dir, unit, entry, (unsigned long long)h);
    FILE *f = fopen(path, "r"); if (f) { fclose(f); return; }
    f = fopen(path, "w"); if (!f) return;
    fprintf(f, "entry %04x hash %016llx unit %d\n", entry, (unsigned long long)h, unit);
    for (uint32_t i = 0; i < npairs; i++) if (mark[i] & 1) {
        uint32_t lo, up; memcpy(&lo, code + i * 8, 4); memcpy(&up, code + i * 8 + 4, 4);
        fprintf(f, "%04x %08x %08x\n", i * 8, lo, up);
    }
    fclose(f);
    INFO("[vu%d] captured program entry %04x hash %016llx\n", unit, entry, (unsigned long long)h);
}
static VuProgFn vu_find(int unit, uint32_t entry) {
    uint32_t cmask = unit ? 0x3fff : 0xfff;
    uint32_t idx = (entry & cmask) >> 3;
    if (vp_ver[unit][idx] == g_vucode_version[unit]) return vp_fn[unit][idx];
    static uint8_t mark[2048];
    const uint8_t *code = unit ? g_vu1code : g_vu0code;
    vu_reach(code, cmask, entry, mark);
    uint64_t h = vu_prog_hash(code, cmask, entry, mark);
    const VuProg *tab = unit ? g_vu1_progs : g_vu0_progs; int n = unit ? g_vu1_nprogs : g_vu0_nprogs;
    VuProgFn fn = NULL;
    for (int i = 0; i < n; i++) if (tab[i].entry == (entry & cmask) && tab[i].hash == h) { fn = tab[i].fn; break; }
    if (!fn) vu_capture(unit, entry & cmask, h, mark);
    vp_ver[unit][idx] = g_vucode_version[unit]; vp_fn[unit][idx] = fn;
    if (unit) vn_fn[idx] = vu1_native_find(entry & cmask, h);
    if (getenv("AE2_VUHASHES")) INFO("[vu%d] code at %04x: hash %016llx%s\n", unit, entry & cmask, (unsigned long long)h, fn ? "" : " (no compiled program)");
    return fn;
}
static VuProgFn vu1_find(uint32_t entry) { return vu_find(1, entry); }

static int vu_use_compiled = -1, vu_verify = -1;
static uint64_t vv_runs, vv_bad;
/* AE2_VUVERIFY: run interpreter and compiled code on the same input and compare. */
static void vu1_verify(uint32_t addr, VuProgFn fn) {
    static VU s0, si; static uint8_t m0[16384], mi[16384];
    VU *u = &vu[1];
    s0 = *u; memcpy(m0, g_vu1mem, 16384);
    g_vu_kick_capture = 1; kick_n = 0;
    vu_run(u, addr);
    si = *u; memcpy(mi, g_vu1mem, 16384);
    int nki = kick_n;
    static uint32_t ka[4096]; static uint8_t *km[4096];
    for (int i = 0; i < nki && i < 4096; i++) { ka[i] = kick_log[i].addr; km[i] = kick_log[i].mem; kick_log[i].mem = NULL; }
    *u = s0; memcpy(g_vu1mem, m0, 16384); kick_n = 0;
    u->vf[0].f[0] = u->vf[0].f[1] = u->vf[0].f[2] = 0; u->vf[0].f[3] = 1.0f;
    u->pc = addr & u->codemask;
    for (;;) {
        if (fn(u) == 0) { u->tpc = u->pc; u->q = u->q_new; u->p = u->p_new; break; }
        fn = vu1_find(u->pc);
        if (!fn) { vu_run(u, u->pc); break; }
    }
    g_vu_kick_capture = 0;
    vv_runs++;
    int bad = 0; char why[256] = "";
    if (memcmp(u->vf, si.vf, sizeof u->vf)) { bad = 1; for (int r = 0; r < 32; r++) if (memcmp(&u->vf[r], &si.vf[r], 16)) { snprintf(why, sizeof why, "vf%d %g %g %g %g vs %g %g %g %g", r, u->vf[r].f[0], u->vf[r].f[1], u->vf[r].f[2], u->vf[r].f[3], si.vf[r].f[0], si.vf[r].f[1], si.vf[r].f[2], si.vf[r].f[3]); break; } }
    else if (memcmp(u->vi, si.vi, sizeof u->vi)) { bad = 2; for (int r = 0; r < 16; r++) if (u->vi[r] != si.vi[r]) { snprintf(why, sizeof why, "vi%d %x vs %x", r, u->vi[r], si.vi[r]); break; } }
    else if (memcmp(u->acc, si.acc, 16) || u->q != si.q || u->p != si.p || u->i != si.i || u->r != si.r) { bad = 3; snprintf(why, sizeof why, "acc/q/p/i/r"); }
    else if (u->cycle != si.cycle) { bad = 4; snprintf(why, sizeof why, "cycle %llu vs %llu", (unsigned long long)u->cycle, (unsigned long long)si.cycle); }
    else if (u->tpc != si.tpc) { bad = 5; snprintf(why, sizeof why, "tpc %x vs %x", u->tpc, si.tpc); }
    else if (memcmp(g_vu1mem, mi, 16384)) { bad = 6; for (int i = 0; i < 16384; i += 16) if (memcmp(g_vu1mem + i, mi + i, 16)) { snprintf(why, sizeof why, "mem qw %03x", i / 16); break; } }
    else if (kick_n != nki) { bad = 7; snprintf(why, sizeof why, "kicks %d vs %d", kick_n, nki); }
    else for (int i = 0; i < nki; i++) if (kick_log[i].addr != ka[i] || memcmp(kick_log[i].mem, km[i], 16384)) { bad = 8; snprintf(why, sizeof why, "kick %d differs", i); break; }
    if (bad) { vv_bad++; if (vv_bad < 30) WARN("[vu1] VERIFY MISMATCH entry %04x: %s\n", addr, why); }
    for (int i = 0; i < nki && i < 4096; i++) free(km[i]);
    /* send the compiled run's GIF packets on */
    for (int i = 0; i < kick_n; i++) gif_path1_kick(kick_log[i].mem, kick_log[i].addr);
}
/* AE2_VU1STATS=1: per program (entry address) runs, XGKICKs, GIF qwords and host time,
 * printed at exit: which drawing programs carry the frame */
int g_vu1_stats = -1; uint32_t g_vu1_cur;
uint64_t g_vu1_st[2048][4];   /* runs, kicks, qwords, ns */
static void vu1_start_(uint32_t addr);
static void vu1_start(uint32_t addr) {
    if (g_vu1_stats < 0) g_vu1_stats = getenv("AE2_VU1STATS") != NULL;
    if (!g_vu1_stats) { vu1_start_(addr); return; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t e = (addr & 0x3fff) >> 3; g_vu1_cur = e; g_vu1_st[e][0]++;
    vu1_start_(addr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_vu1_st[e][3] += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
}
/* run compiled programs (and with 'native', the hand-written ones where there is one
 * for the address) from u->pc until an E bit: 0, or 1 if there is no code for u->pc */
static uint64_t vn_used, vn_declined;
static int vu1_chain(VU *u, VuProgFn fn, int native) {
    u->vf[0].f[0] = u->vf[0].f[1] = u->vf[0].f[2] = 0; u->vf[0].f[3] = 1.0f;
    for (;;) {
        VuNativeFn nf = native ? vn_fn[(u->pc & 0x3fff) >> 3] : NULL;
        int r = nf ? nf(u) : -1;
        if (nf) { if (r < 0) vn_declined++; else vn_used++; }
        if (r < 0) r = fn(u);
        if (r == 0) { u->tpc = u->pc; u->q = u->q_new; u->p = u->p_new; return 0; }
        /* left through a computed jump: continue with the program at the target */
        fn = vu1_find(u->pc);
        if (!fn) return 1;
    }
}
/* AE2_VUNATIVE=verify: run each program once with the compiled code only and once with
 * the hand-written versions, and compare memory, kicks and the registers later programs
 * can see */
static uint64_t vn_runs, vn_bad;
static void vn_verify(uint32_t addr, VuProgFn fn) {
    static VU s0, sc; static uint8_t m0[16384], mc[16384];
    static uint32_t ka[4096]; static uint8_t *km[4096];
    VU *u = &vu[1];
    s0 = *u; memcpy(m0, g_vu1mem, 16384);
    g_vu_kick_capture = 1; kick_n = 0;
    u->pc = addr & u->codemask;
    if (vu1_chain(u, fn, 0)) vu_run(u, u->pc);
    sc = *u; memcpy(mc, g_vu1mem, 16384);
    int nkc = kick_n;
    for (int i = 0; i < nkc && i < 4096; i++) { ka[i] = kick_log[i].addr; km[i] = kick_log[i].mem; kick_log[i].mem = NULL; }
    *u = s0; memcpy(g_vu1mem, m0, 16384); kick_n = 0;
    uint64_t used = vn_used;
    u->pc = addr & u->codemask;
    if (vu1_chain(u, fn, 1)) vu_run(u, u->pc);
    g_vu_kick_capture = 0;
    if (vn_used != used) {
        vn_runs++;
        char why[512] = ""; int bad = 0;
        if (memcmp(g_vu1mem, mc, 16384)) { bad = 1; for (int i = 0; i < 16384; i += 16) if (memcmp(g_vu1mem + i, mc + i, 16)) { const uint32_t *a = (const uint32_t *)(g_vu1mem + i), *b = (const uint32_t *)(mc + i); snprintf(why, sizeof why, "mem qw %03x: %08x %08x %08x %08x vs %08x %08x %08x %08x", i / 16, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]); break; } }
        else if (kick_n != nkc) { bad = 2; snprintf(why, sizeof why, "kicks %d vs %d", kick_n, nkc); }
        else for (int i = 0; i < nkc; i++) if (kick_log[i].addr != ka[i] || memcmp(kick_log[i].mem, km[i], 16384)) { bad = 3; snprintf(why, sizeof why, "kick %d differs", i); break; }
        if (!bad) for (int k = 15; k < 32; k++) if (memcmp(&u->vf[k], &sc.vf[k], 16)) { bad = 4; snprintf(why, sizeof why, "vf%d", k); break; }
        if (!bad && (u->i != sc.i || u->q != sc.q || u->clip_latest != sc.clip_latest || u->tpc != sc.tpc || u->vi[15] != sc.vi[15])) { bad = 5; snprintf(why, sizeof why, "i %g/%g q %g/%g clip %06x/%06x tpc %x/%x vi15 %x/%x", u->i, sc.i, u->q, sc.q, u->clip_latest, sc.clip_latest, u->tpc, sc.tpc, u->vi[15], sc.vi[15]); }
        { static int nd; if (!bad && nd < 5) for (int k = 1; k < 15; k++) if (memcmp(&u->vf[k], &sc.vf[k], 16)) { nd++; INFO("[vu1] native %04x: scratch vf%d differs (%08x %08x %08x %08x vs %08x %08x %08x %08x)\n", addr, k, u->vf[k].uw[0], u->vf[k].uw[1], u->vf[k].uw[2], u->vf[k].uw[3], sc.vf[k].uw[0], sc.vf[k].uw[1], sc.vf[k].uw[2], sc.vf[k].uw[3]); break; } }
        if (bad) { vn_bad++; if (vn_bad < 30) WARN("[vu1] NATIVE MISMATCH entry %04x flags %04x: %s\n", addr, g_vif1_itop & 0xffff, why); }
    }
    for (int i = 0; i < nkc && i < 4096; i++) free(km[i]);
    for (int i = 0; i < kick_n; i++) gif_path1_kick(kick_log[i].mem, kick_log[i].addr);
}
static int vu_native = -2;
static void vu1_start_(uint32_t addr) {
    VU *u = &vu[1];
    if (vu_use_compiled < 0) { vu_use_compiled = !getenv("AE2_VUINTERP"); vu_verify = getenv("AE2_VUVERIFY") != NULL; }
    if (vu_native == -2) { const char *e = getenv("AE2_VUNATIVE"); vu_native = !e ? 1 : !strcmp(e, "verify") ? 2 : atoi(e) != 0; }
    VuProgFn fn = vu_use_compiled || getenv("AE2_VUCAPTURE") ? vu1_find(addr) : NULL;
    if (fn && vu_native == 2 && vu_use_compiled && !vu_verify) { vp_hits++; vn_verify(addr, fn); return; }
    if (fn && vu_verify) { vu1_verify(addr, fn); return; }
    if (fn && vu_use_compiled) {
        vp_hits++;
        u->pc = addr & u->codemask;
        if (!vu1_chain(u, fn, vu_native)) return;
        vp_fallback++;
        { static int nl; if (nl++ < 10) INFO("[vu1] compiled program from %04x left at %04x\n", addr, u->pc); }
        vu_run(u, u->pc);   /* no compiled code for the target: finish in the interpreter */
        return;
    }
    vp_misses++;
    vu_run(u, addr);
}

uint32_t g_vu1_last_entry;
void vu1_exec(uint32_t addr) { g_vu1_last_entry = addr; vu1_start(addr); }
void vu1_continue(void) { vu1_start(vu[1].tpc); }
void vu1_print_stats(void) {
    INFO("[vu1] compiled program runs %llu (fell back %llu), interpreted %llu; VU0 micro calls %llu (compiled %llu)\n", (unsigned long long)vp_hits, (unsigned long long)vp_fallback, (unsigned long long)vp_misses, (unsigned long long)vu0_calls, (unsigned long long)vu0_compiled);
    INFO("[vu] interpreted instruction pairs: vu0 %llu, vu1 %llu\n", (unsigned long long)vu_instr_total[0], (unsigned long long)vu_instr_total[1]);
    if (g_vu1_stats > 0) for (int e = 0; e < 2048; e++) if (g_vu1_st[e][0])
        INFO("[vu1stat] entry %04x: runs %llu kicks %llu qwords %llu time %.1f ms\n", e * 8, (unsigned long long)g_vu1_st[e][0], (unsigned long long)g_vu1_st[e][1], (unsigned long long)g_vu1_st[e][2], g_vu1_st[e][3] / 1e6);
    if (vn_used || vn_declined) INFO("[vu1] hand-written programs: %llu runs, %llu left to the compiled code\n", (unsigned long long)vn_used, (unsigned long long)vn_declined);
    if (vn_runs) WARN("[vu1] native verify: %llu program runs compared, %llu mismatches\n", (unsigned long long)vn_runs, (unsigned long long)vn_bad);
    if (vv_runs) WARN("[vu1] verify: %llu runs, %llu mismatches\n", (unsigned long long)vv_runs, (unsigned long long)vv_bad);
}
void vu1_dump_regs(void) {
    for (int r = 18; r < 32; r++) fprintf(stderr, "  vf%02d: %12g %12g %12g %12g\n", r, vu[1].vf[r].f[0], vu[1].vf[r].f[1], vu[1].vf[r].f[2], vu[1].vf[r].f[3]);
}
void vu1_init(void) {
    memset(vu, 0, sizeof vu);
    vu[0].mem = g_vu0mem; vu[0].memmask = 0xfff; vu[0].code = g_vu0code; vu[0].codemask = 0xfff; vu[0].unit = 0;
    vu[1].mem = g_vu1mem; vu[1].memmask = 0x3fff; vu[1].code = g_vu1code; vu[1].codemask = 0x3fff; vu[1].unit = 1;
}
uint64_t vu1_instr_count(void) { return vu_instr_total[1]; }

/* VU0 micro subroutine called from EE (VCALLMS): share registers with the EE COP2 state */
void rt_vu0_exec(Ctx *c, uint32_t addr) {
    VU *u = &vu[0];
    vu0_calls++;
    { static uint64_t n; static uint32_t seen[64]; static int ns; n++;
      int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == addr) f = 1;
      if (!f && ns < 64) { seen[ns++] = addr; uint32_t nz = 0; for (int i = 0; i < 4096; i++) nz += g_vu0code[i] != 0;
        INFO("[vu0] vcallms %04x (code bytes nonzero: %u)\n", addr, nz); } }
    memcpy(u->vf, c->vf, sizeof u->vf);
    for (int k = 0; k < 16; k++) u->vi[k] = c->vi[k];
    memcpy(u->acc, c->vacc.f, 16);
    u->q = u->q_new = c->vq; u->q_done = 0;
    memcpy(&u->i, &c->vi[21], 4);
    u->r = c->vr;
    u->status_sticky = c->vi[16] & 0xfc0;   /* macro-mode flags carry into the micro program */
    u->clip_latest = c->vi[18] & 0xffffff;
    {
        if (vu_use_compiled < 0) { vu_use_compiled = !getenv("AE2_VUINTERP"); vu_verify = getenv("AE2_VUVERIFY") != NULL; }
        VuProgFn fn = (vu_use_compiled || getenv("AE2_VUCAPTURE")) ? vu_find(0, addr) : NULL;
        if (fn && vu_verify) {
            static VU s0, si; static uint8_t m0[4096], mi[4096];
            s0 = *u; memcpy(m0, g_vu0mem, 4096);
            vu_run(u, addr);
            si = *u; memcpy(mi, g_vu0mem, 4096);
            *u = s0; memcpy(g_vu0mem, m0, 4096);
            u->vf[0].f[0] = u->vf[0].f[1] = u->vf[0].f[2] = 0; u->vf[0].f[3] = 1.0f;
            u->pc = addr & u->codemask;
            for (;;) {
                if (fn(u) == 0) { u->tpc = u->pc; u->q = u->q_new; u->p = u->p_new; break; }
                fn = vu_find(0, u->pc);
                if (!fn) { vu_run(u, u->pc); break; }
            }
            uint32_t m1, s1, c1, m2, s2, c2;
            flags_visible(u, &m1, &s1, &c1); flags_visible(&si, &m2, &s2, &c2);
            vv_runs++;
            if (memcmp(u->vf, si.vf, sizeof u->vf) || memcmp(u->vi, si.vi, sizeof u->vi) || memcmp(u->acc, si.acc, 16) ||
                u->q != si.q || u->cycle != si.cycle || memcmp(g_vu0mem, mi, 4096) || m1 != m2 || s1 != s2 || c1 != c2) {
                vv_bad++; if (vv_bad < 30) WARN("[vu0] VERIFY MISMATCH entry %04x (flags %x/%x %x/%x %x/%x)\n", addr, m1, m2, s1, s2, c1, c2);
            }
        } else if (fn && vu_use_compiled) {
            vu0_compiled++;
            u->vf[0].f[0] = u->vf[0].f[1] = u->vf[0].f[2] = 0; u->vf[0].f[3] = 1.0f;
            u->pc = addr & u->codemask;
            for (;;) {
                if (fn(u) == 0) { u->tpc = u->pc; u->q = u->q_new; u->p = u->p_new; break; }
                fn = vu_find(0, u->pc);
                if (!fn) { vu_run(u, u->pc); break; }
            }
        } else vu_run(u, addr);
    }
    memcpy(c->vf, u->vf, sizeof u->vf);
    for (int k = 1; k < 16; k++) c->vi[k] = u->vi[k];
    memcpy(c->vacc.f, u->acc, 16);
    c->vq = u->q;
    c->vr = u->r;
    uint32_t mac, st, clip; flags_visible(u, &mac, &st, &clip);
    c->vi[17] = mac; c->vi[16] = st; c->vi[18] = clip;
    c->vf[0].f[3] = 1.0f;
}
