"""GTF textures (PS3) <-> DDS, for editing the game's textures in GIMP / Paint.NET / Photoshop.

  python -I tools/gtf.py info <file.gtf>...
  python -I tools/gtf.py export <file.gtf> <out.dds>
  python -I tools/gtf.py import <original.gtf> <edited.dds> <out.gtf>

GTF: u32 version (0x00090701), u32 data size, u32 texture count (always 1 in this game), then per texture
{u32 id, u32 data offset, u32 data size, CellGcmTexture: u8 format, u8 mip count, u8 dimension, u8 cubemap,
u32 remap, u16 width, u16 height, u16 depth, u8 location, u8 pad, u32 pitch, u32 offset}, big endian.
Formats used by the game: 0x86 DXT1, 0x87 DXT3, 0x88 DXT5 (block data as on PC), 0x85 A8R8G8B8 swizzled
(Z-order per mip level), 0xA5 = 0x85 | 0x20 linear. Mip levels follow each other; cube maps hold 6 faces, each
128-byte aligned.
Import keeps the original's format, size and mip count: the DDS must match them (save DXT1/3/5 with mipmaps as the
original had). Changing those needs the header rewritten too (not done yet).
"""
import struct
import sys

LN = 0x20
FORMATS = {0x86: ('DXT1', 8), 0x87: ('DXT3', 16), 0x88: ('DXT5', 16), 0x85: ('A8R8G8B8', 4)}


class Gtf:
    def __init__(self, data: bytes):
        ver, size, n = struct.unpack_from('>III', data, 0)
        if ver >> 8 != 0x000907 or n < 1:
            raise ValueError('not a GTF texture')
        self.raw = bytearray(data)
        self.id, self.off, self.size = struct.unpack_from('>III', data, 12)
        (self.fmt, self.mips, self.dim, self.cube, self.remap, self.w, self.h, self.depth, self.loc, _pad,
         self.pitch, _o) = struct.unpack_from('>BBBBIHHHBBII', data, 24)
        self.base = self.fmt & ~(LN | 0x40)
        self.linear = bool(self.fmt & LN)
        if self.base not in FORMATS:
            raise ValueError(f'format {self.fmt:#x} not handled')

    @property
    def kind(self):
        return FORMATS[self.base][0]

    def level_sizes(self):
        name, bpb = FORMATS[self.base]
        w, h, out = self.w, self.h, []
        for _ in range(max(1, self.mips)):
            if name.startswith('DXT'):
                out.append((w, h, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpb))
            else:
                out.append((w, h, w * h * 4))
            w, h = max(1, w // 2), max(1, h // 2)
        return out

    def faces(self):
        return 6 if self.cube else 1

    def face_size(self):
        return sum(s for _, _, s in self.level_sizes())

    def face_stride(self):
        n = self.face_size()
        return (n + 127) & ~127 if self.cube else n

    def data(self):
        return bytes(self.raw[self.off:self.off + self.size])


def morton_index(x, y, w, h):
    """Offset (in texels) of (x, y) in a Z-order swizzled w x h level (rectangles: the square part is swizzled,
    the longer side's high bits come on top, as on the RSX)."""
    i, bit, sx, sy = 0, 0, 1, 1
    while sx < w or sy < h:
        if sx < w:
            i |= (x & sx) and (1 << bit); bit += 1; sx <<= 1
        if sy < h:
            i |= (y & sy) and (1 << bit); bit += 1; sy <<= 1
    return i


def argb_level(src: bytes, w, h, swizzled, to_dds: bool):
    """A8R8G8B8 level, GTF byte order (A R G B, Z-order if swizzled) <-> DDS (B G R A, linear rows)."""
    out = bytearray(len(src))
    for y in range(h):
        for x in range(w):
            g = (morton_index(x, y, w, h) if swizzled else y * w + x) * 4
            d = (y * w + x) * 4
            if to_dds:
                a, r, gg, b = src[g:g + 4]
                out[d:d + 4] = bytes((b, gg, r, a))
            else:
                b, gg, r, a = src[d:d + 4]
                out[g:g + 4] = bytes((a, r, gg, b))
    return bytes(out)


DDSD_FLAGS = 0x1 | 0x2 | 0x4 | 0x1000      # caps, height, width, pixelformat
DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE, DDSD_PITCH = 0x20000, 0x80000, 0x8


def dds_header(t: Gtf):
    flags = DDSD_FLAGS | (DDSD_MIPMAPCOUNT if t.mips > 1 else 0)
    caps, caps2 = 0x1000, 0
    if t.mips > 1:
        caps |= 0x400008
    if t.cube:
        caps |= 0x8
        caps2 = 0x200 | 0xFC00
    if t.kind.startswith('DXT'):
        flags |= DDSD_LINEARSIZE
        pf = struct.pack('<II4sIIIII', 32, 0x4, t.kind.encode(), 0, 0, 0, 0, 0)
        pitch = t.level_sizes()[0][2]
    else:
        flags |= DDSD_PITCH
        pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
        pitch = t.w * 4
    hdr = struct.pack('<4sIIIIIII44s', b'DDS ', 124, flags, t.h, t.w, pitch, 0, max(1, t.mips), b'\0' * 44)
    return hdr + pf + struct.pack('<IIIII', caps, caps2, 0, 0, 0)


def to_dds(t: Gtf) -> bytes:
    src = t.data()
    out = bytearray(dds_header(t))
    for f in range(t.faces()):
        p = f * t.face_stride()
        for w, h, n in t.level_sizes():
            lvl = src[p:p + n]
            out += lvl if t.kind.startswith('DXT') else argb_level(lvl, w, h, not t.linear, True)
            p += n
    return bytes(out)


def from_dds(t: Gtf, dds: bytes) -> bytes:
    if dds[:4] != b'DDS ':
        raise ValueError('not a DDS file')
    h, w = struct.unpack_from('<II', dds, 12)
    mips = max(1, struct.unpack_from('<I', dds, 28)[0])
    fourcc = dds[84:88]
    want = t.kind.encode() if t.kind.startswith('DXT') else b'\0\0\0\0'
    problems = []
    if (w, h) != (t.w, t.h): problems.append(f'size {w}x{h}, original {t.w}x{t.h}')
    if mips != max(1, t.mips): problems.append(f'{mips} mip levels, original {max(1, t.mips)}')
    if fourcc != want: problems.append(f'format {fourcc!r}, original {t.kind}')
    if problems:
        raise ValueError('DDS does not match the original: ' + '; '.join(problems))
    p = 128
    data = bytearray(t.size)
    for f in range(t.faces()):
        q = f * t.face_stride()
        for lw, lh, n in t.level_sizes():
            lvl = dds[p:p + n]
            if len(lvl) != n:
                raise ValueError('DDS is truncated')
            data[q:q + n] = lvl if t.kind.startswith('DXT') else argb_level(lvl, lw, lh, not t.linear, False)
            p += n
            q += n
    out = bytearray(t.raw)
    out[t.off:t.off + t.size] = data
    return bytes(out)


def main(argv):
    if not argv:
        print(__doc__); return 1
    cmd = argv[0]
    if cmd == 'info':
        for p in argv[1:]:
            t = Gtf(open(p, 'rb').read())
            print(f'{p}: {t.kind}{"" if t.linear or t.kind != "A8R8G8B8" else " swizzled"} {t.w}x{t.h} '
                  f'mips {t.mips}{" cube" if t.cube else ""} remap {t.remap:#x}')
    elif cmd == 'export':
        t = Gtf(open(argv[1], 'rb').read())
        open(argv[2], 'wb').write(to_dds(t))
    elif cmd == 'import':
        t = Gtf(open(argv[1], 'rb').read())
        open(argv[3], 'wb').write(from_dds(t, open(argv[2], 'rb').read()))
    else:
        print(__doc__); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
