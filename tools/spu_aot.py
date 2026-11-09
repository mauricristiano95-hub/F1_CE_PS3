"""Ahead-of-time translation of SPU basic blocks into C++.

The runtime (F1CE_SPU_DUMP_BLOCKS=<file>) records every basic block it decodes without a native version as
"pc w0 w1 ... wn" (hex). This tool turns the recorded blocks into straight-line C++ that calls the
interpreter's instruction handlers (runtime/spu_ops.h) with constant instruction words, so the compiler
inlines them and folds the decoding away. Output: generated/spu/spu_aot_<n>.cpp + spu_aot_table.cpp
(sorted by key = FNV-1a 64 of the start pc and the words, as computed in SPUThread::build_block).

usage: python -I tools/spu_aot.py <dump file> [<dump file> ...]
       (blocks already in the existing generated table are kept)
With SPU_AOT_HEAT=1 in the environment the code also counts the instructions run per block (shared counters, for
profiling only) and the game writes spu_heat.txt ("count pc block_key") to its working directory every 10 s.
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated" / "spu"
PER_FILE = 1500
HEAT = os.environ.get("SPU_AOT_HEAT") == "1"
HEAT_INDEX = {}   # block key -> counter index


# loops replaced in bulk at their head (see "idioms" in runtime/spu_ops.h): (pc, first word, length) -> helper
IDIOMS = {(0x04AE4, 0x38944402, 7): "spu_idiom_fill_word",
          (0x08910, 0x1801C385, 10): "spu_idiom_ramp_u16",
          (0x0E3F0, 0x38940282, 10): "spu_idiom_age_table",
          (0x05778, 0x76A6C303, 78): "spu_idiom_audio_voice"}


# optional shortcuts at a block's head: the helper returns true after setting pc to where to continue
SKIPS = {(0x0D7F8, 0x4020007F, 134): "spu_skip_triangle_cull",
         (0x0D628, 0x7A8FDCBF, 44): "spu_skip_vertex_transform",
         (0x04824, 0x18028502, 43): "spu_skip_tri_search",   # the rain job's triangle search
         # job manager policy module: functions rewritten natively (runtime/spu_pm.cpp)
         (0x019F8, 0x42810182, 16): "spu_skip_pm_buffers",
         (0x02D48, 0x1C000302, 15): "spu_skip_pm_dma_info",
         (0x02CE8, 0x42168C02, 4): "spu_skip_pm_release",
         (0x01670, 0x24FE0081, 18): "spu_skip_pm_job_done",
         (0x02288, 0x42004032, 36): "spu_skip_pm_grab",
         (0x00A2C, 0x00200000, 17): "spu_skip_pm_irq_enter",
         (0x00A98, 0x40867803, 11): "spu_skip_pm_irq_leave"}


def skip_call(pc, words, ctx, leave):
    h = SKIPS.get((pc, words[0], len(words)))
    return f"if ({h}({ctx})) {leave}" if h else ""


def idiom_call(pc, words, ctx):
    h = IDIOMS.get((pc, words[0], len(words)))
    return f"{h}({ctx});" if h else ""


def heat(key, n):
    return f"g_aot_heat[{HEAT_INDEX[key]}] += {n};" if HEAT else ""

CONTROL = {"op_br", "op_bra", "op_brsl", "op_brasl", "op_brnz", "op_brz", "op_brhnz", "op_brhz", "op_bi", "op_iret",
           "op_bisl", "op_bisled", "op_biz", "op_binz", "op_bihz", "op_bihnz"}
# handlers that may halt the SPU or block on a channel: the generated code checks `halted` after them
MAY_HALT = {"op_stop", "op_stopd", "op_rdch", "op_wrch", "op_rchcnt", "op_invalid"}


def decode_table():
    """opcode index (w >> 21) -> handler name, parsed from build_table() in runtime/spu.cpp"""
    src = (ROOT / "runtime" / "spu.cpp").read_text()
    body = src[src.index("void build_table() {"):src.index("struct TableInit")]
    shift = {"rr": 0, "ri8": 1, "ri16": 2, "ri10": 3, "ri18": 4, "rrr": 7}
    table = ["op_invalid"] * 2048
    for kind, op, name in re.findall(r"\b(rr|ri8|ri16|ri10|ri18|rrr)\((0x[0-9A-Fa-f]+), (op_\w+)\)", body):
        k = shift[kind]
        base = int(op, 16) << k
        for i in range(1 << k):
            table[base | i] = name
            KIND[base | i] = kind
    return table


KIND = ["rr"] * 2048   # instruction format per opcode index (filled by decode_table)
# handlers that run on the thread itself in cluster code (channels, stop, halt): registers are synced around them
SYNC = MAY_HALT | {"op_heq", "op_heqi", "op_hgt", "op_hgti", "op_hlgt", "op_hlgti"}
NO_WRITE = {"op_stqd", "op_stqx", "op_stqa", "op_stqr", "op_wrch", "op_br", "op_bra", "op_brnz", "op_brz", "op_brhnz",
            "op_brhz", "op_bi", "op_iret", "op_biz", "op_binz", "op_bihz", "op_bihnz", "op_bisled", "op_nop", "op_stop",
            "op_stopd", "op_mtspr", "op_fscrwr", "op_invalid", "op_heq", "op_heqi", "op_hgt", "op_hgti", "op_hlgt",
            "op_hlgti"}


def regs_of(w, name):
    """(registers possibly read, registers possibly written) by one instruction (immediates in register fields
    only add harmless copies)"""
    kind = KIND[w >> 21]
    rt, ra, rb = w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F
    if kind == "rrr":
        reads, wr = {ra, rb, w & 0x7F}, (w >> 21) & 0x7F
    elif kind == "rr":
        reads, wr = {rt, ra, rb}, rt
    elif kind in ("ri8", "ri10"):
        reads, wr = {rt, ra}, rt
    else:
        reads, wr = {rt}, rt
    return reads, (set() if name in NO_WRITE else {wr})


def key_of(pc, words):
    h = 1469598103934665603
    for v in [pc & 0x3FFFC] + words:
        for k in range(4):
            h ^= (v >> (k * 8)) & 0xFF
            h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def load_blocks(paths):
    blocks = {}
    for p in paths:
        for line in Path(p).read_text().split("\n"):
            parts = line.split()
            if len(parts) < 2:
                continue
            pc = int(parts[0], 16)
            words = [int(x, 16) for x in parts[1:]]
            blocks[key_of(pc, words)] = (pc, words)
    return blocks


def existing_blocks():
    """blocks of a previous run of this tool (re-read from the generated sources' comments)"""
    blocks = {}
    for f in sorted(OUT.glob("spu_aot_[0-9]*.cpp")):
        for m in re.finditer(r"// block ([0-9a-f]+):((?: [0-9a-f]{8})+)", f.read_text()):
            pc = int(m.group(1), 16)
            words = [int(x, 16) for x in m.group(2).split()]
            blocks[key_of(pc, words)] = (pc, words)
    return blocks


RELATIVE = {"op_br", "op_brz", "op_brnz", "op_brhz", "op_brhnz"}
# pure blocks that branch back to their own start are emitted as loops (instead of one dispatch per iteration)
LOOP_UNSAFE = MAY_HALT | {"op_bisl", "op_bisled", "op_brsl", "op_brasl", "op_iret"}
LOOP_CAP = 256   # iterations per call, so the dispatcher still sees interrupts regularly


def branch_target(pc, w, h):
    i16 = (w >> 7) & 0xFFFF
    i16 -= (i16 & 0x8000) << 1
    if h in RELATIVE:
        return (pc + i16 * 4) & 0x3FFFC
    if h == "op_bra":
        return (i16 * 4) & 0x3FFFC
    return None


def emit_block(key, pc, words, table):
    out = [f"// block {pc:05x}:" + "".join(f" {w:08x}" for w in words),
           f"void spu_aot_{key:016x}(SPUThread& __restrict s) {{"]
    names = [table[w >> 21] for w in words]
    end = (pc + 4 * (len(words) - 1)) & 0x3FFFC
    loop = branch_target(end, words[-1], names[-1]) == pc and not any(h in LOOP_UNSAFE for h in names)
    ind = "\t"
    if loop:
        # stores into the loop's own code lines bump code_writes and leave the loop for revalidation
        out.append("\tconst u64 cw = s.code_writes;")
        out.append("\tfor (u32 it = 0; ; it++) {")
        ind = "\t\t"
    if HEAT:
        out.append(f"{ind}{heat(key, len(words))}")
    if idiom_call(pc, words, "s"):
        out.append(f"{ind}{idiom_call(pc, words, 's')}")
    if skip_call(pc, words, "s", "return;"):
        out.append(f"{ind}{skip_call(pc, words, 's', 'return;')}")
    for i, w in enumerate(words):
        a = (pc + 4 * i) & 0x3FFFC
        h = names[i]
        out.append(f"{ind}s.pc = 0x{a:05X}; {h}(s, 0x{w:08X}u);")
        last = i == len(words) - 1
        if h in MAY_HALT and not last:
            out.append(f"{ind}if (s.halted) {{ s.pc = 0x{(a + 4) & 0x3FFFC:05X}; return; }}")
        if last and h not in CONTROL:
            out.append(f"{ind}s.pc = 0x{(a + 4) & 0x3FFFC:05X};")
    if loop:
        out.append(f"\t\tif (s.pc != 0x{pc:05X} || s.code_writes != cw || it == {LOOP_CAP - 1}) "
                   f"{{ s.instructions += (u64)it * {len(words)}; return; }}")
        out.append("\t}")
    out.append("}")
    return "\n".join(out)


UNCOND = {"op_br", "op_bra", "op_brsl", "op_brasl"}
COND = {"op_brz", "op_brnz", "op_brhz", "op_brhnz"}
CLUSTER_SPAN = 0x2000      # bytes spanned by a cluster (its LS lines are guarded as one unit)
CLUSTER_MEMBERS = 64
CLUSTER_BUDGET = 4096      # internal jumps per call, so the dispatcher still sees interrupts regularly


def successors(pc, words, names):
    """statically known next pcs of a block (indirect branches and stops have none)"""
    end = (pc + 4 * (len(words) - 1)) & 0x3FFFC
    nxt = (end + 4) & 0x3FFFC
    h = names[-1]
    if h in UNCOND:
        return [branch_target(end, words[-1], "op_bra" if h in ("op_bra", "op_brasl") else "op_br")]
    if h in COND:
        return [branch_target(end, words[-1], h), nxt]
    if h in CONTROL or h in ("op_stop", "op_stopd", "op_invalid"):
        return []
    return [nxt]   # block cut at 256 instructions


def make_clusters(blocks, table):
    """greedy union of linked blocks (shortest links first) under span/size limits; a link needs a single
    known block at the target pc"""
    by_pc = {}
    for k, (pc, words) in blocks.items():
        by_pc.setdefault(pc, []).append(k)
    names = {k: [table[w >> 21] for w in words] for k, (pc, words) in blocks.items()}
    links = {}
    edges = []
    for k, (pc, words) in blocks.items():
        links[k] = []
        for t in successors(pc, words, names[k]):
            if t is not None and len(by_pc.get(t, [])) == 1:
                d = by_pc[t][0]
                links[k].append((t, d))
                edges.append((abs(t - pc), k, d))
    edges.sort()
    cl = {k: {"m": [k], "lo": pc, "hi": pc + 4 * len(words), "pcs": {pc}} for k, (pc, words) in blocks.items()}
    owner = {k: k for k in blocks}
    for _, a, b in edges:
        ra, rb = owner[a], owner[b]
        if ra == rb:
            continue
        A, B = cl[ra], cl[rb]
        lo, hi = min(A["lo"], B["lo"]), max(A["hi"], B["hi"])
        if hi - lo > CLUSTER_SPAN or len(A["m"]) + len(B["m"]) > CLUSTER_MEMBERS or A["pcs"] & B["pcs"]:
            continue
        A["m"] += B["m"]; A["lo"], A["hi"] = lo, hi; A["pcs"] |= B["pcs"]
        for m in B["m"]:
            owner[m] = ra
        del cl[rb]
    out = []
    for r, c in cl.items():
        if len(c["m"]) > 1:
            c["m"].sort(key=lambda k: blocks[k][0])
            out.append(c)
    out.sort(key=lambda c: c["lo"])
    return out, links, names


def words_hash(blocks, members):
    h = 1469598103934665603
    for k in members:
        for v in blocks[k][1]:
            for b in range(4):
                h ^= (v >> (b * 8)) & 0xFF
                h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


LOCAL_MIN_BLOCK = 32   # a block this long (or a loop) makes the local register copies pay off


def emit_cluster(idx, c, blocks, links, names):
    """Cluster code. Clusters with loops or long blocks and no channel/stop instructions run on a SpuLocal register
    context (see runtime/spu_ops.h), copied in on entry and back on exit; the others work on the thread directly."""
    mem = set(c["m"])
    used, written = set(), set()
    has_sync = has_loop = False
    longest = 0
    for k in c["m"]:
        pc, words = blocks[k]
        longest = max(longest, len(words))
        for w, h in zip(words, names[k]):
            r, wr = regs_of(w, h)
            used |= r | wr
            written |= wr
            has_sync |= h in SYNC
        has_loop |= any(d in mem and t <= pc for t, d in links[k])
    local = not has_sync and (has_loop or longest >= LOCAL_MIN_BLOCK)
    x = "L" if local else "s"
    wb = " ".join(f"s.gpr[{r}] = L.gpr[{r}];" for r in sorted(written))
    rl = " ".join(f"L.gpr[{r}] = s.gpr[{r}];" for r in sorted(used))
    out = [f"void spu_aotc_{idx}(SPUThread& __restrict s) {{",
           "\tconst u64 cw = s.code_writes;",
           f"\tu32 budget = {CLUSTER_BUDGET};"]
    if local:
        out += ["\tSpuLocal L(s);", f"\t{rl}", "\tL.pc = s.pc;"]
    out.append(f"\tswitch ({x}.pc) {{")
    out += [f"\t\tcase 0x{blocks[k][0]:05X}: goto L_{k:016x};" for k in c["m"]]
    out += ["\t\tdefault: goto out;", "\t}"]
    for k in c["m"]:
        pc, words = blocks[k]
        nm = names[k]
        out.append(f"L_{k:016x}:")
        out.append(f"\ts.instructions += {len(words)}; {heat(k, len(words))}".rstrip())
        if idiom_call(pc, words, x):
            out.append(f"\t{idiom_call(pc, words, x)}")
        if skip_call(pc, words, x, "goto out;"):
            if local:   # the native function works on the thread: the registers go back to it first (only the
                # cluster's own are valid in L); it returns false before changing anything, so L stays good
                out.append(f"\t{{ {wb} s.pc = L.pc; {skip_call(pc, words, 's', 'return;')} }}")
            else:
                out.append(f"\t{skip_call(pc, words, x, 'goto out;')}")
        for i, w in enumerate(words):
            a = (pc + 4 * i) & 0x3FFFC
            out.append(f"\t{x}.pc = 0x{a:05X}; {nm[i]}({x}, 0x{w:08X}u);")
            last = i == len(words) - 1
            if nm[i] in MAY_HALT and not last:
                out.append(f"\tif (s.halted) {{ s.pc = 0x{(a + 4) & 0x3FFFC:05X}; goto out; }}")
            if last and nm[i] not in CONTROL:
                out.append(f"\t{x}.pc = 0x{(a + 4) & 0x3FFFC:05X};")
        if nm[-1] in MAY_HALT:
            out.append("\tif (s.halted) goto out;")
        for t, d in links[k]:
            if d in mem:
                out.append(f"\tif ({x}.pc == 0x{t:05X}) {{ if (s.can_chain(cw, 0x{t:05X}) && --budget) goto L_{d:016x}; goto out; }}")
        out.append("\tgoto out;")
    out.append("out:")
    if local:
        out += [f"\t{wb}", "\ts.pc = L.pc;"]
    else:
        out.append("\treturn;")
    out.append("}")
    return "\n".join(out), local


def main():
    table = decode_table()
    blocks = existing_blocks()
    blocks.update(load_blocks(sys.argv[1:]))
    OUT.mkdir(parents=True, exist_ok=True)
    for f in OUT.glob("*.cpp"):
        f.unlink()
    keys = sorted(blocks)
    HEAT_INDEX.update({k: i for i, k in enumerate(keys)})
    heat_decl = ["extern unsigned long long g_aot_heat[];", ""] if HEAT else []
    for n in range(0, len(keys), PER_FILE):
        chunk = keys[n:n + PER_FILE]
        src = ['// Generated by tools/spu_aot.py - do not edit.', '#define SPU_AOT_FAST', '#include "spu_ops.h"', ''] + heat_decl
        src += [emit_block(k, *blocks[k], table) for k in chunk]
        (OUT / f"spu_aot_{n // PER_FILE:03d}.cpp").write_text("\n".join(src) + "\n")
    clusters, links, names = make_clusters(blocks, table)
    cluster_of = {}
    files, cur, cur_n, n_local = [], [], 0, 0
    for i, c in enumerate(clusters):
        for k in c["m"]:
            cluster_of[k] = i
        code, local = emit_cluster(i, c, blocks, links, names)
        n_local += local
        cur.append(code)
        cur_n += len(c["m"])
        if cur_n >= PER_FILE:
            files.append(cur); cur, cur_n = [], 0
    if cur:
        files.append(cur)
    for n, body in enumerate(files):
        src = ['// Generated by tools/spu_aot.py - do not edit.', '#define SPU_AOT_FAST', '#include "spu_ops.h"', ''] + heat_decl
        (OUT / f"spu_aotc_{n:03d}.cpp").write_text("\n".join(src + body) + "\n")
    tab = ['// Generated by tools/spu_aot.py - do not edit.', '#include "spu.h"', '']
    if HEAT:
        tab += ['#include <cstdio>', '#include <thread>', '#include <chrono>',
                f'unsigned long long g_aot_heat[{len(keys)}];',
                'static const struct { unsigned pc; unsigned long long key; } g_heat_blocks[] = {']
        tab += [f"\t{{0x{blocks[k][0]:05X}, 0x{k:016x}ull}}," for k in keys]
        tab += ['};', 'static const bool g_heat_thread = [] {', '\tstd::thread([] {', '\t\tfor (;;) {',
                '\t\t\tstd::this_thread::sleep_for(std::chrono::seconds(10));',
                '\t\t\tif (FILE* f = fopen("spu_heat.txt", "w")) {',
                f'\t\t\t\tfor (unsigned i = 0; i < {len(keys)}; i++)',
                '\t\t\t\t\tif (g_aot_heat[i]) fprintf(f, "%llu %05x %016llx\\n", g_aot_heat[i], g_heat_blocks[i].pc, g_heat_blocks[i].key);',
                '\t\t\t\tfclose(f);', '\t\t\t}', '\t\t}', '\t}).detach();', '\treturn true;', '}();', '']
    tab += [f"void spu_aot_{k:016x}(SPUThread&);" for k in keys]
    tab += [f"void spu_aotc_{i}(SPUThread&);" for i in range(len(clusters))]
    tab.append(f"const SpuAotBlock g_spu_aot[{max(1, len(keys))}] = {{")
    tab += [f"\t{{0x{k:016x}ull, spu_aot_{k:016x}, {cluster_of.get(k, -1)}}}," for k in keys] or ["\t{0, nullptr, -1},"]
    tab.append("};")
    tab.append(f"const u32 g_spu_aot_count = {len(keys)};")
    for i, c in enumerate(clusters):
        tab.append(f"static const u32 spu_aotc_{i}_m[] = {{" + ", ".join(f"0x{blocks[k][0]:05X}, {len(blocks[k][1])}" for k in c["m"]) + "};")
    for i, c in enumerate(clusters):
        lines = sorted({((blocks[k][0] + 4 * j) & 0x3FFFF) >> 7 for k in c["m"] for j in range(len(blocks[k][1]))})
        c["lines"] = len(lines)
        tab.append(f"static const u16 spu_aotc_{i}_l[] = {{" + ", ".join(str(l) for l in lines) + "};")
    tab.append(f"const SpuAotCluster g_spu_aot_clusters[{max(1, len(clusters))}] = {{")
    tab += [f"\t{{0x{words_hash(blocks, c['m']):016x}ull, 0x{c['lo']:05X}, 0x{c['hi']:05X}, spu_aotc_{i}_m, {len(c['m'])}, spu_aotc_{i}, spu_aotc_{i}_l, {c['lines']}}},"
            for i, c in enumerate(clusters)] or ["\t{0, 0, 0, nullptr, 0, nullptr, nullptr, 0},"]
    tab.append("};")
    tab.append(f"const u32 g_spu_aot_cluster_count = {len(clusters)};")
    (OUT / "spu_aot_table.cpp").write_text("\n".join(tab) + "\n")
    print(f"{len(keys)} blocks, {sum(len(b[1]) for b in blocks.values())} instructions, "
          f"{(len(keys) + PER_FILE - 1) // PER_FILE} files; {len(clusters)} clusters with "
          f"{sum(len(c['m']) for c in clusters)} blocks in {len(files)} files ({n_local} with local registers)")


if __name__ == "__main__":
    main()
