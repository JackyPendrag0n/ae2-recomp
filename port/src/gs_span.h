/* Span renderer template. Included several times from gs_raster.h with
 *   SP_NAME  function name
 *   SP_TEX   0 untextured, 1 nearest, 2 bilinear
 *   SP_FST   1: UV (already in texels), 0: STQ perspective
 *   SP_ABE   alpha blending possible
 *   SP_Z     depth buffer read or written
 * Everything else stays a (loop-invariant, well predicted) runtime test. */
static uint64_t SP_NAME(const DrawState *d, int y, int x0, int x1, SpanAttr *A) {
    RowAddr fr, zr;
    row_addr(&fr, d->fbp, d->fbw, d->fpsm, y);
#if SP_Z
    row_addr(&zr, d->zbp, d->fbw, d->zpsm, y);
    const int zread = d->zread, zwrite = d->zwrite, z16 = zr.is16, z24 = d->zpsm == PSMZ24, zgreater = d->ztst == 3;
#else
    (void)zr;
#endif
    uint16_t *v16 = (uint16_t *)vram;
    const int fb16 = fr.is16, fb24 = d->fb24;
#if SP_TEX
    const TexState *t = &d->tex;
    const int tfx = t->tfx, tcc = t->tcc;
#if !SP_FST
    const float tws = (float)(1 << t->tw), ths = (float)(1 << t->th);
#endif
#endif
    const int iip = d->iip, fge = d->fge, ate = d->ate, date = d->date;
    const uint32_t fbmsk = d->fbmsk;
#if SP_ABE
    const int abe = d->abe, std_blend = d->a_a == 0 && d->a_b == 1 && d->a_c == 0 && d->a_d == 1;
#endif
    const int need_dst = d->need_dst;
    /* attribute of pixel i = start + (float)i * step (same formula as the SIMD span) */
    const double z0 = A->z, dz = A->dz;
    const float r0 = A->r, g0 = A->g, b0 = A->b, a0 = A->a;
    const float dr = A->dr, dg = A->dg, db = A->db, da = A->da;
#if SP_TEX
#if SP_FST
    const float u0 = A->u, v0 = A->v, du = A->du, dv = A->dv;
#else
    const float s0 = A->s, t0 = A->t, q0 = A->q, ds = A->ds, dt = A->dt, dq = A->dq;
#endif
#endif
    const float f0 = A->f, df = A->df;
    const uint32_t zmax = d->zmax;
    uint64_t n = 0;
    for (int x = x0; x < x1; x++) {
        int ir, ig, ib, ia;
        uint32_t za = 0, zv = 0;
        const int pi = x - x0; const float fi = (float)pi;
#if SP_Z
        double z = z0 + (double)pi * dz;
        zv = z >= (double)zmax ? zmax : z <= 0 ? 0 : (uint32_t)z;
        za = z16 ? ra16(&zr, x) : ra32(&zr, x);
        if (zread) {
            uint32_t zd = z16 ? v16[za] : (z24 ? vram[za] & 0xffffff : vram[za]);
            if (zgreater ? zv <= zd : zv < zd) goto next;
        }
#endif
        if (iip) { ir = (int)(r0 + fi * dr); ig = (int)(g0 + fi * dg); ib = (int)(b0 + fi * db); ia = (int)(a0 + fi * da); }
        else { ir = (int)A->r; ig = (int)A->g; ib = (int)A->b; ia = (int)A->a; }
#if SP_TEX
        {
#if SP_FST
            float fu = u0 + fi * du, fv = v0 + fi * dv;
#else
            float q = q0 + fi * dq, s = s0 + fi * ds, tt = t0 + fi * dt;
            float iq = q != 0.0f ? 1.0f / q : 0.0f;
            float fu = s * iq * tws, fv = tt * iq * ths;
#endif
#if SP_TEX == 1
            uint32_t tc = tfetch(t, ffloor(fu), ffloor(fv));
#else
            uint32_t tc = tsample(t, fu, fv);
#endif
            int tr = tc & 255, tg = (tc >> 8) & 255, tb = (tc >> 16) & 255, ta = tc >> 24;
            switch (tfx) {
            case 0: ir = (ir * tr) >> 7; ig = (ig * tg) >> 7; ib = (ib * tb) >> 7; if (tcc) ia = (ia * ta) >> 7; break;
            case 1: ir = tr; ig = tg; ib = tb; if (tcc) ia = ta; break;
            case 2: ir = ((ir * tr) >> 7) + ia; ig = ((ig * tg) >> 7) + ia; ib = ((ib * tb) >> 7) + ia; if (tcc) ia = ia + ta; break;
            default: ir = ((ir * tr) >> 7) + ia; ig = ((ig * tg) >> 7) + ia; ib = ((ib * tb) >> 7) + ia; if (tcc) ia = ta; break;
            }
            if ((ir | ig | ib | ia) & ~255) { ir = clampc(ir); ig = clampc(ig); ib = clampc(ib); ia = clampc(ia); }
        }
#endif
        if (fge) {
            int fo = (int)(f0 + fi * df); uint32_t fc = d->fogcol;
            ir = (fo * ir + (255 - fo) * (int)(fc & 255)) >> 8;
            ig = (fo * ig + (255 - fo) * (int)((fc >> 8) & 255)) >> 8;
            ib = (fo * ib + (255 - fo) * (int)((fc >> 16) & 255)) >> 8;
        }
        {
        int write_fb = 1, write_z = 0, rgb_only = 0;
#if SP_Z
        write_z = zwrite;
#endif
        if (ate) {
            int pass;
            switch (d->atst) {
            case 0: pass = 0; break; case 1: pass = 1; break;
            case 2: pass = ia < d->aref; break; case 3: pass = ia <= d->aref; break;
            case 4: pass = ia == d->aref; break; case 5: pass = ia >= d->aref; break;
            case 6: pass = ia > d->aref; break; default: pass = ia != d->aref; break;
            }
            if (!pass) {
                switch (d->afail) {
                case 0: goto next;
                case 1: write_z = 0; break;
                case 2: write_fb = 0; break;
                case 3: write_z = 0; rgb_only = 1; break;
                }
            }
        }
        uint32_t fa = fb16 ? ra16(&fr, x) : ra32(&fr, x);
        uint32_t dst = 0;
        if (need_dst || rgb_only) {
            if (fb16) { uint32_t raw = v16[fa]; dst = ((raw & 0x1f) << 3) | (((raw >> 5) & 0x1f) << 11) | (((raw >> 10) & 0x1f) << 19) | ((raw & 0x8000) ? 0x80000000u : 0); }
            else if (fb24) dst = vram[fa] | 0x80000000u;
            else dst = vram[fa];
        }
        if (date && (int)((dst >> 31) & 1) != d->datm) goto next;
        n++;
        if (write_fb) {
#if SP_ABE
            if (abe && !(d->pabe && ia < 0x80)) {
                int cd0 = dst & 255, cd1 = (dst >> 8) & 255, cd2 = (dst >> 16) & 255;
                if (std_blend) {
                    ir = (((ir - cd0) * ia) >> 7) + cd0; ig = (((ig - cd1) * ia) >> 7) + cd1; ib = (((ib - cd2) * ia) >> 7) + cd2;
                } else {
                    int ad = dst >> 24;
                    int cc = d->a_c == 0 ? ia : d->a_c == 1 ? ad : d->fix;
                    int A0 = d->a_a == 0 ? ir : d->a_a == 1 ? cd0 : 0, A1 = d->a_a == 0 ? ig : d->a_a == 1 ? cd1 : 0, A2 = d->a_a == 0 ? ib : d->a_a == 1 ? cd2 : 0;
                    int B0 = d->a_b == 0 ? ir : d->a_b == 1 ? cd0 : 0, B1 = d->a_b == 0 ? ig : d->a_b == 1 ? cd1 : 0, B2 = d->a_b == 0 ? ib : d->a_b == 1 ? cd2 : 0;
                    int D0 = d->a_d == 0 ? ir : d->a_d == 1 ? cd0 : 0, D1 = d->a_d == 0 ? ig : d->a_d == 1 ? cd1 : 0, D2 = d->a_d == 0 ? ib : d->a_d == 1 ? cd2 : 0;
                    ir = (((A0 - B0) * cc) >> 7) + D0; ig = (((A1 - B1) * cc) >> 7) + D1; ib = (((A2 - B2) * cc) >> 7) + D2;
                }
                if (d->colclamp) { if ((ir | ig | ib) & ~255) { ir = clampc(ir); ig = clampc(ig); ib = clampc(ib); } }
                else { ir &= 255; ig &= 255; ib &= 255; }
            }
#endif
            int oa = ia; if (d->fba) oa |= 0x80;
            uint32_t col = (uint32_t)(ir & 255) | ((uint32_t)(ig & 255) << 8) | ((uint32_t)(ib & 255) << 16) | ((uint32_t)(oa & 255) << 24);
            if (rgb_only) col = (col & 0xffffff) | (dst & 0xff000000u);
            if (fb16) {
                uint32_t c16 = ((col >> 3) & 0x1f) | (((col >> 11) & 0x1f) << 5) | (((col >> 19) & 0x1f) << 10) | ((col >> 31) << 15);
                if (fbmsk) {
                    uint32_t m16 = ((fbmsk >> 3) & 0x1f) | (((fbmsk >> 11) & 0x1f) << 5) | (((fbmsk >> 19) & 0x1f) << 10) | ((fbmsk >> 31) << 15);
                    c16 = (c16 & ~m16) | (v16[fa] & m16);
                }
                v16[fa] = (uint16_t)c16;
            } else {
                if (fbmsk) col = (col & ~fbmsk) | (vram[fa] & fbmsk);
                if (fb24) col = (vram[fa] & 0xff000000u) | (col & 0xffffff);
                vram[fa] = col;
            }
        }
#if SP_Z
        if (write_z) {
            if (z16) v16[za] = (uint16_t)zv;
            else if (z24) vram[za] = (vram[za] & 0xff000000u) | (zv & 0xffffff);
            else vram[za] = zv;
        }
#endif
        }
    next:;
    }

    return n;
}
#undef SP_NAME
#undef SP_TEX
#undef SP_FST
#undef SP_ABE
#undef SP_Z
