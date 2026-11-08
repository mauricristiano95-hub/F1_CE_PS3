#!/usr/bin/env python3
"""Minimal pkg-config for the zig cross build: reads the .pc files of third_party/linux-sysroot (tools/linux_sysroot.py)
and prefixes their paths with the sysroot. Covers what CMake's FindPkgConfig asks: --version, --exists, --modversion,
--variable=, --cflags/--libs and their -only- variants, version specs (name >= x). Called through cmake/pkg-config.cmd.
"""
import os
import re
import sys

ROOT = os.environ.get("F1CE_LINUX_SYSROOT") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party", "linux-sysroot")
PC_DIRS = [os.path.join(ROOT, "usr", "lib", "x86_64-linux-gnu", "pkgconfig"), os.path.join(ROOT, "usr", "share", "pkgconfig")]
SYSROOT = ROOT.replace("\\", "/")


def load(name):
    for d in PC_DIRS:
        p = os.path.join(d, name + ".pc")
        if os.path.isfile(p):
            break
    else:
        return None
    vars_, fields = {"pc_sysrootdir": SYSROOT}, {}
    def expand(v):
        for _ in range(10):
            v2 = re.sub(r"\$\{(\w+)\}", lambda m: vars_.get(m.group(1), ""), v)
            if v2 == v:
                break
            v = v2
        return v
    for line in open(p, encoding="utf-8", errors="replace"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^([\w.]+)\s*=\s*(.*)$", line)
        if m:
            vars_[m.group(1)] = expand(m.group(2))
            continue
        m = re.match(r"^([\w.]+)\s*:\s*(.*)$", line)
        if m:
            fields[m.group(1)] = expand(m.group(2))
    return {"vars": vars_, "fields": fields}


def vkey(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.\-+~]", v)]


def cmp_ok(have, op, want):
    a, b = vkey(have), vkey(want)
    try:
        return {">=": a >= b, "<=": a <= b, ">": a > b, "<": a < b, "=": a == b, "==": a == b, "!=": a != b}[op]
    except TypeError:
        return True


def parse_specs(words):
    specs, i = [], 0
    toks = re.findall(r">=|<=|!=|==|[<>=]|[^\s,<>=!]+", " ".join(words))
    while i < len(toks):
        name, op, ver = toks[i], None, None
        if i + 2 < len(toks) + 0 and i + 1 < len(toks) and toks[i + 1] in (">=", "<=", "!=", "==", "<", ">", "="):
            op, ver = toks[i + 1], toks[i + 2]
            i += 3
        else:
            i += 1
        specs.append((name, op, ver))
    return specs


def closure(names, private):
    seen, order = set(), []
    def visit(n):
        if n in seen:
            return
        seen.add(n)
        pc = load(n)
        if pc is None:
            return
        order.append(pc)
        reqs = pc["fields"].get("Requires", "")
        if private:
            reqs += " " + pc["fields"].get("Requires.private", "")
        for rn, _, _ in parse_specs([reqs]):
            visit(rn)
    for n in names:
        visit(n)
    return order


def sysrooted(flag):
    for pre in ("-I", "-L"):
        if flag.startswith(pre) and flag[2:].startswith("/") and not flag[2:].startswith(SYSROOT):
            return pre + SYSROOT + flag[2:]
    return flag


def main():
    args = sys.argv[1:]
    opts = [a for a in args if a.startswith("--")]
    words = [a for a in args if not a.startswith("--")]
    if "--version" in opts:
        print("0.29.2")
        return 0
    specs = parse_specs(words)
    for name, op, ver in specs:
        pc = load(name)
        if pc is None or (op and not cmp_ok(pc["fields"].get("Version", "0"), op, ver)):
            if "--print-errors" in opts or "--short-errors" in opts:
                sys.stderr.write(f"Package '{name}' not found\n")
            return 1
    if "--exists" in opts:
        return 0
    names = [s[0] for s in specs]
    out = []
    for o in opts:
        if o.startswith("--variable="):
            pc = load(names[0])
            val = pc["vars"].get(o.split("=", 1)[1], "")
            if val.startswith("/") and o.split("=", 1)[1] in ("prefix", "exec_prefix", "libdir", "includedir"):
                val = SYSROOT + val
            print(val)
            return 0
    if "--modversion" in opts:
        for n in names:
            print(load(n)["fields"].get("Version", ""))
        return 0
    static = "--static" in opts
    pcs = closure(names, static)
    cflags = [sysrooted(f) for pc in pcs for f in pc["fields"].get("Cflags", "").split()]
    libs = []
    for pc in pcs:
        libs += pc["fields"].get("Libs", "").split()
        if static:
            libs += pc["fields"].get("Libs.private", "").split()
    libs = [sysrooted(f) for f in libs]
    def uniq(xs):
        seen, r = set(), []
        for x in xs:
            if x not in seen:
                seen.add(x)
                r.append(x)
        return r
    for o in opts:
        if o == "--cflags":
            out += cflags
        elif o == "--cflags-only-I":
            out += [f for f in cflags if f.startswith("-I")]
        elif o == "--cflags-only-other":
            out += [f for f in cflags if not f.startswith("-I")]
        elif o == "--libs":
            out += libs
        elif o == "--libs-only-L":
            out += [f for f in libs if f.startswith("-L")]
        elif o == "--libs-only-l":
            out += [f for f in libs if f.startswith("-l")]
        elif o == "--libs-only-other":
            out += [f for f in libs if not f.startswith(("-l", "-L"))]
    print(" ".join(uniq(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
