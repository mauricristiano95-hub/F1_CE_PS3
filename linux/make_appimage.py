#!/usr/bin/env python3
"""Packs the Linux build into an AppImage, on Windows or Linux (no mksquashfs or appimagetool needed).

    python linux/make_appimage.py [--exe build-linux/f1ce] [--out out/F1CE-x86_64.AppImage]

AppDir: AppRun (linux/AppRun, the launcher), the .desktop file and icon, usr/bin/f1ce and, when it exists, the program
database usr/share/f1ce-recomp/programs.bin (build-win/shader_cache/programs.bin: the shader microcode the game has
used, so the first start compiles every shader up front). The image is a SquashFS 4.0 filesystem (zlib, no
fragments) written by this script, after the AppImage type 2 runtime (downloaded once into
third_party/appimage-runtime). A .tar.gz holding the AppImage with its executable bit is written next to it, for
copying from Windows (the bit does not survive most transfers).

The AppImage contains code generated from the game's executable: it is for your own use, not for distribution.
"""
import argparse
import hashlib
import io
import os
import stat
import struct
import sys
import tarfile
import time
import urllib.request
import zlib

RUNTIME_URL = "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"

BLOCK_SIZE = 1 << 17   # data block size (128 KiB)
META_SIZE = 8192       # metadata block size
NO_TABLE = 0xFFFFFFFFFFFFFFFF
FLAG_NO_FRAGMENTS, FLAG_NO_XATTRS = 0x0010, 0x0200
DIR_TYPE, FILE_TYPE = 1, 2


class Meta:
    """Metadata stream: 8 KiB blocks, each compressed if that helps, behind a 16-bit length header (bit 15 =
    stored). An item's reference is (start of its block in the compressed stream, offset in the block); the start of
    the block being filled is already known, so references can be taken while writing."""

    def __init__(self):
        self.out = bytearray()
        self.buf = bytearray()

    def ref(self):
        return len(self.out), len(self.buf)

    def write(self, data):
        self.buf += data
        while len(self.buf) >= META_SIZE:
            self._flush(bytes(self.buf[:META_SIZE]))
            del self.buf[:META_SIZE]

    def _flush(self, block):
        c = zlib.compress(block, 9)
        if len(c) < len(block):
            self.out += struct.pack("<H", len(c)) + c
        else:
            self.out += struct.pack("<H", len(block) | 0x8000) + block

    def finish(self):
        if self.buf:
            self._flush(bytes(self.buf))
            self.buf = bytearray()
        return bytes(self.out)


class Node:
    def __init__(self, name, mode, data=None):
        self.name, self.mode, self.data = name, mode, data
        self.children = {} if data is None else None
        self.ino = 0
        self.ref = None   # (block start, offset) in the inode table

    @property
    def is_dir(self):
        return self.children is not None


def build_squashfs(root, mtime):
    """root: Node tree. Returns the filesystem image."""
    img = bytearray(96)   # superblock, filled in at the end
    # inode numbers top-down (a directory's inode records its parent's number), written bottom-up
    nodes = []
    def number(n):
        nodes.append(n)
        n.ino = len(nodes)
        if n.is_dir:
            for c in sorted(n.children.values(), key=lambda c: c.name):
                number(c)
    number(root)
    count = len(nodes)

    # file contents first: data blocks right after the superblock
    for n in nodes:
        if n.is_dir:
            continue
        n.start = len(img)
        n.sizes = []
        for i in range(0, len(n.data), BLOCK_SIZE):
            block = n.data[i:i + BLOCK_SIZE]
            c = zlib.compress(block, 9)
            if len(c) < len(block):
                img += c
                n.sizes.append(len(c))
            else:
                img += block
                n.sizes.append(len(block) | 0x1000000)   # stored
    inodes, dirs = Meta(), Meta()

    def header(type_, n):
        return struct.pack("<HHHHII", type_, n.mode & 0o7777, 0, 0, mtime, n.ino)

    def write(n, parent_ino):
        if not n.is_dir:
            n.ref = inodes.ref()
            inodes.write(header(FILE_TYPE, n) + struct.pack("<IIII", n.start, 0xFFFFFFFF, 0, len(n.data)) +
                         struct.pack("<%dI" % len(n.sizes), *n.sizes))
            return
        kids = sorted(n.children.values(), key=lambda c: c.name)
        for c in kids:
            write(c, n.ino)
        # listing: runs of entries whose inodes share one inode-table block (at most 256 per run)
        listing = bytearray()
        i = 0
        while i < len(kids):
            start = kids[i].ref[0]
            run = [kids[i]]
            while (i + len(run) < len(kids) and len(run) < 256 and kids[i + len(run)].ref[0] == start and
                   abs(kids[i + len(run)].ino - kids[i].ino) < 32767):
                run.append(kids[i + len(run)])
            listing += struct.pack("<III", len(run) - 1, start, kids[i].ino)
            for c in run:
                name = c.name.encode("utf-8")
                listing += struct.pack("<HhHH", c.ref[1], c.ino - kids[i].ino, DIR_TYPE if c.is_dir else FILE_TYPE,
                                       len(name) - 1) + name
            i += len(run)
        dref = dirs.ref()
        dirs.write(bytes(listing))
        subdirs = sum(1 for c in kids if c.is_dir)
        if len(listing) + 3 > 0xFFFF:
            raise ValueError("directory too large for a basic directory inode: " + n.name)
        n.ref = inodes.ref()
        inodes.write(header(DIR_TYPE, n) + struct.pack("<IIHHI", dref[0], 2 + subdirs, len(listing) + 3, dref[1],
                                                       parent_ino))

    write(root, count + 1)   # (the root's parent is the inode count + 1, as mksquashfs writes it)
    inode_table = len(img)
    img += inodes.finish()
    dir_table = len(img)
    img += dirs.finish()
    frag_table = len(img)   # (no fragments: an empty table)
    ids = Meta()
    ids.write(struct.pack("<I", 0))   # one id: 0 (root), for every file's uid and gid
    id_block = len(img)
    img += ids.finish()
    id_table = len(img)
    img += struct.pack("<Q", id_block)
    root_ref = root.ref[0] << 16 | root.ref[1]
    struct.pack_into("<IIIIIHHHHHHQQQQQQQQ", img, 0,
                     0x73717368, count, mtime, BLOCK_SIZE, 0, 1, BLOCK_SIZE.bit_length() - 1,
                     FLAG_NO_FRAGMENTS | FLAG_NO_XATTRS, 1, 4, 0,
                     root_ref, len(img), id_table, NO_TABLE, inode_table, dir_table, frag_table, NO_TABLE)
    img += b"\0" * (-len(img) % 4096)
    return bytes(img)


def add(root, path, data, mode):
    parts = path.split("/")
    d = root
    for p in parts[:-1]:
        d = d.children.setdefault(p, Node(p, 0o755))
    d.children[parts[-1]] = Node(parts[-1], mode, data)


def runtime(path):
    if path:
        return open(path, "rb").read()
    cache = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party", "appimage-runtime",
                         "runtime-x86_64")
    if not os.path.isfile(cache):
        print(f"downloading the AppImage runtime: {RUNTIME_URL}")
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with urllib.request.urlopen(RUNTIME_URL, timeout=120) as r:
            data = r.read()
        if data[:4] != b"\x7fELF":
            sys.exit("the downloaded runtime is not an ELF file")
        open(cache, "wb").write(data)
    data = open(cache, "rb").read()
    print(f"runtime: {cache} (sha256 {hashlib.sha256(data).hexdigest()[:16]}...)")
    return data


def stripped(path):
    """The executable without debug information (zig builds carry ~200 MB of it), when a strip tool is found:
    $STRIP, llvm-strip, strip, or llvm-mingw's llvm-strip under C:/Tools. build-linux/f1ce itself keeps it."""
    import glob
    import shutil
    import subprocess
    import tempfile
    tools = [os.environ.get("STRIP"), shutil.which("llvm-strip"), shutil.which("strip")]
    tools += sorted(glob.glob("C:/Tools/llvm-mingw-*/bin/llvm-strip.exe"), reverse=True)
    for tool in filter(None, tools):
        fd, tmp = tempfile.mkstemp(suffix=".f1ce")
        os.close(fd)
        try:
            if subprocess.run([tool, "-o", tmp, path], capture_output=True).returncode == 0:
                data = open(tmp, "rb").read()
                if data[:4] == b"\x7fELF":
                    print(f"stripped with {tool}: {os.path.getsize(path) / 1e6:.0f} -> {len(data) / 1e6:.0f} MB")
                    return data
        except OSError:
            pass
        finally:
            os.remove(tmp)
    print("no strip tool found (set STRIP=...): packing the executable with its debug information")
    return open(path, "rb").read()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    top = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(top, "build-linux", "f1ce"))
    ap.add_argument("--out", default=os.path.join(top, "out", "F1CE-x86_64.AppImage"))
    ap.add_argument("--programs", default=os.path.join(top, "build-win", "shader_cache", "programs.bin"),
                    help="program database to include (missing: none)")
    ap.add_argument("--no-programs", action="store_true", help="leave the program database out")
    ap.add_argument("--icon", default=os.path.join(os.path.dirname(top), "icon.png"))
    ap.add_argument("--runtime", help="AppImage type 2 runtime to use instead of the downloaded one")
    ap.add_argument("--no-strip", action="store_true", help="keep the debug information (gdb backtraces)")
    args = ap.parse_args()

    with open(args.exe, "rb") as f:
        if f.read(4) != b"\x7fELF":
            sys.exit(f"{args.exe} is not a Linux executable")
    exe = stripped(args.exe) if not args.no_strip else open(args.exe, "rb").read()
    icon = open(args.icon, "rb").read() if os.path.isfile(args.icon) else open(os.path.join(here, "f1ce-recomp.png"), "rb").read()
    apprun = open(os.path.join(here, "AppRun"), "rb").read().replace(b"\r\n", b"\n")
    desktop = open(os.path.join(here, "f1ce-recomp.desktop"), "rb").read().replace(b"\r\n", b"\n")

    root = Node("", 0o755)
    add(root, "AppRun", apprun, 0o755)
    add(root, "f1ce-recomp.desktop", desktop, 0o644)
    add(root, "f1ce-recomp.png", icon, 0o644)
    add(root, ".DirIcon", icon, 0o644)
    add(root, "usr/bin/f1ce", exe, 0o755)
    add(root, "usr/share/applications/f1ce-recomp.desktop", desktop, 0o644)
    add(root, "usr/share/icons/hicolor/512x512/apps/f1ce-recomp.png", icon, 0o644)
    if not args.no_programs and os.path.isfile(args.programs):
        add(root, "usr/share/f1ce-recomp/programs.bin", open(args.programs, "rb").read(), 0o644)
        print(f"program database: {args.programs}")
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))

    t = time.time()
    fs = build_squashfs(root, mtime)
    image = runtime(args.runtime) + fs
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)
    try:
        os.chmod(args.out, os.stat(args.out).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass
    tgz = os.path.splitext(args.out)[0] + ".tar.gz"
    with tarfile.open(tgz, "w:gz") as tar:
        info = tarfile.TarInfo(os.path.basename(args.out))
        info.size, info.mode, info.mtime = len(image), 0o755, mtime
        tar.addfile(info, io.BytesIO(image))
    print(f"{args.out}: {len(image) / 1e6:.1f} MB ({len(fs) / 1e6:.1f} MB filesystem, {time.time() - t:.0f} s)")
    print(f"{tgz}: the same with its executable bit, for copying from Windows")


if __name__ == "__main__":
    main()
