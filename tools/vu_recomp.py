#!/usr/bin/env python3
"""Static recompiler for captured VU1 microprograms.

Input: files written by the runtime with AE2_VUCAPTURE=dir
       ("entry XXXX hash H" + "pc lower upper" lines for every reachable pair).
Output: C source (default ../port/gen/vu1_progs.c) with one function per
program and a lookup table.  The generated code follows the interpreter in
port/src/vu.c exactly (same cycle accounting, stalls, Q/P latency, flag
history, branch delay slots, E-bit delay), but with decoding done at
generation time, and MAC/status/clip flag bookkeeping dropped when the
program never reads those flags.

usage: vu_recomp.py capture_dir [out.c]
"""
import os, sys, glob

FLAG_READ_MS = {0x14, 0x16, 0x17, 0x18, 0x1A, 0x1B}   # FSEQ FSAND FSOR FMEQ FMAND FMOR
FLAG_READ_CLIP = {0x10, 0x12, 0x13, 0x1C}             # FCEQ FCAND FCOR FCGET
BRANCH_OPS = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
LANES = 'xyzw'


def s11(w):
    v = w & 0x7ff
    return v - 0x800 if v & 0x400 else v


def lanes(dest):
    return [k for k in range(4) if dest & (8 >> k)]


class Prog:
    def __init__(self, path):
        lines = open(path).read().split('\n')
        p = lines[0].split()
        self.entry = int(p[1], 16)
        self.hash = int(p[3], 16)
        self.unit = int(p[5]) if len(p) > 5 else 1
        self.cm = 0x3fff if self.unit else 0xfff
        self.pairs = {}
        for l in lines[1:]:
            if not l.strip():
                continue
            a, lo, up = l.split()
            self.pairs[int(a, 16)] = (int(lo, 16), int(up, 16))
        self.name = 'vp%d_%04x_%016x' % (self.unit, self.entry, self.hash)

    def is_branch(self, pc):
        if pc not in self.pairs:
            return False
        lo, up = self.pairs[pc]
        return not (up >> 31) and (lo >> 25) in BRANCH_OPS

    def ebit(self, pc):
        return pc in self.pairs and (self.pairs[pc][1] >> 30) & 1

    def compilable(self):
        for pc in self.pairs:
            if self.is_branch(pc) and self.is_branch(pc + 8):
                return False, 'branch in delay slot at %04x' % pc
            if self.ebit(pc) and self.is_branch(pc - 8):
                return False, 'E-bit in branch delay slot at %04x' % pc
        return True, ''

    def analyse_flags(self):
        """Decide which FMAC ops must record MAC/status flags.

        A flag reader sees the most recent flag-history entry that is at least
        4 cycles old. Each pair takes >= 1 cycle, so only ops within the 4
        pairs before a reader, plus the newest op 4+ pairs back on each path,
        can be what it sees. CLIP/FCSET entries copy the then-visible flags,
        so they are walked from as if they were readers. Clip flags never
        need FMAC entries (an FMAC entry only repeats the latest clip value)."""
        self.need_ms = False
        self.need_clip = False
        self.flags_all = False
        readers = []
        for pc, (lo, up) in self.pairs.items():
            if up >> 31:
                continue
            op = lo >> 25
            if op in FLAG_READ_MS:
                self.need_ms = True
                readers.append(pc)
                if op in (0x14, 0x16, 0x17) and ((((lo >> 10) & 0x800) | (lo & 0x7ff)) & 0xfc0):
                    self.flags_all = True    # reads sticky status bits: every op contributes
            if op in FLAG_READ_CLIP:
                self.need_clip = True
        self.flag_pcs = set()
        if self.unit == 0:
            self.flags_all = True    # VU0 flags are handed back to the EE (CFC2) after the call
            self.need_ms = True
        if not self.need_ms or self.flags_all:
            return
        # control-flow successors / predecessors
        succ = {}
        bal_returns = set()
        for pc, (lo, up) in self.pairs.items():
            if self.is_branch(pc) and (lo >> 25) in (0x21, 0x25):
                bal_returns.add((pc + 16) & self.cm)
        for pc in self.pairs:
            prev = (pc - 8) & self.cm
            if self.ebit(prev):
                succ[pc] = []
            elif self.ebit(pc) or self.is_branch(pc):
                succ[pc] = [(pc + 8) & self.cm]
            elif self.is_branch(prev):
                plo = self.pairs[prev][0]
                op = plo >> 25
                tgt = (prev + 8 + s11(plo) * 8) & self.cm
                if op in (0x24, 0x25):
                    succ[pc] = sorted(bal_returns)
                elif op in (0x20, 0x21):
                    succ[pc] = [tgt]
                else:
                    succ[pc] = [tgt, (prev + 16) & self.cm]
            else:
                succ[pc] = [(pc + 8) & self.cm]
        pred = {pc: [] for pc in self.pairs}
        for a, ss in succ.items():
            for b in ss:
                if b in pred:
                    pred[b].append(a)
        walked = set()

        def is_fmac(pc):
            lo, up = self.pairs[pc]
            return upper_pushes_flags(up)

        def is_copy(pc):
            lo, up = self.pairs[pc]
            if upper_is_clip(up):
                return True
            return not (up >> 31) and (lo >> 25) == 0x11

        def walk(r):
            if r in walked:
                return
            walked.add(r)
            stack = [(p, 1) for p in pred[r]]
            seen = set()
            while stack:
                n, d = stack.pop()
                if (n, d) in seen:
                    continue
                seen.add((n, d))
                if d > 64:
                    self.flags_all = True
                    return
                if is_fmac(n):
                    self.flag_pcs.add(n)
                    if d >= 4:
                        continue
                if is_copy(n):
                    walk(n)
                    if d >= 4:
                        continue
                for p in pred[n]:
                    stack.append((p, d + 1))
        for r in readers:
            walk(r)


# ---------------------------------------------------------------- upper ops
def upper_is_clip(w):
    return (w & 63) >= 60 and ((w & 3) | ((w >> 4) & 0x7c)) == 31


def upper_pushes_flags(w):
    """True if the interpreter's commit_upper calls fmac_flags for this op."""
    fn = w & 63
    if fn < 60:
        if fn < 28:
            return (fn & ~3) not in (16, 20)
        return fn in (28, 30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45, 46)
    op2 = (w & 3) | ((w >> 4) & 0x7c)
    if op2 < 16 or 24 <= op2 < 28:
        return True
    return op2 in (28, 30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45, 46)


def gen_upper(w, P, pc=0):
    """Returns (compute_lines, commit_fn(lower_dest) -> lines)."""
    dest = (w >> 21) & 15
    ft = (w >> 16) & 31
    fs = (w >> 11) & 31
    fd = (w >> 6) & 31
    fn = w & 63
    L = []

    def stall(r):
        if r:
            L.append('STALL(%d);' % r)

    nothing = (L, lambda ld: [])
    op = None
    bsrc = None        # expression producing scalar b, or 'VEC' for vf[ft]
    to_acc = False
    flags = True
    dest_reg = 0
    if fn < 60:
        if fn < 28:
            base, bc = fn & ~3, fn & 3
            stall(fs); stall(ft)
            ops = {0: 'add', 4: 'sub', 8: 'madd', 12: 'msub', 16: 'max', 20: 'mini', 24: 'mul'}
            if base not in ops:
                return nothing
            op = ops[base]
            L.append('float B = u->vf[%d].f[%d];' % (ft, bc))
            bsrc = 'S'
        else:
            stall(fs)
            q = {28: ('mul', 'q'), 29: ('max', 'i'), 30: ('mul', 'i'), 31: ('mini', 'i'), 32: ('add', 'q'), 33: ('madd', 'q'),
                 34: ('add', 'i'), 35: ('madd', 'i'), 36: ('sub', 'q'), 37: ('msub', 'q'), 38: ('sub', 'i'), 39: ('msub', 'i')}
            if fn in q:
                op, src = q[fn]
                L.append('float B = %s;' % ('getq(u)' if src == 'q' else 'u->i'))
                bsrc = 'S'
            elif fn in (40, 41, 42, 43, 44, 45, 47):
                stall(ft)
                op = {40: 'add', 41: 'madd', 42: 'mul', 43: 'max', 44: 'sub', 45: 'msub', 47: 'mini'}[fn]
                bsrc = 'VEC'
            elif fn == 46:
                stall(ft)
                L.append('UR[0] = vclampf(u->acc[0] - u->vf[%d].f[1] * u->vf[%d].f[2]);' % (fs, ft))
                L.append('UR[1] = vclampf(u->acc[1] - u->vf[%d].f[2] * u->vf[%d].f[0]);' % (fs, ft))
                L.append('UR[2] = vclampf(u->acc[2] - u->vf[%d].f[0] * u->vf[%d].f[1]);' % (fs, ft))
                L.append('UR[3] = 0;')
                return L, commit_gen(0xE, fd, False, True, P)
            else:
                return nothing
        dest_reg = fd
    else:
        op2 = (w & 3) | ((w >> 4) & 0x7c)
        if op2 < 16 or 24 <= op2 < 28:
            base, bc = op2 & ~3, op2 & 3
            stall(fs); stall(ft)
            op = {0: 'add', 4: 'sub', 8: 'madd', 12: 'msub', 24: 'mul'}[base]
            L.append('float B = u->vf[%d].f[%d];' % (ft, bc))
            bsrc = 'S'
            to_acc = True
        elif 16 <= op2 < 24:
            stall(fs)
            n = [0, 4, 12, 15][op2 & 3]
            if op2 < 20:
                sc = '(1.0f / %d.0f)' % (1 << n)
                for k in range(4):
                    L.append('UR[%d] = (float)u->vf[%d].sw[%d] * %s;' % (k, fs, k, sc))
            else:
                for k in range(4):
                    L.append('{ int32_t iv = CVTWS(u->vf[%d].f[%d] * %d.0f); memcpy(&UR[%d], &iv, 4); }' % (fs, k, 1 << n, k))
            return L, commit_gen(dest, ft, False, False, P)
        else:
            if op2 == 29:
                stall(fs)
                for k in range(4):
                    L.append('UR[%d] = fabsf(u->vf[%d].f[%d]);' % (k, fs, k))
                return L, commit_gen(dest, ft, False, False, P)
            if op2 == 31:   # CLIP
                stall(fs); stall(ft)
                L.append('float W = fabsf(u->vf[%d].f[3]); const float *S = u->vf[%d].f; uint32_t CF = 0;' % (ft, fs))
                L.append('if (S[0] > +W) CF |= 1; if (S[0] < -W) CF |= 2; if (S[1] > +W) CF |= 4; if (S[1] < -W) CF |= 8; if (S[2] > +W) CF |= 16; if (S[2] < -W) CF |= 32;')

                def clipcommit(ld):
                    c = ['u->clip_latest = ((u->clip_latest << 6) | CF) & 0xffffff;']
                    if P.need_ms or P.need_clip:
                        c.append('{ uint32_t m_, s_; flags_visible(u, &m_, &s_, NULL); flags_push(u, m_, s_ & 0x3f, u->clip_latest); }')
                    return c
                return L, clipcommit
            if op2 == 46:   # OPMULA
                stall(fs); stall(ft)
                L.append('UR[0] = vclampf(u->vf[%d].f[1] * u->vf[%d].f[2]);' % (fs, ft))
                L.append('UR[1] = vclampf(u->vf[%d].f[2] * u->vf[%d].f[0]);' % (fs, ft))
                L.append('UR[2] = vclampf(u->vf[%d].f[0] * u->vf[%d].f[1]);' % (fs, ft))
                L.append('UR[3] = 0;')
                return L, commit_gen(0xE, 0, True, True, P)
            q = {28: ('mul', 'q'), 30: ('mul', 'i'), 32: ('add', 'q'), 33: ('madd', 'q'), 34: ('add', 'i'), 35: ('madd', 'i'),
                 36: ('sub', 'q'), 37: ('msub', 'q'), 38: ('sub', 'i'), 39: ('msub', 'i')}
            if op2 in q:
                op, src = q[op2]
                L.append('float B = %s;' % ('getq(u)' if src == 'q' else 'u->i'))
                stall(fs)
                bsrc = 'S'
                to_acc = True
            elif op2 in (40, 41, 42, 44, 45):
                stall(fs); stall(ft)
                op = {40: 'add', 41: 'madd', 42: 'mul', 44: 'sub', 45: 'msub'}[op2]
                bsrc = 'VEC'
                to_acc = True
            else:
                return nothing
    # SSE: the same IEEE operations, lane by lane (lanes outside dest are computed and not
    # committed; _mm_max_ps/_mm_min_ps are exactly a > b ? a : b and a < b ? a : b)
    if op in ('add', 'sub', 'mul', 'madd', 'msub', 'max', 'mini'):
        a = '_mm_loadu_ps(u->vf[%d].f)' % fs
        b = '_mm_set1_ps(B)' if bsrc == 'S' else '_mm_loadu_ps(u->vf[%d].f)' % ft
        e = {'add': '_mm_add_ps(%s, %s)' % (a, b), 'sub': '_mm_sub_ps(%s, %s)' % (a, b), 'mul': '_mm_mul_ps(%s, %s)' % (a, b),
             'madd': '_mm_add_ps(_mm_loadu_ps(u->acc), _mm_mul_ps(%s, %s))' % (a, b),
             'msub': '_mm_sub_ps(_mm_loadu_ps(u->acc), _mm_mul_ps(%s, %s))' % (a, b),
             'max': '_mm_max_ps(%s, %s)' % (a, b), 'mini': '_mm_min_ps(%s, %s)' % (a, b)}[op]
        L.append('_mm_storeu_ps(UR, vclamp4(%s));' % e)
        return L, commit_gen(dest, dest_reg, to_acc, op not in ('max', 'mini'), P)
    # generic arithmetic over the 4 lanes (all lanes computed, like the interpreter)
    for k in range(4):
        a = 'u->vf[%d].f[%d]' % (fs, k)
        b = 'B' if bsrc == 'S' else 'u->vf[%d].f[%d]' % (ft, k)
        e = {'add': '%s + %s' % (a, b), 'sub': '%s - %s' % (a, b), 'mul': '%s * %s' % (a, b),
             'madd': 'u->acc[%d] + %s * %s' % (k, a, b), 'msub': 'u->acc[%d] - %s * %s' % (k, a, b),
             'max': '(%s > %s ? %s : %s)' % (a, b, a, b), 'mini': '(%s < %s ? %s : %s)' % (a, b, a, b)}[op]
        if k in lanes(dest):
            L.append('UR[%d] = vclampf(%s);' % (k, e))
    flags = op not in ('max', 'mini')
    return L, commit_gen(dest, dest_reg, to_acc, flags, P)


def commit_gen(dest, dest_reg, to_acc, flags, P):
    def f(lower_dest):
        c = []
        if to_acc:
            for k in lanes(dest):
                c.append('u->acc[%d] = UR[%d];' % (k, k))
        elif dest_reg and dest_reg != lower_dest:
            for k in lanes(dest):
                c.append('u->vf[%d].f[%d] = UR[%d];' % (dest_reg, k, k))
            c.append('u->vf_ready[%d] = u->cycle + 4;' % dest_reg)
        if flags and P.need_ms and (P.flags_all or P.cur_pc in P.flag_pcs):
            c.append('fmac_flags(u, UR, %d);' % dest)
        return c
    return f


# ---------------------------------------------------------------- lower ops
def gen_lower(w, pc, P):
    """Returns (lines, ldest, branch) where branch is None or (kind, target-or-None, cond-expr)."""
    op7 = w >> 25
    dest = (w >> 21) & 15
    it = (w >> 16) & 31
    is_ = (w >> 11) & 31
    id_ = (w >> 6) & 31
    ft, fs = it, is_
    imm11 = s11(w)
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7ff)
    L = []
    VI = lambda n: '0' if (n & 15) == 0 else '(u->vi[%d] & 0xffff)' % (n & 15)

    def SETVI(n, expr):
        n &= 15
        if n:
            L.append('u->vi[%d] = (uint32_t)(%s) & 0xffff;' % (n, expr))

    def stall(r):
        if r:
            L.append('STALL(%d);' % r)
    tgt = (pc + 8 + imm11 * 8) & P.cm
    if op7 == 0x00:   # LQ
        L.append('{ const r128 *m = vmem(u, %s + %d);' % (VI(is_), imm11))
        if ft:
            for k in lanes(dest):
                L.append('u->vf[%d].uw[%d] = m->uw[%d];' % (ft, k, k))
        L.append('}')
        return L, ft, None
    if op7 == 0x01:   # SQ
        stall(fs)
        L.append('{ r128 *m = vmem(u, %s + %d);' % (VI(it), imm11))
        for k in lanes(dest):
            L.append('m->uw[%d] = u->vf[%d].uw[%d];' % (k, fs, k))
        L.append('}')
        return L, 0, None
    if op7 == 0x04:   # ILW
        ks = lanes(dest)
        if ks:
            L.append('{ const r128 *m = vmem(u, %s + %d);' % (VI(is_), imm11))
            SETVI(it, 'm->uw[%d]' % ks[0])
            L.append('}')
        return L, 0, None
    if op7 == 0x05:   # ISW
        L.append('{ r128 *m = vmem(u, %s + %d); uint32_t v_ = %s;' % (VI(is_), imm11, VI(it)))
        for k in lanes(dest):
            L.append('m->uw[%d] = v_;' % k)
        L.append('}')
        return L, 0, None
    if op7 == 0x08:
        SETVI(it, '%s + %d' % (VI(is_), imm15)); return L, 0, None
    if op7 == 0x09:
        SETVI(it, '%s - %d' % (VI(is_), imm15)); return L, 0, None
    if op7 == 0x10:
        L.append('{ uint32_t c_; flags_visible(u, NULL, NULL, &c_); u->vi[1] = (c_ & 0xffffff) == 0x%x; }' % (w & 0xffffff)); return L, 0, None
    if op7 == 0x11:
        L.append('u->clip_latest = 0x%x; { uint32_t m_, s_; flags_visible(u, &m_, &s_, NULL); flags_push(u, m_, s_ & 0x3f, u->clip_latest); u->fh[u->fhead].at = u->cycle; }' % (w & 0xffffff))
        return L, 0, None
    if op7 == 0x12:
        L.append('{ uint32_t c_; flags_visible(u, NULL, NULL, &c_); u->vi[1] = (c_ & 0x%x) != 0; }' % (w & 0xffffff)); return L, 0, None
    if op7 == 0x13:
        L.append('{ uint32_t c_; flags_visible(u, NULL, NULL, &c_); u->vi[1] = ((c_ | 0x%x) & 0xffffff) == 0xffffff; }' % (w & 0xffffff)); return L, 0, None
    imm12 = ((w >> 10) & 0x800) | (w & 0x7ff)
    if op7 == 0x14:
        L.append('{ uint32_t s_; flags_visible(u, NULL, &s_, NULL);'); SETVI(it, '(s_ & 0xfff) == %d' % imm12); L.append('}'); return L, 0, None
    if op7 == 0x15:
        L.append('u->status_sticky = %d;' % (imm12 & 0xfc0)); return L, 0, None
    if op7 == 0x16:
        L.append('{ uint32_t s_; flags_visible(u, NULL, &s_, NULL);'); SETVI(it, 's_ & %d' % imm12); L.append('}'); return L, 0, None
    if op7 == 0x17:
        L.append('{ uint32_t s_; flags_visible(u, NULL, &s_, NULL);'); SETVI(it, '(s_ | %d) & 0xfff' % imm12); L.append('}'); return L, 0, None
    if op7 == 0x18:
        L.append('{ uint32_t m_; flags_visible(u, &m_, NULL, NULL);'); SETVI(it, '(m_ & 0xffff) == %s' % VI(is_)); L.append('}'); return L, 0, None
    if op7 == 0x1A:
        L.append('{ uint32_t m_; flags_visible(u, &m_, NULL, NULL);'); SETVI(it, 'm_ & %s' % VI(is_)); L.append('}'); return L, 0, None
    if op7 == 0x1B:
        L.append('{ uint32_t m_; flags_visible(u, &m_, NULL, NULL);'); SETVI(it, '(m_ | %s) & 0xffff' % VI(is_)); L.append('}'); return L, 0, None
    if op7 == 0x1C:
        L.append('{ uint32_t c_; flags_visible(u, NULL, NULL, &c_);'); SETVI(it, 'c_ & 0xfff'); L.append('}'); return L, 0, None
    if op7 == 0x20:
        return L, 0, ('b', tgt, '1')
    if op7 == 0x21:
        SETVI(it, (pc + 16) // 8)
        return L, 0, ('b', tgt, '1')
    if op7 == 0x24:
        L.append('btgt = %s * 8;' % VI(is_))
        return L, 0, ('jr', None, '1')
    if op7 == 0x25:
        L.append('btgt = %s * 8;' % VI(is_))
        SETVI(it, (pc + 16) // 8)
        return L, 0, ('jr', None, '1')
    conds = {0x28: '%s == %s' % (VI(it), VI(is_)), 0x29: '%s != %s' % (VI(it), VI(is_)),
             0x2C: '(int16_t)%s < 0' % VI(is_), 0x2D: '(int16_t)%s > 0' % VI(is_),
             0x2E: '(int16_t)%s <= 0' % VI(is_), 0x2F: '(int16_t)%s >= 0' % VI(is_)}
    if op7 in conds:
        return L, 0, ('b', tgt, conds[op7])
    if op7 != 0x40:
        return L, 0, None
    fn = w & 63
    i_t, i_s, i_d = it & 15, is_ & 15, id_ & 15
    if fn == 0x30: SETVI(i_d, '%s + %s' % (VI(i_s), VI(i_t))); return L, 0, None
    if fn == 0x31: SETVI(i_d, '%s - %s' % (VI(i_s), VI(i_t))); return L, 0, None
    if fn == 0x32:
        im = (w >> 6) & 31
        if im & 16: im -= 32
        SETVI(i_t, '%s + %d' % (VI(i_s), im)); return L, 0, None
    if fn == 0x34: SETVI(i_d, '%s & %s' % (VI(i_s), VI(i_t))); return L, 0, None
    if fn == 0x35: SETVI(i_d, '%s | %s' % (VI(i_s), VI(i_t))); return L, 0, None
    if fn < 0x3C:
        return L, 0, None
    op2 = (w & 3) | ((w >> 4) & 0x7c)
    fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
    if op2 == 0x30:   # MOVE
        stall(fs)
        if ft:
            if ft == fs:
                pass
            for k in lanes(dest):
                L.append('u->vf[%d].uw[%d] = u->vf[%d].uw[%d];' % (ft, k, fs, k))
        return L, ft, None
    if op2 == 0x31:   # MR32
        stall(fs)
        if ft:
            L.append('{ r128 s_ = u->vf[%d]; uint32_t t_[4] = {s_.uw[1], s_.uw[2], s_.uw[3], s_.uw[0]};' % fs)
            for k in lanes(dest):
                L.append('u->vf[%d].uw[%d] = t_[%d];' % (ft, k, k))
            L.append('}')
        return L, ft, None
    if op2 == 0x34:   # LQI
        L.append('{ const r128 *m = vmem(u, %s);' % VI(i_s))
        if ft:
            for k in lanes(dest):
                L.append('u->vf[%d].uw[%d] = m->uw[%d];' % (ft, k, k))
        L.append('}')
        SETVI(i_s, '%s + 1' % VI(i_s))
        return L, ft, None
    if op2 == 0x35:   # SQI
        stall(fs)
        L.append('{ r128 *m = vmem(u, %s);' % VI(i_t))
        for k in lanes(dest):
            L.append('m->uw[%d] = u->vf[%d].uw[%d];' % (k, fs, k))
        L.append('}')
        SETVI(i_t, '%s + 1' % VI(i_t))
        return L, 0, None
    if op2 == 0x36:   # LQD
        SETVI(i_s, '%s - 1' % VI(i_s))
        L.append('{ const r128 *m = vmem(u, %s);' % VI(i_s))
        if ft:
            for k in lanes(dest):
                L.append('u->vf[%d].uw[%d] = m->uw[%d];' % (ft, k, k))
        L.append('}')
        return L, ft, None
    if op2 == 0x37:   # SQD
        stall(fs)
        SETVI(i_t, '%s - 1' % VI(i_t))
        L.append('{ r128 *m = vmem(u, %s);' % VI(i_t))
        for k in lanes(dest):
            L.append('m->uw[%d] = u->vf[%d].uw[%d];' % (k, fs, k))
        L.append('}')
        return L, 0, None
    if op2 in (0x38, 0x39, 0x3A):
        stall(fs); stall(ft)
        L.append('if (u->cycle < u->q_done) u->cycle = u->q_done; getq(u);')
        a, b = 'u->vf[%d].f[%d]' % (fs, fsf), 'u->vf[%d].f[%d]' % (ft, ftf)
        if op2 == 0x38: L.append('u->q_new = FDIV(%s, %s); u->q_done = u->cycle + 7;' % (a, b))
        elif op2 == 0x39: L.append('u->q_new = sqrtf(fabsf(%s)); u->q_done = u->cycle + 7;' % b)
        else: L.append('u->q_new = FRSQRT(%s, %s); u->q_done = u->cycle + 13;' % (a, b))
        return L, 0, None
    if op2 == 0x3B:
        L.append('if (u->cycle < u->q_done) u->cycle = u->q_done; getq(u);'); return L, 0, None
    if op2 == 0x3C:
        stall(fs); SETVI(i_t, 'u->vf[%d].uw[%d]' % (fs, fsf)); return L, 0, None
    if op2 == 0x3D:
        if ft:
            for k in lanes(dest):
                L.append('u->vf[%d].sw[%d] = (int16_t)%s;' % (ft, k, VI(i_s)))
        return L, ft, None
    if op2 == 0x3E:
        ks = lanes(dest)
        if ks:
            L.append('{ const r128 *m = vmem(u, %s);' % VI(i_s)); SETVI(i_t, 'm->uw[%d]' % ks[0]); L.append('}')
        return L, 0, None
    if op2 == 0x3F:
        L.append('{ r128 *m = vmem(u, %s); uint32_t v_ = %s;' % (VI(i_s), VI(i_t)))
        for k in lanes(dest):
            L.append('m->uw[%d] = v_;' % k)
        L.append('}')
        return L, 0, None
    if op2 in (0x40, 0x41):
        if op2 == 0x40:
            L.append('{ uint32_t x_ = u->r; int b_ = ((x_ >> 4) ^ (x_ >> 22)) & 1; u->r = ((x_ << 1) | b_) & 0x7fffff; }')
        if ft:
            L.append('{ uint32_t v_ = 0x3f800000 | (u->r & 0x7fffff);')
            for k in lanes(dest):
                L.append('u->vf[%d].uw[%d] = v_;' % (ft, k))
            L.append('}')
        return L, ft, None
    if op2 == 0x42:
        L.append('u->r = u->vf[%d].uw[%d] & 0x7fffff;' % (fs, fsf)); return L, 0, None
    if op2 == 0x43:
        L.append('u->r = (u->r ^ u->vf[%d].uw[%d]) & 0x7fffff;' % (fs, fsf)); return L, 0, None
    if op2 == 0x64:
        if ft:
            L.append('{ float p_ = getp(u);')
            for k in lanes(dest):
                L.append('u->vf[%d].f[%d] = p_;' % (ft, k))
            L.append('}')
        return L, ft, None
    if op2 == 0x68: SETVI(i_t, 'g_vif1_top'); return L, 0, None
    if op2 == 0x69: SETVI(i_t, 'g_vif1_itop'); return L, 0, None
    if op2 == 0x6C: L.append('vu_xgkick(u, %s);' % VI(i_s)); return L, 0, None
    S3 = 'u->vf[%d].f[0] * u->vf[%d].f[0] + u->vf[%d].f[1] * u->vf[%d].f[1] + u->vf[%d].f[2] * u->vf[%d].f[2]' % ((fs,) * 6)
    F = 'u->vf[%d].f[%d]' % (fs, fsf)
    efu = {0x70: ('%s' % S3, 11), 0x71: ('FDIV(1.0f, %s)' % S3, 18), 0x72: ('sqrtf(%s)' % S3, 18),
           0x73: ('FDIV(1.0f, sqrtf(%s))' % S3, 24), 0x74: ('atan2f(u->vf[%d].f[1], u->vf[%d].f[0])' % (fs, fs), 54),
           0x75: ('atan2f(u->vf[%d].f[2], u->vf[%d].f[0])' % (fs, fs), 54),
           0x76: ('u->vf[%d].f[0] + u->vf[%d].f[1] + u->vf[%d].f[2] + u->vf[%d].f[3]' % ((fs,) * 4), 12),
           0x78: ('sqrtf(fabsf(%s))' % F, 12), 0x79: ('FDIV(1.0f, sqrtf(fabsf(%s)))' % F, 18), 0x7A: ('FDIV(1.0f, %s)' % F, 12),
           0x7C: ('sinf(%s)' % F, 29), 0x7D: ('atanf(%s)' % F, 54), 0x7E: ('expf(-%s)' % F, 44)}
    if op2 in efu:
        stall(fs)
        e, lat = efu[op2]
        L.append('efu(u, %s, %d);' % (e, lat))
        return L, 0, None
    if op2 == 0x7B:
        L.append('if (u->cycle < u->p_done) u->cycle = u->p_done; getp(u);'); return L, 0, None
    return L, 0, None


# ---------------------------------------------------------------- program
def gen_prog(P, out):
    P.analyse_flags()
    pcs = sorted(P.pairs)
    O = []
    O.append('static int %s(VU *u) {' % P.name)
    O.append('    int bp = 0, ep = 0; uint32_t btgt = 0; float UR[4]; (void)bp; (void)ep; (void)btgt; (void)UR;')
    O.append('    goto L_%04x;' % P.entry)
    for pc in pcs:
        lo, up = P.pairs[pc]
        ibit = up >> 31
        ebit = (up >> 30) & 1
        O.append('L_%04x:' % pc)
        O.append('    u->cycle++;')
        if not ibit and (lo >> 25) == 0x40 and (lo & 0x3c) == 0x3c:
            op2 = (lo & 3) | ((lo >> 4) & 0x7c)
            if op2 in (0x3B, 0x38, 0x39, 0x3A):
                O.append('    if (u->cycle < u->q_done) u->cycle = u->q_done;')
            elif op2 == 0x7B:
                O.append('    if (u->cycle < u->p_done) u->cycle = u->p_done;')
        P.cur_pc = pc
        comp, commit = gen_upper(up, P)
        O.append('    {')
        for l in comp:
            O.append('        ' + l)
        ldest, br = 0, None
        if not ibit:
            ll, ldest, br = gen_lower(lo, pc, P)
            for l in ll:
                O.append('        ' + l)
            if br:
                O.append('        bp = (%s);' % br[2])
        else:
            O.append('        { uint32_t iv_ = 0x%08x; memcpy(&u->i, &iv_, 4); }' % lo)
        for l in commit(ldest):
            O.append('        ' + l)
        if ldest:
            O.append('        u->vf_ready[%d] = u->cycle + 1;' % ldest)
        O.append('    }')
        # end of pair: pending branch from the previous pair, then E-bit end
        nxt = (pc + 8) & P.cm
        prev = (pc - 8) & P.cm
        prev_is_branch = P.is_branch(prev) and prev in P.pairs
        prev_ebit = P.ebit(prev)
        if prev_is_branch:
            plo = P.pairs[prev][0]
            pop = plo >> 25
            O.append('    if (bp) { bp = 0;')
            if pop in (0x24, 0x25):
                if prev_ebit:
                    O.append('        if (ep) { u->pc = btgt & 0x%x; return 0; }' % P.cm)
                if ebit:
                    O.append('        ep = 1;')
                O.append('        goto dispatch; }')
            else:
                t = (prev + 8 + s11(plo) * 8) & P.cm
                if prev_ebit:
                    O.append('        if (ep) { u->pc = 0x%04x; return 0; }' % t)
                if ebit:
                    O.append('        ep = 1;')
                O.append('        goto L_%04x; }' % t)
        if prev_ebit:
            O.append('    if (ep) { u->pc = 0x%04x; return 0; }' % nxt)
        if ebit:
            O.append('    ep = 1;')
        if nxt not in P.pairs:
            O.append('    u->pc = 0x%04x; return 1;' % nxt)
    O.append('dispatch:')
    O.append('    switch (btgt & 0x%x) {' % P.cm)
    for pc in pcs:
        O.append('    case 0x%04x: goto L_%04x;' % (pc, pc))
    O.append('    default: u->pc = btgt & 0x%x; return 1;' % P.cm)
    O.append('    }')
    O.append('}')
    out.extend(O)


def main():
    cap = sys.argv[1]
    outp = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'port', 'gen', 'vu1_progs.c')
    progs = []
    for f in sorted(glob.glob(os.path.join(cap, 'vu[01]_*.txt'))):
        P = Prog(f)
        ok, why = P.compilable()
        if not ok:
            print('skip %s: %s' % (P.name, why))
            continue
        progs.append(P)
    out = ['/* generated by tools/vu_recomp.py from %d captured VU1 programs -- do not edit */' % len(progs),
           '#include "vu_impl.h"', '#define STALL(r) do { if (u->vf_ready[r] > u->cycle) u->cycle = u->vf_ready[r]; } while (0)', '']
    for P in progs:
        gen_prog(P, out)
        out.append('')
    for unit in (0, 1):
        ps = [P for P in progs if P.unit == unit]
        out.append('const VuProg g_vu%d_progs[] = {' % unit)
        for P in ps:
            out.append('    {0x%04x, 0x%016xull, %s},' % (P.entry, P.hash, P.name))
        out.append('    {0, 0, 0}};')
        out.append('const int g_vu%d_nprogs = %d;' % (unit, len(ps)))
    open(outp, 'w').write('\n'.join(out) + '\n')
    for P in progs:
        print('%s: %d pairs, flags ms=%d (all=%d, %d ops) clip=%d' % (P.name, len(P.pairs), P.need_ms, P.flags_all, len(P.flag_pcs), P.need_clip))


if __name__ == '__main__':
    main()
