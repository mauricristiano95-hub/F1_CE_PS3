"""Disassembler for the front-end Lua chunks in offline.wad: python -I tools/luadis.py <wad> <chunk offset hex>
Container: "\\x1bLua" 0x51 0x01, sizes int/size_t/instr/number (4), float check; little endian.
Function: source, linedefined, nups/npar/vararg/maxstack bytes, lineinfo, locvars, upvalues, constants, protos, code.
Instructions use the Lua 5.0 layout: op bits 0-5, C 6-14, B 15-23, A 24-31; RK >= 250 is a constant."""
import struct, sys

OPS = ("MOVE LOADK LOADBOOL LOADNIL GETUPVAL GETGLOBAL GETTABLE SETGLOBAL SETUPVAL SETTABLE NEWTABLE SELF ADD SUB "
       "MUL DIV POW UNM NOT CONCAT JMP EQ LT LE TEST CALL TAILCALL RETURN FORLOOP TFORLOOP TFORPREP SETLIST SETLISTO "
       "CLOSE CLOSURE OP35 OP36 OP37 OP38 OP39").split()


class R:
    def __init__(s, d, p):
        assert d[p:p + 4] == b"\x1bLua" and d[p + 5] == 1
        s.d, s.p, s.funcs = d, p + 14, []

    def u(s, n=4):
        v = s.d[s.p:s.p + n]; s.p += n
        return int.from_bytes(v, "little")

    def string(s):
        n = s.u()
        v = s.d[s.p:s.p + n - 1] if n else None; s.p += n
        return v.decode("latin1") if v is not None else None

    def func(s, depth=0, name="main"):
        src = s.string(); ld = s.u()
        nups, npar, va, ms = s.d[s.p:s.p + 4]; s.p += 4
        lines = [s.u() for _ in range(s.u())]
        for _ in range(s.u()): s.string(); s.u(); s.u()
        ups = [s.string() for _ in range(s.u())]
        consts = []
        for _ in range(s.u()):
            t = s.d[s.p]; s.p += 1
            if t == 0: consts.append("nil")
            elif t == 1: consts.append(bool(s.u(1)))
            elif t == 3: consts.append(round(struct.unpack("<f", s.d[s.p:s.p + 4])[0], 4)); s.p += 4
            elif t == 4: consts.append(repr(s.string()))
            else: raise ValueError(f"const type {t} at {s.p:x}")
        f = dict(name=name, line=ld, npar=npar, k=consts, ups=ups, depth=depth)
        s.funcs.append(f)
        for i in range(s.u()): s.func(depth + 1, f"{name}/{i}")
        f["code"] = [s.u() for _ in range(s.u())]
        return f


def dis(f):
    ind = "  " * f["depth"]; k = f["k"]
    rk = lambda x: f"{k[x - 250]}" if x >= 250 else f"R{x}"  # noqa: E731
    print(f"{ind}== function {f['name']} line {f['line']} params {f['npar']} ups {f['ups']}")
    for pc, i in enumerate(f["code"]):
        o = i & 63; op = OPS[o] if o < len(OPS) else f"OP{o}"
        a = i >> 24; b = (i >> 15) & 511; c = (i >> 6) & 511; bx = (i >> 6) & 0x3FFFF; sbx = bx - 131071
        if op in ("LOADK", "GETGLOBAL", "SETGLOBAL"): t = f"R{a} {k[bx]}"
        elif op in ("JMP", "FORLOOP", "TFORPREP"): t = f"-> {pc + 1 + sbx}" + (f" R{a}" if op != "JMP" else "")
        elif op in ("GETTABLE", "SELF"): t = f"R{a} = R{b}[{rk(c)}]"
        elif op == "SETTABLE": t = f"R{a}[{rk(b)}] = {rk(c)}"
        elif op in ("ADD", "SUB", "MUL", "DIV", "POW"): t = f"R{a} = {rk(b)} {rk(c)}"
        elif op in ("EQ", "LT", "LE"): t = f"if ({rk(b)} {op} {rk(c)}) ~= {a} then pc++"
        elif op == "TEST": t = f"R{a} R{b} {c}  (if R{b} <=> {c} then R{a}=R{b} else pc++)"
        elif op == "CLOSURE": t = f"R{a} = proto {bx}"
        elif op in ("GETUPVAL", "SETUPVAL"): t = f"R{a} U{b}({f['ups'][b] if b < len(f['ups']) else '?'})"
        elif op == "LOADBOOL": t = f"R{a} = {bool(b)}" + (" pc++" if c else "")
        else: t = f"{a} {b} {c}"
        print(f"{ind}  {pc:3} {op:9} {t}")


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    r = R(d, int(sys.argv[2], 16)); r.func()
    for f in r.funcs: dis(f)
