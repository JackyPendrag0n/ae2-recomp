#!/usr/bin/env python3
"""VU microprograms without shipping game code.

The runtime's AE2_VUCAPTURE files ("entry XXXX hash H" + "pc lower upper" lines) are
copies of VU micro memory, so they are the game's code and cannot go in the repository.
Every non-zero instruction in them comes from SCES_508.85, so the repository keeps only a
recipe: for each program, which runs of ELF bytes (file offset) land at which micro
memory addresses. `build` turns the recipe plus your own ELF back into the capture files.

usage: vu_programs.py make  capture_dir SCES_508.85 out.recipe   (maintainers)
       vu_programs.py build recipe      SCES_508.85 out_dir
"""
import glob, os, struct, sys

ZERO = b'\0' * 8


def read_capture(path):
    lines = open(path).read().split('\n')
    pairs = []
    for l in lines[1:]:
        if l.strip():
            a, lo, up = l.split()
            pairs.append((int(a, 16), int(lo, 16), int(up, 16)))
    return lines[0], pairs


def make(capdir, elf_path, out):
    elf = open(elf_path, 'rb').read()
    idx = {}
    for o in range(0, len(elf) - 7, 4):
        k = elf[o:o + 8]
        if k != ZERO:
            idx.setdefault(k, o)
    res = ['# VU microprogram recipe: rebuilt from your SCES_508.85 by tools/vu_programs.py',
           '# prog <file> | <header>; run <pc> <count> <elf offset>; zero <pc> <count>']
    for path in sorted(glob.glob(os.path.join(capdir, '*.txt'))):
        head, pairs = read_capture(path)
        res.append('prog %s | %s' % (os.path.basename(path), head))
        cur = None   # [kind, pc, count, off]
        for a, lo, up in pairs:
            k = struct.pack('<II', lo, up)
            if cur and cur[1] + cur[2] * 8 == a:
                if cur[0] == 'zero' and k == ZERO: cur[2] += 1; continue
                if cur[0] == 'run' and elf[cur[3] + cur[2] * 8:cur[3] + cur[2] * 8 + 8] == k: cur[2] += 1; continue
            if cur: res.append(fmt(cur))
            if k == ZERO: cur = ['zero', a, 1, 0]
            else:
                if k not in idx: sys.exit('%s: instruction at %04x is not in the ELF' % (path, a))
                cur = ['run', a, 1, idx[k]]
        if cur: res.append(fmt(cur))
    open(out, 'w').write('\n'.join(res) + '\n')


def fmt(c):
    return 'zero %04x %d' % (c[1], c[2]) if c[0] == 'zero' else 'run %04x %d %x' % (c[1], c[2], c[3])


def build(recipe, elf_path, outdir):
    elf = open(elf_path, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    out = None; name = None; n = 0

    def flush():
        if name: open(os.path.join(outdir, name), 'w').write('\n'.join(out) + '\n')

    for l in open(recipe):
        l = l.rstrip('\n')
        if not l or l.startswith('#'): continue
        t = l.split()
        if t[0] == 'prog':
            flush(); n += 1
            name, head = l[5:].split(' | ', 1); name = name.strip(); out = [head]
        elif t[0] == 'zero':
            pc, cnt = int(t[1], 16), int(t[2])
            out += ['%04x %08x %08x' % (pc + i * 8, 0, 0) for i in range(cnt)]
        elif t[0] == 'run':
            pc, cnt, off = int(t[1], 16), int(t[2]), int(t[3], 16)
            for i in range(cnt):
                lo, up = struct.unpack_from('<II', elf, off + i * 8)
                out.append('%04x %08x %08x' % (pc + i * 8, lo, up))
    flush()
    return n


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == 'make': make(*sys.argv[2:])
    elif len(sys.argv) == 5 and sys.argv[1] == 'build': print('%d VU programs written to %s' % (build(*sys.argv[2:]), sys.argv[4]))
    else: sys.exit(__doc__)
