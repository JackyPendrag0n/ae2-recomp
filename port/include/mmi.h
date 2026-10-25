/* R5900 multimedia (128-bit SIMD integer) instructions. */
#pragma once

#define MMI_BEGIN r128 _d; const r128 _s = c->r[rs], _t = c->r[rt]; (void)_s; (void)_t;
#define MMI_END   if (rd) c->r[rd] = _d;
#define LANES(n, body) for (int i = 0; i < n; i++) { body; }

static inline int32_t sat32(int64_t v) { return v > 0x7fffffff ? 0x7fffffff : v < -0x80000000ll ? (int32_t)0x80000000 : (int32_t)v; }
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int8_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }

#define DEF3(name, body) static inline void mmi_##name(Ctx *c, int rd, int rs, int rt) { MMI_BEGIN body; MMI_END }
DEF3(PADDW, LANES(4, _d.uw[i] = _s.uw[i] + _t.uw[i]))
DEF3(PSUBW, LANES(4, _d.uw[i] = _s.uw[i] - _t.uw[i]))
DEF3(PCGTW, LANES(4, _d.uw[i] = _s.sw[i] > _t.sw[i] ? ~0u : 0))
DEF3(PMAXW, LANES(4, _d.sw[i] = _s.sw[i] > _t.sw[i] ? _s.sw[i] : _t.sw[i]))
DEF3(PADDH, LANES(8, _d.uh[i] = _s.uh[i] + _t.uh[i]))
DEF3(PSUBH, LANES(8, _d.uh[i] = _s.uh[i] - _t.uh[i]))
DEF3(PCGTH, LANES(8, _d.uh[i] = _s.sh[i] > _t.sh[i] ? 0xffff : 0))
DEF3(PMAXH, LANES(8, _d.sh[i] = _s.sh[i] > _t.sh[i] ? _s.sh[i] : _t.sh[i]))
DEF3(PADDB, LANES(16, _d.ub[i] = _s.ub[i] + _t.ub[i]))
DEF3(PSUBB, LANES(16, _d.ub[i] = _s.ub[i] - _t.ub[i]))
DEF3(PCGTB, LANES(16, _d.ub[i] = _s.sb[i] > _t.sb[i] ? 0xff : 0))
DEF3(PADDSW, LANES(4, _d.sw[i] = sat32((int64_t)_s.sw[i] + _t.sw[i])))
DEF3(PSUBSW, LANES(4, _d.sw[i] = sat32((int64_t)_s.sw[i] - _t.sw[i])))
DEF3(PEXTLW, _d.uw[0] = _t.uw[0]; _d.uw[1] = _s.uw[0]; _d.uw[2] = _t.uw[1]; _d.uw[3] = _s.uw[1])
DEF3(PPACW, _d.uw[0] = _t.uw[0]; _d.uw[1] = _t.uw[2]; _d.uw[2] = _s.uw[0]; _d.uw[3] = _s.uw[2])
DEF3(PADDSH, LANES(8, _d.sh[i] = sat16(_s.sh[i] + _t.sh[i])))
DEF3(PSUBSH, LANES(8, _d.sh[i] = sat16(_s.sh[i] - _t.sh[i])))
DEF3(PEXTLH, LANES(4, _d.uh[2*i] = _t.uh[i]; _d.uh[2*i+1] = _s.uh[i]))
DEF3(PPACH, LANES(4, _d.uh[i] = _t.uh[2*i]; _d.uh[i+4] = _s.uh[2*i]))
DEF3(PADDSB, LANES(16, _d.sb[i] = sat8(_s.sb[i] + _t.sb[i])))
DEF3(PSUBSB, LANES(16, _d.sb[i] = sat8(_s.sb[i] - _t.sb[i])))
DEF3(PEXTLB, LANES(8, _d.ub[2*i] = _t.ub[i]; _d.ub[2*i+1] = _s.ub[i]))
DEF3(PPACB, LANES(8, _d.ub[i] = _t.ub[2*i]; _d.ub[i+8] = _s.ub[2*i]))
DEF3(PEXT5, LANES(4, uint32_t v = _t.uw[i]; _d.uw[i] = ((v & 0x1f) << 3) | ((v & 0x3e0) << 6) | ((v & 0x7c00) << 9) | ((v & 0x8000) << 16)))
DEF3(PPAC5, LANES(4, uint32_t v = _t.uw[i]; _d.uw[i] = ((v >> 3) & 0x1f) | ((v >> 6) & 0x3e0) | ((v >> 9) & 0x7c00) | ((v >> 16) & 0x8000)))
DEF3(PABSW, LANES(4, _d.sw[i] = _t.sw[i] == (int32_t)0x80000000 ? 0x7fffffff : (_t.sw[i] < 0 ? -_t.sw[i] : _t.sw[i])))
DEF3(PCEQW, LANES(4, _d.uw[i] = _s.uw[i] == _t.uw[i] ? ~0u : 0))
DEF3(PMINW, LANES(4, _d.sw[i] = _s.sw[i] < _t.sw[i] ? _s.sw[i] : _t.sw[i]))
DEF3(PADSBH, LANES(4, _d.uh[i] = _s.uh[i] - _t.uh[i]); for (int i = 4; i < 8; i++) _d.uh[i] = _s.uh[i] + _t.uh[i])
DEF3(PABSH, LANES(8, _d.sh[i] = _t.sh[i] == -32768 ? 32767 : (_t.sh[i] < 0 ? -_t.sh[i] : _t.sh[i])))
DEF3(PCEQH, LANES(8, _d.uh[i] = _s.uh[i] == _t.uh[i] ? 0xffff : 0))
DEF3(PMINH, LANES(8, _d.sh[i] = _s.sh[i] < _t.sh[i] ? _s.sh[i] : _t.sh[i]))
DEF3(PCEQB, LANES(16, _d.ub[i] = _s.ub[i] == _t.ub[i] ? 0xff : 0))
DEF3(PADDUW, LANES(4, uint64_t v = (uint64_t)_s.uw[i] + _t.uw[i]; _d.uw[i] = v > 0xffffffffull ? 0xffffffffu : (uint32_t)v))
DEF3(PSUBUW, LANES(4, int64_t v = (int64_t)_s.uw[i] - _t.uw[i]; _d.uw[i] = v < 0 ? 0 : (uint32_t)v))
DEF3(PEXTUW, _d.uw[0] = _t.uw[2]; _d.uw[1] = _s.uw[2]; _d.uw[2] = _t.uw[3]; _d.uw[3] = _s.uw[3])
DEF3(PADDUH, LANES(8, uint32_t v = (uint32_t)_s.uh[i] + _t.uh[i]; _d.uh[i] = v > 0xffff ? 0xffff : v))
DEF3(PSUBUH, LANES(8, int32_t v = (int32_t)_s.uh[i] - _t.uh[i]; _d.uh[i] = v < 0 ? 0 : v))
DEF3(PEXTUH, LANES(4, _d.uh[2*i] = _t.uh[i+4]; _d.uh[2*i+1] = _s.uh[i+4]))
DEF3(PADDUB, LANES(16, uint32_t v = (uint32_t)_s.ub[i] + _t.ub[i]; _d.ub[i] = v > 0xff ? 0xff : v))
DEF3(PSUBUB, LANES(16, int32_t v = (int32_t)_s.ub[i] - _t.ub[i]; _d.ub[i] = v < 0 ? 0 : v))
DEF3(PEXTUB, LANES(8, _d.ub[2*i] = _t.ub[i+8]; _d.ub[2*i+1] = _s.ub[i+8]))
static inline void mmi_QFSRV(Ctx *c, int rd, int rs, int rt) {
    MMI_BEGIN
    unsigned sh = (unsigned)(c->sa & 15) * 8;
    if (sh == 0) _d = _t;
    else if (sh < 64) {
        _d.ud[0] = (_t.ud[0] >> sh) | (_t.ud[1] << (64 - sh));
        _d.ud[1] = (_t.ud[1] >> sh) | (_s.ud[0] << (64 - sh));
    } else if (sh == 64) { _d.ud[0] = _t.ud[1]; _d.ud[1] = _s.ud[0]; }
    else {
        sh -= 64;
        _d.ud[0] = (_t.ud[1] >> sh) | (_s.ud[0] << (64 - sh));
        _d.ud[1] = (_s.ud[0] >> sh) | (_s.ud[1] << (64 - sh));
    }
    MMI_END
}
/* MMI2 */
static inline void mmi_PMADDW(Ctx *c, int rd, int rs, int rt) {
    MMI_BEGIN
    for (int i = 0; i < 2; i++) {
        int64_t acc = ((int64_t)c->hi.uw[2*i] << 32) | c->lo.uw[2*i];
        acc += (int64_t)_s.sw[2*i] * _t.sw[2*i];
        c->lo.sd[i] = (int32_t)acc; c->hi.sd[i] = (int32_t)(acc >> 32); _d.sd[i] = acc;
    }
    MMI_END
}
static inline void mmi_PMSUBW(Ctx *c, int rd, int rs, int rt) {
    MMI_BEGIN
    for (int i = 0; i < 2; i++) {
        int64_t acc = ((int64_t)c->hi.uw[2*i] << 32) | c->lo.uw[2*i];
        acc -= (int64_t)_s.sw[2*i] * _t.sw[2*i];
        c->lo.sd[i] = (int32_t)acc; c->hi.sd[i] = (int32_t)(acc >> 32); _d.sd[i] = acc;
    }
    MMI_END
}
static inline void mmi_PMULTW(Ctx *c, int rd, int rs, int rt) {
    MMI_BEGIN
    for (int i = 0; i < 2; i++) {
        int64_t p = (int64_t)_s.sw[2*i] * _t.sw[2*i];
        c->lo.sd[i] = (int32_t)p; c->hi.sd[i] = (int32_t)(p >> 32); _d.sd[i] = p;
    }
    MMI_END
}
static inline void mmi_PMULTUW(Ctx *c, int rd, int rs, int rt) {
    MMI_BEGIN
    for (int i = 0; i < 2; i++) {
        uint64_t p = (uint64_t)_s.uw[2*i] * _t.uw[2*i];
        c->lo.sd[i] = (int32_t)p; c->hi.sd[i] = (int32_t)(p >> 32); _d.ud[i] = p;
    }
    MMI_END
}
static inline void mmi_PMADDUW(Ctx *c, int rd, int rs, int rt) {
    MMI_BEGIN
    for (int i = 0; i < 2; i++) {
        uint64_t acc = ((uint64_t)c->hi.uw[2*i] << 32) | c->lo.uw[2*i];
        acc += (uint64_t)_s.uw[2*i] * _t.uw[2*i];
        c->lo.sd[i] = (int32_t)acc; c->hi.sd[i] = (int32_t)(acc >> 32); _d.ud[i] = acc;
    }
    MMI_END
}
static inline void mmi_PDIVW(Ctx *c, int rd, int rs, int rt) {
    (void)rd; for (int i = 0; i < 2; i++) op_div(c, c->r[rs].sw[2*i], c->r[rt].sw[2*i], i);
}
static inline void mmi_PDIVUW(Ctx *c, int rd, int rs, int rt) {
    (void)rd; for (int i = 0; i < 2; i++) op_divu(c, c->r[rs].uw[2*i], c->r[rt].uw[2*i], i);
}
static inline void mmi_PDIVBW(Ctx *c, int rd, int rs, int rt) {
    (void)rd; int16_t d = c->r[rt].sh[0];
    for (int i = 0; i < 4; i++) {
        int32_t n = c->r[rs].sw[i];
        if (d == 0) { c->lo.sw[i] = n < 0 ? 1 : -1; c->hi.sw[i] = n; }
        else if (n == (int32_t)0x80000000 && d == -1) { c->lo.sw[i] = n; c->hi.sw[i] = 0; }
        else { c->lo.sw[i] = n / d; c->hi.sw[i] = (int16_t)(n % d); }
    }
}
DEF3(PSLLVW, for (int i = 0; i < 2; i++) _d.sd[i] = (int32_t)(_t.uw[2*i] << (_s.uw[2*i] & 31)))
DEF3(PSRLVW, for (int i = 0; i < 2; i++) _d.sd[i] = (int32_t)(_t.uw[2*i] >> (_s.uw[2*i] & 31)))
DEF3(PSRAVW, for (int i = 0; i < 2; i++) _d.sd[i] = (int32_t)(_t.sw[2*i] >> (_s.uw[2*i] & 31)))
static inline void mmi_PMFHI(Ctx *c, int rd, int rs, int rt) { (void)rs; (void)rt; if (rd) c->r[rd] = c->hi; }
static inline void mmi_PMFLO(Ctx *c, int rd, int rs, int rt) { (void)rs; (void)rt; if (rd) c->r[rd] = c->lo; }
static inline void mmi_PMTHI(Ctx *c, int rd, int rs, int rt) { (void)rd; (void)rt; c->hi = c->r[rs]; }
static inline void mmi_PMTLO(Ctx *c, int rd, int rs, int rt) { (void)rd; (void)rt; c->lo = c->r[rs]; }
DEF3(PINTH, LANES(4, _d.uh[2*i] = _t.uh[i]; _d.uh[2*i+1] = _s.uh[i+4]))
DEF3(PINTEH, LANES(4, _d.uh[2*i] = _t.uh[2*i]; _d.uh[2*i+1] = _s.uh[2*i]))
DEF3(PCPYLD, _d.ud[0] = _t.ud[0]; _d.ud[1] = _s.ud[0])
DEF3(PCPYUD, _d.ud[0] = _s.ud[1]; _d.ud[1] = _t.ud[1])
DEF3(PAND, _d.ud[0] = _s.ud[0] & _t.ud[0]; _d.ud[1] = _s.ud[1] & _t.ud[1])
DEF3(POR, _d.ud[0] = _s.ud[0] | _t.ud[0]; _d.ud[1] = _s.ud[1] | _t.ud[1])
DEF3(PXOR, _d.ud[0] = _s.ud[0] ^ _t.ud[0]; _d.ud[1] = _s.ud[1] ^ _t.ud[1])
DEF3(PNOR, _d.ud[0] = ~(_s.ud[0] | _t.ud[0]); _d.ud[1] = ~(_s.ud[1] | _t.ud[1]))
static inline void mmi_hmul(Ctx *c, int rd, int rs, int rt, int mode) {
    /* mode 0 PMADDH, 1 PMSUBH, 2 PMULTH, 3 PHMADH, 4 PHMSBH */
    MMI_BEGIN
    int32_t p[8];
    for (int i = 0; i < 8; i++) p[i] = (int32_t)_s.sh[i] * _t.sh[i];
    if (mode <= 2) {
        /* lane order: lo.w0=p0 lo.w1=p1 hi.w0=p2 hi.w1=p3 lo.w2=p4 lo.w3=p5 hi.w2=p6 hi.w3=p7 */
        int map_lo[4] = {0, 1, 4, 5}, map_hi[4] = {2, 3, 6, 7};
        for (int i = 0; i < 4; i++) {
            int32_t l = p[map_lo[i]], h = p[map_hi[i]];
            if (mode == 0) { l += c->lo.sw[i]; h += c->hi.sw[i]; }
            if (mode == 1) { l = c->lo.sw[i] - l; h = c->hi.sw[i] - h; }
            c->lo.sw[i] = l; c->hi.sw[i] = h;
        }
        _d.sw[0] = c->lo.sw[0]; _d.sw[1] = c->hi.sw[0]; _d.sw[2] = c->lo.sw[2]; _d.sw[3] = c->hi.sw[2];
    } else {
        int32_t r[4];
        for (int i = 0; i < 4; i++) r[i] = mode == 3 ? p[2*i+1] + p[2*i] : p[2*i+1] - p[2*i];
        c->lo.sw[0] = r[0]; c->lo.sw[1] = p[1]; c->hi.sw[0] = r[1]; c->hi.sw[1] = p[3];
        c->lo.sw[2] = r[2]; c->lo.sw[3] = p[5]; c->hi.sw[2] = r[3]; c->hi.sw[3] = p[7];
        for (int i = 0; i < 4; i++) _d.sw[i] = r[i];
    }
    MMI_END
}
static inline void mmi_PMADDH(Ctx *c, int rd, int rs, int rt) { mmi_hmul(c, rd, rs, rt, 0); }
static inline void mmi_PMSUBH(Ctx *c, int rd, int rs, int rt) { mmi_hmul(c, rd, rs, rt, 1); }
static inline void mmi_PMULTH(Ctx *c, int rd, int rs, int rt) { mmi_hmul(c, rd, rs, rt, 2); }
static inline void mmi_PHMADH(Ctx *c, int rd, int rs, int rt) { mmi_hmul(c, rd, rs, rt, 3); }
static inline void mmi_PHMSBH(Ctx *c, int rd, int rs, int rt) { mmi_hmul(c, rd, rs, rt, 4); }
DEF3(PEXEH, _d = _t; _d.uh[0] = _t.uh[2]; _d.uh[2] = _t.uh[0]; _d.uh[4] = _t.uh[6]; _d.uh[6] = _t.uh[4])
DEF3(PREVH, LANES(4, _d.uh[i] = _t.uh[3-i]; _d.uh[i+4] = _t.uh[7-i]))
DEF3(PEXEW, _d.uw[0] = _t.uw[2]; _d.uw[1] = _t.uw[1]; _d.uw[2] = _t.uw[0]; _d.uw[3] = _t.uw[3])
DEF3(PROT3W, _d.uw[0] = _t.uw[1]; _d.uw[1] = _t.uw[2]; _d.uw[2] = _t.uw[0]; _d.uw[3] = _t.uw[3])
DEF3(PEXCH, _d.uh[0] = _t.uh[0]; _d.uh[1] = _t.uh[2]; _d.uh[2] = _t.uh[1]; _d.uh[3] = _t.uh[3];
            _d.uh[4] = _t.uh[4]; _d.uh[5] = _t.uh[6]; _d.uh[6] = _t.uh[5]; _d.uh[7] = _t.uh[7])
DEF3(PCPYH, LANES(4, _d.uh[i] = _t.uh[0]; _d.uh[i+4] = _t.uh[4]))
DEF3(PEXCW, _d.uw[0] = _t.uw[0]; _d.uw[1] = _t.uw[2]; _d.uw[2] = _t.uw[1]; _d.uw[3] = _t.uw[3])

#define MMI_PADDW(d,s,t) mmi_PADDW(c,d,s,t)
#define MMI_PSUBW(d,s,t) mmi_PSUBW(c,d,s,t)
#define MMI_PCGTW(d,s,t) mmi_PCGTW(c,d,s,t)
#define MMI_PMAXW(d,s,t) mmi_PMAXW(c,d,s,t)
#define MMI_PADDH(d,s,t) mmi_PADDH(c,d,s,t)
#define MMI_PSUBH(d,s,t) mmi_PSUBH(c,d,s,t)
#define MMI_PCGTH(d,s,t) mmi_PCGTH(c,d,s,t)
#define MMI_PMAXH(d,s,t) mmi_PMAXH(c,d,s,t)
#define MMI_PADDB(d,s,t) mmi_PADDB(c,d,s,t)
#define MMI_PSUBB(d,s,t) mmi_PSUBB(c,d,s,t)
#define MMI_PCGTB(d,s,t) mmi_PCGTB(c,d,s,t)
#define MMI_PADDSW(d,s,t) mmi_PADDSW(c,d,s,t)
#define MMI_PSUBSW(d,s,t) mmi_PSUBSW(c,d,s,t)
#define MMI_PEXTLW(d,s,t) mmi_PEXTLW(c,d,s,t)
#define MMI_PPACW(d,s,t) mmi_PPACW(c,d,s,t)
#define MMI_PADDSH(d,s,t) mmi_PADDSH(c,d,s,t)
#define MMI_PSUBSH(d,s,t) mmi_PSUBSH(c,d,s,t)
#define MMI_PEXTLH(d,s,t) mmi_PEXTLH(c,d,s,t)
#define MMI_PPACH(d,s,t) mmi_PPACH(c,d,s,t)
#define MMI_PADDSB(d,s,t) mmi_PADDSB(c,d,s,t)
#define MMI_PSUBSB(d,s,t) mmi_PSUBSB(c,d,s,t)
#define MMI_PEXTLB(d,s,t) mmi_PEXTLB(c,d,s,t)
#define MMI_PPACB(d,s,t) mmi_PPACB(c,d,s,t)
#define MMI_PEXT5(d,s,t) mmi_PEXT5(c,d,s,t)
#define MMI_PPAC5(d,s,t) mmi_PPAC5(c,d,s,t)
#define MMI_PABSW(d,s,t) mmi_PABSW(c,d,s,t)
#define MMI_PCEQW(d,s,t) mmi_PCEQW(c,d,s,t)
#define MMI_PMINW(d,s,t) mmi_PMINW(c,d,s,t)
#define MMI_PADSBH(d,s,t) mmi_PADSBH(c,d,s,t)
#define MMI_PABSH(d,s,t) mmi_PABSH(c,d,s,t)
#define MMI_PCEQH(d,s,t) mmi_PCEQH(c,d,s,t)
#define MMI_PMINH(d,s,t) mmi_PMINH(c,d,s,t)
#define MMI_PCEQB(d,s,t) mmi_PCEQB(c,d,s,t)
#define MMI_PADDUW(d,s,t) mmi_PADDUW(c,d,s,t)
#define MMI_PSUBUW(d,s,t) mmi_PSUBUW(c,d,s,t)
#define MMI_PEXTUW(d,s,t) mmi_PEXTUW(c,d,s,t)
#define MMI_PADDUH(d,s,t) mmi_PADDUH(c,d,s,t)
#define MMI_PSUBUH(d,s,t) mmi_PSUBUH(c,d,s,t)
#define MMI_PEXTUH(d,s,t) mmi_PEXTUH(c,d,s,t)
#define MMI_PADDUB(d,s,t) mmi_PADDUB(c,d,s,t)
#define MMI_PSUBUB(d,s,t) mmi_PSUBUB(c,d,s,t)
#define MMI_PEXTUB(d,s,t) mmi_PEXTUB(c,d,s,t)
#define MMI_QFSRV(d,s,t) mmi_QFSRV(c,d,s,t)
#define MMI_PMADDW(d,s,t) mmi_PMADDW(c,d,s,t)
#define MMI_PSLLVW(d,s,t) mmi_PSLLVW(c,d,s,t)
#define MMI_PSRLVW(d,s,t) mmi_PSRLVW(c,d,s,t)
#define MMI_PMSUBW(d,s,t) mmi_PMSUBW(c,d,s,t)
#define MMI_PMFHI(d,s,t) mmi_PMFHI(c,d,s,t)
#define MMI_PMFLO(d,s,t) mmi_PMFLO(c,d,s,t)
#define MMI_PINTH(d,s,t) mmi_PINTH(c,d,s,t)
#define MMI_PMULTW(d,s,t) mmi_PMULTW(c,d,s,t)
#define MMI_PDIVW(d,s,t) mmi_PDIVW(c,d,s,t)
#define MMI_PCPYLD(d,s,t) mmi_PCPYLD(c,d,s,t)
#define MMI_PMADDH(d,s,t) mmi_PMADDH(c,d,s,t)
#define MMI_PHMADH(d,s,t) mmi_PHMADH(c,d,s,t)
#define MMI_PAND(d,s,t) mmi_PAND(c,d,s,t)
#define MMI_PXOR(d,s,t) mmi_PXOR(c,d,s,t)
#define MMI_PMSUBH(d,s,t) mmi_PMSUBH(c,d,s,t)
#define MMI_PHMSBH(d,s,t) mmi_PHMSBH(c,d,s,t)
#define MMI_PEXEH(d,s,t) mmi_PEXEH(c,d,s,t)
#define MMI_PREVH(d,s,t) mmi_PREVH(c,d,s,t)
#define MMI_PMULTH(d,s,t) mmi_PMULTH(c,d,s,t)
#define MMI_PDIVBW(d,s,t) mmi_PDIVBW(c,d,s,t)
#define MMI_PEXEW(d,s,t) mmi_PEXEW(c,d,s,t)
#define MMI_PROT3W(d,s,t) mmi_PROT3W(c,d,s,t)
#define MMI_PMADDUW(d,s,t) mmi_PMADDUW(c,d,s,t)
#define MMI_PSRAVW(d,s,t) mmi_PSRAVW(c,d,s,t)
#define MMI_PMTHI(d,s,t) mmi_PMTHI(c,d,s,t)
#define MMI_PMTLO(d,s,t) mmi_PMTLO(c,d,s,t)
#define MMI_PINTEH(d,s,t) mmi_PINTEH(c,d,s,t)
#define MMI_PMULTUW(d,s,t) mmi_PMULTUW(c,d,s,t)
#define MMI_PDIVUW(d,s,t) mmi_PDIVUW(c,d,s,t)
#define MMI_PCPYUD(d,s,t) mmi_PCPYUD(c,d,s,t)
#define MMI_POR(d,s,t) mmi_POR(c,d,s,t)
#define MMI_PNOR(d,s,t) mmi_PNOR(c,d,s,t)
#define MMI_PEXCH(d,s,t) mmi_PEXCH(c,d,s,t)
#define MMI_PCPYH(d,s,t) mmi_PCPYH(c,d,s,t)
#define MMI_PEXCW(d,s,t) mmi_PEXCW(c,d,s,t)

/* shifts by immediate */
#define MMI_PSLLH(d,t,sa) do { r128 _x = c->r[t]; for (int i=0;i<8;i++) _x.uh[i] <<= ((sa)&15); if (d) c->r[d] = _x; } while (0)
#define MMI_PSRLH(d,t,sa) do { r128 _x = c->r[t]; for (int i=0;i<8;i++) _x.uh[i] >>= ((sa)&15); if (d) c->r[d] = _x; } while (0)
#define MMI_PSRAH(d,t,sa) do { r128 _x = c->r[t]; for (int i=0;i<8;i++) _x.sh[i] >>= ((sa)&15); if (d) c->r[d] = _x; } while (0)
#define MMI_PSLLW(d,t,sa) do { r128 _x = c->r[t]; for (int i=0;i<4;i++) _x.uw[i] <<= (sa); if (d) c->r[d] = _x; } while (0)
#define MMI_PSRLW(d,t,sa) do { r128 _x = c->r[t]; for (int i=0;i<4;i++) _x.uw[i] >>= (sa); if (d) c->r[d] = _x; } while (0)
#define MMI_PSRAW(d,t,sa) do { r128 _x = c->r[t]; for (int i=0;i<4;i++) _x.sw[i] >>= (sa); if (d) c->r[d] = _x; } while (0)

static inline int lzc_minus1(int32_t v) { uint32_t x = v < 0 ? ~(uint32_t)v : (uint32_t)v; return (x ? __builtin_clz(x) : 32) - 1; }
#define MMI_PLZCW(d,s) do { if (d) { c->r[d].uw[0] = lzc_minus1(c->r[s].sw[0]); c->r[d].uw[1] = lzc_minus1(c->r[s].sw[1]); } } while (0)

static inline void mmi_pmfhl(Ctx *c, int rd, int fmt) {
    r128 d;
    switch (fmt) {
    case 0: d.uw[0] = c->lo.uw[0]; d.uw[1] = c->hi.uw[0]; d.uw[2] = c->lo.uw[2]; d.uw[3] = c->hi.uw[2]; break;
    case 1: d.uw[0] = c->lo.uw[1]; d.uw[1] = c->hi.uw[1]; d.uw[2] = c->lo.uw[3]; d.uw[3] = c->hi.uw[3]; break;
    case 2: for (int i = 0; i < 2; i++) {
                int64_t v = ((int64_t)c->hi.uw[2*i] << 32) | c->lo.uw[2*i];
                d.sd[i] = sat32(v);
            } break;
    case 3: d.uh[0] = c->lo.uh[0]; d.uh[1] = c->lo.uh[2]; d.uh[2] = c->hi.uh[0]; d.uh[3] = c->hi.uh[2];
            d.uh[4] = c->lo.uh[4]; d.uh[5] = c->lo.uh[6]; d.uh[6] = c->hi.uh[4]; d.uh[7] = c->hi.uh[6]; break;
    default: d.sh[0] = sat16(c->lo.sw[0]); d.sh[1] = sat16(c->lo.sw[1]); d.sh[2] = sat16(c->hi.sw[0]); d.sh[3] = sat16(c->hi.sw[1]);
             d.sh[4] = sat16(c->lo.sw[2]); d.sh[5] = sat16(c->lo.sw[3]); d.sh[6] = sat16(c->hi.sw[2]); d.sh[7] = sat16(c->hi.sw[3]); break;
    }
    if (rd) c->r[rd] = d;
}
#define MMI_PMFHL(d, fmt) mmi_pmfhl(c, d, fmt)
#define MMI_PMTHL(s, fmt) do { if ((fmt) == 0) { c->lo.uw[0] = c->r[s].uw[0]; c->hi.uw[0] = c->r[s].uw[1]; \
    c->lo.uw[2] = c->r[s].uw[2]; c->hi.uw[2] = c->r[s].uw[3]; } } while (0)
