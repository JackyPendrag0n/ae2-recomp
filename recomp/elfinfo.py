"""ELF loading + function discovery for the AE2 executable."""
import struct
from elftools.elf.elffile import ELFFile

def s16(x):
    return x - 0x10000 if x & 0x8000 else x

class Image:
    def __init__(self, path):
        self.elf = ELFFile(open(path, 'rb'))
        self.entry = self.elf['e_entry']
        self.sections = {}
        for s in self.elf.iter_sections():
            if s['sh_addr']:
                self.sections[s.name] = (s['sh_addr'], s['sh_size'], s['sh_type'] != 'SHT_NOBITS' and s.data() or None)
        ta, tsz, td = self.sections['.text']
        self.text_base, self.text_end = ta, ta + tsz
        self.words = struct.unpack('<%dI' % (tsz // 4), td)
        self.segments = []
        for seg in self.elf.iter_segments():
            if seg['p_type'] == 'PT_LOAD':
                self.segments.append((seg['p_vaddr'], seg.data(), seg['p_memsz']))

    def in_text(self, a):
        return self.text_base <= a < self.text_end and (a & 3) == 0

    def word(self, a):
        return self.words[(a - self.text_base) >> 2]

    def data_words(self):
        for nm in ('.data', '.rodata', '.sdata', '.lit4', '.ctors', '.dtors'):
            if nm not in self.sections:
                continue
            a, sz, d = self.sections[nm]
            if not d:
                continue
            for j in range(0, len(d) - 3, 4):
                yield a + j, struct.unpack_from('<I', d, j)[0]

    def read_cstr(self, a):
        for nm, (sa, sz, d) in self.sections.items():
            if d and sa <= a < sa + sz:
                o = a - sa
                e = d.find(b'\0', o)
                if e < 0 or e - o > 200:
                    return None
                try:
                    s = d[o:e].decode('ascii')
                except UnicodeDecodeError:
                    return None
                return s if s and all(32 <= ord(c) < 127 for c in s) else None
        return None


JR_RA = 0x03E00008
NOP = 0


def is_branch(w):
    op = w >> 26
    if op in (4, 5, 6, 7, 20, 21, 22, 23):
        return True
    if op == 1 and ((w >> 16) & 0x1f) in (0, 1, 2, 3, 16, 17, 18, 19):
        return True
    if op in (16, 17, 18) and ((w >> 21) & 0x1f) == 8:
        return True
    return False


def branch_target(a, w):
    return a + 4 + (s16(w & 0xffff) << 2)


def find_functions(img):
    words, base = img.words, img.text_base
    n = len(words)
    text_addr = lambda idx: base + idx * 4
    jal_targets = set()
    lui_ptrs = set()
    jtargets = set()
    # track simple lui/addiu address materialisation
    lui_val = {}
    pending_clear = False
    for i, w in enumerate(words):
        a = base + i * 4
        op = w >> 26
        if op == 3:
            t = ((w & 0x3ffffff) << 2) | (a & 0xf0000000)
            if img.in_text(t):
                jal_targets.add(t)
        elif op == 2:
            t = ((w & 0x3ffffff) << 2) | (a & 0xf0000000)
            if img.in_text(t):
                jtargets.add(t)
        if op == 15:  # lui
            lui_val[(w >> 16) & 31] = (w & 0xffff) << 16
        elif op in (9, 13) and ((w >> 21) & 31) in lui_val:  # addiu / ori
            hi = lui_val[(w >> 21) & 31]
            v = (hi + s16(w & 0xffff)) & 0xffffffff if op == 9 else hi | (w & 0xffff)
            if img.in_text(v):
                lui_ptrs.add(v)
        if pending_clear == 'all':
            lui_val.clear()
        elif pending_clear == 'call':
            for r in list(lui_val):
                if not (16 <= r <= 23 or r in (28, 30)):
                    del lui_val[r]
        pending_clear = False
        if op == 3 or (op == 0 and (w & 0x3f) == 9):
            pending_clear = 'call'   # callee-saved registers survive calls
        elif op == 2 or (op == 0 and (w & 0x3f) == 8) or is_branch(w):
            pending_clear = 'all'

    data_ptrs = set()
    dmap = {}
    for a, v in img.data_words():
        dmap[a] = v
        if img.in_text(v):
            data_ptrs.add(v)

    # jump tables: lui rB,hi ; [addu rB,rB,idx] ; lw rX,lo(rB) ; jr rX   (bounded by sltiu ..,N)
    jt_labels = set()
    jt_owner = {}
    for i, w in enumerate(words):
        if (w & 0xfc1fffff) != 0x00000008:
            continue
        rx = (w >> 21) & 31
        if rx == 31:
            continue
        tbase = None; limit = None
        for k in range(i - 1, max(i - 10, -1), -1):
            x = words[k]
            if x >> 26 == 35 and (x >> 16) & 31 == rx and tbase is None:
                rb = (x >> 21) & 31; lo = s16(x & 0xffff)
                for m in range(k - 1, max(k - 10, -1), -1):
                    y = words[m]
                    if y >> 26 == 15 and (y >> 16) & 31 == rb:
                        tbase = ((y & 0xffff) << 16) + lo
                        break
            if x >> 26 == 11 and limit is None:
                limit = x & 0xffff
        for k in range(i - 10, max(i - 24, -1), -1):
            if limit is None and words[k] >> 26 == 11:
                limit = words[k] & 0xffff
        if tbase is None:
            continue
        ntab = limit if limit and limit < 1024 else 256   # (not "n": that is the word count)
        for e in range(ntab):
            v = dmap.get(tbase + e * 4)
            if v is None or not img.in_text(v) or abs(v - (base_text := text_addr(i))) > 0x20000:
                break
            jt_labels.add(v)
            jt_owner[v] = base_text

    def prev_ends_function(a):
        # walk back over nop padding; expect "jr ra ; <slot>" or "j x ; <slot>"
        j = (a - base) // 4 - 1
        while j >= 0 and words[j] == NOP and j > (a - base) // 4 - 8:
            j -= 1
        # j is the last non-nop; it could be the delay slot or the jump itself
        for k in (j, j - 1):
            if k < 0:
                return True
            w = words[k]
            if w == JR_RA or (w >> 26) == 2:
                # the next instruction after jump (delay slot) must be <= j
                return k + 1 >= j
        return False

    def looks_prologue(a):
        w = img.word(a)
        # addiu/daddiu sp,sp,-N
        return (w >> 16) in (0x27bd, 0x67bd) and (w & 0x8000)

    entries = set(jal_targets)
    entries.add(img.entry)
    strong = set(entries)
    for p in data_ptrs | lui_ptrs:
        if p in jt_labels and p not in strong:
            continue
        if p == base or looks_prologue(p) or prev_ends_function(p):
            entries.add(p)
    # functions reachable only by fallthrough after a return that start with a prologue
    for i, w in enumerate(words[:-2]):
        if w == JR_RA or (w >> 26) == 2:
            k = i + 2
            while k < n and words[k] == NOP:
                k += 1
            if k < n and looks_prologue(base + k * 4):
                entries.add(base + k * 4)
    import os
    xf = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_entries.txt')
    if os.path.exists(xf):
        for line in open(xf):
            line = line.split('#')[0].strip()
            if line:
                v = int(line, 16)
                entries.add(v); strong.add(v)
    entries = set(e for e in entries if img.in_text(e))

    # Each entry becomes its own function. A function's range starts at the entry
    # and ends at the next entry, extended forward to cover any branch / jump-table
    # target past the end (code may be duplicated between overlapping functions).
    # Backward targets before the start become synthetic entries (tail calls).
    import bisect
    jt_by_jr = {}
    for lab, jr in jt_owner.items():
        jt_by_jr.setdefault(jr, []).append(lab)
    synth = set()
    for _ in range(10):
        el = sorted(entries)
        funcs = []
        new_entries = set()
        for fi, e in enumerate(el):
            end = el[fi + 1] if fi + 1 < len(el) else img.text_end
            a = e
            while a < end:
                w = img.word(a)
                ts = []
                if is_branch(w):
                    ts.append(branch_target(a, w))
                elif (w >> 26) == 2:
                    t = ((w & 0x3ffffff) << 2) | (a & 0xf0000000)
                    if img.in_text(t) and not (e <= t < end) and t not in entries:
                        new_entries.add(t)   # far jump outside the function: tail call
                elif (w & 0xfc1fffff) == 0x00000008 and a in jt_by_jr:
                    ts.extend(jt_by_jr[a])
                for t in ts:
                    if not img.in_text(t):
                        continue
                    if t >= end and t - e < 0x40000 and t not in entries:
                        # forward target past our end: extend through the function containing t
                        k = bisect.bisect_right(el, t)
                        end = el[k] if k < len(el) else img.text_end
                    elif t < e and t not in entries:
                        new_entries.add(t)
                a += 4
            funcs.append((e, end))
        if not new_entries - entries:
            break
        entries |= new_entries
        synth |= new_entries

    labels = data_ptrs | lui_ptrs
    return funcs, dict(jal=jal_targets, data=data_ptrs, lui=lui_ptrs, labels=labels, synth=synth, strong=strong, jt=jt_labels)
