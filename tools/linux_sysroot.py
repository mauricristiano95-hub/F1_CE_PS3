#!/usr/bin/env python3
"""Linux headers and library stubs for the zig cross build of SDL3 (window, audio and input backends).

SDL3 loads X11, PulseAudio, ALSA, libudev and D-Bus at run time, but its build needs their headers, their .pc files and
the shared objects (to read the sonames it later loads). This script downloads those packages from Debian 10 "buster"
(glibc 2.28, the zig target), checks them against the archive's package index and unpacks the headers, .so files and
.pc files into third_party/linux-sysroot. Nothing from it is linked into f1ce.

Wayland is not included: SDL generates its protocol code with wayland-scanner, a Linux program. The cross build uses
X11 (XWayland on Wayland desktops and in the Steam Deck's gamescope); a native Linux build (linux/build.sh) has both.

Usage: python tools/linux_sysroot.py [--out third_party/linux-sysroot]
"""
import argparse
import hashlib
import io
import lzma
import os
import shutil
import sys
import tarfile
import urllib.request

MIRROR = "https://archive.debian.org/debian"
DIST = "buster"
PACKAGES = [
    "x11proto-dev", "libx11-dev", "libx11-6", "libxext-dev", "libxext6", "libxcursor-dev", "libxcursor1",
    "libxi-dev", "libxi6", "libxfixes-dev", "libxfixes3", "libxrandr-dev", "libxrandr2", "libxrender-dev",
    "libxrender1", "libxss-dev", "libxss1", "libxtst-dev", "libxtst6",
    "libpulse-dev", "libpulse0", "libasound2-dev", "libasound2", "libudev-dev", "libudev1",
    "libdbus-1-dev", "libdbus-1-3", "libxcb1-dev", "libxcb1", "libx11-xcb-dev", "libx11-xcb1",
]
# parts of each package that are kept (headers, shared objects, pkg-config files)
KEEP = ("usr/include/", "usr/lib/x86_64-linux-gnu/", "lib/x86_64-linux-gnu/", "usr/share/pkgconfig/")


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def package_index():
    raw = lzma.decompress(fetch(f"{MIRROR}/dists/{DIST}/main/binary-amd64/Packages.xz")).decode("utf-8", "replace")
    index = {}
    for stanza in raw.split("\n\n"):
        fields = {}
        for line in stanza.splitlines():
            if line and not line[0].isspace() and ":" in line:
                k, v = line.split(":", 1)
                fields[k] = v.strip()
        if "Package" in fields:
            index[fields["Package"]] = fields
    return index


def ar_members(data):
    if data[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")
    pos = 8
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[:16].decode().strip().rstrip("/")
        size = int(hdr[48:58].decode().strip())
        yield name, data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)


def unpack(deb, out, links):
    for name, body in ar_members(deb):
        if not name.startswith("data.tar"):
            continue
        with tarfile.open(fileobj=io.BytesIO(body), mode="r:*") as tar:
            for m in tar.getmembers():
                path = m.name[2:] if m.name.startswith("./") else m.name
                if not path.startswith(KEEP) or ".." in path.split("/"):
                    continue
                dest = os.path.join(out, *path.split("/"))
                if m.isdir():
                    os.makedirs(dest, exist_ok=True)
                elif m.issym():
                    links.append((path, m.linkname))   # resolved once every package is unpacked
                elif m.isfile():
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    with tar.extractfile(m) as src, open(dest, "wb") as f:
                        shutil.copyfileobj(src, f)
        return
    raise ValueError("no data.tar in package")


def resolve_links(out, links):
    # Symbolic links become copies of their targets (they need privileges on Windows); chains are resolved in passes.
    pending = list(links)
    while pending:
        left = []
        for path, target in pending:
            base = "" if target.startswith("/") else os.path.dirname(path)
            tpath = os.path.normpath(os.path.join(base, target.lstrip("/"))).replace("\\", "/")
            src = os.path.join(out, *tpath.split("/"))
            dest = os.path.join(out, *path.split("/"))
            if os.path.isfile(src):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(src, dest)
            elif any(p == tpath for p, _ in pending):
                left.append((path, target))
        if len(left) == len(pending):
            break   # links to files outside the kept parts (documentation and the like)
        pending = left


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--out", default=os.path.join(root, "third_party", "linux-sysroot"))
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    print(f"package index: {MIRROR} {DIST}")
    index = package_index()
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    links = []
    for pkg in PACKAGES:
        info = index.get(pkg)
        if not info:
            sys.exit(f"{pkg}: not in the package index")
        deb = fetch(f"{MIRROR}/{info['Filename']}")
        if hashlib.sha256(deb).hexdigest() != info["SHA256"]:
            sys.exit(f"{pkg}: checksum mismatch")
        print(f"  {pkg} {info['Version']}")
        unpack(deb, out, links)
    resolve_links(out, links)
    with open(os.path.join(out, "README.txt"), "w") as f:
        f.write(f"Generated by tools/linux_sysroot.py from Debian {DIST}: {' '.join(PACKAGES)}\n")
    print(f"sysroot ready: {out}")


if __name__ == "__main__":
    main()
