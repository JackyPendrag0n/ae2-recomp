/* GS rasteriser: primitive queue + band-parallel worker threads.
 *
 * The EE thread parses GIF data and GS registers, snapshots the drawing
 * state into a ring of DrawStates (only when registers change), and pushes
 * primitives into a ring queue. N worker threads each rasterise every
 * primitive but only on their own interleaved 8-row bands, so per-pixel
 * ordering is preserved without locks. Anything that reads or writes VRAM
 * outside the rasteriser (transfers, CLUT loads, texture decode, display
 * readout) first checks the VRAM pages touched by queued primitives and
 * waits for the workers when they overlap.
 *
 * Included from gs.c (needs its statics). */
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

typedef struct {
    int ctx;
    uint32_t fbp, fbw, fpsm, fbmsk;
    uint32_t zbp, zpsm, zmsk;
    int zte, ztst, ate, atst, aref, afail, date, datm;
    int abe, a_a, a_b, a_c, a_d, fix, pabe, colclamp, fba;
    int tme, fge, iip, fst;
    int scx0, scx1, scy0, scy1;
    TexState tex;
    int mip_max, mip_lcm, mip_l, mip_mmin, mip_mmag, mip_eff, mip_want;   /* TEX1; mip_eff: level used (-1: magnified), mip_want: asked for */
    float mip_k;
    uint32_t fogcol;
    /* derived */
    int fb16, fb24, need_dst, zread, zwrite;
    uint32_t zmax, zx;
    int simple_fill;
} DrawState;

/* ---- swizzle tables for framebuffer / z access (per 64x32 or 64x64 tile) */
static uint16_t X32[32][64], X16[64][64], X16S[64][64];
static void init_addr_tables(void) {
    for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) X32[y][x] = bt32[(y >> 3) & 3][(x >> 3) & 7] * 64 + ct32[y & 7][x & 7];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        X16[y][x] = bt16[(y >> 3) & 7][(x >> 4) & 3] * 128 + ct16[y & 7][x & 15];
        X16S[y][x] = bt16s[(y >> 3) & 7][(x >> 4) & 3] * 128 + ct16[y & 7][x & 15];
    }
}
static inline int psm_is16(uint32_t p) { return p == PSMCT16 || p == PSMCT16S || p == PSMZ16 || p == PSMZ16S; }

/* Row accessor: resolves the swizzled word/halfword address of (x, y). */
typedef struct { uint32_t base; const uint16_t *xt; uint32_t bw; uint32_t zx; int is16; } RowAddr;
static inline void row_addr(RowAddr *r, uint32_t bp, uint32_t bw, uint32_t psm, int y) {
    r->bw = bw;
    if (psm_is16(psm)) {
        r->is16 = 1;
        r->base = bp * 128 + (uint32_t)(y >> 6) * bw * 4096;
        r->xt = (psm == PSMCT16S || psm == PSMZ16S) ? X16S[y & 63] : X16[y & 63];
        r->zx = (psm == PSMZ16 || psm == PSMZ16S) ? 0xC00 : 0;
    } else {
        r->is16 = 0;
        r->base = bp * 64 + (uint32_t)(y >> 5) * bw * 2048;
        r->xt = X32[y & 31];
        r->zx = (psm >= 0x30) ? 0x600 : 0;
    }
}
static inline uint32_t ra32(const RowAddr *r, int x) { return (r->base + (uint32_t)(x >> 6) * 2048 + (r->xt[x & 63] ^ r->zx)) & (VRAM_WORDS - 1); }
static inline uint32_t ra16(const RowAddr *r, int x) { return (r->base + (uint32_t)(x >> 6) * 4096 + (r->xt[x & 63] ^ r->zx)) & (VRAM_WORDS * 2 - 1); }

/* ---- texture fetch from the decoded cache */
static inline int twrap(int v, int size, uint32_t mode, uint32_t mn, uint32_t mx) {
    switch (mode) {
    case 0: return v & (size - 1);
    case 1: return v < 0 ? 0 : v >= size ? size - 1 : v;
    case 2: v = v < (int)mn ? (int)mn : v > (int)mx ? (int)mx : v; return v & (size - 1);
    default: return (int)(((uint32_t)v & mn) | mx) & (size - 1);
    }
}
static inline int wrap_u(const TexState *t, int x) { return t->u_rep ? (int)(((uint32_t)x & t->u_am) | t->u_ao) : (x < t->u_lo ? t->u_lo : x > t->u_hi ? t->u_hi : x); }
static inline int wrap_v(const TexState *t, int y) { return t->v_rep ? (int)(((uint32_t)y & t->v_am) | t->v_ao) : (y < t->v_lo ? t->v_lo : y > t->v_hi ? t->v_hi : y); }
static inline uint32_t tfetch(const TexState *t, int x, int y) {
    return t->cache[(wrap_v(t, y) << t->tw) + wrap_u(t, x)];
}
static inline uint32_t tsample(const TexState *t, float u, float v) {
    if (!t->bilinear) return tfetch(t, ffloor(u), ffloor(v));
    u -= 0.5f; v -= 0.5f;
    int x0 = ffloor(u), y0 = ffloor(v);
    uint32_t fu = (uint32_t)((u - x0) * 256.0f), fv = (uint32_t)((v - y0) * 256.0f);
    int xa = wrap_u(t, x0), xb = wrap_u(t, x0 + 1);
    int ya = wrap_v(t, y0) << t->tw, yb = wrap_v(t, y0 + 1) << t->tw;
    const uint32_t *c = t->cache;
    uint32_t c00 = c[ya + xa], c10 = c[ya + xb], c01 = c[yb + xa], c11 = c[yb + xb];
    uint32_t w11 = (fu * fv) >> 8, w10 = fu - w11, w01 = fv - w11, w00 = 256 - fu - fv + w11;
    uint32_t rb = (((c00 & 0xff00ff) * w00 + (c10 & 0xff00ff) * w10 + (c01 & 0xff00ff) * w01 + (c11 & 0xff00ff) * w11) >> 8) & 0xff00ff;
    uint32_t ga = ((((c00 >> 8) & 0xff00ff) * w00 + ((c10 >> 8) & 0xff00ff) * w10 + ((c01 >> 8) & 0xff00ff) * w01 + ((c11 >> 8) & 0xff00ff) * w11)) & 0xff00ff00;
    return rb | ga;
}

/* ---- span rendering */
typedef struct {
    double z, dz;
    float r, g, b, a, dr, dg, db, da;
    float s, t, q, ds, dt, dq;
    float u, v, du, dv;
    float f, df;
} SpanAttr;

static inline int clampc(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

typedef uint64_t (*SpanFn)(const DrawState *, int, int, int, SpanAttr *);
#include "gs_simd.h"
static int gs_simd = -1;
#define SP_NAME span_t0100
#define SP_TEX 0
#define SP_FST 1
#define SP_ABE 0
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t0101
#define SP_TEX 0
#define SP_FST 1
#define SP_ABE 0
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t0110
#define SP_TEX 0
#define SP_FST 1
#define SP_ABE 1
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t0111
#define SP_TEX 0
#define SP_FST 1
#define SP_ABE 1
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t1000
#define SP_TEX 1
#define SP_FST 0
#define SP_ABE 0
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t1001
#define SP_TEX 1
#define SP_FST 0
#define SP_ABE 0
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t1010
#define SP_TEX 1
#define SP_FST 0
#define SP_ABE 1
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t1011
#define SP_TEX 1
#define SP_FST 0
#define SP_ABE 1
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t1100
#define SP_TEX 1
#define SP_FST 1
#define SP_ABE 0
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t1101
#define SP_TEX 1
#define SP_FST 1
#define SP_ABE 0
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t1110
#define SP_TEX 1
#define SP_FST 1
#define SP_ABE 1
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t1111
#define SP_TEX 1
#define SP_FST 1
#define SP_ABE 1
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t2000
#define SP_TEX 2
#define SP_FST 0
#define SP_ABE 0
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t2001
#define SP_TEX 2
#define SP_FST 0
#define SP_ABE 0
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t2010
#define SP_TEX 2
#define SP_FST 0
#define SP_ABE 1
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t2011
#define SP_TEX 2
#define SP_FST 0
#define SP_ABE 1
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t2100
#define SP_TEX 2
#define SP_FST 1
#define SP_ABE 0
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t2101
#define SP_TEX 2
#define SP_FST 1
#define SP_ABE 0
#define SP_Z 1
#include "gs_span.h"
#define SP_NAME span_t2110
#define SP_TEX 2
#define SP_FST 1
#define SP_ABE 1
#define SP_Z 0
#include "gs_span.h"
#define SP_NAME span_t2111
#define SP_TEX 2
#define SP_FST 1
#define SP_ABE 1
#define SP_Z 1
#include "gs_span.h"
static const SpanFn span_tab[3][2][2][2] = {
  {{{span_t0100, span_t0101}, {span_t0110, span_t0111}}, {{span_t0100, span_t0101}, {span_t0110, span_t0111}}},
  {{{span_t1000, span_t1001}, {span_t1010, span_t1011}}, {{span_t1100, span_t1101}, {span_t1110, span_t1111}}},
  {{{span_t2000, span_t2001}, {span_t2010, span_t2011}}, {{span_t2100, span_t2101}, {span_t2110, span_t2111}}}};

static uint64_t span(const DrawState *d, int y, int x0, int x1, SpanAttr *A) {
    if (y < d->scy0 || y > d->scy1) return 0;
    if (x0 < d->scx0) {
        float k = (float)(d->scx0 - x0);
        A->z += A->dz * (d->scx0 - x0); A->r += A->dr * k; A->g += A->dg * k; A->b += A->db * k; A->a += A->da * k;
        A->s += A->ds * k; A->t += A->dt * k; A->q += A->dq * k; A->u += A->du * k; A->v += A->dv * k; A->f += A->df * k;
        x0 = d->scx0;
    }
    if (x1 > d->scx1 + 1) x1 = d->scx1 + 1;
    if (x0 >= x1) return 0;
    int T = !d->tme ? 0 : d->tex.bilinear ? 2 : 1;
#ifdef GS_HAVE_SIMD
    if (gs_simd < 0) gs_simd = __builtin_cpu_supports("sse4.1") && !getenv("AE2_NOSIMD");
    if (gs_simd && simd_ok(d)) return span_sse_tab[T][(d->zread | d->zwrite) & 1](d, y, x0, x1, A);
#endif
#ifdef SPAN_STATS
    if (T == 2 && (d->zread | d->zwrite)) { extern uint64_t feat_px[16]; uint64_t n = x1 - x0;
        feat_px[0] += n; if (d->ate) feat_px[1] += n; if (d->fge) feat_px[2] += n; if (d->fbmsk) feat_px[3] += n; if (d->date) feat_px[4] += n;
        if (d->fb16) feat_px[5] += n; if (psm_is16(d->zpsm)) feat_px[6] += n; if (d->zpsm == PSMZ32) feat_px[7] += n;
        if (d->tex.tfx == 0) feat_px[8] += n; if (d->tex.tfx == 1) feat_px[9] += n; if (d->tex.tcc) feat_px[10] += n;
        if (d->abe && d->a_a == 0 && d->a_b == 1 && d->a_c == 0 && d->a_d == 1) feat_px[11] += n; else if (d->abe) feat_px[12] += n;
        if (!d->tex.u_rep || !d->tex.v_rep) feat_px[13] += n; if (d->zwrite) feat_px[14] += n; if (d->iip) feat_px[15] += n; }
    { extern uint64_t span_px[3][2][2][2], span_calls[3][2][2][2]; span_px[T][d->fst & 1][d->abe & 1][(d->zread | d->zwrite) & 1] += x1 - x0; span_calls[T][d->fst & 1][d->abe & 1][(d->zread | d->zwrite) & 1]++; }
#endif
    return span_tab[T][d->fst & 1][d->abe & 1][(d->zread | d->zwrite) & 1](d, y, x0, x1, A);
}

/* ---- primitives (rasterised by each worker on its own row bands) */
typedef struct { uint8_t type; uint16_t st; Vtx v[3]; } Prim;

static inline int64_t floordiv64(int64_t n, int64_t d) { int64_t q = n / d; if ((n % d) && ((n < 0) != (d < 0))) q--; return q; }

#define BAND_SHIFT 3
static inline int band_mine(int y, int k, int nb) { return nb <= 1 || ((y >> BAND_SHIFT) % nb) == k; }

static uint64_t raster_triangle(const DrawState *d, const Vtx *v0, const Vtx *v1, const Vtx *v2, int k, int nb) {
    const Vtx *flat = v2;
    int64_t x0 = v0->x, y0 = v0->y, x1 = v1->x, y1 = v1->y, x2 = v2->x, y2 = v2->y;
    int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return 0;
    if (area < 0) { const Vtx *tv = v1; v1 = v2; v2 = tv; x1 = v1->x; y1 = v1->y; x2 = v2->x; y2 = v2->y; area = -area; }
    int minx = (int)((x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2)) >> 4);
    int maxx = (int)(((x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) + 15) >> 4);
    int miny = (int)((y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2)) >> 4);
    int maxy = (int)(((y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) + 15) >> 4);
    if (minx < d->scx0) minx = d->scx0; if (maxx > d->scx1) maxx = d->scx1;
    if (miny < d->scy0) miny = d->scy0; if (maxy > d->scy1) maxy = d->scy1;
    if (minx > maxx || miny > maxy) return 0;
    if (nb > 1) {   /* skip the setup when none of this worker's bands is touched */
        int any = 0;
        for (int yb = miny & ~((1 << BAND_SHIFT) - 1); yb <= maxy && !any; yb += 1 << BAND_SHIFT) any = band_mine(yb, k, nb);
        if (!any) return 0;
    }
    /* edges: w_i = A*sx + B*sy + C (+bias); inside when w_i >= 0 */
    int64_t EA[3] = {y1 - y2, y2 - y0, y0 - y1}, EB[3] = {x2 - x1, x0 - x2, x1 - x0};
    int64_t EC[3] = {-(EA[0] * x1 + EB[0] * y1), -(EA[1] * x2 + EB[1] * y2), -(EA[2] * x0 + EB[2] * y0)};
    int64_t bias[3];
    for (int i = 0; i < 3; i++) bias[i] = (EA[i] > 0 || (EA[i] == 0 && EB[i] < 0)) ? 0 : -1;
    /* attribute planes: attr = c + gx*sx + gy*sy (sx, sy in 1/16 pixel) */
    double inv = 1.0 / (double)area;
    double l1x = EA[1] * inv, l1y = EB[1] * inv, l1c = EC[1] * inv;
    double l2x = EA[2] * inv, l2y = EB[2] * inv, l2c = EC[2] * inv;
#define PLANE(a0, a1, a2, GX, GY, GC) do { double d1 = (double)(a1) - (a0), d2 = (double)(a2) - (a0); \
        GX = d1 * l1x + d2 * l2x; GY = d1 * l1y + d2 * l2y; GC = (a0) + d1 * l1c + d2 * l2c; } while (0)
    double zx, zy, zc; PLANE((double)v0->z, (double)v1->z, (double)v2->z, zx, zy, zc);
    double rx = 0, ry = 0, rc = flat->r, gx = 0, gy = 0, gc = flat->g, bx = 0, by = 0, bc = flat->b, ax = 0, ay = 0, ac = flat->a;
    if (d->iip) {
        PLANE(v0->r, v1->r, v2->r, rx, ry, rc); PLANE(v0->g, v1->g, v2->g, gx, gy, gc);
        PLANE(v0->b, v1->b, v2->b, bx, by, bc); PLANE(v0->a, v1->a, v2->a, ax, ay, ac);
    }
    double sx = 0, sy = 0, sc = 0, tx = 0, ty = 0, tc = 0, qx = 0, qy = 0, qc = 1, ux = 0, uy = 0, uc = 0, vx = 0, vy = 0, vc = 0;
    if (d->tme) {
        if (d->fst) { PLANE(v0->u / 16.0, v1->u / 16.0, v2->u / 16.0, ux, uy, uc); PLANE(v0->v / 16.0, v1->v / 16.0, v2->v / 16.0, vx, vy, vc); }
        else { PLANE(v0->s, v1->s, v2->s, sx, sy, sc); PLANE(v0->t, v1->t, v2->t, tx, ty, tc); PLANE(v0->q, v1->q, v2->q, qx, qy, qc); }
    }
    double fx = 0, fy = 0, fc = 0;
    if (d->fge) PLANE(v0->f, v1->f, v2->f, fx, fy, fc);
#undef PLANE
    uint64_t n = 0;
    const int use_c = d->iip, use_uv = d->tme && d->fst, use_stq = d->tme && !d->fst, use_f = d->fge;
    for (int yb = miny & ~((1 << BAND_SHIFT) - 1); yb <= maxy; yb += 1 << BAND_SHIFT) {
        if (!band_mine(yb, k, nb)) continue;
        int ys = yb < miny ? miny : yb, ye = yb + (1 << BAND_SHIFT) - 1; if (ye > maxy) ye = maxy;
        /* exact incremental edge stepping: q = floor(K / D), stepped by S = 16*EB per row */
        int64_t q[3], r[3], D[3], qs[3], rs[3], K0[3];
        for (int e = 0; e < 3; e++) {
            int64_t K = EB[e] * ((int64_t)ys * 16) + EC[e] + bias[e];
            K0[e] = K;
            if (EA[e] == 0) { D[e] = 0; continue; }
            D[e] = EA[e] > 0 ? 16 * EA[e] : -16 * EA[e];
            q[e] = floordiv64(K, D[e]); r[e] = K - q[e] * D[e];
            int64_t S = 16 * EB[e];
            qs[e] = floordiv64(S, D[e]); rs[e] = S - qs[e] * D[e];
        }
        for (int py = ys; py <= ye; py++) {
            int64_t xl = minx, xr = maxx;
            int empty = 0;
            for (int e = 0; e < 3; e++) {
                if (EA[e] > 0) { if (-q[e] > xl) xl = -q[e]; }
                else if (EA[e] < 0) { if (q[e] < xr) xr = q[e]; }
                else if (K0[e] + EB[e] * 16 * (py - ys) < 0) empty = 1;
                if (D[e]) { q[e] += qs[e]; r[e] += rs[e]; if (r[e] >= D[e]) { r[e] -= D[e]; q[e]++; } }
            }
            if (empty || xl > xr) continue;
            double X = (double)xl * 16, Y = (double)py * 16;
            SpanAttr A;
            A.z = zc + zx * X + zy * Y; A.dz = zx * 16;
            if (use_c) {
                A.r = (float)(rc + rx * X + ry * Y); A.dr = (float)(rx * 16);
                A.g = (float)(gc + gx * X + gy * Y); A.dg = (float)(gx * 16);
                A.b = (float)(bc + bx * X + by * Y); A.db = (float)(bx * 16);
                A.a = (float)(ac + ax * X + ay * Y); A.da = (float)(ax * 16);
            } else { A.r = (float)rc; A.g = (float)gc; A.b = (float)bc; A.a = (float)ac; A.dr = A.dg = A.db = A.da = 0; }
            if (use_stq) {
                A.s = (float)(sc + sx * X + sy * Y); A.ds = (float)(sx * 16);
                A.t = (float)(tc + tx * X + ty * Y); A.dt = (float)(tx * 16);
                A.q = (float)(qc + qx * X + qy * Y); A.dq = (float)(qx * 16);
            } else { A.s = A.t = 0; A.q = 1; A.ds = A.dt = A.dq = 0; }
            if (use_uv) {
                A.u = (float)(uc + ux * X + uy * Y); A.du = (float)(ux * 16);
                A.v = (float)(vc + vx * X + vy * Y); A.dv = (float)(vx * 16);
            } else { A.u = A.v = A.du = A.dv = 0; }
            if (use_f) { A.f = (float)(fc + fx * X + fy * Y); A.df = (float)(fx * 16); }
            else { A.f = 0; A.df = 0; }
            n += span(d, py, (int)xl, (int)xr + 1, &A);
        }
    }
    return n;
}

static uint64_t raster_sprite(const DrawState *d, const Vtx *a, const Vtx *b, int k, int nb) {
    int x0 = (a->x < b->x ? a->x : b->x), x1 = (a->x < b->x ? b->x : a->x);
    int y0 = (a->y < b->y ? a->y : b->y), y1 = (a->y < b->y ? b->y : a->y);
    int px0 = (x0 + 15) >> 4, px1 = (x1 + 15) >> 4;   /* [x0, x1) */
    int py0 = (y0 + 15) >> 4, py1 = (y1 + 15) >> 4;
    if (px0 < d->scx0) px0 = d->scx0; if (px1 > d->scx1 + 1) px1 = d->scx1 + 1;
    if (py0 < d->scy0) py0 = d->scy0; if (py1 > d->scy1 + 1) py1 = d->scy1 + 1;
    if (px0 >= px1 || py0 >= py1) return 0;
    uint32_t z = b->z > d->zmax ? d->zmax : b->z;
    uint64_t n = 0;
    if (d->simple_fill) {
        uint32_t col = b->r | (b->g << 8) | (b->b << 16) | ((uint32_t)b->a << 24);
        if (d->fba) col |= 0x80000000u;
        uint32_t c16 = ((col >> 3) & 0x1f) | (((col >> 11) & 0x1f) << 5) | (((col >> 19) & 0x1f) << 10) | ((col >> 31) << 15);
        uint16_t *v16 = (uint16_t *)vram;
        for (int py = py0; py < py1; py++) {
            if (!band_mine(py, k, nb)) continue;
            RowAddr fr, zr; row_addr(&fr, d->fbp, d->fbw, d->fpsm, py);
            if (d->zwrite) row_addr(&zr, d->zbp, d->fbw, d->zpsm, py);
            for (int x = px0; x < px1; x++) {
                if (fr.is16) v16[ra16(&fr, x)] = (uint16_t)c16;
                else { uint32_t fa = ra32(&fr, x); vram[fa] = d->fb24 ? ((vram[fa] & 0xff000000u) | (col & 0xffffff)) : col; }
                if (d->zwrite) {
                    if (zr.is16) v16[ra16(&zr, x)] = (uint16_t)z;
                    else { uint32_t za = ra32(&zr, x); vram[za] = d->zpsm == PSMZ24 ? ((vram[za] & 0xff000000u) | (z & 0xffffff)) : z; }
                }
            }
            n += px1 - px0;
        }
        return n;
    }
    float dx = (float)(b->x - a->x), dy = (float)(b->y - a->y);
    float su, sv, eu, ev;
    if (d->tme && !d->fst) {
        float qb = b->q != 0.0f ? b->q : 1.0f;
        su = a->s / qb * (1 << d->tex.tw); sv = a->t / qb * (1 << d->tex.th);
        eu = b->s / qb * (1 << d->tex.tw); ev = b->t / qb * (1 << d->tex.th);
    } else { su = a->u / 16.0f; sv = a->v / 16.0f; eu = b->u / 16.0f; ev = b->v / 16.0f; }
    float du = dx != 0.0f ? (eu - su) * 16.0f / dx : 0.0f;
    float dv = dy != 0.0f ? (ev - sv) * 16.0f / dy : 0.0f;
    float u0 = su + (px0 * 16.0f - a->x) / 16.0f * du;
    DrawState ds = *d; ds.fst = 1;
    for (int py = py0; py < py1; py++) {
        if (!band_mine(py, k, nb)) continue;
        SpanAttr A; memset(&A, 0, sizeof A);
        A.z = z; A.r = b->r; A.g = b->g; A.b = b->b; A.a = b->a; A.f = b->f; A.q = 1;
        A.u = u0; A.du = du; A.v = sv + (py * 16.0f - a->y) / 16.0f * dv;
        n += span(&ds, py, px0, px1, &A);
    }
    return n;
}

static uint64_t raster_line(const DrawState *d, const Vtx *v0, const Vtx *v1, int k, int nb) {
    int x0 = v0->x >> 4, y0 = v0->y >> 4, x1 = v1->x >> 4, y1 = v1->y >> 4;
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    if (n > 4096) return 0;
    uint64_t cnt = 0;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / n : 0.0f;
        int x = x0 + (int)((x1 - x0) * t), y = y0 + (int)((y1 - y0) * t);
        if (!band_mine(y, k, nb)) continue;
        const Vtx *c = d->iip ? NULL : v1;
        SpanAttr A; memset(&A, 0, sizeof A);
        A.r = c ? c->r : v0->r + (v1->r - v0->r) * t; A.g = c ? c->g : v0->g + (v1->g - v0->g) * t;
        A.b = c ? c->b : v0->b + (v1->b - v0->b) * t; A.a = c ? c->a : v0->a + (v1->a - v0->a) * t;
        A.z = v0->z + ((double)v1->z - v0->z) * t;
        A.s = v0->s + (v1->s - v0->s) * t; A.t = v0->t + (v1->t - v0->t) * t; A.q = v0->q + (v1->q - v0->q) * t;
        A.u = (v0->u + (v1->u - v0->u) * t) / 16.0f; A.v = (v0->v + (v1->v - v0->v) * t) / 16.0f; A.f = v1->f;
        cnt += span(d, y, x, x + 1, &A);
    }
    return cnt;
}

static uint64_t raster_point(const DrawState *d, const Vtx *v, int k, int nb) {
    int y = v->y >> 4;
    if (!band_mine(y, k, nb)) return 0;
    SpanAttr A; memset(&A, 0, sizeof A);
    A.r = v->r; A.g = v->g; A.b = v->b; A.a = v->a; A.z = v->z; A.s = v->s; A.t = v->t; A.q = v->q;
    A.u = v->u / 16.0f; A.v = v->v / 16.0f; A.f = v->f;
    return span(d, y, v->x >> 4, (v->x >> 4) + 1, &A);
}

/* ------------------------------------------------------------------ queue + workers */
#define PQ_N (1u << 16)
#define ST_N 4096
#define MAX_WORKERS 16
static Prim *pq;
static DrawState *st_ring;
static uint64_t st_lastq[ST_N];
static int st_cur = -1, state_dirty = 1;
static uint64_t q_w;                         /* producer position (atomic) */
static struct { uint64_t r; uint64_t pix; char pad[48]; } wk[MAX_WORKERS] __attribute__((aligned(64)));
static int nworkers;                         /* 0 = rasterise synchronously on the EE thread */
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER, q_done_cv = PTHREAD_COND_INITIALIZER;
static int q_sleepers, q_flush_wait;
static uint64_t sync_pix, flush_us, flush_count;
static uint64_t flush_epoch = 1;
static uint8_t pend_page[512];
static int pend_any;

static uint64_t raster_prim(const Prim *p, int k, int nb) {
    const DrawState *d = &st_ring[p->st];
    switch (p->type) {
    case 0: return raster_point(d, &p->v[0], k, nb);
    case 1: return raster_line(d, &p->v[0], &p->v[1], k, nb);
    case 3: return raster_triangle(d, &p->v[0], &p->v[1], &p->v[2], k, nb);
    case 6: return raster_sprite(d, &p->v[0], &p->v[1], k, nb);
    }
    return 0;
}

static uint64_t min_rpos(void) {
    if (!nworkers) return __atomic_load_n(&q_w, __ATOMIC_SEQ_CST);
    uint64_t m = ~0ull;
    for (int i = 0; i < nworkers; i++) { uint64_t r = __atomic_load_n(&wk[i].r, __ATOMIC_SEQ_CST); if (r < m) m = r; }
    return m;
}

static void *gs_worker(void *arg) {
    int k = (int)(intptr_t)arg;
    uint64_t r = 0;
    for (;;) {
        uint64_t w = __atomic_load_n(&q_w, __ATOMIC_SEQ_CST);
        if (r == w) {
            int spins = 0;
            while (r == (w = __atomic_load_n(&q_w, __ATOMIC_SEQ_CST)) && spins < 2000) { spins++; __builtin_ia32_pause(); }
            if (r == w) {
                if (__atomic_load_n(&q_flush_wait, __ATOMIC_SEQ_CST)) {
                    pthread_mutex_lock(&q_mtx); pthread_cond_broadcast(&q_done_cv); pthread_mutex_unlock(&q_mtx);
                }
                pthread_mutex_lock(&q_mtx);
                __atomic_add_fetch(&q_sleepers, 1, __ATOMIC_SEQ_CST);
                while (r == __atomic_load_n(&q_w, __ATOMIC_SEQ_CST)) {
                    if (__atomic_load_n(&q_flush_wait, __ATOMIC_SEQ_CST)) pthread_cond_broadcast(&q_done_cv);
                    pthread_cond_wait(&q_cv, &q_mtx);
                }
                __atomic_sub_fetch(&q_sleepers, 1, __ATOMIC_SEQ_CST);
                pthread_mutex_unlock(&q_mtx);
                continue;
            }
        }
        while (r < w) {
            wk[k].pix += raster_prim(&pq[r & (PQ_N - 1)], k, nworkers);
            r++;
            if ((r & 31) == 0) __atomic_store_n(&wk[k].r, r, __ATOMIC_SEQ_CST);
        }
        __atomic_store_n(&wk[k].r, r, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

/* Wait until every queued primitive has been rasterised. */
static uint64_t flush_reason[12];
static const char *flush_reason_name[12] = {"other", "tex-stale", "tex-evict", "state-ring", "csr-reset", "rec", "vsync", "queue-full", "clut", "tex-decode", "trx-dst", "trx-src"};
static void gs_flush_r(int reason);
#define gs_flush() gs_flush_r(0)
static void gs_flush_r(int reason) {
    if (nworkers && min_rpos() != q_w) flush_reason[reason]++;
    if (nworkers && min_rpos() != q_w) {
        uint64_t t0 = host_usec();
        __atomic_store_n(&q_flush_wait, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&q_sleepers, __ATOMIC_SEQ_CST)) { pthread_mutex_lock(&q_mtx); pthread_cond_broadcast(&q_cv); pthread_mutex_unlock(&q_mtx); }
        int spins = 0;
        while (min_rpos() != q_w) {
            if (spins < 4000) { spins++; __builtin_ia32_pause(); continue; }
            pthread_mutex_lock(&q_mtx);
            if (min_rpos() != q_w) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&q_done_cv, &q_mtx, &ts);
            }
            pthread_mutex_unlock(&q_mtx);
        }
        __atomic_store_n(&q_flush_wait, 0, __ATOMIC_SEQ_CST);
        flush_us += host_usec() - t0; flush_count++;
    }
    if (pend_any) { memset(pend_page, 0, sizeof pend_page); pend_any = 0; }
    flush_epoch++;
}

static void q_push(const Prim *p) {
    if (!nworkers) { sync_pix += raster_prim(p, 0, 1); q_w++; return; }
    if (q_w - min_rpos() >= PQ_N - 1) gs_flush_r(7);
    pq[q_w & (PQ_N - 1)] = *p;
    __atomic_store_n(&q_w, q_w + 1, __ATOMIC_SEQ_CST);
    /* wake sleeping workers only once a batch has built up (a futex call per
     * primitive costs more than rasterising most of them); flushes wake them too */
    static uint64_t last_wake;
    if (q_w - last_wake >= 48 && __atomic_load_n(&q_sleepers, __ATOMIC_SEQ_CST)) {
        last_wake = q_w;
        pthread_mutex_lock(&q_mtx); pthread_cond_broadcast(&q_cv); pthread_mutex_unlock(&q_mtx);
    }
}

static uint64_t gs_pixels_drawn(void) {
    uint64_t n = sync_pix;
    for (int i = 0; i < nworkers; i++) n += __atomic_load_n(&wk[i].pix, __ATOMIC_RELAXED);
    return n;
}

static void gs_start_workers(int threads) {
    pq = malloc(sizeof(Prim) * PQ_N);
    st_ring = calloc(ST_N, sizeof(DrawState));
    const char *e = getenv("AE2_GS_THREADS");
    long ncpu = plat_ncpu();
    /* default: leave one core for the EE (and one more for the GS front-end
     * thread when there are enough cores for it, see gs_mt_start) */
    int n = e ? atoi(e) : ncpu >= 4 ? (int)(ncpu - 2 > 8 ? 8 : ncpu - 2) : ncpu >= 2 ? 2 : 0;
    if (!threads) n = 0;
    if (n < 0) n = 0; if (n > MAX_WORKERS) n = MAX_WORKERS;
    nworkers = n;
    for (int i = 0; i < n; i++) {
        pthread_t th; pthread_create(&th, NULL, gs_worker, (void *)(intptr_t)i); pthread_detach(th);
    }
    INFO("[gs] rasteriser: %d worker thread(s)%s\n", n, n ? "" : " (synchronous)");
}

/* ---- VRAM page bookkeeping */
static void page_dims(uint32_t psm, int *pw, int *ph, int *half) {
    *half = 0;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *pw = 64; *ph = 64; break;
    case PSMT8: *pw = 128; *ph = 64; *half = 1; break;
    case PSMT4: *pw = 128; *ph = 128; *half = 1; break;
    default: *pw = 64; *ph = 32; break;
    }
}
/* op 0: test pending, 1: mark pending + written, 2: mark written only */
static int pages_rect(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1, int op) {
    int pw, ph, half; page_dims(psm, &pw, &ph, &half);
    uint32_t ppr = half ? (bw >> 1) : bw; if (!ppr) ppr = 1;
    uint32_t base = bp >> 5;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    int px0 = x0 / pw, px1 = x1 / pw, py0 = y0 / ph, py1 = y1 / ph;
    /* a block offset can push the rect's last blocks into the following page */
    int extra = 0;
    if (bp & 31) {
        int maxblk = 31;
        if (px0 == px1 && py0 == py1) {
            int lx = x1 % pw, ly = y1 % ph;
            switch (psm) {
            case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: maxblk = bt32[(ly >> 3) & 3][(lx >> 3) & 7]; break;
            case PSMCT16: maxblk = bt16[(ly >> 3) & 7][(lx >> 4) & 3]; break;
            case PSMCT16S: maxblk = bt16s[(ly >> 3) & 7][(lx >> 4) & 3]; break;
            case PSMT8: maxblk = bt8[(ly >> 4) & 3][(lx >> 4) & 7]; break;
            case PSMT4: maxblk = bt4[(ly >> 4) & 7][(lx >> 5) & 3]; break;
            default: maxblk = 31; break;
            }
        }
        extra = (int)(bp & 31) + maxblk > 31;
    }
    if (px1 - px0 > 32) px1 = px0 + 32;
    if (py1 - py0 > 64) py1 = py0 + 64;
    for (int py = py0; py <= py1; py++)
        for (int px = px0; px <= px1; px++)
            for (int e = 0; e <= extra; e++) {
                uint32_t p = (base + (uint32_t)py * ppr + (uint32_t)px + (uint32_t)e) & 511;
                if (op == 0) { if (pend_page[p]) return 1; }
                else { page_seq[p] = global_seq; if (op == 1) { pend_page[p] = 1; pend_any = 1; } }
            }
    return 0;
}
static inline void flush_if_pending(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1, int reason) {
    if (pend_any && pages_rect(bp, bw, psm, x0, y0, x1, y1, 0)) {
        static int nl; static int dbg = -1; if (dbg < 0) dbg = getenv("AE2_FLUSHDBG") != NULL;
        if (dbg && nl < 40) { nl++;
            fprintf(stderr, "[flushdbg] reason %d bp=%x bw=%u psm=%x rect=%d,%d-%d,%d pending pages:", reason, bp, bw, psm, x0, y0, x1, y1);
            for (int i = 0; i < 512; i++) if (pend_page[i]) fprintf(stderr, " %x", i);
            fprintf(stderr, "\n"); }
        gs_flush_r(reason);
    }
}
