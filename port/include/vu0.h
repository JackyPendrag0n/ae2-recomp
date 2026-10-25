/* VU0 macro-mode (COP2) operations. dest mask: 8=x 4=y 2=z 1=w */
#pragma once

extern uint8_t g_vu0mem[4096];

static inline float vclamp(float x) { return fclamp(x); }

/* operand "b" sources: produce a 4-float vector */
typedef struct { float v[4]; } v4;
static inline v4 vbc(Ctx *c, int ft, int bc) { float s = c->vf[ft].f[bc]; v4 r = {{s, s, s, s}}; return r; }
static inline v4 vvec(Ctx *c, int ft) { v4 r; memcpy(r.v, c->vf[ft].f, 16); return r; }
static inline v4 vscal(float s) { v4 r = {{s, s, s, s}}; return r; }
static inline float vu_i(Ctx *c) { float f; memcpy(&f, &c->vi[21], 4); return f; }
#define VBC(ft, bc) vbc(c, ft, bc)
#define VV(ft) vvec(c, ft)
#define VQ vscal(c->vq)
#define VI_ vscal(vu_i(c))

static inline void vstore(Ctx *c, int fd, int dest, const float *t) {
    if (fd == 0) return;
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) c->vf[fd].f[i] = t[i];
}
static inline void vstacc(Ctx *c, int dest, const float *t) {
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) c->vacc.f[i] = t[i];
}

/* MAC and status flags (COP2 control registers 17 and 16), updated by every FMAC
 * operation as on VU0. MAC bits: Z 0-3, S 4-7, U 8-11, O 12-15 (x is the high bit of
 * each nibble). Status: Z S U O I D in bits 0-5, sticky copies in bits 6-11. Games
 * test them after VSUBs to compare vectors (cfc2 $vi16 / $vi17). */
static inline void vflags(Ctx *c, int dest, const float *raw, const float *t) {
    uint32_t mac = 0;
    for (int i = 0; i < 4; i++) {
        if (!(dest & (8 >> i))) continue;
        int bit = 3 - i;
        uint32_t u, ur; memcpy(&u, &t[i], 4); memcpy(&ur, &raw[i], 4);
        if ((ur & 0x7f800000) == 0x7f800000) mac |= 0x1000u << bit;                 /* overflow */
        else if ((u & 0x7fffffff) == 0 && (ur & 0x7fffffff) != 0) mac |= 0x100u << bit;   /* underflow */
        if ((u & 0x7fffffff) == 0) mac |= 1u << bit;
        if (u & 0x80000000) mac |= 0x10u << bit;
    }
    c->vi[17] = mac;
    uint32_t st = (c->vi[16] & ~0xfu) | ((mac & 0xf) ? 1 : 0) | ((mac & 0xf0) ? 2 : 0) | ((mac & 0xf00) ? 4 : 0) | ((mac & 0xf000) ? 8 : 0);
    c->vi[16] = st | ((st & 0xf) << 6);
}
#define VOP(expr) float t[4]; for (int i = 0; i < 4; i++) { float a = c->vf[fs].f[i], bb = b.v[i]; (void)a; (void)bb; t[i] = vclamp(expr); }
#define VOPF(expr) float t[4], raw[4]; for (int i = 0; i < 4; i++) { float a = fop(c->vf[fs].f[i]), bb = fop(b.v[i]); if (dest & (8 >> i)) FPCK("vu0", c->vf[fs].f[i], b.v[i]); raw[i] = (expr); t[i] = vclamp(raw[i]); } vflags(c, dest, raw, t);
static inline void v_add(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF(a + bb) vstore(c, fd, dest, t); }
static inline void v_sub(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF(a - bb) vstore(c, fd, dest, t); }
static inline void v_mul(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF(a * bb) vstore(c, fd, dest, t); }
static inline void v_madd(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF(c->vacc.f[i] + a * bb) vstore(c, fd, dest, t); }
static inline void v_msub(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF(c->vacc.f[i] - a * bb) vstore(c, fd, dest, t); }
static inline void v_max(Ctx *c, int dest, int fd, int fs, v4 b) { VOP(fkey(a) > fkey(bb) ? a : bb) vstore(c, fd, dest, t); }
static inline void v_mini(Ctx *c, int dest, int fd, int fs, v4 b) { VOP(fkey(a) < fkey(bb) ? a : bb) vstore(c, fd, dest, t); }
static inline void v_adda(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF(a + bb) vstacc(c, dest, t); }
static inline void v_suba(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF(a - bb) vstacc(c, dest, t); }
static inline void v_mula(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF(a * bb) vstacc(c, dest, t); }
static inline void v_madda(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF(c->vacc.f[i] + a * bb) vstacc(c, dest, t); }
static inline void v_msuba(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF(c->vacc.f[i] - a * bb) vstacc(c, dest, t); }
#ifndef AE2_FPCHECK
/* The FMAC operations above on four lanes at once (SSE2): the same IEEE operations lane
 * by lane, with fop, vclamp and the MAC/status flags done with masks */
#include <emmintrin.h>
static inline __m128 vfop4(__m128 x) {   /* fop: exponent 255 -> +-FLT_MAX */
    __m128i u = _mm_castps_si128(x), e = _mm_set1_epi32(0x7f800000);
    __m128i big = _mm_cmpeq_epi32(_mm_and_si128(u, e), e);
    __m128i m = _mm_or_si128(_mm_and_si128(u, _mm_set1_epi32((int)0x80000000u)), _mm_set1_epi32(0x7f7fffff));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(big, m), _mm_andnot_si128(big, u)));
}
static inline __m128 vclamp4e(__m128 x) {   /* vclamp: exponent 255 -> +-FLT_MAX (min gives its second operand for NaN);
                                              denormal results cannot occur: guest threads flush to zero (rt_kernel.c) */
    __m128 sm = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
    __m128 a = _mm_andnot_ps(sm, x), s = _mm_and_ps(sm, x);
    return _mm_or_ps(_mm_min_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7f7fffff))), s);
}
static const uint8_t vrev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
/* dest -> lane mask (dest bit 8 >> i selects lane i), for storing with one blend */
#define VDM(d) {(d) & 8 ? -1 : 0, (d) & 4 ? -1 : 0, (d) & 2 ? -1 : 0, (d) & 1 ? -1 : 0}
static const int32_t vdmask[16][4] __attribute__((aligned(16))) = {VDM(0), VDM(1), VDM(2), VDM(3), VDM(4), VDM(5), VDM(6), VDM(7),
    VDM(8), VDM(9), VDM(10), VDM(11), VDM(12), VDM(13), VDM(14), VDM(15)};
#undef VDM
static inline void vblend4(float *p, int dest, __m128 t) {
    __m128 m = _mm_load_ps((const float *)vdmask[dest & 15]);
    _mm_storeu_ps(p, _mm_or_ps(_mm_and_ps(m, t), _mm_andnot_ps(m, _mm_loadu_ps(p))));
}
static inline void vflags4(Ctx *c, int dest, __m128 raw, __m128 t) {
    __m128i ur = _mm_castps_si128(raw), u = _mm_castps_si128(t), ex = _mm_set1_epi32(0x7f800000), ab = _mm_set1_epi32(0x7fffffff), z = _mm_setzero_si128();
    int lm = vrev4[dest & 15];   /* dest bit (8 >> i) -> lane bit i */
    int ov = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(ur, ex), ex))) & lm;
    int zt = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(u, ab), z))) & lm;
    int zr = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(ur, ab), z)));
    int sg = _mm_movemask_ps(t) & lm, un = zt & ~zr & ~ov;
#ifdef AE2_VU0_MAC
    /* the MAC flags (vi17): Ape Escape 2 never reads them (no cfc2 $vi17, and they are not
     * carried into VU0 micro programs), so they are only kept up to date on request */
    c->vi[17] = vrev4[zt] | (uint32_t)vrev4[sg] << 4 | (uint32_t)vrev4[un] << 8 | (uint32_t)vrev4[ov] << 12;
#endif
    uint32_t st = (c->vi[16] & ~0xfu) | (zt ? 1 : 0) | (sg ? 2 : 0) | (un ? 4 : 0) | (ov ? 8 : 0);
    c->vi[16] = st | ((st & 0xf) << 6);
}
#define VOPF4(EXPR, STORE) { __m128 A = vfop4(_mm_loadu_ps(c->vf[fs].f)), B = vfop4(_mm_loadu_ps(b.v)), ACC = _mm_loadu_ps(c->vacc.f); (void)ACC; \
    __m128 raw = (EXPR), tt = vclamp4e(raw); vflags4(c, dest, raw, tt); STORE; }
#define vstore4(c, fd, dest, t) do { if (fd) vblend4(c->vf[fd].f, dest, t); } while (0)
#define vstacc4(c, dest, t) vblend4(c->vacc.f, dest, t)
#define v_add(c_, d_, fd_, fs_, b_) v_add4(c_, d_, fd_, fs_, b_)
static inline void v_add4(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF4(_mm_add_ps(A, B), vstore4(c, fd, dest, tt)) }
static inline void v_sub4(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF4(_mm_sub_ps(A, B), vstore4(c, fd, dest, tt)) }
static inline void v_mul4(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF4(_mm_mul_ps(A, B), vstore4(c, fd, dest, tt)) }
static inline void v_madd4(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF4(_mm_add_ps(ACC, _mm_mul_ps(A, B)), vstore4(c, fd, dest, tt)) }
static inline void v_msub4(Ctx *c, int dest, int fd, int fs, v4 b) { VOPF4(_mm_sub_ps(ACC, _mm_mul_ps(A, B)), vstore4(c, fd, dest, tt)) }
static inline void v_adda4(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF4(_mm_add_ps(A, B), vstacc4(c, dest, tt)) }
static inline void v_suba4(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF4(_mm_sub_ps(A, B), vstacc4(c, dest, tt)) }
static inline void v_mula4(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF4(_mm_mul_ps(A, B), vstacc4(c, dest, tt)) }
static inline void v_madda4(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF4(_mm_add_ps(ACC, _mm_mul_ps(A, B)), vstacc4(c, dest, tt)) }
static inline void v_msuba4(Ctx *c, int dest, int fd, int fs, v4 b) { (void)fd; VOPF4(_mm_sub_ps(ACC, _mm_mul_ps(A, B)), vstacc4(c, dest, tt)) }
#undef v_add
#define v_add v_add4
#define v_sub v_sub4
#define v_mul v_mul4
#define v_madd v_madd4
#define v_msub v_msub4
#define v_adda v_adda4
#define v_suba v_suba4
#define v_mula v_mula4
#define v_madda v_madda4
#define v_msuba v_msuba4
#endif
#define V_ADD(d, fd, fs, b) v_add(c, d, fd, fs, b)
#define V_SUB(d, fd, fs, b) v_sub(c, d, fd, fs, b)
#define V_MUL(d, fd, fs, b) v_mul(c, d, fd, fs, b)
#define V_MADD(d, fd, fs, b) v_madd(c, d, fd, fs, b)
#define V_MSUB(d, fd, fs, b) v_msub(c, d, fd, fs, b)
#define V_MAX(d, fd, fs, b) v_max(c, d, fd, fs, b)
#define V_MINI(d, fd, fs, b) v_mini(c, d, fd, fs, b)
#define V_ADDA(d, fd, fs, b) v_adda(c, d, fd, fs, b)
#define V_SUBA(d, fd, fs, b) v_suba(c, d, fd, fs, b)
#define V_MULA(d, fd, fs, b) v_mula(c, d, fd, fs, b)
#define V_MADDA(d, fd, fs, b) v_madda(c, d, fd, fs, b)
#define V_MSUBA(d, fd, fs, b) v_msuba(c, d, fd, fs, b)

static inline void V_OPMULA_f(Ctx *c, int fs, int ft) {
    float *s = c->vf[fs].f, *t = c->vf[ft].f;
    float x = s[1] * t[2], y = s[2] * t[0], z = s[0] * t[1];
    c->vacc.f[0] = vclamp(x); c->vacc.f[1] = vclamp(y); c->vacc.f[2] = vclamp(z);
    { float raw[4] = {x, y, z, 0}; vflags(c, 0xE, raw, c->vacc.f); }
}
static inline void V_OPMSUB_f(Ctx *c, int fd, int fs, int ft) {
    float *s = c->vf[fs].f, *t = c->vf[ft].f;
    float raw[4] = {c->vacc.f[0] - s[1] * t[2], c->vacc.f[1] - s[2] * t[0], c->vacc.f[2] - s[0] * t[1], 0};
    float r[4] = {vclamp(raw[0]), vclamp(raw[1]), vclamp(raw[2]), 0};
    vflags(c, 0xE, raw, r);
    vstore(c, fd, 0xE, r);
}
#define V_OPMULA(fs, ft) V_OPMULA_f(c, fs, ft)
#define V_OPMSUB(fd, fs, ft) V_OPMSUB_f(c, fd, fs, ft)

static inline void V_ITOF_f(Ctx *c, int dest, int ft, int fs, int fr) {
    float t[4]; float sc = 1.0f / (float)(1 << fr);
    for (int i = 0; i < 4; i++) t[i] = (float)c->vf[fs].sw[i] * sc;
    vstore(c, ft, dest, t);
}
static inline void V_FTOI_f(Ctx *c, int dest, int ft, int fs, int fr) {
    if (!ft) return;
    float sc = (float)(1 << fr);
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) c->vf[ft].sw[i] = CVTWS(c->vf[fs].f[i] * sc);
}
#define V_ITOF(d, ft, fs, n) V_ITOF_f(c, d, ft, fs, n)
#define V_FTOI(d, ft, fs, n) V_FTOI_f(c, d, ft, fs, n)

static inline void V_ABS_f(Ctx *c, int dest, int ft, int fs) {
    float t[4]; for (int i = 0; i < 4; i++) t[i] = fabsf(c->vf[fs].f[i]); vstore(c, ft, dest, t);
}
#define V_ABS(d, ft, fs) V_ABS_f(c, d, ft, fs)
static inline void V_CLIP_f(Ctx *c, int fs, int ft) {
    float w = fabsf(c->vf[ft].f[3]);
    uint32_t fl = 0;
    float *s = c->vf[fs].f;
    if (s[0] > +w) fl |= 1; if (s[0] < -w) fl |= 2;
    if (s[1] > +w) fl |= 4; if (s[1] < -w) fl |= 8;
    if (s[2] > +w) fl |= 16; if (s[2] < -w) fl |= 32;
    c->vi[18] = ((c->vi[18] << 6) | fl) & 0xffffff;
}
#define V_CLIP(fs, ft) V_CLIP_f(c, fs, ft)
static inline void V_MOVE_f(Ctx *c, int dest, int ft, int fs) { float t[4]; memcpy(t, c->vf[fs].f, 16); vstore(c, ft, dest, t); }
static inline void V_MR32_f(Ctx *c, int dest, int ft, int fs) {
    float *s = c->vf[fs].f; float t[4] = {s[1], s[2], s[3], s[0]}; vstore(c, ft, dest, t);
}
#define V_MOVE(d, ft, fs) V_MOVE_f(c, d, ft, fs)
#define V_MR32(d, ft, fs) V_MR32_f(c, d, ft, fs)

#define VIR(n) ((n) ? (c->vi[n] & 0xffff) : 0)
#define VIW(n, v) do { if (n) c->vi[n] = (uint32_t)(v) & 0xffff; } while (0)

static inline void vu0_ldq(Ctx *c, int dest, int ft, uint32_t addr) {
    float *m = (float *)(g_vu0mem + ((addr * 16) & 0xff0));
    vstore(c, ft, dest, m);
}
static inline void vu0_stq(Ctx *c, int dest, int fs, uint32_t addr) {
    float *m = (float *)(g_vu0mem + ((addr * 16) & 0xff0));
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m[i] = c->vf[fs].f[i];
}
#define V_LQI(d, ft, is) do { vu0_ldq(c, d, ft, VIR(is)); VIW(is, VIR(is) + 1); } while (0)
#define V_SQI(d, fs, it) do { vu0_stq(c, d, fs, VIR(it)); VIW(it, VIR(it) + 1); } while (0)
#define V_LQD(d, ft, is) do { VIW(is, VIR(is) - 1); vu0_ldq(c, d, ft, VIR(is)); } while (0)
#define V_SQD(d, fs, it) do { VIW(it, VIR(it) - 1); vu0_stq(c, d, fs, VIR(it)); } while (0)
/* FDIV unit: Q result plus the I (invalid) and D (divide by zero) status bits */
static inline void vdflags(Ctx *c, int inv, int dz) {
    uint32_t st = (c->vi[16] & ~0x30u) | (inv ? 0x10 : 0) | (dz ? 0x20 : 0);
    c->vi[16] = st | ((st & 0x30) << 6);
}
static inline void V_DIV_f(Ctx *c, float a, float b) {
    int bz = (b == 0.0f), az = (a == 0.0f);
    vdflags(c, bz && az, bz && !az); c->vq = FDIV(a, b);
}
static inline void V_SQRT_f(Ctx *c, float b) { vdflags(c, b < 0.0f, 0); c->vq = sqrtf(fabsf(b)); }
static inline void V_RSQRT_f(Ctx *c, float a, float b) {
    int bz = (b == 0.0f), az = (a == 0.0f);
    vdflags(c, (bz && az) || b < 0.0f, bz && !az); c->vq = FRSQRT(a, b);
}
#define V_DIV(fs, fsf, ft, ftf) V_DIV_f(c, c->vf[fs].f[fsf], c->vf[ft].f[ftf])
#define V_SQRT(ft, ftf) V_SQRT_f(c, c->vf[ft].f[ftf])
#define V_RSQRT(fs, fsf, ft, ftf) V_RSQRT_f(c, c->vf[fs].f[fsf], c->vf[ft].f[ftf])
static inline void V_MFIR_f(Ctx *c, int dest, int ft, int is) {
    if (!ft) return;
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) c->vf[ft].sw[i] = (int16_t)VIR(is);
}
#define V_MFIR(d, ft, is) V_MFIR_f(c, d, ft, is)
static inline void V_ILWR_f(Ctx *c, int dest, int it, int is) {
    uint32_t *m = (uint32_t *)(g_vu0mem + ((VIR(is) * 16) & 0xff0));
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) { VIW(it, m[i]); break; }
}
static inline void V_ISWR_f(Ctx *c, int dest, int it, int is) {
    uint32_t *m = (uint32_t *)(g_vu0mem + ((VIR(is) * 16) & 0xff0));
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m[i] = VIR(it);
}
#define V_ILWR(d, it, is) V_ILWR_f(c, d, it, is)
#define V_ISWR(d, it, is) V_ISWR_f(c, d, it, is)
static inline void vr_advance(Ctx *c) {
    uint32_t x = c->vr; int b = ((x >> 4) ^ (x >> 22)) & 1; c->vr = ((x << 1) | b) & 0x7fffff;
}
static inline void V_RNEXT_f(Ctx *c, int dest, int ft) {
    vr_advance(c); uint32_t v = 0x3f800000 | (c->vr & 0x7fffff); float f; memcpy(&f, &v, 4);
    float t[4] = {f, f, f, f}; vstore(c, ft, dest, t);
}
static inline void V_RGET_f(Ctx *c, int dest, int ft) {
    uint32_t v = 0x3f800000 | (c->vr & 0x7fffff); float f; memcpy(&f, &v, 4);
    float t[4] = {f, f, f, f}; vstore(c, ft, dest, t);
}
#define V_RNEXT(d, ft) V_RNEXT_f(c, d, ft)
#define V_RGET(d, ft) V_RGET_f(c, d, ft)
#define V_RINIT(fs, fsf) (c->vr = c->vf[fs].uw[fsf] & 0x7fffff)
#define V_RXOR(fs, fsf) (c->vr = (c->vr ^ c->vf[fs].uw[fsf]) & 0x7fffff)
