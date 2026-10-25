#!/usr/bin/env python3
"""R5900 (PS2 Emotion Engine) -> C static recompiler for Ape Escape 2 (SCES-50885).

Every discovered function becomes a C function `void f_XXXXXXXX(Ctx *c)`.
Intra-function control flow becomes gotos; jal becomes a C call; jr ra becomes
return; indirect jumps go through a switch of known labels or the runtime
dispatcher rt_call().
"""
import os, sys, bisect
from elfinfo import Image, find_functions, s16, is_branch, branch_target

SETJMP = 0x394f88
LONGJMP = 0x394fe0

GPRN = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']

MMI0 = {0: 'PADDW', 1: 'PSUBW', 2: 'PCGTW', 3: 'PMAXW', 4: 'PADDH', 5: 'PSUBH', 6: 'PCGTH', 7: 'PMAXH', 8: 'PADDB',
        9: 'PSUBB', 10: 'PCGTB', 16: 'PADDSW', 17: 'PSUBSW', 18: 'PEXTLW', 19: 'PPACW', 20: 'PADDSH', 21: 'PSUBSH',
        22: 'PEXTLH', 23: 'PPACH', 24: 'PADDSB', 25: 'PSUBSB', 26: 'PEXTLB', 27: 'PPACB', 30: 'PEXT5', 31: 'PPAC5'}
MMI1 = {1: 'PABSW', 2: 'PCEQW', 3: 'PMINW', 4: 'PADSBH', 5: 'PABSH', 6: 'PCEQH', 7: 'PMINH', 10: 'PCEQB', 16: 'PADDUW',
        17: 'PSUBUW', 18: 'PEXTUW', 20: 'PADDUH', 21: 'PSUBUH', 22: 'PEXTUH', 24: 'PADDUB', 25: 'PSUBUB', 26: 'PEXTUB',
        27: 'QFSRV'}
MMI2 = {0: 'PMADDW', 2: 'PSLLVW', 3: 'PSRLVW', 4: 'PMSUBW', 8: 'PMFHI', 9: 'PMFLO', 10: 'PINTH', 12: 'PMULTW',
        13: 'PDIVW', 14: 'PCPYLD', 16: 'PMADDH', 17: 'PHMADH', 18: 'PAND', 19: 'PXOR', 20: 'PMSUBH', 21: 'PHMSBH',
        26: 'PEXEH', 27: 'PREVH', 28: 'PMULTH', 29: 'PDIVBW', 30: 'PEXEW', 31: 'PROT3W'}
MMI3 = {0: 'PMADDUW', 3: 'PSRAVW', 8: 'PMTHI', 9: 'PMTLO', 10: 'PINTEH', 12: 'PMULTUW', 13: 'PDIVUW', 14: 'PCPYUD',
        18: 'POR', 19: 'PNOR', 26: 'PEXCH', 27: 'PCPYH', 30: 'PEXCW'}

# COP2 macro-mode upper/lower ops (funct 0..59)
V_BC = {0: 'ADD', 4: 'SUB', 8: 'MADD', 12: 'MSUB', 16: 'MAX', 20: 'MINI', 24: 'MUL'}
V_FUNCT = {28: ('MUL', 'q'), 29: ('MAX', 'i'), 30: ('MUL', 'i'), 31: ('MINI', 'i'), 32: ('ADD', 'q'), 33: ('MADD', 'q'),
           34: ('ADD', 'i'), 35: ('MADD', 'i'), 36: ('SUB', 'q'), 37: ('MSUB', 'q'), 38: ('SUB', 'i'),
           39: ('MSUB', 'i'), 40: ('ADD', ''), 41: ('MADD', ''), 42: ('MUL', ''), 43: ('MAX', ''), 44: ('SUB', ''),
           45: ('MSUB', ''), 47: ('MINI', '')}
V_SPEC2_BC = {0: 'ADDA', 4: 'SUBA', 8: 'MADDA', 12: 'MSUBA', 24: 'MULA'}
V_SPEC2 = {28: ('MULA', 'q'), 30: ('MULA', 'i'), 32: ('ADDA', 'q'), 33: ('MADDA', 'q'), 34: ('ADDA', 'i'),
           35: ('MADDA', 'i'), 36: ('SUBA', 'q'), 37: ('MSUBA', 'q'), 38: ('SUBA', 'i'), 39: ('MSUBA', 'i'),
           40: ('ADDA', ''), 41: ('MADDA', ''), 42: ('MULA', ''), 44: ('SUBA', ''), 45: ('MSUBA', '')}


class Unsupported(Exception):
    pass


def hx(v):
    return '0x%08X' % (v & 0xffffffff)


class Gen:
    def __init__(self, img, funcs, info):
        self.img = img
        self.funcs = funcs
        self.entries = set(f[0] for f in funcs)
        self.info = info
        self.labels_sorted = sorted(info['labels'])
        self.unknown = {}
        self.overrides = {}
        self.trace_addrs = set(int(x, 16) for x in os.environ.get('RECOMP_TRACE', '').split(',') if x)
        of = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides.txt')
        if os.path.exists(of):
            for line in open(of):
                line = line.split('#')[0].split()
                if len(line) == 2:
                    self.overrides[int(line[0], 16)] = line[1]

    # ---------------------------------------------------------- simple ops
    def simple(self, a, w):
        """Return C for a non-control-flow instruction (or None if not simple)."""
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        sa = (w >> 6) & 31
        fn = w & 63
        imm = s16(w & 0xffff)
        uimm = w & 0xffff
        A = '(RW(%d)+%d)' % (rs, imm) if imm else 'RW(%d)' % rs

        def wr(r, expr):
            return '' if r == 0 else expr

        if w == 0:
            return ''
        if op == 0:
            if fn == 0: return wr(rd, 'SETW(%d, RW(%d) << %d);' % (rd, rt, sa))
            if fn == 2: return wr(rd, 'SETW(%d, RW(%d) >> %d);' % (rd, rt, sa))
            if fn == 3: return wr(rd, 'SETW(%d, RSW(%d) >> %d);' % (rd, rt, sa))
            if fn == 4: return wr(rd, 'SETW(%d, RW(%d) << (RW(%d)&31));' % (rd, rt, rs))
            if fn == 6: return wr(rd, 'SETW(%d, RW(%d) >> (RW(%d)&31));' % (rd, rt, rs))
            if fn == 7: return wr(rd, 'SETW(%d, RSW(%d) >> (RW(%d)&31));' % (rd, rt, rs))
            if fn == 10: return wr(rd, 'if (RD(%d) == 0) RD(%d) = RD(%d);' % (rt, rd, rs))
            if fn == 11: return wr(rd, 'if (RD(%d) != 0) RD(%d) = RD(%d);' % (rt, rd, rs))
            if fn == 12: return 'c->pc = %s; rt_syscall(c);' % hx(a)
            if fn == 13: return 'rt_break(c, %s);' % hx(a)
            if fn == 15: return ''
            if fn == 16: return wr(rd, 'RD(%d) = c->hi.ud[0];' % rd)
            if fn == 17: return 'c->hi.ud[0] = RD(%d);' % rs
            if fn == 18: return wr(rd, 'RD(%d) = c->lo.ud[0];' % rd)
            if fn == 19: return 'c->lo.ud[0] = RD(%d);' % rs
            if fn == 20: return wr(rd, 'RD(%d) = RD(%d) << (RW(%d)&63);' % (rd, rt, rs))
            if fn == 22: return wr(rd, 'RD(%d) = RD(%d) >> (RW(%d)&63);' % (rd, rt, rs))
            if fn == 23: return wr(rd, 'RS(%d) = RS(%d) >> (RW(%d)&63);' % (rd, rt, rs))
            if fn == 24: return 'OP_MULT(%d,%d,%d,0);' % (rd, rs, rt)
            if fn == 25: return 'OP_MULTU(%d,%d,%d,0);' % (rd, rs, rt)
            if fn == 26: return 'OP_DIV(%d,%d,0);' % (rs, rt)
            if fn == 27: return 'OP_DIVU(%d,%d,0);' % (rs, rt)
            if fn in (32, 33): return wr(rd, 'SETW(%d, RW(%d) + RW(%d));' % (rd, rs, rt))
            if fn in (34, 35): return wr(rd, 'SETW(%d, RW(%d) - RW(%d));' % (rd, rs, rt))
            if fn == 36: return wr(rd, 'RD(%d) = RD(%d) & RD(%d);' % (rd, rs, rt))
            if fn == 37: return wr(rd, 'RD(%d) = RD(%d) | RD(%d);' % (rd, rs, rt))
            if fn == 38: return wr(rd, 'RD(%d) = RD(%d) ^ RD(%d);' % (rd, rs, rt))
            if fn == 39: return wr(rd, 'RD(%d) = ~(RD(%d) | RD(%d));' % (rd, rs, rt))
            if fn == 40: return wr(rd, 'RD(%d) = c->sa;' % rd)
            if fn == 41: return 'c->sa = RW(%d);' % rs
            if fn == 42: return wr(rd, 'RD(%d) = RS(%d) < RS(%d);' % (rd, rs, rt))
            if fn == 43: return wr(rd, 'RD(%d) = RD(%d) < RD(%d);' % (rd, rs, rt))
            if fn in (44, 45): return wr(rd, 'RD(%d) = RD(%d) + RD(%d);' % (rd, rs, rt))
            if fn in (46, 47): return wr(rd, 'RD(%d) = RD(%d) - RD(%d);' % (rd, rs, rt))
            if fn in (48, 49, 50, 51, 52, 54): return ''  # traps: ignore
            if fn == 56: return wr(rd, 'RD(%d) = RD(%d) << %d;' % (rd, rt, sa))
            if fn == 58: return wr(rd, 'RD(%d) = RD(%d) >> %d;' % (rd, rt, sa))
            if fn == 59: return wr(rd, 'RS(%d) = RS(%d) >> %d;' % (rd, rt, sa))
            if fn == 60: return wr(rd, 'RD(%d) = RD(%d) << %d;' % (rd, rt, sa + 32))
            if fn == 62: return wr(rd, 'RD(%d) = RD(%d) >> %d;' % (rd, rt, sa + 32))
            if fn == 63: return wr(rd, 'RS(%d) = RS(%d) >> %d;' % (rd, rt, sa + 32))
            return None
        if op == 1:
            if rt in (8, 9, 10, 11, 12, 14): return ''
            if rt == 24: return 'c->sa = ((RW(%d) & 15) ^ (%d & 15));' % (rs, uimm)
            if rt == 25: return 'c->sa = ((RW(%d) & 7) ^ (%d & 7)) << 1;' % (rs, uimm)
            return None
        if op in (8, 9): return wr(rt, 'SETW(%d, RW(%d) + %d);' % (rt, rs, imm))
        if op == 10: return wr(rt, 'RD(%d) = RS(%d) < (int64_t)%d;' % (rt, rs, imm))
        if op == 11: return wr(rt, 'RD(%d) = RD(%d) < (uint64_t)(int64_t)%d;' % (rt, rs, imm))
        if op == 12: return wr(rt, 'RD(%d) = RD(%d) & 0x%x;' % (rt, rs, uimm))
        if op == 13: return wr(rt, 'RD(%d) = RD(%d) | 0x%x;' % (rt, rs, uimm))
        if op == 14: return wr(rt, 'RD(%d) = RD(%d) ^ 0x%x;' % (rt, rs, uimm))
        if op == 15: return wr(rt, 'RS(%d) = (int32_t)0x%x;' % (rt, (uimm << 16)))
        if op in (24, 25): return wr(rt, 'RD(%d) = RD(%d) + (int64_t)%d;' % (rt, rs, imm))
        if op == 26: return wr(rt, 'OP_LDL(%d, %s);' % (rt, A))
        if op == 27: return wr(rt, 'OP_LDR(%d, %s);' % (rt, A))
        if op == 28: return self.mmi(a, w)
        if op == 30: return 'MR128(%d, %s);' % (rt, A) if rt else '(void)MR32(%s & ~15u);' % A
        if op == 31: return 'MW128(%s, &c->r[%d]);' % (A, rt)
        if op == 32: return wr(rt, 'RS(%d) = (int8_t)MR8(%s);' % (rt, A)) or '(void)MR8(%s);' % A
        if op == 33: return wr(rt, 'RS(%d) = (int16_t)MR16(%s);' % (rt, A)) or '(void)MR16(%s);' % A
        if op == 34: return wr(rt, 'OP_LWL(%d, %s);' % (rt, A))
        if op == 35: return wr(rt, 'RS(%d) = (int32_t)MR32(%s);' % (rt, A)) or '(void)MR32(%s);' % A
        if op == 36: return wr(rt, 'RD(%d) = MR8(%s);' % (rt, A)) or '(void)MR8(%s);' % A
        if op == 37: return wr(rt, 'RD(%d) = MR16(%s);' % (rt, A)) or '(void)MR16(%s);' % A
        if op == 38: return wr(rt, 'OP_LWR(%d, %s);' % (rt, A))
        if op == 39: return wr(rt, 'RD(%d) = MR32(%s);' % (rt, A)) or '(void)MR32(%s);' % A
        if op == 40: return 'MW8(%s, RW(%d));' % (A, rt)
        if op == 41: return 'MW16(%s, RW(%d));' % (A, rt)
        if op == 42: return 'OP_SWL(%d, %s);' % (rt, A)
        if op == 43: return 'MW32(%s, RW(%d));' % (A, rt)
        if op == 44: return 'OP_SDL(%d, %s);' % (rt, A)
        if op == 45: return 'OP_SDR(%d, %s);' % (rt, A)
        if op == 46: return 'OP_SWR(%d, %s);' % (rt, A)
        if op == 47: return ''  # cache
        if op == 49: return 'c->f[%d].u = MR32(%s);' % (rt, A)
        if op == 51: return ''  # pref
        if op == 54: return 'MR128VF(%d, %s);' % (rt, A)
        if op == 55: return wr(rt, 'RD(%d) = MR64(%s);' % (rt, A)) or '(void)MR64(%s);' % A
        if op == 57: return 'MW32(%s, c->f[%d].u);' % (A, rt)
        if op == 62: return 'MW128(%s, &c->vf[%d]);' % (A, rt)
        if op == 63: return 'MW64(%s, RD(%d));' % (A, rt)
        if op == 16: return self.cop0(a, w)
        if op == 17: return self.cop1(a, w)
        if op == 18: return self.cop2(a, w)
        return None

    def mmi(self, a, w):
        rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; sa = (w >> 6) & 31; fn = w & 63
        if fn == 0: return 'OP_MADD(%d,%d,%d,0);' % (rd, rs, rt)
        if fn == 1: return 'OP_MADDU(%d,%d,%d,0);' % (rd, rs, rt)
        if fn == 32: return 'OP_MADD(%d,%d,%d,1);' % (rd, rs, rt)
        if fn == 33: return 'OP_MADDU(%d,%d,%d,1);' % (rd, rs, rt)
        if fn == 4: return 'MMI_PLZCW(%d,%d);' % (rd, rs)
        if fn == 16: return rd and 'RD(%d) = c->hi.ud[1];' % rd or ''
        if fn == 17: return 'c->hi.ud[1] = RD(%d);' % rs
        if fn == 18: return rd and 'RD(%d) = c->lo.ud[1];' % rd or ''
        if fn == 19: return 'c->lo.ud[1] = RD(%d);' % rs
        if fn == 24: return 'OP_MULT(%d,%d,%d,1);' % (rd, rs, rt)
        if fn == 25: return 'OP_MULTU(%d,%d,%d,1);' % (rd, rs, rt)
        if fn == 26: return 'OP_DIV(%d,%d,1);' % (rs, rt)
        if fn == 27: return 'OP_DIVU(%d,%d,1);' % (rs, rt)
        if fn == 48: return 'MMI_PMFHL(%d,%d);' % (rd, sa)
        if fn == 49: return 'MMI_PMTHL(%d,%d);' % (rs, sa)
        if fn in (52, 54, 55, 60, 62, 63):
            nm = {52: 'PSLLH', 54: 'PSRLH', 55: 'PSRAH', 60: 'PSLLW', 62: 'PSRLW', 63: 'PSRAW'}[fn]
            return 'MMI_%s(%d,%d,%d);' % (nm, rd, rt, sa)
        tab = {8: MMI0, 40: MMI1, 9: MMI2, 41: MMI3}.get(fn)
        if tab is None or sa not in tab:
            return None
        return 'MMI_%s(%d,%d,%d);' % (tab[sa], rd, rs, rt)

    def cop0(self, a, w):
        rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
        if rs == 0: return rt and 'RS(%d) = (int32_t)rt_mfc0(c, %d);' % (rt, rd) or ''
        if rs == 4: return 'rt_mtc0(c, %d, RW(%d));' % (rd, rt)
        if rs == 16:
            f = w & 63
            if f == 0x38: return 'c->cop0_status |= 0x10000u;'   # ei
            if f == 0x39: return 'c->cop0_status &= ~0x10000u;'  # di
            if f in (2, 6, 1, 8): return ''  # tlb
            if f == 0x18: return 'rt_eret(c);'
        return None

    def cop1(self, a, w):
        rs = (w >> 21) & 31; rt = (w >> 16) & 31
        ft = rt; fs = (w >> 11) & 31; fd = (w >> 6) & 31; fn = w & 63
        if rs == 0: return rt and 'RS(%d) = (int32_t)c->f[%d].u;' % (rt, fs) or ''
        if rs == 2: return rt and 'RS(%d) = (int32_t)(%s);' % (rt, 'c->fcr31' if fs == 31 else '0x2e30') or ''
        if rs == 4: return 'c->f[%d].u = RW(%d);' % (fs, rt)
        if rs == 6: return fs == 31 and 'c->fcr31 = RW(%d);' % rt or ''
        F = lambda r: 'c->f[%d].f' % r
        if rs == 16:
            if fn == 0: return '%s = FADD(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 1: return '%s = FSUB(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 2: return '%s = FMUL(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 3: return '%s = FDIV(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 4: return '%s = FSQRT(%s);' % (F(fd), F(ft))
            if fn == 5: return 'c->f[%d].u = c->f[%d].u & 0x7fffffff;' % (fd, fs)
            if fn == 6: return 'c->f[%d].u = c->f[%d].u;' % (fd, fs)
            if fn == 7: return 'c->f[%d].u = c->f[%d].u ^ 0x80000000;' % (fd, fs)
            if fn == 22: return '%s = FRSQRT(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 24: return 'c->facc = FADD(%s, %s);' % (F(fs), F(ft))
            if fn == 25: return 'c->facc = FSUB(%s, %s);' % (F(fs), F(ft))
            if fn == 26: return 'c->facc = FMUL(%s, %s);' % (F(fs), F(ft))
            if fn == 28: return '%s = FADD(c->facc, FMUL(%s, %s));' % (F(fd), F(fs), F(ft))
            if fn == 29: return '%s = FSUB(c->facc, FMUL(%s, %s));' % (F(fd), F(fs), F(ft))
            if fn == 30: return 'c->facc = FADD(c->facc, FMUL(%s, %s));' % (F(fs), F(ft))
            if fn == 31: return 'c->facc = FSUB(c->facc, FMUL(%s, %s));' % (F(fs), F(ft))
            if fn == 36: return 'c->f[%d].s = CVTWS(%s);' % (fd, F(fs))
            if fn == 40: return '%s = FMAX(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 41: return '%s = FMIN(%s, %s);' % (F(fd), F(fs), F(ft))
            if fn == 48: return 'c->fcr31 &= ~0x800000u;'
            if fn == 50: return 'FCOND(FCEQ(%s, %s));' % (F(fs), F(ft))
            if fn == 52: return 'FCOND(FCLT(%s, %s));' % (F(fs), F(ft))
            if fn == 54: return 'FCOND(FCLE(%s, %s));' % (F(fs), F(ft))
            return None
        if rs == 20 and fn == 32:
            return '%s = (float)c->f[%d].s;' % (F(fd), fs)
        return None

    def cop2(self, a, w):
        rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
        if rs == 1: return rt and 'c->r[%d] = c->vf[%d];' % (rt, rd) or ''
        if rs == 2: return rt and 'RS(%d) = (int32_t)rt_cfc2(c, %d);' % (rt, rd) or ''
        if rs == 5: return rd and 'c->vf[%d] = c->r[%d];' % (rd, rt) or ''
        if rs == 6: return 'rt_ctc2(c, %d, RW(%d));' % (rd, rt)
        if not (rs & 16):
            return None
        dest = (w >> 21) & 15
        ft = (w >> 16) & 31; fs = (w >> 11) & 31; fd = (w >> 6) & 31
        fn = w & 63
        if fn < 60:
            if fn < 28:
                base = fn & ~3; bc = fn & 3
                if base in V_BC:
                    return 'V_%s(%d,%d,%d,VBC(%d,%d));' % (V_BC[base], dest, fd, fs, ft, bc)
                return None
            if fn in V_FUNCT:
                nm, kind = V_FUNCT[fn]
                if kind == 'q': return 'V_%s(%d,%d,%d,VQ);' % (nm, dest, fd, fs)
                if kind == 'i': return 'V_%s(%d,%d,%d,VI_);' % (nm, dest, fd, fs)
                return 'V_%s(%d,%d,%d,VV(%d));' % (nm, dest, fd, fs, ft)
            if fn == 46: return 'V_OPMSUB(%d,%d,%d);' % (fd, fs, ft)
            it = ft & 15; is_ = fs & 15; id_ = fd & 15
            if fn == 48: return 'VIW(%d, VIR(%d) + VIR(%d));' % (id_, is_, it)
            if fn == 49: return 'VIW(%d, VIR(%d) - VIR(%d));' % (id_, is_, it)
            if fn == 50:
                im = fd if fd < 16 else fd - 32
                return 'VIW(%d, VIR(%d) + (%d));' % (it, is_, im)
            if fn == 52: return 'VIW(%d, VIR(%d) & VIR(%d));' % (id_, is_, it)
            if fn == 53: return 'VIW(%d, VIR(%d) | VIR(%d));' % (id_, is_, it)
            if fn == 56: return 'rt_vcallms(c, %d);' % (((w >> 6) & 0x7fff) * 8)
            if fn == 57: return 'rt_vcallms(c, c->vi[27] * 8);'
            return None
        op2 = (w & 3) | ((w >> 4) & 0x7c)
        if op2 < 28:
            base = op2 & ~3; bc = op2 & 3
            if base in V_SPEC2_BC:
                return 'V_%s(%d,0,%d,VBC(%d,%d));' % (V_SPEC2_BC[base], dest, fs, ft, bc)
            if 16 <= op2 < 20: return 'V_ITOF(%d,%d,%d,%d);' % (dest, ft, fs, [0, 4, 12, 15][op2 - 16])
            if 20 <= op2 < 24: return 'V_FTOI(%d,%d,%d,%d);' % (dest, ft, fs, [0, 4, 12, 15][op2 - 20])
            return None
        if op2 in V_SPEC2:
            nm, kind = V_SPEC2[op2]
            if kind == 'q': return 'V_%s(%d,0,%d,VQ);' % (nm, dest, fs)
            if kind == 'i': return 'V_%s(%d,0,%d,VI_);' % (nm, dest, fs)
            return 'V_%s(%d,0,%d,VV(%d));' % (nm, dest, fs, ft)
        fsf = (w >> 21) & 3; ftf = (w >> 23) & 3
        if op2 == 29: return 'V_ABS(%d,%d,%d);' % (dest, ft, fs)
        if op2 == 31: return 'V_CLIP(%d,%d);' % (fs, ft)
        if op2 == 46: return 'V_OPMULA(%d,%d);' % (fs, ft)
        if op2 == 47: return ''
        if op2 == 48: return 'V_MOVE(%d,%d,%d);' % (dest, ft, fs)
        if op2 == 49: return 'V_MR32(%d,%d,%d);' % (dest, ft, fs)
        if op2 == 52: return 'V_LQI(%d,%d,%d);' % (dest, ft, fs & 15)
        if op2 == 53: return 'V_SQI(%d,%d,%d);' % (dest, fs, ft & 15)
        if op2 == 54: return 'V_LQD(%d,%d,%d);' % (dest, ft, fs & 15)
        if op2 == 55: return 'V_SQD(%d,%d,%d);' % (dest, fs, ft & 15)
        if op2 == 56: return 'V_DIV(%d,%d,%d,%d);' % (fs, fsf, ft, ftf)
        if op2 == 57: return 'V_SQRT(%d,%d);' % (ft, ftf)
        if op2 == 58: return 'V_RSQRT(%d,%d,%d,%d);' % (fs, fsf, ft, ftf)
        if op2 == 59: return ''
        if op2 == 60: return 'VIW(%d, c->vf[%d].uw[%d] & 0xffff);' % (ft & 15, fs, fsf)
        if op2 == 61: return 'V_MFIR(%d,%d,%d);' % (dest, ft, fs & 15)
        if op2 == 62: return 'V_ILWR(%d,%d,%d);' % (dest, ft & 15, fs & 15)
        if op2 == 63: return 'V_ISWR(%d,%d,%d);' % (dest, ft & 15, fs & 15)
        if op2 == 64: return 'V_RNEXT(%d,%d);' % (dest, ft)
        if op2 == 65: return 'V_RGET(%d,%d);' % (dest, ft)
        if op2 == 66: return 'V_RINIT(%d,%d);' % (fs, fsf)
        if op2 == 67: return 'V_RXOR(%d,%d);' % (fs, fsf)
        return None

    # ---------------------------------------------------------- control flow
    def control(self, a, w):
        """Classify control-flow ops. Returns dict or None."""
        op = w >> 26
        rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
        if op == 2: return dict(k='j', t=((w & 0x3ffffff) << 2) | ((a + 4) & 0xf0000000))
        if op == 3: return dict(k='jal', t=((w & 0x3ffffff) << 2) | ((a + 4) & 0xf0000000))
        if op == 0 and (w & 63) == 8: return dict(k='jr', rs=rs)
        if op == 0 and (w & 63) == 9: return dict(k='jalr', rs=rs, rd=rd)
        t = branch_target(a, w)
        conds = {4: 'RD(%d) == RD(%d)' % (rs, rt), 5: 'RD(%d) != RD(%d)' % (rs, rt),
                 6: 'RS(%d) <= 0' % rs, 7: 'RS(%d) > 0' % rs}
        if op in (4, 5, 6, 7): return dict(k='b', t=t, c=conds[op], likely=False, link=False)
        if op in (20, 21, 22, 23): return dict(k='b', t=t, c=conds[op - 16], likely=True, link=False)
        if op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19):
            cnd = 'RS(%d) < 0' % rs if rt & 1 == 0 else 'RS(%d) >= 0' % rs
            return dict(k='b', t=t, c=cnd, likely=bool(rt & 2), link=bool(rt & 16))
        if op in (16, 17, 18) and rs == 8:
            if op == 16: cnd = 'rt_cpcond0(c)'
            elif op == 17: cnd = '(c->fcr31 & 0x800000)'
            else: cnd = 'c->vu0_busy'
            if rt & 1 == 0: cnd = '!' + cnd
            return dict(k='b', t=t, c=cnd, likely=bool(rt & 2), link=False)
        return None

    def body(self, a, w):
        s = self.simple(a, w)
        if s is None:
            self.unknown[w >> 26] = self.unknown.get(w >> 26, 0) + 1
            return 'rt_badop(c, %s, %s);' % (hx(a), hx(w))
        return s

    def gen_function(self, start, end, out):
        img = self.img
        name = 'f_%08X' % start
        if start in self.overrides:
            out.write('void hle_%s(Ctx *c);\nvoid %s(Ctx *c) { hle_%s(c); }\n\n' % (self.overrides[start], name, self.overrides[start]))
            name = name + '_orig'   # original body still available to the HLE
        # collect labels
        targets = set()
        jrsw = []
        lo = bisect.bisect_left(self.labels_sorted, start)
        hi_ = bisect.bisect_left(self.labels_sorted, end)
        sw_cands = [x for x in self.labels_sorted[lo:hi_]]
        has_jr = False
        for a in range(start, end, 4):
            w = img.word(a)
            cf = self.control(a, w)
            if not cf:
                continue
            if cf['k'] in ('b', 'j') and start <= cf['t'] < end:
                targets.add(cf['t'])
            if cf['k'] == 'jr' and cf['rs'] != 31:
                has_jr = True
        if has_jr:
            targets.update(sw_cands)
        lines = ['void %s(Ctx *c) {' % name, '  FENTER(%s);' % hx(start)]
        if name.endswith('_orig'):
            lines[0] = 'void %s(Ctx *c);\n' % name + lines[0]
        a = start
        while a < end:
            w = img.word(a)
            if a in targets:
                lines.append('L_%08X:;' % a)
            if a in self.trace_addrs:
                lines.append('  rt_trace_pc(c, %s);' % hx(a))
            cf = self.control(a, w)
            if cf is None:
                s = self.body(a, w)
                if s:
                    lines.append('  ' + s)
                a += 4
                continue
            # control flow with delay slot
            ds_a = a + 4
            ds_w = img.word(ds_a) if img.in_text(ds_a) else 0
            if self.control(ds_a, ds_w):
                ds = '/* branch in delay slot */'
                ds_w = 0
            ds = self.body(ds_a, ds_w) if ds_w else ''
            k = cf['k']
            code = []
            if k == 'b':
                t = cf['t']
                inside = start <= t < end
                if cf['link']:
                    code.append('{ int cnd = %s; RD(31) = %s; %s if (cnd) { %s } }' % (
                        cf['c'], hx(a + 8), ds, self.call_code(t)))
                else:
                    jump = self.goto_code(a, t, inside)
                    always = cf['c'] in ('RD(0) == RD(0)', 'RS(0) >= 0', 'RS(0) <= 0')
                    if always:
                        code.append('%s %s' % (ds, jump))
                    elif cf['likely']:
                        code.append('if (%s) { %s %s }' % (cf['c'], ds, jump))
                    else:
                        code.append('{ int cnd = %s; %s if (cnd) %s }' % (cf['c'], ds, jump))
            elif k == 'j':
                t = cf['t']
                code.append('%s %s' % (ds, self.goto_code(a, t, start <= t < end)))
            elif k == 'jal':
                t = cf['t']
                if t == SETJMP:
                    code.append('RD(31) = %s; %s RT_SETJMP(c);' % (hx(a + 8), ds))
                elif t == LONGJMP:
                    code.append('RD(31) = %s; %s rt_longjmp(c);' % (hx(a + 8), ds))
                else:
                    code.append('RD(31) = %s; %s %s' % (hx(a + 8), ds, self.call_code(t)))
            elif k == 'jalr':
                rd = cf['rd']
                code.append('{ uint32_t tgt = RW(%d); %s %s rt_call(c, tgt); }' % (
                    cf['rs'], ('RD(%d) = %s;' % (rd, hx(a + 8))) if rd else '', ds))
            elif k == 'jr':
                if cf['rs'] == 31:
                    code.append('%s return;' % ds)
                else:
                    cases = ' '.join('case %s: goto L_%08X;' % (hx(x), x) for x in sw_cands)
                    code.append('{ uint32_t tgt = RW(%d); %s switch (tgt) { %s default: { rt_call(c, tgt); return; } } }' % (
                        cf['rs'], ds, cases))
            lines.append('  ' + ' '.join(code))
            if ds_a in targets:
                # someone jumps straight to the delay slot
                lines.append('  goto L_%08X;' % (ds_a + 4))
                lines.append('L_%08X:;' % ds_a)
                if ds:
                    lines.append('  ' + ds)
                targets.add(ds_a + 4)
                if ds_a + 4 >= end:
                    pass
            a += 8
        # fallthrough at end of range
        if end < img.text_end:
            if end in targets:
                lines.append('L_%08X:;' % end)
            lines.append('  %s return;' % self.call_code(end).rstrip())
        else:
            lines.append('  return;')
        # labels beyond range referenced (delay-slot skip past end)
        for t in sorted(targets):
            if t > end:
                lines.append('L_%08X: %s return;' % (t, self.call_code(t)))
        lines.append('}')
        out.write('\n'.join(lines))
        out.write('\n\n')

    def goto_code(self, a, t, inside):
        if inside:
            if t <= a:
                return '{ POLL(); goto L_%08X; }' % t
            return 'goto L_%08X;' % t
        return '{ %s return; }' % self.call_code(t)

    def call_code(self, t):
        if t in self.entries:
            return 'f_%08X(c);' % t
        return 'rt_call(c, %s);' % hx(t)


def main():
    out_dir = sys.argv[2] if len(sys.argv) > 2 else 'out'
    img = Image(sys.argv[1] if len(sys.argv) > 1 else 'SCES_508.85')
    funcs, info = find_functions(img)
    g = Gen(img, funcs, info)
    os.makedirs(out_dir, exist_ok=True)
    nfiles = 48
    per = (len(funcs) + nfiles - 1) // nfiles
    with open(os.path.join(out_dir, 'funcs.h'), 'w') as h:
        h.write('#pragma once\n#include "ps2.h"\n')
        for s, e in funcs:
            h.write('void f_%08X(Ctx *c);\n' % s)
    for i in range(nfiles):
        chunk = funcs[i * per:(i + 1) * per]
        with open(os.path.join(out_dir, 'code_%02d.c' % i), 'w') as f:
            f.write('#include "funcs.h"\n\n')
            for s, e in chunk:
                g.gen_function(s, e, f)
    with open(os.path.join(out_dir, 'functable.c'), 'w') as f:
        f.write('#include "funcs.h"\n')
        f.write('const FuncEntry g_functable[] = {\n')
        for s, e in funcs:
            f.write('  {0x%08X, 0x%08X, f_%08X},\n' % (s, e, s))
        f.write('};\nconst int g_functable_n = %d;\n' % len(funcs))
    print('functions:', len(funcs), 'unknown opcode classes:', g.unknown)


if __name__ == '__main__':
    main()
