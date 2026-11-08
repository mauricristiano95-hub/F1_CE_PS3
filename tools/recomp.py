"""Static recompiler: translates every PPU function of EBOOT.elf into C++.

Output (in ../generated):
  ppu_funcs.h          declarations of every translated function
  ppu_NNN.cpp          function bodies (chunks)
  ppu_table.cpp        guest address -> host function table
  ppu_imports.cpp      import stub table (library, name, nid)
  recomp_report.txt    statistics, unresolved jump tables, unsupported instructions
"""
import bisect
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import ppc  # noqa: E402
import ps3elf  # noqa: E402
from functions import CODE_SECTIONS, discover  # noqa: E402
from ppc import (RA, RB, RC, RD, RS, SIMM, UIMM, DS, BO, BI, BD, LI, AA, LK, CRFD, CRFS, L, SH, MB, ME,  # noqa: E402
                 SH64, MB64, SPR, CRM, VD, VA, VB, VC, VSIMM, VUIMM, VSH, Rc)

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated"
CHUNK = 400
HOOKS = {0x3FB310: "camera_projection_hook",   # camera projection and frustum, see runtime/hle_aspect.cpp
         0x2F0D14: "cube_render_hook", 0x2F3B34: "cube_render2_hook", 0x2F0390: "cube_wait_hook",   # hle_cubemap.cpp
         0x2F141C: "skip_2f141c_hook", 0x2F1BEC: "skip_2f1bec_hook", 0x43F620: "skip_43f620_hook",   # hle_skipfn.cpp
         0x43FFA0: "skip_43ffa0_hook", 0x30388C: "skip_30388c_hook",
         0x48F88: "boot_language_hook", 0x1FE0EC: "boot_init_hook",   # hle_boot.cpp
         0x1F9EBC: "boot_attract_hook",
         0x11C150: "tyre_forces_hook", 0x117ED0: "wheel_update_hook",
         0x17B200: "car_tick_hook",   # hle_physics.cpp
         0x3574C0: "controller_update_hook"}   # hle_rumble.cpp
# debug probes: runtime code called with the context before the instruction at that address
PROBES = {0x1187EC: "probe_1187ec", 0x11B05C: "probe_11b05c",   # hle_physics.cpp
          0x489AAC: "flip_label_written",   # hle_gcm.cpp: the flip handler set a label the RSX may wait for
          0x48B054: "put_written",   # hle_gcm.cpp: libgcm wrote PUT
          0x55165C: "joblist_job", 0x5516F0: "joblist_job", 0x551780: "joblist_entry",   # hle_joblist.cpp
          0x5517F8: "joblist_entry", 0x551878: "joblist_wait", 0x5515D0: "joblist_reserve", 0x550F7C: "joblist_pad",
          0x48ACC8: "joblist_compact", 0x489CB8: "joblist_w0_drained", 0x48ACA8: "joblist_prev_done",
          0x5514A4: "joblist_push",
          0x551278: "jobwait_551228", 0x551354: "jobwait_55127c", 0x55140C: "jobwait_5513b8"}   # spu_pm.cpp: taken jobs
SPIN_LOOPS = {0x551228, 0x5513BC}   # the job manager's waits: they only poll counters the SPUs write

LOAD_INT = {  # mnemonic: (size, signed, update, indexed)
    "lbz": (1, 0, 0, 0), "lbzu": (1, 0, 1, 0), "lbzx": (1, 0, 0, 1), "lbzux": (1, 0, 1, 1),
    "lhz": (2, 0, 0, 0), "lhzu": (2, 0, 1, 0), "lhzx": (2, 0, 0, 1), "lhzux": (2, 0, 1, 1),
    "lha": (2, 1, 0, 0), "lhau": (2, 1, 1, 0), "lhax": (2, 1, 0, 1), "lhaux": (2, 1, 1, 1),
    "lwz": (4, 0, 0, 0), "lwzu": (4, 0, 1, 0), "lwzx": (4, 0, 0, 1), "lwzux": (4, 0, 1, 1),
    "lwa": (4, 1, 0, 0), "lwax": (4, 1, 0, 1), "lwaux": (4, 1, 1, 1),
    "ld": (8, 0, 0, 0), "ldu": (8, 0, 1, 0), "ldx": (8, 0, 0, 1), "ldux": (8, 0, 1, 1),
}
STORE_INT = {
    "stb": (1, 0, 0), "stbu": (1, 1, 0), "stbx": (1, 0, 1), "stbux": (1, 1, 1),
    "sth": (2, 0, 0), "sthu": (2, 1, 0), "sthx": (2, 0, 1), "sthux": (2, 1, 1),
    "stw": (4, 0, 0), "stwu": (4, 1, 0), "stwx": (4, 0, 1), "stwux": (4, 1, 1),
    "std": (8, 0, 0), "stdu": (8, 1, 0), "stdx": (8, 0, 1), "stdux": (8, 1, 1),
}
LOAD_FP = {"lfs": (4, 0, 0), "lfsu": (4, 1, 0), "lfsx": (4, 0, 1), "lfsux": (4, 1, 1),
           "lfd": (8, 0, 0), "lfdu": (8, 1, 0), "lfdx": (8, 0, 1), "lfdux": (8, 1, 1)}
STORE_FP = {"stfs": (4, 0, 0), "stfsu": (4, 1, 0), "stfsx": (4, 0, 1), "stfsux": (4, 1, 1),
            "stfd": (8, 0, 0), "stfdu": (8, 1, 0), "stfdx": (8, 0, 1), "stfdux": (8, 1, 1)}
NOPS = {"dcbt", "dcbst", "dcbf", "dcbtst", "icbi", "dcbi", "dst", "dstst", "dss"}
# Memory barriers must survive translation: other host threads (PPU threads, SPU interpreters, the RSX
# front end) observe guest memory, so the C++ compiler may not reorder accesses across them.
BARRIERS = {"sync": "PPU_SYNC();", "eieio": "PPU_EIEIO();", "isync": "PPU_EIEIO();"}

# VMX ops that map 1:1 onto a runtime helper taking register operands
VMX_VA_VB = {"vaddfp", "vsubfp", "vxor", "vor", "vand", "vandc", "vnor", "vmaxfp", "vminfp", "vslw", "vsrw",
             "vsraw", "vslh", "vsrh", "vsrah", "vrlw", "vrlb", "vrlh", "vadduwm", "vadduhm", "vaddubm",
             "vsubuwm", "vsubuhm", "vsububm", "vmrghw", "vmrglw", "vmrghh", "vmrglh", "vmrghb", "vmrglb",
             "vpkswss", "vpkshss", "vpkswus", "vpkshus", "vpkuwum", "vpkuhum", "vsumsws", "vmulouh",
             "vmuleuh", "vmulosh", "vmulesh", "vcmpeqfp", "vcmpgtfp", "vcmpgefp", "vcmpbfp", "vcmpequw",
             "vcmpequh", "vcmpequb", "vcmpgtsw", "vcmpgtuw", "vcmpgtsh", "vcmpgtuh", "vcmpgtsb", "vcmpgtub",
             "vaddsws", "vaddshs", "vsubsws", "vsubshs", "vmaxsw", "vminsw", "vmaxsh", "vminsh",
             "vavgub", "vslo", "vsro", "vsl", "vsr"}
VMX_VB = {"vrefp", "vrsqrtefp", "vexptefp", "vlogefp", "vrfin", "vrfiz", "vrfip", "vrfim",
          "vupkhsb", "vupkhsh", "vupklsb", "vupklsh"}
VMX_VA_VB_VC = {"vmaddfp", "vnmsubfp", "vsel", "vperm", "vmsumuhm", "vmsumshm", "vmhaddshs", "vmladduhm"}


def mask64(mb: int, me: int) -> int:
    """IBM-numbered mask with bits mb..me set (bit 0 = MSB), wrapping if mb > me."""
    def ones_from(b):            # bits b..63
        return (1 << (64 - b)) - 1
    if mb <= me:
        return ones_from(mb) & ~ones_from(me + 1) & 0xFFFFFFFFFFFFFFFF if me < 63 else ones_from(mb)
    return (ones_from(mb) | (~ones_from(me + 1) & 0xFFFFFFFFFFFFFFFF)) & 0xFFFFFFFFFFFFFFFF


def hx(v: int) -> str:
    return f"0x{v & 0xFFFFFFFFFFFFFFFF:X}ull"


class FuncGen:
    def __init__(self, rc, start, end):
        self.rc, self.start, self.end = rc, start, end
        self.elf = rc.elf
        self.labels = set()
        self.loop_heads = set()   # targets of backward branches: guest memory must be read again (spin loops)
        self.cur = self.start
        self.lines = []
        self.dispatch_all = False

    # ---------------------------------------------------------- helpers
    def ra0(self, w):
        return "0" if RA(w) == 0 else f"c.r[{RA(w)}]"

    def ea_d(self, w, disp):
        return f"(u32)({self.ra0(w)} + {disp})" if RA(w) else f"(u32){disp & 0xFFFFFFFF}u"

    def ea_x(self, w):
        return f"(u32)({self.ra0(w)} + c.r[{RB(w)}])" if RA(w) else f"(u32)c.r[{RB(w)}]"

    def cr0(self, expr):
        return f"set_cr_s(c, 0, (s64)({expr}));"

    def in_func(self, t):
        return self.start <= t < self.end

    def call_target(self, t, ret_addr):
        name = self.rc.func_name(t)
        if name is None:
            self.rc.report["bad call target"].append(f"{t:#x}")
            return f"call_indirect(c, 0x{t:X});"
        return f"c.lr = 0x{ret_addr:X}; {name}(c); c.cur_fn = 0x{self.start:X};"

    def jump(self, t):
        """Unconditional transfer to t (no link)."""
        if self.in_func(t):
            self.labels.add(t)
            if t <= self.cur:
                self.loop_heads.add(t)
            return f"goto L_{t:X};"
        name = self.rc.func_name(t)
        if name is None:
            self.rc.report["bad jump target"].append(f"{t:#x}")
            return f"call_indirect(c, 0x{t:X}); return;"
        return f"{name}(c); return;"

    def bc_cond(self, bo, bi):
        pre, parts = "", []
        if not bo & 4:
            pre = "c.ctr--; "
            parts.append("c.ctr == 0" if bo & 2 else "c.ctr != 0")
        if not bo & 16:
            parts.append(f"c.cr[{bi}]" if bo & 8 else f"!c.cr[{bi}]")
        return pre, (" && ".join(parts) if parts else "true")

    # ---------------------------------------------------------- body
    def generate(self):
        body = []
        a = self.start
        last_m = None
        while a < self.end:
            w = self.elf.u32(a)
            m = ppc.decode(w)
            self.cur = a
            code = self.translate(a, w, m) if m else None
            if code is None:
                if m is None:
                    code = f"trap_invalid(c, 0x{a:X}, 0x{w:08X}u); return;"
                else:
                    self.rc.report["unsupported"][m] += 1
                    code = f"trap_unsupported(c, 0x{a:X}, \"{m}\"); return;"
            if m and (m in LOAD_INT or m in STORE_INT or m in LOAD_FP or m in STORE_FP or m.startswith(("lv", "stv"))
                      or (m in ("b", "bc", "bcctr", "bclr") and LK(w))):
                code = f"PPU_PC(0x{a:X}); {code}"
            body.append((a, code, m))
            last_m = (m, w)
            a += 4
        # fall-through into the next function
        tail = None
        m, w = last_m
        ends_flow = m in ("b",) and not LK(w) or (m == "bclr" and BO(w) & 0x14 == 0x14) or \
            (m == "bcctr" and BO(w) & 0x14 == 0x14 and not LK(w)) or (m is None)
        if not ends_flow:
            nxt = self.rc.func_name(self.end)
            tail = f"{nxt}(c); return;" if nxt else "return;"

        out = [f"PPU_FUNC({self.rc.func_name(self.start)}) {{", f"\tc.cur_fn = 0x{self.start:X};"]
        if self.start in HOOKS:   # runtime code that may take the function over
            out.insert(0, f"bool {HOOKS[self.start]}(PPUContext& c);")
            out.append(f"\tif ({HOOKS[self.start]}(c)) return;")
        for pa in sorted(PROBES):
            if self.start <= pa < self.end:
                out.insert(0, f"void {PROBES[pa]}(PPUContext& c);")
        for a, code, m in body:
            if a in self.labels or self.dispatch_all:
                out.append(f"L_{a:X}:")
            if a in SPIN_LOOPS:
                out.append("\tPPU_SPIN();")
            elif a in self.loop_heads or self.dispatch_all:
                out.append("\tPPU_LOOP();")
            if a in PROBES:
                out.append(f"\t{PROBES[a]}(c);")
            out.append(f"\t{code}")
        if tail:
            out.append(f"\t{tail}")
        out.append("}")
        return "\n".join(out)

    # ---------------------------------------------------------- translation
    def translate(self, a, w, m):
        d, s, ra, rb = RD(w), RS(w), RA(w), RB(w)
        r = lambda n: f"c.r[{n}]"  # noqa: E731
        rc_ = Rc(w)

        # ---- integer arithmetic (immediate)
        if m == "addi":
            return f"c.r[{d}] = {self.ra0(w)} + {hx(SIMM(w))};"
        if m == "addis":
            return f"c.r[{d}] = {self.ra0(w)} + {hx(SIMM(w) << 16)};"
        if m in ("addic", "addic."):
            code = f"{{ u64 x = {r(ra)}; c.r[{d}] = x + {hx(SIMM(w))}; c.xer_ca = c.r[{d}] < x; }}"
            return code + (self.cr0(r(d)) if m == "addic." else "")
        if m == "subfic":
            return f"{{ u64 x = {r(ra)}; c.r[{d}] = {hx(SIMM(w))} - x; c.xer_ca = x <= {hx(SIMM(w))}; }}"
        if m == "mulli":
            return f"c.r[{d}] = (u64)((s64){r(ra)} * {SIMM(w)}ll);"
        if m in ("ori", "oris", "xori", "xoris"):
            imm = UIMM(w) << (16 if m.endswith("s") else 0)
            op = "|" if m.startswith("o") else "^"
            if m == "ori" and s == ra and imm == 0:
                return ";"
            return f"c.r[{ra}] = {r(s)} {op} {hx(imm)};"
        if m in ("andi.", "andis."):
            imm = UIMM(w) << (16 if m == "andis." else 0)
            return f"c.r[{ra}] = {r(s)} & {hx(imm)}; " + self.cr0(r(ra))

        # ---- compares
        if m in ("cmpi", "cmp", "cmpli", "cmpl"):
            f = CRFD(w)
            other = hx(SIMM(w)) if m == "cmpi" else (hx(UIMM(w)) if m == "cmpli" else r(rb))
            if m in ("cmpi", "cmp"):
                if L(w):
                    return f"set_cr_cmp(c, {f}, (s64){r(ra)}, (s64){other});"
                return f"set_cr_cmp(c, {f}, (s64)(s32){r(ra)}, (s64)(s32){other});"
            if L(w):
                return f"set_cr_cmp(c, {f}, (u64){r(ra)}, (u64){other});"
            return f"set_cr_cmp(c, {f}, (u64)(u32){r(ra)}, (u64)(u32){other});"

        # ---- XO-form arithmetic
        if m in ppc.OP31_XO.values():
            if ppc.OE(w):
                return None
            A, B, D = r(ra), r(rb), f"c.r[{d}]"
            e = {
                "add": f"{D} = {A} + {B};",
                "subf": f"{D} = {B} - {A};",
                "neg": f"{D} = (u64)(-(s64){A});",
                "mullw": f"{D} = (u64)((s64)(s32){A} * (s64)(s32){B});",
                "mulld": f"{D} = (u64)((s64){A} * (s64){B});",
                "mulhw": f"{D} = (u64)(((s64)(s32){A} * (s64)(s32){B}) >> 32);",
                "mulhwu": f"{D} = (u64)(((u64)(u32){A} * (u64)(u32){B}) >> 32);",
                "mulhd": f"{D} = (u64)(((__int128)(s64){A} * (__int128)(s64){B}) >> 64);",
                "mulhdu": f"{D} = (u64)(((unsigned __int128){A} * (unsigned __int128){B}) >> 64);",
                "divw": f"{D} = (u64)(u32)ppu_divw((s32){A}, (s32){B});",
                "divwu": f"{D} = (u32){B} ? (u64)((u32){A} / (u32){B}) : 0;",
                "divd": f"{D} = (u64)ppu_divd((s64){A}, (s64){B});",
                "divdu": f"{D} = {B} ? {A} / {B} : 0;",
                "addc": f"{{ u64 x = {A}, y = {B}; {D} = x + y; c.xer_ca = {D} < x; }}",
                "subfc": f"{{ u64 x = {A}, y = {B}; {D} = y - x; c.xer_ca = y >= x; }}",
                "adde": f"{{ u64 x = {A}, y = {B}; u64 t = x + y; u8 c1 = t < x; {D} = t + c.xer_ca; c.xer_ca = c1 | ({D} < t); }}",
                "subfe": f"{{ u64 x = ~{A}, y = {B}; u64 t = x + y; u8 c1 = t < x; {D} = t + c.xer_ca; c.xer_ca = c1 | ({D} < t); }}",
                "addze": f"{{ u64 x = {A}; {D} = x + c.xer_ca; c.xer_ca = {D} < x; }}",
                "subfze": f"{{ u64 x = ~{A}; {D} = x + c.xer_ca; c.xer_ca = {D} < x; }}",
                "addme": f"{{ u64 x = {A}; {D} = x + c.xer_ca - 1; c.xer_ca = (x != 0) | c.xer_ca; }}",
                "subfme": f"{{ u64 x = ~{A}; {D} = x + c.xer_ca - 1; c.xer_ca = (x != 0) | c.xer_ca; }}",
            }[m]
            return e + (self.cr0(D) if rc_ else "")

        # ---- logical / shifts (X-form, result in RA)
        A, S, B = f"c.r[{ra}]", r(s), r(rb)
        logic = {
            "and": f"{A} = {S} & {B};", "or": f"{A} = {S} | {B};", "xor": f"{A} = {S} ^ {B};",
            "nor": f"{A} = ~({S} | {B});", "andc": f"{A} = {S} & ~{B};", "orc": f"{A} = {S} | ~{B};",
            "nand": f"{A} = ~({S} & {B});", "eqv": f"{A} = ~({S} ^ {B});",
            "extsb": f"{A} = (u64)(s64)(s8){S};", "extsh": f"{A} = (u64)(s64)(s16){S};",
            "extsw": f"{A} = (u64)(s64)(s32){S};",
            "cntlzw": f"{A} = clz32((u32){S});", "cntlzd": f"{A} = clz64({S});",
            "slw": f"{{ u64 n = {B} & 63; {A} = n > 31 ? 0 : (u64)(u32)((u32){S} << n); }}",
            "srw": f"{{ u64 n = {B} & 63; {A} = n > 31 ? 0 : (u64)((u32){S} >> n); }}",
            "sld": f"{{ u64 n = {B} & 127; {A} = n > 63 ? 0 : {S} << n; }}",
            "srd": f"{{ u64 n = {B} & 127; {A} = n > 63 ? 0 : {S} >> n; }}",
            "sraw": f"{{ s32 x = (s32){S}; u64 n = {B} & 63; if (n > 31) {{ {A} = (u64)(s64)(x >> 31); c.xer_ca = x < 0; }} "
                    f"else {{ {A} = (u64)(s64)(x >> n); c.xer_ca = x < 0 && (x & ((1u << n) - 1)); }} }}",
            "srad": f"{{ s64 x = (s64){S}; u64 n = {B} & 127; if (n > 63) {{ {A} = (u64)(x >> 63); c.xer_ca = x < 0; }} "
                    f"else {{ {A} = (u64)(x >> n); c.xer_ca = x < 0 && (n && ((u64)x << (64 - n))); }} }}",
        }
        if m in logic:
            if m == "or" and s == rb:
                code = f"{A} = {S};" if s != ra else ";"
            else:
                code = logic[m]
            return code + (self.cr0(A) if rc_ else "")
        if m == "srawi":
            n = SH(w)
            ca = f"x < 0 && (x & 0x{(1 << n) - 1:X})" if n else "0"
            return f"{{ s32 x = (s32){S}; {A} = (u64)(s64)(x >> {n}); c.xer_ca = {ca}; }}" + (self.cr0(A) if rc_ else "")
        if m == "sradi":
            n = SH64(w)
            ca = f"x < 0 && (x & 0x{(1 << n) - 1:X}ll)" if n else "0"
            return f"{{ s64 x = (s64){S}; {A} = (u64)(x >> {n}); c.xer_ca = {ca}; }}" + (self.cr0(A) if rc_ else "")

        # ---- rotates
        if m in ("rlwinm", "rlwimi", "rlwnm"):
            msk = mask64(MB(w) + 32, ME(w) + 32)
            rot = f"rotl32x((u32){S}, {SH(w)})" if m != "rlwnm" else f"rotl32x((u32){S}, (u32){B} & 31)"
            if m == "rlwimi":
                code = f"{A} = ({rot} & {hx(msk)}) | ({A} & {hx(~msk)});"
            else:
                code = f"{A} = {rot} & {hx(msk)};"
            return code + (self.cr0(A) if rc_ else "")
        if m in ("rldicl", "rldicr", "rldic", "rldimi"):
            sh, mb = SH64(w), MB64(w)
            msk = {"rldicl": mask64(mb, 63), "rldicr": mask64(0, mb), "rldic": mask64(mb, 63 - sh),
                   "rldimi": mask64(mb, 63 - sh)}[m]
            rot = f"rotl64({S}, {sh})" if sh else S
            if m == "rldimi":
                code = f"{A} = ({rot} & {hx(msk)}) | ({A} & {hx(~msk)});"
            else:
                code = f"{A} = {rot} & {hx(msk)};"
            return code + (self.cr0(A) if rc_ else "")
        if m in ("rldcl", "rldcr"):
            mb = MB64(w)
            msk = mask64(mb, 63) if m == "rldcl" else mask64(0, mb)
            return f"{A} = rotl64({S}, {B} & 63) & {hx(msk)};" + (self.cr0(A) if rc_ else "")

        # ---- loads / stores
        if m in LOAD_INT:
            size, signed, upd, idx = LOAD_INT[m]
            disp = DS(w) if m in ("ld", "ldu", "lwa") else SIMM(w)
            ea = self.ea_x(w) if idx else self.ea_d(w, disp)
            fn = {1: "ld8", 2: "ld16", 4: "ld32", 8: "ld64"}[size]
            cast = {1: "s8", 2: "s16", 4: "s32"}.get(size) if signed else None
            val = f"(u64)(s64)({cast}){fn}(ea)" if cast else f"(u64){fn}(ea)"
            return f"{{ u32 ea = {ea}; c.r[{d}] = {val};" + (f" c.r[{ra}] = ea;" if upd else "") + " }"
        if m in STORE_INT:
            size, upd, idx = STORE_INT[m]
            disp = DS(w) if m in ("std", "stdu") else SIMM(w)
            ea = self.ea_x(w) if idx else self.ea_d(w, disp)
            fn = {1: "st8", 2: "st16", 4: "st32", 8: "st64"}[size]
            cast = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}[size]
            return f"{{ u32 ea = {ea}; {fn}(ea, ({cast}){r(s)});" + (f" c.r[{ra}] = ea;" if upd else "") + " }"
        if m in LOAD_FP:
            size, upd, idx = LOAD_FP[m]
            ea = self.ea_x(w) if idx else self.ea_d(w, SIMM(w))
            val = f"c.f[{d}].d = (double)ldf32(ea);" if size == 4 else f"c.f[{d}].u = ld64(ea);"
            return f"{{ u32 ea = {ea}; {val}" + (f" c.r[{ra}] = ea;" if upd else "") + " }"
        if m in STORE_FP:
            size, upd, idx = STORE_FP[m]
            ea = self.ea_x(w) if idx else self.ea_d(w, SIMM(w))
            val = f"stf32(ea, (float)c.f[{s}].d);" if size == 4 else f"st64(ea, c.f[{s}].u);"
            return f"{{ u32 ea = {ea}; {val}" + (f" c.r[{ra}] = ea;" if upd else "") + " }"
        if m == "stfiwx":
            return f"st32({self.ea_x(w)}, (u32)c.f[{s}].u);"
        if m in ("lwbrx", "lhbrx", "ldbrx"):
            fn = {"lwbrx": "ld32le", "lhbrx": "ld16le", "ldbrx": "ld64le"}[m]
            return f"c.r[{d}] = (u64){fn}({self.ea_x(w)});"
        if m in ("stwbrx", "sthbrx", "stdbrx"):
            fn, cast = {"stwbrx": ("st32le", "u32"), "sthbrx": ("st16le", "u16"), "stdbrx": ("st64le", "u64")}[m]
            return f"{fn}({self.ea_x(w)}, ({cast}){r(s)});"
        if m == "lwarx":
            return f"c.r[{d}] = ppu_lwarx(c, {self.ea_x(w)});"
        if m == "ldarx":
            return f"c.r[{d}] = ppu_ldarx(c, {self.ea_x(w)});"
        if m == "stwcx.":
            return f"ppu_stwcx(c, {self.ea_x(w)}, (u32){r(s)});"
        if m == "stdcx.":
            return f"ppu_stdcx(c, {self.ea_x(w)}, {r(s)});"
        if m in NOPS:
            return ";"
        if m in BARRIERS:
            return BARRIERS[m]
        if m == "dcbz":
            return f"ppu_dcbz({self.ea_x(w)});"

        # ---- special registers / CR
        if m == "mfspr":
            spr = SPR(w)
            src = {1: "ppu_get_xer(c)", 8: "c.lr", 9: "c.ctr", 256: "c.vrsave"}.get(spr)
            if src is None:
                return f"trap_unsupported(c, 0x{a:X}, \"mfspr {spr}\"); return;"
            return f"c.r[{d}] = {src};"
        if m == "mtspr":
            spr = SPR(w)
            dst = {1: f"ppu_set_xer(c, {r(s)});", 8: f"c.lr = {r(s)};", 9: f"c.ctr = {r(s)};",
                   256: f"c.vrsave = (u32){r(s)};"}.get(spr)
            return dst or f"trap_unsupported(c, 0x{a:X}, \"mtspr {spr}\"); return;"
        if m == "mftb":
            return f"c.r[{d}] = ppu_timebase();"
        if m == "mfcr":
            if w & (1 << 20):    # mfocrf: only the selected field is defined, full value is fine
                pass
            return f"c.r[{d}] = ppu_get_cr(c);"
        if m == "mtcrf":
            return f"ppu_set_cr(c, (u32){r(s)}, 0x{CRM(w):02X});"
        if m == "mcrf":
            return f"ppu_mcrf(c, {CRFD(w)}, {CRFS(w)});"
        cr_ops = {"crnor": "!(A | B)", "crxor": "A ^ B", "cror": "A | B", "crandc": "A & !B", "crand": "A & B",
                  "crnand": "!(A & B)", "creqv": "!(A ^ B)", "crorc": "A | !B"}
        if m in cr_ops:
            e = cr_ops[m].replace("A", f"c.cr[{ra}]").replace("B", f"c.cr[{rb}]")
            return f"c.cr[{d}] = (u8)({e});"

        # ---- traps / syscalls
        if m in ("tw", "twi", "td", "tdi"):
            to = d
            wide = m.startswith("td")
            x = f"(s64){r(ra)}" if wide else f"(s64)(s32){r(ra)}"
            y = (hx(SIMM(w)) if m.endswith("i") else r(rb))
            y = f"(s64){y}" if wide else f"(s64)(s32){y}"
            return f"if (ppu_trap_cond({to}, {x}, {y})) {{ trap_hit(c, 0x{a:X}); return; }}"
        if m == "sc":
            return f"c.pc = 0x{a:X}; ppu_syscall(c);"

        # ---- branches
        if m == "b":
            t = (LI(w) if AA(w) else a + LI(w)) & 0xFFFFFFFF
            if LK(w):
                return self.call_target(t, a + 4) if t != a + 4 else f"c.lr = 0x{a + 4:X};"
            return self.jump(t)
        if m == "bc":
            t = (BD(w) if AA(w) else a + BD(w)) & 0xFFFFFFFF
            pre, cond = self.bc_cond(BO(w), BI(w))
            if LK(w):
                if t == a + 4:
                    return f"{pre}c.lr = 0x{a + 4:X};"
                return f"{pre}if ({cond}) {{ {self.call_target(t, a + 4)} }}"
            return f"{pre}if ({cond}) {{ {self.jump(t)} }}"
        if m == "bclr":
            pre, cond = self.bc_cond(BO(w), BI(w))
            if LK(w):
                return f"{pre}if ({cond}) {{ u32 t = (u32)c.lr; c.lr = 0x{a + 4:X}; call_indirect(c, t); }}"
            return f"{pre}if ({cond}) return;"
        if m == "bcctr":
            pre, cond = self.bc_cond(BO(w) | 4, BI(w))  # bcctr never decrements CTR
            if LK(w):
                return f"if ({cond}) {{ c.lr = 0x{a + 4:X}; call_indirect(c, (u32)c.ctr); c.cur_fn = 0x{self.start:X}; }}"
            return f"if ({cond}) {{ {self.switch_jump(a)} }}"

        # ---- floating point
        fd, fa, fb, fc = d, ra, rb, RC(w)
        F = lambda n: f"c.f[{n}].d"  # noqa: E731
        fp_a = {
            "fadd": f"{F(fa)} + {F(fb)}", "fsub": f"{F(fa)} - {F(fb)}", "fmul": f"{F(fa)} * {F(fc)}",
            "fdiv": f"{F(fa)} / {F(fb)}", "fmadd": f"std::fma({F(fa)}, {F(fc)}, {F(fb)})",
            "fmsub": f"std::fma({F(fa)}, {F(fc)}, -{F(fb)})", "fnmadd": f"-std::fma({F(fa)}, {F(fc)}, {F(fb)})",
            "fnmsub": f"-std::fma({F(fa)}, {F(fc)}, -{F(fb)})", "fsqrt": f"std::sqrt({F(fb)})",
            "fre": f"1.0 / {F(fb)}", "frsqrte": f"1.0 / std::sqrt({F(fb)})",
            "fsel": f"({F(fa)} >= 0.0) ? {F(fc)} : {F(fb)}",
        }
        if m in fp_a:
            return f"{F(fd)} = {fp_a[m]};" + self.fp_rc(rc_)
        if m in ("fadds", "fsubs", "fmuls", "fdivs", "fmadds", "fmsubs", "fnmadds", "fnmsubs", "fsqrts", "fres",
                 "frsqrtes"):
            base = m[:-1]
            return f"{F(fd)} = (double)(float)({fp_a[base]});" + self.fp_rc(rc_)
        fp_x = {
            "fmr": f"c.f[{fd}].u = c.f[{fb}].u;",
            "fneg": f"c.f[{fd}].u = c.f[{fb}].u ^ 0x8000000000000000ull;",
            "fabs": f"c.f[{fd}].u = c.f[{fb}].u & 0x7FFFFFFFFFFFFFFFull;",
            "fnabs": f"c.f[{fd}].u = c.f[{fb}].u | 0x8000000000000000ull;",
            "frsp": f"{F(fd)} = (double)(float){F(fb)};",
            "fctiwz": f"c.f[{fd}].u = (u64)(u32)ppu_fctiw({F(fb)}, true);",
            "fctiw": f"c.f[{fd}].u = (u64)(u32)ppu_fctiw({F(fb)}, false);",
            "fctidz": f"c.f[{fd}].u = (u64)ppu_fctid({F(fb)}, true);",
            "fctid": f"c.f[{fd}].u = (u64)ppu_fctid({F(fb)}, false);",
            "fcfid": f"{F(fd)} = (double)(s64)c.f[{fb}].u;",
            "mffs": f"c.f[{fd}].u = c.fpscr;",
            "mtfsf": f"c.fpscr = ppu_mtfsf(c.fpscr, (u32)c.f[{fb}].u, 0x{ppc.FM(w):02X});",
            "mtfsb0": f"c.fpscr &= ~(0x80000000u >> {fd});", "mtfsb1": f"c.fpscr |= (0x80000000u >> {fd});",
            "mtfsfi": f"c.fpscr = ppu_mtfsfi(c.fpscr, {CRFD(w)}, {(w >> 12) & 15});",
        }
        if m in fp_x:
            return fp_x[m] + self.fp_rc(rc_)
        if m in ("fcmpu", "fcmpo"):
            return f"set_cr_fcmp(c, {CRFD(w)}, {F(fa)}, {F(fb)});"
        if m == "mcrfs":
            return f"ppu_mcrfs(c, {CRFD(w)}, {CRFS(w)});"

        # ---- VMX
        V = lambda n: f"c.v[{n}]"  # noqa: E731
        vd, va, vb, vc = VD(w), VA(w), VB(w), VC(w)
        if m in ("lvx", "lvxl"):
            return f"vmx::lvx({V(vd)}, {self.ea_x(w)});"
        if m in ("stvx", "stvxl"):
            return f"vmx::stvx({V(vd)}, {self.ea_x(w)});"
        if m in ("lvsl", "lvsr", "lvebx", "lvehx", "lvewx", "stvebx", "stvehx", "stvewx", "lvlx", "lvrx",
                 "stvlx", "stvrx"):
            return f"vmx::{m}({V(vd)}, {self.ea_x(w)});"
        base = m.rstrip(".")
        rec = m.endswith(".")
        if base in VMX_VA_VB:
            if base.startswith("vcmp"):
                return f"vmx::{base}({V(vd)}, {V(va)}, {V(vb)});" + (f" vmx::set_cr6(c, {V(vd)});" if rec else "")
            return f"vmx::{base}({V(vd)}, {V(va)}, {V(vb)});"
        if base in VMX_VB:
            return f"vmx::{base}({V(vd)}, {V(vb)});"
        if base in VMX_VA_VB_VC:
            return f"vmx::{base}({V(vd)}, {V(va)}, {V(vb)}, {V(vc)});"
        if base == "vsldoi":
            return f"vmx::vsldoi({V(vd)}, {V(va)}, {V(vb)}, {VSH(w)});"
        if base in ("vspltw", "vsplth", "vspltb"):
            return f"vmx::{base}({V(vd)}, {V(vb)}, {VUIMM(w)});"
        if base in ("vspltisw", "vspltish", "vspltisb"):
            return f"vmx::{base}({V(vd)}, {VSIMM(w)});"
        if base in ("vcfsx", "vcfux", "vctsxs", "vctuxs"):
            return f"vmx::{base}({V(vd)}, {V(vb)}, {VUIMM(w)});"
        if base == "mfvscr":
            return f"vmx::mfvscr(c, {V(vd)});"
        if base == "mtvscr":
            return f"vmx::mtvscr(c, {V(vb)});"
        return None

    def fp_rc(self, rc_):
        return " ppu_fp_rc(c);" if rc_ else ""

    # ---------------------------------------------------------- jump tables
    def switch_jump(self, a):
        targets = self.rc.jump_tables.get(a)
        if targets is not None:
            cases = []
            for t in sorted(set(targets)):
                if self.in_func(t):
                    self.labels.add(t)
                    if t <= self.cur:
                        self.loop_heads.add(t)
                    cases.append(f"case 0x{t:X}: goto L_{t:X};")
                else:
                    name = self.rc.func_name(t)
                    if name:
                        cases.append(f"case 0x{t:X}: {name}(c); return;")
            return f"switch ((u32)c.ctr) {{ {' '.join(cases)} default: call_indirect(c, (u32)c.ctr); return; }}"
        # indirect tail call (function pointer) or unresolved jump table
        if self.rc.looks_like_tail_call(a):
            return "call_indirect(c, (u32)c.ctr); return;"
        self.rc.report["unresolved bctr"].append(f"{a:#x} in {self.start:#x}")
        self.dispatch_all = True
        cases = " ".join(f"case 0x{t:X}: goto L_{t:X};" for t in range(self.start, self.end, 4))
        return f"switch ((u32)c.ctr) {{ {cases} default: call_indirect(c, (u32)c.ctr); return; }}"


class Recompiler:
    def __init__(self, elf):
        self.elf = elf
        self.report = {"unsupported": collections.Counter(), "unresolved bctr": [], "bad call target": [],
                       "bad jump target": [], "split": 0}
        funcs, self.stubs = discover(elf)
        self.starts = {s for s, _, _ in funcs}
        self.code_ranges = [(elf.section(i)[0], elf.section(i)[0] + elf.section(i)[2]) for i in CODE_SECTIONS]
        self.jump_tables = {}
        self.find_jump_tables()
        self.split_on_external_branches()
        self.build_bounds()

    # ---------------------------------------------------------- analysis
    def in_code(self, t):
        return any(lo <= t < hi for lo, hi in self.code_ranges)

    def func_name(self, t):
        if t in self.starts:
            return f"sub_{t:08X}"
        if t in self.stubs:
            return f"stub_{t:08X}"
        return None

    def find_jump_tables(self):
        """Resolve GCC-style switch tables preceding each bctr via a tiny constant tracker.

        Pattern: cmplwi crN,rI,max ; bgt ... ; (lis/addi | ld rT,x(r2)) ; slwi rX,rI,2 ; lwzx rO,rT,rX ;
                 add rO,rO,rT ; mtctr rO ; bctr        (entries are s32 offsets from the table base)
        """
        toc = self.elf.toc
        ordered = sorted(self.starts)
        for idx in CODE_SECTIONS:
            addr, off, size = self.elf.section(idx)
            for i in range(0, size, 4):
                a = addr + i
                w = self.elf.u32(a)
                if w != 0x4E800420:                 # bctr
                    continue
                window = [(b, self.elf.u32(b)) for b in range(max(addr, a - 4 * 24), a, 4)]
                regs = {2: toc}
                bound = None
                table = None
                # index register: source of the shift (slwi / sldi) that scales it before the lwzx
                idx_reg = None
                for b, x in window:
                    if ppc.decode(x) in ("rlwinm", "rldicr", "rldic") and SH(x) == 2:
                        idx_reg = RS(x)
                # the bounds check (cmplwi on the index register) sits in the switch header block, possibly far
                # before the bctr; without a known index register take the nearest cmplwi
                nearest = None
                for b in range(a - 4, max(addr, a - 4 * 256), -4):
                    x = self.elf.u32(b)
                    if ppc.decode(x) != "cmpli":
                        continue
                    if nearest is None and b >= a - 4 * 64:
                        nearest = UIMM(x)
                    if idx_reg is not None and RA(x) == idx_reg:
                        bound = UIMM(x)
                        break
                # a cmplwi on another register says nothing about this table (e.g. an index set by li in each
                # path): then the table is read up to the first entry that leaves the function
                trusted = bound is not None
                if bound is None and idx_reg is None:
                    bound, trusted = nearest, nearest is not None
                for b, x in window:
                    mm = ppc.decode(x)
                    if mm in ("addis", "addi"):
                        base = 0 if RA(x) == 0 else regs.get(RA(x))
                        imm = SIMM(x) << 16 if mm == "addis" else SIMM(x)
                        regs[RD(x)] = (base + imm) & 0xFFFFFFFF if base is not None else None
                    elif mm == "ori" and regs.get(RS(x)) is not None:
                        regs[RA(x)] = regs[RS(x)] | UIMM(x)
                    elif mm in ("ld", "lwz") and RA(x) in regs and regs[RA(x)] is not None:
                        ea = (regs[RA(x)] + (DS(x) if mm == "ld" else SIMM(x))) & 0xFFFFFFFF
                        o = self.elf.v2o(ea)
                        regs[RD(x)] = (struct.unpack(">Q", self.elf.data[o:o + 8])[0] & 0xFFFFFFFF if mm == "ld"
                                       else self.elf.u32(ea)) if o is not None else None
                    elif mm == "lwzx":
                        table = [v for v in (regs.get(RB(x)), regs.get(RA(x))) if v is not None] or None
                    elif mm is not None and mm not in ("rlwinm", "rldicr", "rldic", "rldicl", "add", "mtspr",
                                                       "bc", "extsw", "cmpi", "or"):
                        if RD(x) in regs and mm not in ("stw", "std", "stb", "sth", "stfs", "stfd"):
                            regs[RD(x)] = None
                if table is None:
                    continue
                # targets must stay inside the function enclosing the bctr
                k = bisect.bisect_right(ordered, a) - 1
                lo = ordered[k]
                hi = ordered[k + 1] if k + 1 < len(ordered) else a + 0x100000
                limit = bound + 1 if trusted and bound <= 4096 else 4096
                for base in table:
                    targets = []
                    for n in range(limit):
                        o = self.elf.v2o(base + 4 * n)
                        if o is None:
                            break
                        t = (base + struct.unpack(">i", self.elf.data[o:o + 4])[0]) & 0xFFFFFFFF
                        if not (lo <= t < hi) or t & 3:
                            break
                        targets.append(t)
                    if targets and (not trusted or len(targets) == bound + 1):
                        self.jump_tables[a] = targets
                        break

    def looks_like_tail_call(self, a):
        """bctr fed by a function-descriptor load (lwz rX,0(rY) ; lwz r2,4(rY) ; mtctr rX)."""
        for b in range(a - 4, a - 4 * 8, -4):
            x = self.elf.u32(b)
            if ppc.decode(x) in ("lwz", "ld") and RD(x) == 2:
                return True
        return False

    def split_on_external_branches(self):
        """Branches that land in the middle of another function create new function starts."""
        changed = True
        while changed:
            changed = False
            ordered = sorted(self.starts)
            import bisect
            new = set()
            for idx in CODE_SECTIONS:
                addr, off, size = self.elf.section(idx)
                for i in range(0, size, 4):
                    a = addr + i
                    w = self.elf.u32(a)
                    m = ppc.decode(w)
                    if m == "b" and not LK(w):
                        t = (LI(w) if AA(w) else a + LI(w)) & 0xFFFFFFFF
                    elif m == "bc" and not LK(w):
                        t = (BD(w) if AA(w) else a + BD(w)) & 0xFFFFFFFF
                    else:
                        continue
                    if not self.in_code(t) or t in self.starts:
                        continue
                    k = bisect.bisect_right(ordered, a) - 1
                    lo = ordered[k] if k >= 0 else 0
                    hi = ordered[k + 1] if k + 1 < len(ordered) else 1 << 32
                    if not (lo <= t < hi):
                        new.add(t)
            for a, targets in self.jump_tables.items():
                k = bisect.bisect_right(ordered, a) - 1
                lo = ordered[k]
                hi = ordered[k + 1] if k + 1 < len(ordered) else 1 << 32
                new.update(t for t in targets if not (lo <= t < hi) and t not in self.starts)
            if new:
                self.report["split"] += len(new)
                self.starts |= new
                changed = True

    def build_bounds(self):
        ordered = sorted(self.starts)
        self.bounds = []
        for i, a in enumerate(ordered):
            sec_end = next(hi for lo, hi in self.code_ranges if lo <= a < hi)
            end = min(ordered[i + 1] if i + 1 < len(ordered) else sec_end, sec_end)
            self.bounds.append((a, end))

    # ---------------------------------------------------------- output
    def run(self):
        OUT.mkdir(exist_ok=True)
        stale = {p.name for p in OUT.glob("ppu_*.cpp")}
        def write(name, text):   # unchanged files keep their time stamp (no rebuild)
            stale.discard(name)
            p = OUT / name
            if not p.exists() or p.read_text() != text:
                p.write_text(text)
        names = []
        chunk, n = [], 0
        for i, (s, e) in enumerate(self.bounds):
            chunk.append(FuncGen(self, s, e).generate())
            names.append((s, f"sub_{s:08X}"))
            if len(chunk) == CHUNK or i == len(self.bounds) - 1:
                write(f"ppu_{n:03d}.cpp", '#include "ppu_funcs.h"\n\n' + "\n\n".join(chunk) + "\n")
                chunk, n = [], n + 1
        stub_names = []
        for s, imp in sorted(self.stubs.items()):
            stub_names.append((s, f"stub_{s:08X}", imp))

        decls = ['#pragma once', '#include "ppu_runtime.h"', ""]
        decls += [f"PPU_DECL({nm});" for _, nm in names]
        decls += [f"PPU_DECL({nm});" for _, nm, _ in stub_names]
        write("ppu_funcs.h", "\n".join(decls) + "\n")

        # stubs call into the HLE dispatcher by import index
        imp_index = {id(imp): i for i, imp in enumerate(self.elf.imports)}
        stub_src = ['#include "ppu_funcs.h"', ""]
        for s, nm, imp in stub_names:
            # the original stub saves the caller's TOC at 40(r1); callers reload r2 from there
            stub_src.append(f"PPU_FUNC({nm}) {{ st64((u32)c.r[1] + 40, c.r[2]); hle_call(c, {imp_index[id(imp)]}); }}")
        stub_src.append("")
        stub_src.append("const ImportInfo g_imports[] = {")
        for imp in self.elf.imports:
            stub_src.append(f'\t{{ "{imp.library}", "{imp.name}", 0x{imp.nid:08X}u, 0x{imp.stub_slot:X}u }},')
        stub_src.append("};")
        stub_src.append(f"const u32 g_import_count = {len(self.elf.imports)};")
        write("ppu_imports.cpp", "\n".join(stub_src) + "\n")

        table = ['#include "ppu_funcs.h"', "", "const FuncEntry g_func_table[] = {"]
        table += [f"\t{{ 0x{s:X}u, {nm} }}," for s, nm in names]
        table += [f"\t{{ 0x{s:X}u, {nm} }}," for s, nm, _ in stub_names]
        table.append("};")
        table.append(f"const u32 g_func_count = {len(names) + len(stub_names)};")
        write("ppu_table.cpp", "\n".join(table) + "\n")
        for name in stale:
            (OUT / name).unlink()

        rep = [f"functions: {len(self.bounds)} (+{len(stub_names)} import stubs), split starts added: {self.report['split']}",
               f"jump tables resolved: {len(self.jump_tables)}",
               f"unresolved bctr (full dispatch): {len(self.report['unresolved bctr'])}",
               f"bad call targets: {len(self.report['bad call target'])}, bad jump targets: {len(self.report['bad jump target'])}",
               f"unsupported instructions: {dict(self.report['unsupported'])}",
               "", "unresolved bctr sites:"] + self.report["unresolved bctr"][:200]
        (OUT / "recomp_report.txt").write_text("\n".join(rep) + "\n")
        print("\n".join(rep[:5]))
        print(f"wrote {n} chunk files")


if __name__ == "__main__":
    Recompiler(ps3elf.load(ROOT / "input" / "EBOOT.elf")).run()
