/* Native versions of VU1 microprograms: the same work as the recompiled code in
 * gen/vu1_progs.c, written as plain C from what each program does rather than one
 * VU instruction at a time (no per-instruction cycle counting, pipeline registers or
 * flag history). Each must give bit-identical GIF output; AE2_VUNATIVE=verify runs
 * both and compares (see vu.c). AE2_VUNATIVE=0 turns them off.
 *
 * A native function returns 0 when it ran the program to its end (u->pc set past the
 * E bit, as the compiled code leaves it), or -1 without touching anything when it does
 * not handle this input, so the caller runs the compiled program instead. */
#include "vu_impl.h"

typedef __m128 f4;
#define CL vclamp4
static inline f4 ld(const void *p) { return _mm_loadu_ps((const float *)p); }
static inline f4 bc(float f) { return _mm_set1_ps(f); }
static inline float lane(f4 v, int i) { float t[4]; _mm_storeu_ps(t, v); return t[i]; }
static inline f4 mul(f4 a, f4 b) { return CL(_mm_mul_ps(a, b)); }
static inline f4 madd(f4 acc, f4 a, f4 b) { return CL(_mm_add_ps(acc, _mm_mul_ps(a, b))); }
/* rows r0..r3 times (x, y, z, 1), each step clamped as the VU's mula/madda chain */
static inline f4 xform4(f4 r0, f4 r1, f4 r2, f4 r3, const float *v) {
    f4 a = mul(r0, bc(v[0])); a = madd(a, r1, bc(v[1])); a = madd(a, r2, bc(v[2]));
    return madd(a, r3, bc(1.0f));
}
static inline f4 xform3(f4 r0, f4 r1, f4 r2, const float *v) {
    f4 a = mul(r0, bc(v[0])); a = madd(a, r1, bc(v[1])); return madd(a, r2, bc(v[2]));
}
/* x*x + y*y + z*z of a vector, as the VU sums it in the w lane */
static inline float len2(f4 v) {
    float s[4]; _mm_storeu_ps(s, mul(v, v));
    return fclamp(fclamp(s[0] + s[1]) + s[2]);
}
static inline void st_xyz(float *d, f4 v) { float t[4]; _mm_storeu_ps(t, v); d[0] = t[0]; d[1] = t[1]; d[2] = t[2]; }
static inline uint32_t clipflags(const float *s) {
    float w = fabsf(s[3]); uint32_t cf = 0;
    if (s[0] > +w) cf |= 1; if (s[0] < -w) cf |= 2; if (s[1] > +w) cf |= 4; if (s[1] < -w) cf |= 8;
    if (s[2] > +w) cf |= 16; if (s[2] < -w) cf |= 32;
    return cf;
}
/* the program's strip culling: given the clip flags of this vertex and the two before it,
 * 0 draws the triangle, 0xffff sets ADC (skips it). 'any' says some vertex is outside. */
static inline uint32_t strip_cull(uint32_t c, int *any) {
    *any = (c & 0x3ffff) != 0;
    if (!*any) return 0;
    if (c & 0x20820) return 0xffff;                       /* a vertex behind the near plane */
    if ((c & 0x002082) == 0x002082 || (c & 0x001041) == 0x001041 ||
        (c & 0x008208) == 0x008208 || (c & 0x004104) == 0x004104) return 0xffff;   /* all past one side */
    if (c & 0x10410) return 0xffff;                       /* a vertex past the far plane */
    return 0;
}
static inline float itof(int32_t i, float s) { return (float)i * s; }

/* ---------------------------------------------------------------------------------
 * entry 0060, hash dc92a79486c148f1: the skinned model program (characters, monkeys).
 * Input at xtop: a GIF tag whose low 15 bits hold the vertex count n, then n vertices of
 * five qwords: position (w low 16 bits: ADC), normal, uv (12-bit fixed), colour (ints),
 * skin (x, y: 12-bit weights; z, w: matrix addresses). itop holds flags:
 *   8: one matrix for every vertex (the first vertex's z), else two weighted matrices
 *   4: backface pass, 2: prelit colours (not done here: the compiled code runs them)
 *   1: environment-mapped texture coordinates, else perspective-correct uv
 * Positions and normals are skinned in place; then n vertices of STQ, RGBA, XYZ2 go to
 * the output buffer (vf31.x, which rotates for double buffering) after the tag, and are
 * kicked. Constants: vf15-17 env matrix, vf18-21 light colours, vf22-24 light
 * directions, vf25/26 viewport scale/offset, vf27-30 world to clip matrix. */
#define QW(a) ((float *)(u->mem + ((((a) & 0xffff) * 16) & u->memmask)))
#define QI(a) ((int32_t *)QW(a))
#define QU(a) ((uint32_t *)QW(a))

static int vn1_0060_dc92(VU *u) {
    uint32_t top = g_vif1_top & 0xffff, flags = g_vif1_itop & 0xffff;
    uint32_t n = QU(top)[0] & 0x7fff;
    if (n == 0 || (flags & 6)) return -1;
    int env = flags & 1;
    /* registers whose stale lanes reach memory (STQ w) */
    float w08 = u->vf[8].f[3], w10 = u->vf[10].f[3];

    if (flags & 8) {   /* one matrix */
        uint32_t m = QU(top + 5)[2] & 0xffff;
        f4 r0 = ld(QW(m)), r1 = ld(QW(m + 1)), r2 = ld(QW(m + 2)), r3 = ld(QW(m + 3));
        for (uint32_t j = 0; j < n; j++) {
            uint32_t b = top + 1 + 5 * j;
            f4 nn = xform3(r0, r1, r2, QW(b + 1));
            float q = FRSQRT(1.0f, len2(nn));
            st_xyz(QW(b + 1), mul(nn, bc(q)));
            st_xyz(QW(b), xform4(r0, r1, r2, r3, QW(b)));
        }
        /* vf08.w is squared once per pass of the pipelined loop, vf10 copies it first */
        for (uint32_t k = 0; k < n + 1; k++) { w10 = w08; w08 = fclamp(w08 * w08); }
    } else {           /* two weighted matrices: normals, then positions */
        for (uint32_t j = 0; j < n; j++) {
            uint32_t b = top + 1 + 5 * j; const int32_t *s = QI(b + 4);
            uint32_t ma = s[2] & 0xffff, mb = s[3] & 0xffff;
            float wa = itof(s[0], 1.0f / 4096.0f), wb = itof(s[1], 1.0f / 4096.0f);
            const float *nv = QW(b + 1);
            f4 na = xform3(ld(QW(ma)), ld(QW(ma + 1)), ld(QW(ma + 2)), nv);
            f4 nb = xform3(ld(QW(mb)), ld(QW(mb + 1)), ld(QW(mb + 2)), nv);
            f4 nn = madd(mul(na, bc(wa)), nb, bc(wb));
            float q = FRSQRT(1.0f, len2(nn));
            st_xyz(QW(b + 1), mul(nn, bc(q)));
        }
        uint32_t lastb = 0;
        for (uint32_t j = 0; j < n; j++) {
            uint32_t b = top + 1 + 5 * j; const int32_t *s = QI(b + 4);
            uint32_t ma = s[2] & 0xffff, mb = s[3] & 0xffff;
            float wa = itof(s[0], 1.0f / 4096.0f), wb = itof(s[1], 1.0f / 4096.0f);
            const float *pv = QW(b);
            f4 pa = xform4(ld(QW(ma)), ld(QW(ma + 1)), ld(QW(ma + 2)), ld(QW(ma + 3)), pv);
            f4 pb = xform4(ld(QW(mb)), ld(QW(mb + 1)), ld(QW(mb + 2)), ld(QW(mb + 3)), pv);
            st_xyz(QW(b), madd(mul(pa, bc(wa)), pb, bc(wb)));
        }
        /* the position loop reads one vertex past the end: vf10 is its second matrix's row 3 */
        lastb = QU(top + 5 + 5 * n)[3] & 0xffff;
        w10 = QW(lastb + 3)[3];
        w08 = QW(lastb + 1)[3];
    }

    uint32_t out = u->vf[31].uw[0] & 0xffff;
    memcpy(QW(out), QW(top), 16);
    f4 c27 = ld(u->vf[27].f), c28 = ld(u->vf[28].f), c29 = ld(u->vf[29].f), c30 = ld(u->vf[30].f);
    f4 c25 = ld(u->vf[25].f), c26 = ld(u->vf[26].f);
    f4 l18 = ld(u->vf[18].f), l19 = ld(u->vf[19].f), l20 = ld(u->vf[20].f), l21 = ld(u->vf[21].f);
    f4 l22 = ld(u->vf[22].f), l23 = ld(u->vf[23].f), l24 = ld(u->vf[24].f);
    f4 e15 = ld(u->vf[15].f), e16 = ld(u->vf[16].f), e17 = ld(u->vf[17].f);
    uint32_t clip = u->clip_latest, vi15 = 0;
    int clipped = 0;
    float q = 0;
    /* the transform runs one (env) or two (perspective) vertices past the end: their clip
     * flags and Q stay in the registers */
    uint32_t nx = env ? n + 1 : n + 2;
    for (uint32_t j = 0; j < nx; j++) {
        uint32_t b = top + 1 + 5 * j;
        float P[4]; _mm_storeu_ps(P, xform4(c27, c28, c29, c30, QW(b)));
        clip = ((clip << 6) | clipflags(P)) & 0xffffff;
        q = FDIV(1.0f, P[3]);
        uint32_t adc = 0;
        if (j == 0) adc = env ? 0x7fff : 0;
        else if (j <= n) { int any; adc = strip_cull(clip, &any); clipped |= any; }
        /* vi15 ends holding the ADC word of the last vertex the loop read */
        if (env ? j == n : j == n + 1) vi15 = (QU(b)[3] | (env ? adc : 0)) & 0xffff;
        if (j >= n) continue;
        f4 proj = mul(ld(P), bc(q));
        if (!env && j && (clip & 0x3ffff)) {   /* guard band: x, y to +-7 */
            f4 c = CL(_mm_max_ps(CL(_mm_min_ps(proj, bc(7.0f))), bc(-7.0f)));
            proj = _mm_castsi128_ps(_mm_or_si128(
                _mm_and_si128(_mm_castps_si128(c), _mm_set_epi32(0, 0, -1, -1)),
                _mm_and_si128(_mm_castps_si128(proj), _mm_set_epi32(-1, -1, 0, 0))));
        }
        float *o = QW(out + 1 + 3 * j);
        const float *nv = QW(b + 1);
        const int32_t *uv = QI(b + 2), *rgba = QI(b + 3);
        /* STQ */
        if (env) {
            float e[4]; _mm_storeu_ps(e, xform3(e15, e16, e17, nv));
            f4 a = CL(_mm_add_ps(_mm_setr_ps(0, 0, 0, 1.0f), bc(0.5f)));
            a = CL(_mm_add_ps(a, _mm_mul_ps(_mm_setr_ps(e[0], e[1], 1.0f, 0), bc(0.35f))));
            f4 st = CL(_mm_add_ps(a, _mm_mul_ps(proj, bc(0.15f))));
            o[0] = lane(st, 0); o[1] = lane(st, 1); o[2] = 1.0f; o[3] = w08;
        } else {
            f4 st = mul(_mm_setr_ps(itof(uv[0], 1.0f / 4096.0f), itof(uv[1], 1.0f / 4096.0f), 1.0f, 0), bc(q));
            o[0] = lane(st, 0); o[1] = lane(st, 1); o[2] = lane(st, 2); o[3] = w10;
        }
        /* RGBA: light colours from the normal, times the vertex colour, to 0..255 */
        {
            f4 c = _mm_setr_ps((float)rgba[0], (float)rgba[1], (float)rgba[2], (float)rgba[3]);
            f4 l = xform3(l22, l23, l24, nv);
            l = CL(_mm_max_ps(l, bc(0.0f)));
            float lv[4]; _mm_storeu_ps(lv, l);
            f4 col = xform4(l18, l19, l20, l21, lv);
            col = env ? mul(c, col) : mul(col, c);
            col = CL(_mm_min_ps(col, bc(255.0f)));
            float cv[4]; _mm_storeu_ps(cv, col);
            int32_t *oc = (int32_t *)(o + 4);
            for (int k = 0; k < 4; k++) oc[k] = CVTWS(cv[k] * 1.0f);
        }
        /* XYZ2 */
        {
            float v[4]; _mm_storeu_ps(v, madd(mul(proj, c25), c26, bc(1.0f)));
            int32_t *ox = (int32_t *)(o + 8);
            for (int k = 0; k < 3; k++) ox[k] = CVTWS(v[k] * 16.0f);
            ox[3] = (int16_t)((QU(b)[3] | adc) & 0xffff);
        }
    }

    /* registers other programs may see */
    u->clip_latest = clip;
    { uint32_t m_, s_; flags_visible(u, &m_, &s_, NULL); flags_push(u, m_, s_ & 0x3f, clip); u->fh[u->fhead].at = 0; }
    u->q = u->q_new = q; u->q_done = 0;
    { float iv = env ? 0.15f : (clipped ? -7.0f : 255.0f); u->i = iv; }
    r128 s = u->vf[31];
    u->vf[31].uw[0] = s.uw[1]; u->vf[31].uw[1] = s.uw[2]; u->vf[31].uw[2] = s.uw[3]; u->vf[31].uw[3] = s.uw[0];
    u->vf[8].f[3] = w08; u->vf[10].f[3] = w10;
    u->vi[9] = flags; u->vi[13] = out; u->vi[14] = top; u->vi[15] = vi15;
    u->cycle += 40 * n;
    vu_xgkick(u, out);
    u->pc = env ? 0x0d70 : 0x0b20;
    return 0;
}

/* ---------------------------------------------------------------------------------
 * The stage microcode (loaded at 0x2000 and up): background and object geometry.
 * Its entry 0040 and the continuation at 2360 (when vi15 says 0x2138) run the
 * unlit strip kernel at 0x2138. Input at xtop: a GIF tag whose low 15 bits hold the
 * vertex count n, then n vertices of three qwords: position (w low 16 bits: ADC), uv
 * (12-bit fixed, xyz), colour (ints). Out (vf31.x, rotated after): the tag, then STQ,
 * RGBA, XYZ2 per vertex, kicked. A vertex whose triangle has a vertex outside the
 * frustum gets ADC; if such a triangle is not wholly outside one plane, the program
 * goes on to its clipper (0x2420, which yields at 0x24f8): this version leaves that
 * whole run to the compiled code. Scratch at qword 0: x = n, y = xtop, z = the
 * continuation (0x427 = 0x2138 / 8). */
static int stage_kernel(VU *u, uint32_t zval) {
    uint32_t top = g_vif1_top & 0xffff;
    uint32_t n = QU(top)[0] & 0x7fff;
    enum { MAXV = 0x400 / 3 };
    if (n == 0 || n + 2 > MAXV) return -1;
    static float P[MAXV][4], qv[MAXV]; static uint32_t hist[MAXV], adc[MAXV];
    f4 c27 = ld(u->vf[27].f), c28 = ld(u->vf[28].f), c29 = ld(u->vf[29].f), c30 = ld(u->vf[30].f);
    uint32_t clip = u->clip_latest;
    /* the pipelined loop transforms two vertices past the end and tests one */
    for (uint32_t j = 0; j < n + 2; j++) {
        _mm_storeu_ps(P[j], xform4(c27, c28, c29, c30, QW(top + 1 + 3 * j)));
        clip = ((clip << 6) | clipflags(P[j])) & 0xffffff;
        hist[j] = clip; qv[j] = FDIV(1.0f, P[j][3]);
    }
    adc[0] = (hist[0] & 0x3ffff) != 0;
    for (uint32_t j = 1; j <= n; j++) {
        uint32_t c = hist[j];
        adc[j] = 0;
        if (!(c & 0x3ffff)) continue;
        adc[j] = 0xffff;
        if ((int16_t)QU(top + 1 + 3 * j)[3] < 0) continue;
        if ((c & 0x020820) == 0x020820 || (c & 0x002082) == 0x002082 || (c & 0x001041) == 0x001041 ||
            (c & 0x008208) == 0x008208 || (c & 0x004104) == 0x004104 || (c & 0x010410) == 0x010410) continue;
        return -1;   /* needs the clipper */
    }

    if (zval) QU(0)[2] = zval;
    QU(0)[0] = n; QU(0)[1] = top;
    uint32_t out = u->vf[31].uw[0] & 0xffff;
    memcpy(QW(out), QW(top), 16);
    f4 c25 = ld(u->vf[25].f), c26 = ld(u->vf[26].f);
    float w05 = u->vf[5].f[3];
    for (uint32_t j = 0; j < n; j++) {
        uint32_t b = top + 1 + 3 * j;
        const int32_t *uv = QI(b + 1), *col = QI(b + 2);
        float *o = QW(out + 1 + 3 * j);
        f4 q = bc(qv[j]);
        f4 st = CL(_mm_add_ps(mul(_mm_setr_ps(itof(uv[0], 1.0f / 4096.0f), itof(uv[1], 1.0f / 4096.0f), itof(uv[2], 1.0f / 4096.0f), w05), q), bc(0.0f)));
        _mm_storeu_ps(o, st);
        int32_t *oc = (int32_t *)(o + 4);
        for (int k = 0; k < 4; k++) oc[k] = CVTWS(((float)col[k] * 1.0f) * 1.0f);
        f4 pq = mul(ld(P[j]), q);
        float v[4]; _mm_storeu_ps(v, madd(mul(pq, c25), c26, bc(1.0f)));
        int32_t *ox = (int32_t *)(o + 8);
        for (int k = 0; k < 3; k++) ox[k] = CVTWS(v[k] * 16.0f);
        ox[3] = (int16_t)((QU(b)[3] | adc[j]) & 0xffff);
    }

    u->clip_latest = hist[n + 1];
    { uint32_t m_, s_; flags_visible(u, &m_, &s_, NULL); flags_push(u, m_, s_ & 0x3f, u->clip_latest); u->fh[u->fhead].at = 0; }
    u->q = u->q_new = qv[n + 1]; u->q_done = 0;
    memcpy(&u->vf[15], QW(top), 16);
    memcpy(&u->vf[4], P[n + 1], 16);
    r128 s = u->vf[31];
    u->vf[31].uw[0] = s.uw[1]; u->vf[31].uw[1] = s.uw[2]; u->vf[31].uw[2] = s.uw[3]; u->vf[31].uw[3] = s.uw[0];
    u->vi[14] = out; u->vi[7] = 0; u->vi[3] = 0;
    u->vi[15] = QU(0)[2] & 0xffff;
    u->cycle += 16 * n;
    vu_xgkick(u, out);
    u->pc = 0x2360;
    return 0;
}
/* The backface pass at 0x2000 (entries 0050 and the continuation with vi15 = 0x16 run
 * it before the strip kernel): for each vertex, the sign of the triangle's area in
 * homogeneous screen space (x, y, w) adds to the count in the low bits of its position
 * w, and bit 2 of the result marks the vertex with ADC (w = 0xfffc). Written as the
 * pipelined loop runs, register for register, since its first passes use values left
 * in vf10 and vi13 by earlier programs. Returns the number of w words it wrote. */
static int stage_backface(VU *u, uint32_t top, uint32_t vi01, uint32_t *wa, uint32_t *wv) {
    float *v1 = u->vf[1].f, *v2 = u->vf[2].f, *v3 = u->vf[3].f, *v4 = u->vf[4].f, *v10 = u->vf[10].f, *v11 = u->vf[11].f, *acc = u->acc;
    f4 c27 = ld(u->vf[27].f), c28 = ld(u->vf[28].f), c29 = ld(u->vf[29].f), c30 = ld(u->vf[30].f);
    int nw = 0;
    uint32_t vi02 = QU(top)[0] & 0x7fff, vi04 = top + 1, vi11, vi13 = u->vi[13] & 0xffff;
    memcpy(v1, QW(vi04), 16); vi04 += 3;
    _mm_storeu_ps(v3, xform4(c27, c28, c29, c30, v1));
    memcpy(v2, QW(vi04), 16); vi04 += 3;
    vi11 = 0xfffc;
    _mm_storeu_ps(v4, xform4(c27, c28, c29, c30, v2));
    _mm_storeu_ps(acc, CL(_mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(c27, bc(v2[0])), _mm_setzero_ps()), _mm_setzero_ps()), _mm_setzero_ps())));
    v3[2] = v3[3]; v4[2] = v4[3];
    _mm_storeu_ps(v3, CL(_mm_sub_ps(ld(v4), ld(v3))));
    memcpy(v11, v4, 16);
    float w05 = u->vf[5].f[3];
    for (;;) {
        float a0 = vclampf(v10[1] * v3[2]), a1 = vclampf(v10[2] * v3[0]), a2 = vclampf(v10[0] * v3[1]);
        memcpy(v1, QW(vi04), 16);
        v4[0] = vclampf(a0 - v3[1] * v10[2]); v4[1] = vclampf(a1 - v3[2] * v10[0]); v4[2] = vclampf(a2 - v3[0] * v10[1]);
        int skip = (vi01 & 0xffff) == 0;
        _mm_storeu_ps(v10, mul(ld(v3), bc(1.0f)));
        vi02 = (vi02 - 1) & 0xffff;
        if (!skip) vi11 = (vi11 + 1) & 0xffff;
        vi11 &= 4;
        vi01 = 0xfffc;
        _mm_storeu_ps(v4, mul(ld(v4), ld(v2)));
        _mm_storeu_ps(v2, xform4(c27, c28, c29, c30, v1));
        if (vi11) { uint32_t a = (vi04 - 6) & 0xffff; wa[nw] = a; wv[nw] = QU(a)[3]; nw++; QU(a)[3] = vi01; }
        vi04 = (vi04 + 3) & 0xffff;
        float aw = v4[0];
        aw = fclamp(aw + v4[1]);
        vi11 = vi13;
        w05 = fclamp(aw + v4[2]);
        v2[2] = v2[3];
        vi13 = u->vf[1].uw[3] & 0xffff;
        _mm_storeu_ps(v3, CL(_mm_sub_ps(ld(v2), ld(v11))));
        _mm_storeu_ps(v11, mul(ld(v2), bc(1.0f)));
        vi01 = signbit(w05) ? 0x10 : 0;
        if ((int16_t)vi02 <= 0) break;
    }
    u->vf[5].f[3] = w05; acc[3] = 0;
    u->vi[1] = vi01; u->vi[2] = vi02; u->vi[4] = vi04; u->vi[11] = vi11; u->vi[13] = vi13; u->vi[12] = 4;
    return nw;
}
static int stage_backface_kernel(VU *u) {
    static VU save; static uint32_t wa[0x400], wv[0x400];
    uint32_t top = g_vif1_top & 0xffff, n = QU(top)[0] & 0x7fff;
    if (n == 0 || n + 2 > 0x400 / 3) return -1;
    save = *u;
    int nw = stage_backface(u, top, top, wa, wv);
    if (stage_kernel(u, 0x16) == 0) return 0;
    for (int i = nw - 1; i >= 0; i--) QU(wa[i])[3] = wv[i];
    *u = save;
    return -1;
}
static int vn1_0040_8857(VU *u) { return stage_kernel(u, 0x427); }
static int vn1_0050_1705(VU *u) { return stage_backface_kernel(u); }
static int vn1_2360_2ff0(VU *u) {
    uint32_t c = u->vi[15] & 0xffff;
    return c == 0x427 ? stage_kernel(u, 0) : c == 0x16 ? stage_backface_kernel(u) : -1;
}

typedef struct { uint32_t entry; uint64_t hash; int (*fn)(VU *); } VuNative;
static const VuNative natives[] = {
    {0x0060, 0xdc92a79486c148f1ull, vn1_0060_dc92},
    {0x0040, 0x88573ff209022f30ull, vn1_0040_8857},
    {0x2360, 0x2ff05f48cd596d4cull, vn1_2360_2ff0},
    {0x0050, 0x170584c9f7d6be8full, vn1_0050_1705},
};
int (*vu1_native_find(uint32_t entry, uint64_t hash))(VU *) {
    for (unsigned i = 0; i < sizeof natives / sizeof natives[0]; i++)
        if (natives[i].entry == entry && natives[i].hash == hash) return natives[i].fn;
    return NULL;
}
