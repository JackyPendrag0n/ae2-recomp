#!/usr/bin/env python3
"""Extract a PS2 disc image (ISO 9660) into a folder. No dependencies.

Takes a plain .iso (2048-byte sectors) or a raw .bin/.img (2352-byte sectors,
mode 1 or mode 2 form 1). File names lose their ";1" version suffix.

usage: iso_extract.py image.iso out_dir
"""
import os, struct, sys

SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


class Image:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2); size = self.f.tell()
        self.ssize, self.hdr = 2048, 0
        self.f.seek(0); first = self.f.read(16)
        if size % 2352 == 0 and first[:12] == SYNC:
            self.ssize = 2352
            self.hdr = 24 if first[15] == 2 else 16   # mode 2 form 1: sync+header+subheader
        pvd = self.sector(16)
        if pvd[1:6] != b'CD001':
            sys.exit('%s: not an ISO 9660 disc image (no CD001 at sector 16)' % path)

    def sector(self, lba, n=1):
        if self.ssize == 2048:
            self.f.seek(lba * 2048); return self.f.read(2048 * n)
        out = bytearray()
        for i in range(n):
            self.f.seek((lba + i) * self.ssize + self.hdr); out += self.f.read(2048)
        return bytes(out)

    def read_extent(self, lba, size, out):
        left = size
        while left > 0:
            chunk = min(left, 2048 * 512)
            n = (chunk + 2047) // 2048
            out.write(self.sector(lba, n)[:chunk])
            lba += n; left -= chunk

    def records(self, lba, size):
        data = self.sector(lba, (size + 2047) // 2048)[:size]
        i = 0
        while i < len(data):
            ln = data[i]
            if ln == 0:   # records never cross a sector boundary: skip to the next sector
                i = (i // 2048 + 1) * 2048; continue
            rec = data[i:i + ln]
            ext, sz = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            flags, nlen = rec[25], rec[32]
            name = rec[33:33 + nlen]
            i += ln
            if name in (b'\0', b'\1'): continue
            name = name.decode('ascii', 'replace').split(';')[0].rstrip('.')
            yield name, ext, sz, bool(flags & 2)


def extract(path, outdir, log=print):
    img = Image(path)
    root = img.sector(16)[156:156 + 34]
    rlba, rsize = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
    count = [0, 0]

    def walk(lba, size, dst, depth=0):
        if depth > 16: return
        os.makedirs(dst, exist_ok=True)
        for name, ext, sz, is_dir in img.records(lba, size):
            if not name or '/' in name or '\\' in name or name in ('.', '..'): continue
            p = os.path.join(dst, name)
            if is_dir: walk(ext, sz, p, depth + 1)
            else:
                with open(p, 'wb') as o: img.read_extent(ext, sz, o)
                count[0] += 1; count[1] += sz
                if count[0] % 50 == 0: log('  %d files, %d MB' % (count[0], count[1] >> 20))

    walk(rlba, rsize, outdir)
    log('extracted %d files (%d MB) to %s' % (count[0], count[1] >> 20, outdir))
    return count[0]


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    extract(sys.argv[1], sys.argv[2])
