/* SSE4.1 span renderer. Covers every state with a 32/24-bit frame buffer and
 * a 32/24-bit (or no) depth buffer: untextured, nearest or bilinear texturing
 * (STQ or UV), all texture functions, fog, alpha test, blending, FBMSK.
 * 16-bit frame/depth buffers and destination-alpha testing stay on the
 * scalar template. Four pixels per iteration; the arithmetic matches the
 * scalar span template bit for bit (texel/frame-buffer/z accesses stay
 * scalar because VRAM is swizzled). Included from gs_raster.h. */
#if defined(__x86_64__) || defined(__i386__)
#include <smmintrin.h>
#define GS_HAVE_SIMD 1

/* can the SSE span handle this state? */
static inline int simd_ok(const DrawState *d) {
    if (d->fb16 || d->date) return 0;
    if ((d->zread || d->zwrite) && d->zpsm != PSMZ32 && d->zpsm != PSMZ24) return 0;
    return 1;
}

__attribute__((target("sse4.1"), always_inline))
static inline uint64_t span_sse_body(const DrawState *d, int y, int x0, int x1, SpanAttr *A, const int T, const int ZON) {
    RowAddr fr, zr;
    row_addr(&fr, d->fbp, d->fbw, d->fpsm, y);
    if (ZON) row_addr(&zr, d->zbp, d->fbw, d->zpsm, y);
    const TexState *t = &d->tex;
    const int fst = d->fst, tfx = t->tfx, tcc = t->tcc;
    const int z24 = d->zpsm == PSMZ24, fb24 = d->fb24;
    const int zread = ZON && d->zread, zwrite = ZON && d->zwrite, zgreater = d->ztst == 3;
    const int ate = d->ate, atst = d->atst, afail = d->afail;
    const int abe = d->abe, fge = d->fge;
    const uint32_t fbmsk = d->fbmsk;
    const int std_blend = d->a_a == 0 && d->a_b == 1 && d->a_c == 0 && d->a_d == 1;
    const float tws = (float)(1 << t->tw), ths = (float)(1 << t->th);
    const uint32_t *tc = t->cache;
    const int tw = t->tw;

    /* attribute of pixel i (counted from x0) = start + (float)i * step, in both this and
     * the scalar template, so the lanes can be computed directly and stay bit-exact */
    const __m128i lane_i = _mm_set_epi32(3, 2, 1, 0);
    const __m128 Ar = _mm_set1_ps(A->r), Ag = _mm_set1_ps(A->g), Ab = _mm_set1_ps(A->b), Aa = _mm_set1_ps(A->a);
    const __m128 Dr = _mm_set1_ps(A->dr), Dg = _mm_set1_ps(A->dg), Db = _mm_set1_ps(A->db), Da = _mm_set1_ps(A->da);
    const __m128 As = _mm_set1_ps(fst ? A->u : A->s), At = _mm_set1_ps(fst ? A->v : A->t), Aq = _mm_set1_ps(A->q);
    const __m128 Ds = _mm_set1_ps(fst ? A->du : A->ds), Dt = _mm_set1_ps(fst ? A->dv : A->dt), Dq = _mm_set1_ps(A->dq);
    const __m128 Af = _mm_set1_ps(A->f), Df = _mm_set1_ps(A->df);
    const __m128d Az = _mm_set1_pd(A->z), Dz = _mm_set1_pd(A->dz), zmaxd = _mm_set1_pd((double)d->zmax), two31 = _mm_set1_pd(2147483648.0);
    const __m128i ff = _mm_set1_epi32(255), m00ff = _mm_set1_epi32(0xff00ff), sign = _mm_set1_epi32((int)0x80000000);
    const __m128i v256 = _mm_set1_epi32(256), one = _mm_set1_epi32(1);
    const __m128i u_am = _mm_set1_epi32((int)t->u_am), u_ao = _mm_set1_epi32((int)t->u_ao), v_am = _mm_set1_epi32((int)t->v_am), v_ao = _mm_set1_epi32((int)t->v_ao);
    const __m128i u_lo = _mm_set1_epi32(t->u_lo), u_hi = _mm_set1_epi32(t->u_hi), v_lo = _mm_set1_epi32(t->v_lo), v_hi = _mm_set1_epi32(t->v_hi);
    const __m128i aref = _mm_set1_epi32(d->aref);
    const __m128i fixv = _mm_set1_epi32(d->fix);
    const __m128i zlow = _mm_set1_epi32(z24 ? 0xffffff : -1);
    const __m128i fogr = _mm_set1_epi32((int)(d->fogcol & 255)), fogg = _mm_set1_epi32((int)((d->fogcol >> 8) & 255)), fogb = _mm_set1_epi32((int)((d->fogcol >> 16) & 255));
    const __m128i fbm = _mm_set1_epi32((int)fbmsk);
    uint64_t n = 0;
    for (int x = x0; x < x1; x += 4) {
        int cnt = x1 - x < 4 ? x1 - x : 4;
        __m128i idx = _mm_add_epi32(_mm_set1_epi32(x - x0), lane_i);
        __m128 fidx = _mm_cvtepi32_ps(idx);
        __m128i live = _mm_cmpgt_epi32(_mm_set1_epi32(cnt), lane_i);
        __m128i pass = live, zv = _mm_setzero_si128();
        /* every lane gets a valid (masked) address; lanes past the span are never written */
        uint32_t za[4];
        if (ZON) {
            /* ---- depth: z = clamp(start + i*dz, 0, zmax), truncated (floor, as z >= 0) */
            __m128d zlo = _mm_add_pd(Az, _mm_mul_pd(_mm_cvtepi32_pd(idx), Dz));
            __m128d zhi = _mm_add_pd(Az, _mm_mul_pd(_mm_cvtepi32_pd(_mm_unpackhi_epi64(idx, idx)), Dz));
            zlo = _mm_floor_pd(_mm_min_pd(_mm_max_pd(zlo, _mm_setzero_pd()), zmaxd));
            zhi = _mm_floor_pd(_mm_min_pd(_mm_max_pd(zhi, _mm_setzero_pd()), zmaxd));
            zv = _mm_xor_si128(_mm_unpacklo_epi64(_mm_cvttpd_epi32(_mm_sub_pd(zlo, two31)), _mm_cvttpd_epi32(_mm_sub_pd(zhi, two31))), sign);
            for (int i = 0; i < 4; i++) za[i] = ra32(&zr, x + i);
            if (zread) {
                __m128i zdv = _mm_and_si128(_mm_set_epi32((int)vram[za[3]], (int)vram[za[2]], (int)vram[za[1]], (int)vram[za[0]]), zlow);
                __m128i a1 = _mm_xor_si128(zv, sign), b1 = _mm_xor_si128(zdv, sign);
                /* scalar: fail if (zgreater ? zv <= zd : zv < zd) */
                __m128i fail = zgreater ? _mm_or_si128(_mm_cmplt_epi32(a1, b1), _mm_cmpeq_epi32(a1, b1)) : _mm_cmplt_epi32(a1, b1);
                pass = _mm_andnot_si128(fail, pass);
                if (!_mm_movemask_ps(_mm_castsi128_ps(pass))) continue;
            }
        }
        /* ---- vertex colour */
        __m128i ir = _mm_cvttps_epi32(_mm_add_ps(Ar, _mm_mul_ps(fidx, Dr))), ig = _mm_cvttps_epi32(_mm_add_ps(Ag, _mm_mul_ps(fidx, Dg)));
        __m128i ib = _mm_cvttps_epi32(_mm_add_ps(Ab, _mm_mul_ps(fidx, Db))), ia = _mm_cvttps_epi32(_mm_add_ps(Aa, _mm_mul_ps(fidx, Da)));
        if (T) {
            /* ---- texture coordinates */
            __m128 fu, fv;
            __m128 vs = _mm_add_ps(As, _mm_mul_ps(fidx, Ds)), vt = _mm_add_ps(At, _mm_mul_ps(fidx, Dt));
            if (fst) { fu = vs; fv = vt; }
            else {
                __m128 qv = _mm_add_ps(Aq, _mm_mul_ps(fidx, Dq));
                __m128 iq = _mm_div_ps(_mm_set1_ps(1.0f), qv);
                iq = _mm_and_ps(iq, _mm_cmpneq_ps(qv, _mm_setzero_ps()));
                fu = _mm_mul_ps(_mm_mul_ps(vs, iq), _mm_set1_ps(tws));
                fv = _mm_mul_ps(_mm_mul_ps(vt, iq), _mm_set1_ps(ths));
            }
#define WRAPU(x) (t->u_rep ? _mm_or_si128(_mm_and_si128(x, u_am), u_ao) : _mm_min_epi32(_mm_max_epi32(x, u_lo), u_hi))
#define WRAPV(x) (t->v_rep ? _mm_or_si128(_mm_and_si128(x, v_am), v_ao) : _mm_min_epi32(_mm_max_epi32(x, v_lo), v_hi))
#define GATHER(iv) _mm_set_epi32((int)tc[(uint32_t)_mm_extract_epi32(iv, 3)], (int)tc[(uint32_t)_mm_extract_epi32(iv, 2)], \
                                 (int)tc[(uint32_t)_mm_extract_epi32(iv, 1)], (int)tc[(uint32_t)_mm_cvtsi128_si32(iv)])
            __m128i texc;
            if (T == 2) {
                fu = _mm_sub_ps(fu, _mm_set1_ps(0.5f)); fv = _mm_sub_ps(fv, _mm_set1_ps(0.5f));
            }
            /* ffloor: (int)f - (f < (float)(int)f) */
            __m128i iu = _mm_cvttps_epi32(fu), iv = _mm_cvttps_epi32(fv);
            iu = _mm_add_epi32(iu, _mm_castps_si128(_mm_cmplt_ps(fu, _mm_cvtepi32_ps(iu))));
            iv = _mm_add_epi32(iv, _mm_castps_si128(_mm_cmplt_ps(fv, _mm_cvtepi32_ps(iv))));
            if (T == 1) {
                __m128i i00 = _mm_add_epi32(_mm_slli_epi32(WRAPV(iv), tw), WRAPU(iu));
                texc = GATHER(i00);
            } else {
                __m128i wu = _mm_cvttps_epi32(_mm_mul_ps(_mm_sub_ps(fu, _mm_cvtepi32_ps(iu)), _mm_set1_ps(256.0f)));
                __m128i wv = _mm_cvttps_epi32(_mm_mul_ps(_mm_sub_ps(fv, _mm_cvtepi32_ps(iv)), _mm_set1_ps(256.0f)));
                __m128i iu1 = _mm_add_epi32(iu, one), iv1 = _mm_add_epi32(iv, one);
                __m128i xa = WRAPU(iu), xb = WRAPU(iu1);
                __m128i ya = _mm_slli_epi32(WRAPV(iv), tw), yb = _mm_slli_epi32(WRAPV(iv1), tw);
                __m128i i00 = _mm_add_epi32(ya, xa), i10 = _mm_add_epi32(ya, xb), i01 = _mm_add_epi32(yb, xa), i11 = _mm_add_epi32(yb, xb);
                __m128i c00 = GATHER(i00), c10 = GATHER(i10), c01 = GATHER(i01), c11 = GATHER(i11);
                __m128i w11 = _mm_srli_epi32(_mm_mullo_epi32(wu, wv), 8);
                __m128i w10 = _mm_sub_epi32(wu, w11), w01 = _mm_sub_epi32(wv, w11);
                __m128i w00 = _mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(v256, wu), wv), w11);
                __m128i rb = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(_mm_and_si128(c00, m00ff), w00), _mm_mullo_epi32(_mm_and_si128(c10, m00ff), w10)),
                                           _mm_add_epi32(_mm_mullo_epi32(_mm_and_si128(c01, m00ff), w01), _mm_mullo_epi32(_mm_and_si128(c11, m00ff), w11)));
                rb = _mm_and_si128(_mm_srli_epi32(rb, 8), m00ff);
                __m128i ga = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(_mm_and_si128(_mm_srli_epi32(c00, 8), m00ff), w00), _mm_mullo_epi32(_mm_and_si128(_mm_srli_epi32(c10, 8), m00ff), w10)),
                                           _mm_add_epi32(_mm_mullo_epi32(_mm_and_si128(_mm_srli_epi32(c01, 8), m00ff), w01), _mm_mullo_epi32(_mm_and_si128(_mm_srli_epi32(c11, 8), m00ff), w11)));
                ga = _mm_and_si128(ga, _mm_set1_epi32((int)0xff00ff00));
                texc = _mm_or_si128(rb, ga);
            }
#undef WRAPU
#undef WRAPV
#undef GATHER
            __m128i tr = _mm_and_si128(texc, ff), tg = _mm_and_si128(_mm_srli_epi32(texc, 8), ff);
            __m128i tb = _mm_and_si128(_mm_srli_epi32(texc, 16), ff), ta = _mm_srli_epi32(texc, 24);
            /* ---- texture function */
            switch (tfx) {
            case 0:
                ir = _mm_srai_epi32(_mm_mullo_epi32(ir, tr), 7); ig = _mm_srai_epi32(_mm_mullo_epi32(ig, tg), 7);
                ib = _mm_srai_epi32(_mm_mullo_epi32(ib, tb), 7);
                if (tcc) ia = _mm_srai_epi32(_mm_mullo_epi32(ia, ta), 7);
                break;
            case 1: ir = tr; ig = tg; ib = tb; if (tcc) ia = ta; break;
            default:
                ir = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(ir, tr), 7), ia); ig = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(ig, tg), 7), ia);
                ib = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(ib, tb), 7), ia);
                if (tcc) ia = tfx == 2 ? _mm_add_epi32(ia, ta) : ta;
                break;
            }
            /* scalar clamps all four to 0..255 */
            ir = _mm_min_epi32(_mm_max_epi32(ir, _mm_setzero_si128()), ff); ig = _mm_min_epi32(_mm_max_epi32(ig, _mm_setzero_si128()), ff);
            ib = _mm_min_epi32(_mm_max_epi32(ib, _mm_setzero_si128()), ff); ia = _mm_min_epi32(_mm_max_epi32(ia, _mm_setzero_si128()), ff);
        }
        if (fge) {
            __m128i fo = _mm_cvttps_epi32(_mm_add_ps(Af, _mm_mul_ps(fidx, Df))), nfo = _mm_sub_epi32(ff, fo);
            ir = _mm_srai_epi32(_mm_add_epi32(_mm_mullo_epi32(fo, ir), _mm_mullo_epi32(nfo, fogr)), 8);
            ig = _mm_srai_epi32(_mm_add_epi32(_mm_mullo_epi32(fo, ig), _mm_mullo_epi32(nfo, fogg)), 8);
            ib = _mm_srai_epi32(_mm_add_epi32(_mm_mullo_epi32(fo, ib), _mm_mullo_epi32(nfo, fogb)), 8);
        }
        /* ---- alpha test */
        __m128i wfb = pass, wz = zwrite ? pass : _mm_setzero_si128(), rgbonly = _mm_setzero_si128(), drawn = pass;
        if (ate) {
            __m128i ap;
            switch (atst) {
            case 0: ap = _mm_setzero_si128(); break;
            case 1: ap = _mm_set1_epi32(-1); break;
            case 2: ap = _mm_cmplt_epi32(ia, aref); break;
            case 3: ap = _mm_or_si128(_mm_cmplt_epi32(ia, aref), _mm_cmpeq_epi32(ia, aref)); break;
            case 4: ap = _mm_cmpeq_epi32(ia, aref); break;
            case 5: ap = _mm_or_si128(_mm_cmpgt_epi32(ia, aref), _mm_cmpeq_epi32(ia, aref)); break;
            case 6: ap = _mm_cmpgt_epi32(ia, aref); break;
            default: ap = _mm_andnot_si128(_mm_cmpeq_epi32(ia, aref), _mm_set1_epi32(-1)); break;
            }
            __m128i af = _mm_andnot_si128(ap, pass);   /* failed the alpha test */
            switch (afail) {
            case 0: wfb = _mm_andnot_si128(af, wfb); wz = _mm_andnot_si128(af, wz); drawn = _mm_andnot_si128(af, drawn); break;
            case 1: wz = _mm_andnot_si128(af, wz); break;
            case 2: wfb = _mm_andnot_si128(af, wfb); break;
            default: wz = _mm_andnot_si128(af, wz); rgbonly = af; break;
            }
        }
        int dm = _mm_movemask_ps(_mm_castsi128_ps(drawn));
        n += (0x4332322132212110ull >> (dm * 4)) & 15;   /* popcount of 4 bits */
        int fm = _mm_movemask_ps(_mm_castsi128_ps(wfb)), zm = _mm_movemask_ps(_mm_castsi128_ps(wz));
        if (fm) {
            uint32_t fa[4];
            for (int i = 0; i < 4; i++) fa[i] = ra32(&fr, x + i);
            __m128i raw = _mm_set_epi32((int)vram[fa[3]], (int)vram[fa[2]], (int)vram[fa[1]], (int)vram[fa[0]]);
            __m128i dv = fb24 ? _mm_or_si128(raw, sign) : raw;
            if (abe) {
                __m128i cd0 = _mm_and_si128(dv, ff), cd1 = _mm_and_si128(_mm_srli_epi32(dv, 8), ff), cd2 = _mm_and_si128(_mm_srli_epi32(dv, 16), ff);
                __m128i pab = d->pabe ? _mm_cmplt_epi32(ia, _mm_set1_epi32(0x80)) : _mm_setzero_si128();   /* lanes that skip blending */
                __m128i nr, ng, nbv;
                if (std_blend) {
                    nr = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(ir, cd0), ia), 7), cd0);
                    ng = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(ig, cd1), ia), 7), cd1);
                    nbv = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(ib, cd2), ia), 7), cd2);
                } else {
                    __m128i ad = _mm_srli_epi32(dv, 24), z0 = _mm_setzero_si128();
                    __m128i cc = d->a_c == 0 ? ia : d->a_c == 1 ? ad : fixv;
#define SEL(m, cs, cdv) ((m) == 0 ? (cs) : (m) == 1 ? (cdv) : z0)
                    nr = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(SEL(d->a_a, ir, cd0), SEL(d->a_b, ir, cd0)), cc), 7), SEL(d->a_d, ir, cd0));
                    ng = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(SEL(d->a_a, ig, cd1), SEL(d->a_b, ig, cd1)), cc), 7), SEL(d->a_d, ig, cd1));
                    nbv = _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(SEL(d->a_a, ib, cd2), SEL(d->a_b, ib, cd2)), cc), 7), SEL(d->a_d, ib, cd2));
#undef SEL
                }
                if (d->colclamp) {
                    nr = _mm_min_epi32(_mm_max_epi32(nr, _mm_setzero_si128()), ff); ng = _mm_min_epi32(_mm_max_epi32(ng, _mm_setzero_si128()), ff);
                    nbv = _mm_min_epi32(_mm_max_epi32(nbv, _mm_setzero_si128()), ff);
                } else { nr = _mm_and_si128(nr, ff); ng = _mm_and_si128(ng, ff); nbv = _mm_and_si128(nbv, ff); }
                ir = _mm_blendv_epi8(nr, ir, pab); ig = _mm_blendv_epi8(ng, ig, pab); ib = _mm_blendv_epi8(nbv, ib, pab);
            }
            __m128i oa = d->fba ? _mm_or_si128(ia, _mm_set1_epi32(0x80)) : ia;
            __m128i col = _mm_or_si128(_mm_or_si128(_mm_and_si128(ir, ff), _mm_slli_epi32(_mm_and_si128(ig, ff), 8)),
                                       _mm_or_si128(_mm_slli_epi32(_mm_and_si128(ib, ff), 16), _mm_slli_epi32(_mm_and_si128(oa, ff), 24)));
            __m128i hi = _mm_set1_epi32((int)0xff000000u);
            col = _mm_blendv_epi8(col, _mm_or_si128(_mm_andnot_si128(hi, col), _mm_and_si128(dv, hi)), rgbonly);
            if (fbmsk) col = _mm_or_si128(_mm_andnot_si128(fbm, col), _mm_and_si128(raw, fbm));
            if (fb24) col = _mm_or_si128(_mm_andnot_si128(hi, col), _mm_and_si128(raw, hi));
            uint32_t C[4]; _mm_storeu_si128((__m128i *)C, col);
            for (int i = 0; i < cnt; i++) if ((fm >> i) & 1) vram[fa[i]] = C[i];
        }
        if (ZON && zm) {
            uint32_t lz[4]; _mm_storeu_si128((__m128i *)lz, zv);
            for (int i = 0; i < cnt; i++) if ((zm >> i) & 1) vram[za[i]] = z24 ? (vram[za[i]] & 0xff000000u) | (lz[i] & 0xffffff) : lz[i];
        }
    }
    return n;
}

#define SSE_SPAN(T, Z) \
    __attribute__((target("sse4.1"))) static uint64_t span_sse_t##T##z##Z(const DrawState *d, int y, int x0, int x1, SpanAttr *A) \
    { return span_sse_body(d, y, x0, x1, A, T, Z); }
SSE_SPAN(0, 0) SSE_SPAN(0, 1) SSE_SPAN(1, 0) SSE_SPAN(1, 1) SSE_SPAN(2, 0) SSE_SPAN(2, 1)
#undef SSE_SPAN
static const SpanFn span_sse_tab[3][2] = {
    {span_sse_t0z0, span_sse_t0z1}, {span_sse_t1z0, span_sse_t1z1}, {span_sse_t2z0, span_sse_t2z1}};
#endif
