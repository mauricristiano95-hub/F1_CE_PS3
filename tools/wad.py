"""F1 Championship Edition .wad archives (offline.wad, <track>.wad, stream.wad, update.wad, ...).

Format (from the game's Wad class, ctor 0x4A8E4, open 0x4A4B8, LZ stream read 0x41208):
  header   u32 version (1), u32 count                         big endian
  entries  count x { u32 hash, u32 offset, u32 size, u32 stored }
  data     each entry at `offset`, `stored` bytes; stored == size means raw, otherwise LZSS (below)
Name hash (0x1DA20): CRC-32 (reflected 0xEDB88320) with register init 0 and final NOT over the path
lower-cased with '\\' -> '/'; "#XXXXXXXX" (9 chars) is a literal hash.
LZSS: MSB-first bit reader; flag 1 = 8-bit literal, flag 0 = 13-bit absolute window position + 4-bit length,
copy length+3 bytes; window 0x2000 bytes, zero-filled, write position starts at 1.

  python -I tools/wad.py list <wad> [names.txt]         entries with names where known
  python -I tools/wad.py extract <wad> <outdir> [names.txt]
  python -I tools/wad.py hash <path>...
names.txt: one candidate path per line, default tools/wad_names.txt (made by tools/wad_names.py); unknown entries
are written as _unknown/<hash>.<guessed ext>, the form runtime/mods.cpp accepts in a mod pack.
"""
import struct
import sys
import zlib
from pathlib import Path


def name_hash(path: str) -> int:
    if len(path) == 9 and path[0] == '#':
        try:
            return int(path[1:], 16)
        except ValueError:
            pass
    return zlib.crc32(path.replace('\\', '/').lower().encode('latin1'), 0xFFFFFFFF)


_BITS = [format(i, '08b') for i in range(256)]


def lzss_decode(src: bytes, size: int) -> bytes:
    # The window is the last 0x2000 bytes of the output (write position = (1 + len) & 0x1FFF, zero-filled at start),
    # so a window position maps to an output index; positions not yet written read as zero.
    s = ''.join(_BITS[b] for b in src) + '0' * 32
    out = bytearray()
    p = 0
    n = 0
    while n < size:
        if s[p] == '1':
            out.append(int(s[p + 1:p + 9], 2))
            p += 9
            n += 1
        else:
            wp = int(s[p + 1:p + 14], 2)
            ln = int(s[p + 14:p + 18], 2) + 3
            p += 18
            # output index of window position wp: the most recent i < n with (1 + i) & 0x1FFF == wp; the game copies
            # byte by byte (read before write), so position wp + k holds index i + k, older than the k-th new byte
            i = n - 1 - ((n - wp) & 0x1FFF)
            for k in range(ln):
                j = i + k
                out.append(out[j] if j >= 0 else 0)
            n += ln
    del out[size:]
    return bytes(out)


class Wad:
    def __init__(self, path):
        self.path = Path(path)
        self.f = open(path, 'rb')
        ver, n = struct.unpack('>II', self.f.read(8))
        tab = self.f.read(16 * n)
        self.version = ver
        self.entries = [struct.unpack_from('>IIII', tab, 16 * i) for i in range(n)]
        self.by_hash = {e[0]: e for e in self.entries}

    def read(self, entry) -> bytes:
        h, off, size, stored = entry
        self.f.seek(off)
        data = self.f.read(stored)
        return data if stored == size else lzss_decode(data, size)

    def get(self, name):
        e = self.by_hash.get(name_hash(name))
        return None if e is None else self.read(e)


MAGIC_EXT = [(b'\x1bLua', '.lub'), (b'DDS ', '.dds'), (b'\x89PNG', '.png'), (b'<?xml', '.xml'), (b'RIFF', '.riff')]


def guess_ext(data: bytes) -> str:
    for m, ext in MAGIC_EXT:
        if data.startswith(m):
            return ext
    return '.bin'


def load_names(path):
    names = {}
    path = path or Path(__file__).with_name('wad_names.txt')
    if Path(path).exists():
        for line in open(path, encoding='latin1'):
            s = line.strip()
            if s:
                names.setdefault(name_hash(s), s)
    return names


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 1
    cmd = argv[0]
    if cmd == 'hash':
        for p in argv[1:]:
            print(f'{name_hash(p):08x} {p}')
        return 0
    w = Wad(argv[1])
    if cmd == 'list':
        names = load_names(argv[2] if len(argv) > 2 else None)
        known = 0
        for h, off, size, stored in w.entries:
            n = names.get(h, '')
            known += bool(n)
            print(f'{h:08x} {off:10d} {size:10d} {stored:10d} {"lz " if stored != size else "raw"} {n}')
        print(f'# {len(w.entries)} entries, {known} named', file=sys.stderr)
    elif cmd == 'extract':
        out = Path(argv[2])
        names = load_names(argv[3] if len(argv) > 3 else None)
        for e in w.entries:
            data = w.read(e)
            n = names.get(e[0])
            p = out / n if n else out / '_unknown' / f'{e[0]:08x}{guess_ext(data)}'
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        print(f'{len(w.entries)} files -> {out}')
    else:
        print(__doc__); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
