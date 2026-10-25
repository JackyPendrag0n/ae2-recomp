#!/usr/bin/env python3
"""Minimal VU0/VU1 microcode disassembler (upper + lower)."""
import struct, sys

BC = 'xyzw'
def dst(d):
    return ''.join(c for i, c in enumerate('xyzw') if d & (8 >> i))

UP_BC = {0: 'add', 4: 'sub', 8: 'madd', 12: 'msub', 16: 'max', 20: 'mini', 24: 'mul'}
UP = {28: 'mulq', 29: 'maxi', 30: 'muli', 31: 'minii', 32: 'addq', 33: 'maddq', 34: 'addi', 35: 'maddi',
      36: 'subq', 37: 'msubq', 38: 'subi', 39: 'msubi', 40: 'add', 41: 'madd', 42: 'mul', 43: 'max', 44: 'sub',
      45: 'msub', 46: 'opmsub', 47: 'mini'}
UP2_BC = {0: 'adda', 4: 'suba', 8: 'madda', 12: 'msuba', 24: 'mula'}
UP2 = {16: 'itof0', 17: 'itof4', 18: 'itof12', 19: 'itof15', 20: 'ftoi0', 21: 'ftoi4', 22: 'ftoi12', 23: 'ftoi15',
       28: 'mulaq', 29: 'abs', 30: 'mulai', 31: 'clipw', 32: 'addaq', 33: 'maddaq', 34: 'addai', 35: 'maddai',
       36: 'subaq', 37: 'msubaq', 38: 'subai', 39: 'msubai', 40: 'adda', 41: 'madda', 42: 'mula', 44: 'suba',
       45: 'msuba', 46: 'opmula', 47: 'nop'}

def upper(w):
    d = dst((w >> 21) & 15); ft = (w >> 16) & 31; fs = (w >> 11) & 31; fd = (w >> 6) & 31
    fn = w & 63
    flags = ''.join(f for b, f in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')) if w >> b & 1)
    pre = ('[%s] ' % flags) if flags else ''
    if fn < 60:
        if fn < 28:
            b = fn & ~3
            if b in UP_BC:
                return pre + '%s%s.%s vf%02d, vf%02d, vf%02d%s' % (UP_BC[b], BC[fn & 3], d, fd, fs, ft, BC[fn & 3])
        elif fn in UP:
            nm = UP[fn]
            if nm in ('add', 'madd', 'mul', 'max', 'sub', 'msub', 'mini', 'opmsub'):
                return pre + '%s.%s vf%02d, vf%02d, vf%02d' % (nm, d, fd, fs, ft)
            return pre + '%s.%s vf%02d, vf%02d' % (nm, d, fd, fs)
        return pre + 'up?%02x' % fn
    op2 = (w & 3) | ((w >> 4) & 0x7c)
    if op2 < 28 and (op2 & ~3) in UP2_BC:
        return pre + '%s%s.%s acc, vf%02d, vf%02d%s' % (UP2_BC[op2 & ~3], BC[op2 & 3], d, fs, ft, BC[op2 & 3])
    if op2 in UP2:
        nm = UP2[op2]
        if nm == 'nop': return pre + 'nop'
        if nm.startswith(('itof', 'ftoi', 'abs')): return pre + '%s.%s vf%02d, vf%02d' % (nm, d, ft, fs)
        if nm == 'clipw': return pre + 'clipw.xyz vf%02d, vf%02dw' % (fs, ft)
        return pre + '%s.%s acc, vf%02d, vf%02d' % (nm, d, fs, ft)
    return pre + 'up2?%02x' % op2

L2 = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x36: 'lqd', 0x37: 'sqd', 0x38: 'div', 0x39: 'sqrt',
      0x3a: 'rsqrt', 0x3b: 'waitq', 0x3c: 'mtir', 0x3d: 'mfir', 0x3e: 'ilwr', 0x3f: 'iswr', 0x40: 'rnext',
      0x41: 'rget', 0x42: 'rinit', 0x43: 'rxor', 0x64: 'mfp', 0x68: 'xtop', 0x69: 'xitop', 0x6c: 'xgkick',
      0x70: 'esadd', 0x71: 'ersadd', 0x72: 'eleng', 0x73: 'erleng', 0x74: 'eatanxy', 0x75: 'eatanxz',
      0x76: 'esum', 0x78: 'esqrt', 0x79: 'ersqrt', 0x7a: 'ercpr', 0x7b: 'waitp', 0x7c: 'esin', 0x7d: 'eatan', 0x7e: 'eexp'}

def lower(w, pc):
    op7 = w >> 25
    d = dst((w >> 21) & 15); it = (w >> 16) & 31; is_ = (w >> 11) & 31; id_ = (w >> 6) & 31
    imm11 = (w & 0x7ff) - (0x800 if w & 0x400 else 0)
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7ff)
    br = lambda: '0x%04x' % (pc + 8 + imm11 * 8)
    names = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq',
             0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor',
             0x18: 'fmeq', 0x1a: 'fmand', 0x1b: 'fmor', 0x1c: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr',
             0x25: 'jalr', 0x28: 'ibeq', 0x29: 'ibne', 0x2c: 'ibltz', 0x2d: 'ibgtz', 0x2e: 'iblez', 0x2f: 'ibgez'}
    if op7 == 0x40:
        fn = w & 63
        if fn == 0x30: return 'iadd vi%02d, vi%02d, vi%02d' % (id_ & 15, is_ & 15, it & 15)
        if fn == 0x31: return 'isub vi%02d, vi%02d, vi%02d' % (id_ & 15, is_ & 15, it & 15)
        if fn == 0x32:
            im = (w >> 6) & 31; im = im - 32 if im & 16 else im
            return 'iaddi vi%02d, vi%02d, %d' % (it & 15, is_ & 15, im)
        if fn == 0x34: return 'iand vi%02d, vi%02d, vi%02d' % (id_ & 15, is_ & 15, it & 15)
        if fn == 0x35: return 'ior vi%02d, vi%02d, vi%02d' % (id_ & 15, is_ & 15, it & 15)
        if fn >= 0x3c:
            op2 = (w & 3) | ((w >> 4) & 0x7c)
            nm = L2.get(op2, 'low2?%02x' % op2)
            fsf = BC[(w >> 21) & 3]; ftf = BC[(w >> 23) & 3]
            if nm in ('move', 'mr32'): return '%s.%s vf%02d, vf%02d' % (nm, d, it, is_)
            if nm in ('lqi', 'lqd'): return '%s.%s vf%02d, (vi%02d%s)' % (nm, d, it, is_ & 15, '++' if nm == 'lqi' else '--')
            if nm in ('sqi', 'sqd'): return '%s.%s vf%02d, (vi%02d%s)' % (nm, d, is_, it & 15, '++' if nm == 'sqi' else '--')
            if nm in ('div', 'rsqrt'): return '%s q, vf%02d%s, vf%02d%s' % (nm, is_, fsf, it, ftf)
            if nm == 'sqrt': return 'sqrt q, vf%02d%s' % (it, ftf)
            if nm == 'mtir': return 'mtir vi%02d, vf%02d%s' % (it & 15, is_, fsf)
            if nm == 'mfir': return 'mfir.%s vf%02d, vi%02d' % (d, it, is_ & 15)
            if nm in ('ilwr', 'iswr'): return '%s.%s vi%02d, (vi%02d)' % (nm, d, it & 15, is_ & 15)
            if nm in ('xtop', 'xitop'): return '%s vi%02d' % (nm, it & 15)
            if nm == 'xgkick': return 'xgkick vi%02d' % (is_ & 15)
            if nm == 'mfp': return 'mfp.%s vf%02d, p' % (d, it)
            if nm in ('esqrt', 'ersqrt', 'ercpr', 'esin', 'eatan', 'eexp'): return '%s p, vf%02d%s' % (nm, is_, fsf)
            if nm in ('esadd', 'ersadd', 'eleng', 'erleng', 'eatanxy', 'eatanxz', 'esum'): return '%s p, vf%02d' % (nm, is_)
            return nm
        return 'low?%02x' % fn
    nm = names.get(op7)
    if nm is None: return 'lower?%02x' % op7
    if nm in ('lq',): return 'lq.%s vf%02d, %d(vi%02d)' % (d, it, imm11, is_ & 15)
    if nm in ('sq',): return 'sq.%s vf%02d, %d(vi%02d)' % (d, is_, imm11, it & 15)
    if nm in ('ilw', 'isw'): return '%s.%s vi%02d, %d(vi%02d)' % (nm, d, it & 15, imm11, is_ & 15)
    if nm in ('iaddiu', 'isubiu'): return '%s vi%02d, vi%02d, 0x%x' % (nm, it & 15, is_ & 15, imm15)
    if nm in ('fceq', 'fcset', 'fcand', 'fcor'): return '%s vi01, 0x%06x' % (nm, w & 0xffffff)
    if nm in ('fseq', 'fsset', 'fsand', 'fsor'): return '%s vi%02d, 0x%03x' % (nm, it & 15, ((w >> 10) & 0x800) | (w & 0x7ff))
    if nm in ('fmeq', 'fmand', 'fmor'): return '%s vi%02d, vi%02d' % (nm, it & 15, is_ & 15)
    if nm == 'fcget': return 'fcget vi%02d' % (it & 15)
    if nm == 'b': return 'b %s' % br()
    if nm == 'bal': return 'bal vi%02d, %s' % (it & 15, br())
    if nm == 'jr': return 'jr vi%02d' % (is_ & 15)
    if nm == 'jalr': return 'jalr vi%02d, vi%02d' % (it & 15, is_ & 15)
    if nm in ('ibeq', 'ibne'): return '%s vi%02d, vi%02d, %s' % (nm, it & 15, is_ & 15, br())
    return '%s vi%02d, %s' % (nm, is_ & 15, br())

def disasm(code, base=0, start=0, count=None):
    out = []
    n = len(code) // 8
    for i in range(start, n if count is None else min(n, start + count)):
        lo, up = struct.unpack_from('<II', code, i * 8)
        pc = base + i * 8
        u = upper(up)
        l = ('loi 0x%08x (%g)' % (lo, struct.unpack('<f', struct.pack('<I', lo))[0])) if up >> 31 else lower(lo, pc)
        out.append('%04x: %-44s %s' % (pc, u, l))
    return '\n'.join(out)

if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) // 8 if len(sys.argv) > 2 else 0
    cnt = int(sys.argv[3]) if len(sys.argv) > 3 else None
    print(disasm(data, 0, start, cnt))
