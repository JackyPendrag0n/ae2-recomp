#!/usr/bin/env python3
"""Static recompiler for IOP modules (R3000 / MIPS I .irx files).

The module is relocated to a fixed IOP address, every function is translated
to a C function taking an IopCtx, calls into imported libraries (found via
the IRX import tables) become calls to C stubs named
    iop_imp_<library>_<index>(IopCtx *)
and computed jumps through relocated jump tables become switches.

usage: iop_recomp.py module.irx base_hex prefix out.c
"""
import struct, sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Module:
    def __init__(self, path, base):
        self.e = ELFFile(open(path, 'rb'))
        self.base = base
        secs = {s.name: s for s in self.e.iter_sections()}
        self.text = bytearray(secs['.text'].data())
        mod = secs['.iopmod'].data()
        self.entry = struct.unpack_from('<I', mod, 4)[0]
        self.gp = struct.unpack_from('<I', mod, 8)[0]
        # image: text + rodata + data (+ bss) laid out by their sh_addr
        end = 0
        for s in self.e.iter_sections():
            if s['sh_flags'] & 2:   # SHF_ALLOC
                end = max(end, s['sh_addr'] + s['sh_size'])
        self.size = end
        self.image = bytearray(end)
        self.bss = []
        for s in self.e.iter_sections():
            if s['sh_flags'] & 2:
                if s['sh_type'] == 'SHT_NOBITS':
                    self.bss.append((s['sh_addr'], s['sh_size']))
                else:
                    self.image[s['sh_addr']:s['sh_addr'] + s['sh_size']] = s.data()
        self.text_end = secs['.text']['sh_size']
        self.data_ptrs = set()     # R_MIPS_32 targets inside .text (jump tables, callbacks)
        self.relocate(secs)

    def relocate(self, secs):
        img = self.image
        for s in self.e.iter_sections():
            if not isinstance(s, RelocationSection):
                continue
            tgt = self.e.get_section(s['sh_info'])
            toff = tgt['sh_addr']
            rels = list(s.iter_relocations())
            i = 0
            while i < len(rels):
                r = rels[i]
                off = r['r_offset']   # IRX relocation offsets are image addresses
                t = r['r_info_type']
                w = struct.unpack_from('<I', img, off)[0]
                if t == 2:      # R_MIPS_32
                    v = (w + self.base) & 0xffffffff
                    struct.pack_into('<I', img, off, v)
                    if w < self.text_end and tgt.name != '.text':
                        self.data_ptrs.add(w)
                elif t == 4:    # R_MIPS_26
                    tg = ((w & 0x3ffffff) << 2) + self.base
                    struct.pack_into('<I', img, off, (w & 0xfc000000) | ((tg >> 2) & 0x3ffffff))
                elif t == 5:    # R_MIPS_HI16 (paired with following LO16)
                    j = i + 1
                    while j < len(rels) and rels[j]['r_info_type'] != 6:
                        j += 1
                    lo = struct.unpack_from('<I', img, rels[j]['r_offset'])[0] if j < len(rels) else 0
                    ahl = ((w & 0xffff) << 16) + s16(lo & 0xffff)
                    v = ahl + self.base
                    hi = ((v + 0x8000) >> 16) & 0xffff
                    struct.pack_into('<I', img, off, (w & 0xffff0000) | hi)
                    if ahl < self.text_end:
                        self.data_ptrs.add(ahl)
                elif t == 6:    # R_MIPS_LO16
                    v = (s16(w & 0xffff) + self.base) & 0xffff
                    struct.pack_into('<I', img, off, (w & 0xffff0000) | v)
                i += 1

    def word(self, a):
        return struct.unpack_from('<I', self.image, a)[0]

    def imports(self):
        """Map of stub address (module-relative) -> (lib, index)."""
        st = {}
        n = self.text_end // 4
        i = 0
        while i < n:
            if self.word(i * 4) == 0x41e00000:
                name = bytes(self.image[(i + 3) * 4:(i + 5) * 4]).split(b'\0')[0].decode()
                j = i + 5
                while j + 1 < n and self.word(j * 4) == 0x03e00008:
                    st[j * 4] = (name, self.word(j * 4 + 4) & 0xffff)
                    j += 2
                i = j
            else:
                i += 1
        return st


R = lambda n: '0' if n == 0 else 'c->r[%d]' % n


class Gen:
    def __init__(self, m, prefix):
        self.m = m
        self.prefix = prefix
        self.stubs = m.imports()
        self.stub_lo = min(self.stubs) - 20 if self.stubs else m.text_end

    def fname(self, a):
        return '%s_%05x' % (self.prefix, a)

    def find_functions(self):
        m = self.m
        ents = {m.entry}
        for a in range(0, self.stub_lo, 4):
            w = m.word(a)
            if (w >> 26) == 3:
                t = ((w & 0x3ffffff) << 2) - m.base
                if 0 <= t < self.stub_lo:
                    ents.add(t)
        # code pointers loaded with lui/addiu (callbacks, thread entries, handlers) and
        # R_MIPS_32 pointers that are not jump-table entries
        self.jt_targets = set()
        for p in m.data_ptrs:
            if p < self.stub_lo:
                ents.add(p)
        # jump tables: data pointers into the middle of functions are labels, not entries.
        # Heuristic: a pointer is a function if it is preceded by jr ra (+delay) or is 0.
        real = set()
        for e in ents:
            if e == 0 or e in (m.entry,):
                real.add(e)
                continue
            k = e - 4
            while k > 0 and m.word(k) == 0 and e - k < 20:
                k -= 4
            after_ret = m.word(k) == 0x03e00008 or (k >= 4 and m.word(k - 4) == 0x03e00008)
            if after_ret or self.is_jal_target(e):
                real.add(e)
            else:
                self.jt_targets.add(e)
        self.funcs = sorted(real)

    def is_jal_target(self, a):
        tg = ((a + self.m.base) >> 2) & 0x3ffffff
        for b in range(0, self.stub_lo, 4):
            w = self.m.word(b)
            if (w >> 26) == 3 and (w & 0x3ffffff) == tg:
                return True
        return False

    # ---------------------------------------------------------------- instructions
    def simple(self, w, a):
        op = w >> 26
        rs, rt, rd, sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        imm = w & 0xffff
        si = s16(imm)
        def setr(n, e):
            return '%s = %s;' % (R(n), e) if n else ';'
        if op == 0:
            fn = w & 63
            if fn == 0: return setr(rd, '%s << %d' % (R(rt), sa)) if w else ';'
            if fn == 2: return setr(rd, '%s >> %d' % (R(rt), sa))
            if fn == 3: return setr(rd, '(uint32_t)((int32_t)%s >> %d)' % (R(rt), sa))
            if fn == 4: return setr(rd, '%s << (%s & 31)' % (R(rt), R(rs)))
            if fn == 6: return setr(rd, '%s >> (%s & 31)' % (R(rt), R(rs)))
            if fn == 7: return setr(rd, '(uint32_t)((int32_t)%s >> (%s & 31))' % (R(rt), R(rs)))
            if fn == 0x0d: return 'iop_break(c, 0x%x);' % (a + self.m.base)
            if fn == 0x10: return setr(rd, 'c->hi')
            if fn == 0x11: return 'c->hi = %s;' % R(rs)
            if fn == 0x12: return setr(rd, 'c->lo')
            if fn == 0x13: return 'c->lo = %s;' % R(rs)
            if fn == 0x18: return '{ int64_t p_ = (int64_t)(int32_t)%s * (int32_t)%s; c->lo = (uint32_t)p_; c->hi = (uint32_t)(p_ >> 32); }' % (R(rs), R(rt))
            if fn == 0x19: return '{ uint64_t p_ = (uint64_t)%s * %s; c->lo = (uint32_t)p_; c->hi = (uint32_t)(p_ >> 32); }' % (R(rs), R(rt))
            if fn == 0x1a: return 'IOP_DIV(c, %s, %s);' % (R(rs), R(rt))
            if fn == 0x1b: return 'IOP_DIVU(c, %s, %s);' % (R(rs), R(rt))
            if fn in (0x20, 0x21): return setr(rd, '%s + %s' % (R(rs), R(rt)))
            if fn in (0x22, 0x23): return setr(rd, '%s - %s' % (R(rs), R(rt)))
            if fn == 0x24: return setr(rd, '%s & %s' % (R(rs), R(rt)))
            if fn == 0x25: return setr(rd, '%s | %s' % (R(rs), R(rt)))
            if fn == 0x26: return setr(rd, '%s ^ %s' % (R(rs), R(rt)))
            if fn == 0x27: return setr(rd, '~(%s | %s)' % (R(rs), R(rt)))
            if fn == 0x2a: return setr(rd, '(int32_t)%s < (int32_t)%s' % (R(rs), R(rt)))
            if fn == 0x2b: return setr(rd, '%s < %s' % (R(rs), R(rt)))
            if fn == 0x0c: return 'iop_syscall(c, 0x%x);' % (a + self.m.base)
            raise Exception('special %x at %x' % (fn, a))
        if op in (8, 9): return setr(rt, '%s + %d' % (R(rs), si))
        if op == 0x0a: return setr(rt, '(int32_t)%s < %d' % (R(rs), si))
        if op == 0x0b: return setr(rt, '%s < 0x%xu' % (R(rs), si & 0xffffffff))
        if op == 0x0c: return setr(rt, '%s & 0x%x' % (R(rs), imm))
        if op == 0x0d: return setr(rt, '%s | 0x%x' % (R(rs), imm))
        if op == 0x0e: return setr(rt, '%s ^ 0x%x' % (R(rs), imm))
        if op == 0x0f: return setr(rt, '0x%x' % (imm << 16))
        ea = '%s + %d' % (R(rs), si)
        if op == 0x20: return setr(rt, '(uint32_t)(int8_t)IR8(%s)' % ea)
        if op == 0x21: return setr(rt, '(uint32_t)(int16_t)IR16(%s)' % ea)
        if op == 0x23: return setr(rt, 'IR32(%s)' % ea)
        if op == 0x24: return setr(rt, 'IR8(%s)' % ea)
        if op == 0x25: return setr(rt, 'IR16(%s)' % ea)
        if op == 0x28: return 'IW8(%s, %s);' % (ea, R(rt))
        if op == 0x29: return 'IW16(%s, %s);' % (ea, R(rt))
        if op == 0x2b: return 'IW32(%s, %s);' % (ea, R(rt))
        if op == 0x10:   # COP0
            sub = (w >> 21) & 31
            if sub == 0: return setr(rt, 'iop_mfc0(c, %d)' % rd)
            if sub == 4: return 'iop_mtc0(c, %d, %s);' % (rd, R(rt))
            if sub == 16 and (w & 63) == 0x10: return 'iop_rfe(c);'
        raise Exception('op %x at %x (%08x)' % (op, a, w))

    def branch_info(self, w, a):
        """Returns (kind, cond_expr, target) for control flow, else None."""
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        tgt = a + 4 + s16(w & 0xffff) * 4
        if op == 0 and (w & 63) == 8: return ('jr', None, rs)
        if op == 0 and (w & 63) == 9: return ('jalr', None, rs)
        if op == 2: return ('j', '1', ((w & 0x3ffffff) << 2) - self.m.base)
        if op == 3: return ('jal', '1', ((w & 0x3ffffff) << 2) - self.m.base)
        if op == 4: return ('b', '%s == %s' % (R(rs), R(rt)), tgt)
        if op == 5: return ('b', '%s != %s' % (R(rs), R(rt)), tgt)
        if op == 6: return ('b', '(int32_t)%s <= 0' % R(rs), tgt)
        if op == 7: return ('b', '(int32_t)%s > 0' % R(rs), tgt)
        if op == 1:
            if rt == 0: return ('b', '(int32_t)%s < 0' % R(rs), tgt)
            if rt == 1: return ('b', '(int32_t)%s >= 0' % R(rs), tgt)
            if rt == 0x10: return ('bal', '(int32_t)%s < 0' % R(rs), tgt)
            if rt == 0x11: return ('bal', '(int32_t)%s >= 0' % R(rs), tgt)
        return None

    def call_code(self, tgt):
        if tgt in self.stubs:
            lib, idx = self.stubs[tgt]
            return 'iop_imp_%s_%d(c);' % (lib, idx)
        return '%s(c);' % self.fname(tgt)

    def gen_function(self, start, end, out):
        m = self.m
        labels = {start}
        for a in range(start, end, 4):
            bi = self.branch_info(m.word(a), a)
            if bi and bi[0] in ('b', 'j') and start <= bi[2] < end:
                labels.add(bi[2])
        jt = sorted(t for t in self.jt_targets if start <= t < end)
        labels.update(jt)
        out.append('void %s(IopCtx *c) {' % self.fname(start))
        a = start
        while a < end:
            w = m.word(a)
            if a in labels:
                out.append('L_%05x:;' % a)
            bi = self.branch_info(w, a)
            if bi is None:
                out.append('  ' + self.simple(w, a))
                a += 4
                continue
            kind, cond, tgt = bi
            dw = m.word(a + 4)
            dslot_is_branch = self.branch_info(dw, a + 4) is not None
            if dslot_is_branch:
                # hand-written code with a branch in a delay slot: treat the slot as empty
                dslot = ';'
            else:
                dslot = self.simple(dw, a + 4) if dw else ';'
            labeled_slot = (a + 4) in labels or dslot_is_branch
            if kind == 'jal':
                out.append('  c->r[31] = 0x%x; %s %s' % (a + 8 + m.base, dslot, self.call_code(tgt)))
            elif kind == 'jalr':
                rd = (w >> 11) & 31
                out.append('  { uint32_t t_ = %s; %s = 0x%x; %s iop_call(c, t_); }' % (R(tgt), R(rd) if rd else 'c->r[0]', a + 8 + m.base, dslot))
            elif kind == 'jr':
                if tgt == 31:
                    out.append('  %s return;' % dslot)
                else:
                    out.append('  { uint32_t t_ = %s - 0x%x; %s switch (t_) {' % (R(tgt), m.base, dslot))
                    for t in jt:
                        out.append('    case 0x%x: goto L_%05x;' % (t, t))
                    out.append('    default: iop_call(c, t_ + 0x%x); return; } }' % m.base)
            elif kind == 'j':
                if start <= tgt < end:
                    out.append('  %s goto L_%05x;' % (dslot, tgt))
                else:
                    out.append('  %s %s return;' % (dslot, self.call_code(tgt)))
            elif kind == 'b':
                go = ('goto L_%05x;' % tgt) if start <= tgt < end else ('{ %s return; }' % self.call_code(tgt))
                out.append('  { int t_ = %s; %s if (t_) %s }' % (cond, dslot, go))
            elif kind == 'bal':
                out.append('  { int t_ = %s; c->r[31] = 0x%x; %s if (t_) %s }' % (cond, a + 8 + m.base, dslot, self.call_code(tgt)))
            if labeled_slot:
                # the slot is also reached on its own: skip over it on fall-through
                if kind in ('b', 'bal', 'jal', 'jalr'):
                    labels.add(a + 8)
                    out.append('  goto L_%05x;' % (a + 8))
                a += 4
                continue
            a += 8
            if a in labels or a >= end:
                pass
        # falling off the end continues in the next function
        if end < self.stub_lo:
            out.append('  %s(c);' % self.fname(end))
        out.append('}')

    def run(self, outp):
        self.find_functions()
        out = ['/* generated by recomp/iop_recomp.py -- do not edit */', '#include "iop_mod.h"', '']
        fl = self.funcs
        for f in fl:
            out.append('void %s(IopCtx *c);' % self.fname(f))
        out.append('')
        for i, f in enumerate(fl):
            end = fl[i + 1] if i + 1 < len(fl) else self.stub_lo
            self.gen_function(f, end, out)
            out.append('')
        # image and dispatch table
        out.append('const uint32_t %s_base = 0x%x, %s_size = 0x%x, %s_gp = 0x%x, %s_entry = 0x%x;' % (
            self.prefix, self.m.base, self.prefix, self.m.size, self.prefix, self.m.gp + self.m.base, self.prefix, self.m.entry + self.m.base))
        out.append('const uint8_t %s_image[] = {' % self.prefix)
        img = self.m.image
        for i in range(0, len(img), 32):
            out.append('  ' + ', '.join('0x%02x' % b for b in img[i:i + 32]) + ',')
        out.append('};')
        out.append('const IopFn %s_funcs[] = {' % self.prefix)
        for f in fl:
            out.append('  {0x%x, %s},' % (f + self.m.base, self.fname(f)))
        out.append('  {0, 0}};')
        open(outp, 'w').write('\n'.join(out) + '\n')
        print('%d functions, %d jump-table labels, %d imports, image 0x%x bytes' % (len(fl), len(self.jt_targets), len(self.stubs), self.m.size))
        for (lib, idx) in sorted(set(self.stubs.values())):
            print('  import %s %d' % (lib, idx))


if __name__ == '__main__':
    m = Module(sys.argv[1], int(sys.argv[2], 16))
    Gen(m, sys.argv[3]).run(sys.argv[4])
