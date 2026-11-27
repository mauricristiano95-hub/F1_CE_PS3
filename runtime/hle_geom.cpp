// HLE: the frame job lists of workloads w1 / w2 (queues 0x10C15100 / 0x10C29100, the geometry job 0x100A1280) run
// without the SPURS policy module (on by default, F1CE_GEOM_HLE=0: the job manager as before; GEOMETRY_HLE_HANDOFF.md
// step 1). The SPU worker that the scheduler gives the workload (hle_spurs.cpp, same rules as before) takes the jobs
// straight from the queue and runs each on a private SPU of its thread: the job's own code (generated / translated as
// before) with a small native job manager in place of the game's PM (spu_pm.cpp). No PM code, no prefetch slots: one
// job at a time, its buffers fetched before it starts. (A pool of threads of its own next to the workers was tried
// first: as fast with 12 hardware threads, much slower with 8, where the extra threads took cores from the workers and
// the renderer.)
//
// What the job sees is what the PM gives it (read from the PM's disassembly, PM addresses in brackets):
//   LS 0xC00   the job descriptor, (entry word 0 >> 16) & 511 bytes from the entry's EA: the job's buffer list
//   LS 0x1340  the service table [0x2a54, 0x3044]: +0x00 SPU number, +0x0C SPURS EA, +0x10 workload << 16 | job index,
//              +0x14 words appended (type 4), +0x18 queue EA, +0x20 the LS address after the list's end entry (x4),
//              +0x30.. services: +0x30 buffer list (0x1580), +0x34 DMA info (0x2d48), +0x38 DMA info of a list (0x15f0),
//              +0x44 job end (0x1670), +0x48 release (0x1968 -> 0x2ce8), +0x4C release a list (0x1988)
//   entry      LS(set 0, buffer 0) + its header word at +0x10, r3 = 0x1340, r4 = entry word 0 >> 28, r1 = 0x3FFD0
// Buffer list entries (8 bytes: w0 = size/16 << 18 | 0x8000 fetch | 0x4000 DMA list | mode 0x3000 | type << 9 | set << 5
// | buffer, w1 = EA): type 1 declares a buffer set (w1 = first 1 KB block << 24 | blocks per element << 16 | elements
// << 8), 2 takes a buffer of a set (fetched with 0x8000; mode 0x2000 = kept for the next job with the same EA, 0x1000 =
// written back), 3 releases set declarations (bits of w1), 4 appends w1 to the job's word list (counters decremented
// when the job is done, [0x2400]), 5 / 6 / 0 / 7 end the list.
//
// The queue side is the PM's: the grab [0x2288] moves the head and sets this SPU's halfword of the queue line (the lowest
// job index it holds) in one atomic update; when the job is done the halfword goes back to 0xFFFF [0x3260] and the
// job's counters are decremented [0x2400] (one reaching 0 sets the ready count named by its next two bytes [0x3480]).
// An empty queue clears the workload's ready count and is tried once more [0x2e50]; the scheduler lets a worker in only
// while the ready count is non-zero (hle_spurs.cpp rule 2: an emptied frame list must not be entered). The PPU's waits
// see the halfwords as before, and the workers inside a queue count for spurs_workers_on_queue() (wait_jobs in spu_pm.cpp).
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#include "kernel.h"
#include "spu.h"

bool geom_nj_kept(const u8* set1, u32 set1_size, std::vector<u16>& kept, u32& pad, u16 pad_abc[3]);   // geom_native_job.cpp
bool geom_sk_shape(const u8* set1, u32 set1_size);   // geom_skin_job.cpp: skinned pieces
bool geom_sk_handler(SPUThread& s, u32 handler);
void geom_sk_report();
extern thread_local std::atomic<u32>* t_spu_pc_hist;   // spu.cpp
extern thread_local bool t_geom_list_dead;
extern thread_local u32 t_geom_list_kept;
extern bool g_geom_op_prof;   // below (F1CE_GEOM_OP_PROF)
void geom_op_prof(u32 handler, u64 tsc);

// debug (F1CE_GEOM_SNAP=<n>): for n jobs after 20 s of jobs, files snap_<job>_<where>.bin in the run directory with the
// private SPU's registers (128 x 16 bytes, BE word order) and local store, at the job's start, its transform and cull
// loops (spu_ops.h) and its end; the job's entry words and descriptor address in snap_<job>.txt
void (*g_geom_snap)(const SPUThread& s, const char* where) = nullptr;
namespace {
thread_local int t_snap_job = -1;
void snap_write(const SPUThread& t, const char* where) {
    if (t_snap_job < 0) return;
    char name[64];
    snprintf(name, sizeof(name), "snap_%d_%s.bin", t_snap_job, where);
    FILE* f = fopen(name, "wb");
    if (!f) return;
    for (int r = 0; r < 128; r++) {
        const u32 w[4] = {__builtin_bswap32(t.gpr[r].w[3]), __builtin_bswap32(t.gpr[r].w[2]), __builtin_bswap32(t.gpr[r].w[1]),
                          __builtin_bswap32(t.gpr[r].w[0])};
        fwrite(w, 4, 4, f);
    }
    fwrite(t.ls, 1, 0x40000, f);
    fclose(f);
}
}  // namespace

namespace {
const bool g_on = !(getenv("F1CE_GEOM_HLE") && atoi(getenv("F1CE_GEOM_HLE")) == 0);   // on by default since 2026-11-27

constexpr u32 LS_LINE = 0x80, LS_READY = 0x100, LS_COUNTER = 0x180, LS_DESC = 0xC00, LS_TABLE = 0x1340;
constexpr u32 SVC_RETURN = 0x100, SVC_BASE = 0x200, KERNEL_LIMIT = 0x400;   // branch targets handled on the host
constexpr u32 STACK_TOP = 0x3FFD0;

struct Queue { u32 ea, wid; };
// diagnostics (the watchdog / stall dump): what each worker's job is doing
struct Current { std::atomic<SPUThread*> spu{nullptr}; std::atomic<u32> queue{0}, idx{0}; std::atomic<u64> since{0}; };
Current g_cur[8];
u32 g_spurs = 0;
std::atomic<u64> g_nj_same{0}, g_nj_differ{0}, g_nj_declined{0};   // F1CE_GEOM_NJ_CHECK
std::atomic<u64> g_nj_used{0}, g_nj_twice_same{0}, g_nj_twice_differ{0}, g_nj_op35{0};   // F1CE_GEOM_NJ
std::atomic<u64> g_nj_op9{0};
thread_local const std::vector<u16>* t_nj_kept = nullptr;   // the running job's kept triangles (native job)
thread_local u32 t_nj_pad = ~0u;          // and the triangle the compaction leaves after them (~0: none)
thread_local u16 t_nj_pad_abc[3];
thread_local bool t_sk = false;   // the running job is a skinned piece with native handlers (geom_skin_job.cpp)

bool nj_handler(SPUThread& s, u32 handler);
std::atomic<u32> g_pc_hist[0x10000];   // F1CE_GEOM_PC_HIST (dumped to op0_hist.txt with the report)
// debug (F1CE_GEOM_NJ_REGDIFF): registers at each handler entry, recorded in the original run and compared in the native one
struct RegCap { u32 handler; v128 gpr[128]; };
thread_local std::vector<RegCap>* t_cap = nullptr;
thread_local const std::vector<RegCap>* t_cmp = nullptr;
thread_local size_t t_cmp_at = 0;
void reg_capture(const SPUThread& s, u32 handler) {
    if (t_cap) { t_cap->push_back({handler, {}}); memcpy(t_cap->back().gpr, s.gpr, sizeof(s.gpr)); }
    if (t_cmp && t_cmp_at < t_cmp->size()) {
        const RegCap& c = (*t_cmp)[t_cmp_at++];
        static std::atomic<int> logged{0};
        std::string d;
        for (int r = 0; r < 128; r++)
            if (memcmp(&c.gpr[r], &s.gpr[r], 16)) {
                char b[80];
                snprintf(b, sizeof(b), " r%d %08x/%08x", r, c.gpr[r].w[3], s.gpr[r].w[3]);
                d += b;
            }
        if (logged < 30 && (c.handler != handler || !d.empty())) {
            logged++;
            LOG_I("geom nj regdiff: call %zu handler %05x/%05x (original/native):%s", t_cmp_at - 1, c.handler, handler, d.c_str());
        }
    }
}
// The command list (0x5960, r3 = context) for a native job, from C++ instead of the SPU loop (spu_fast_cmdlist): per
// command what the loop hands its handler: [ctx+352] = the command (the stream at [ctx+560] starts with a halfword
// the list skips),
// r3 = context, r4 / r5 = the 32 stream bytes from the command (also at 64(r1) / 80(r1) of the loop's 448-byte frame),
// r0 = 0x5C88; after it [ctx+352] (read again) moves on by 2 * the command's length (table 0x12230); opcode 0 ends the
// list. Handlers keep r80-r127 (the ABI), so returning to the caller needs only r1 and the pc.
thread_local u64 t_job_t0 = 0, t_list_end = 0;   // F1CE_GEOM_OP_PROF: job start, end of the native list
bool nj_list(SPUThread& s) {
    if (!t_nj_kept && !t_sk) return false;
    if (g_geom_op_prof && t_job_t0) geom_op_prof(27, __rdtsc() - t_job_t0);
    const u32 ctx = s.pref(3) & 0x3FFF0, ret = s.pref(0) & 0x3FFFC, sp = s.pref(1);
    s.ls32(ctx + 352, s.ls32(ctx + 560) + 2);
    const u32 frame = sp - 448;
    s.ls32(frame, sp);   // back chain
    const v128 r1 = s.gpr[1], r0 = s.gpr[0];
    for (u32 guard = 0; guard < 64; guard++) {
        const u32 p = s.ls32(ctx + 352), cmd = p & 0x3FFFF;
        const u32 op = s.ls[cmd];
        if (op >= 41) return false;   // (not reached: the stream was checked)
        u32 w[8];
        for (int k = 0; k < 8; k++) w[k] = (u32)s.ls[(cmd + 4 * k) & 0x3FFFF] << 24 | (u32)s.ls[(cmd + 4 * k + 1) & 0x3FFFF] << 16 |
                                          (u32)s.ls[(cmd + 4 * k + 2) & 0x3FFFF] << 8 | s.ls[(cmd + 4 * k + 3) & 0x3FFFF];
        for (int k = 0; k < 8; k++) s.ls32(frame + 64 + 4 * k, w[k]);
        s.gpr[4].w[3] = w[0]; s.gpr[4].w[2] = w[1]; s.gpr[4].w[1] = w[2]; s.gpr[4].w[0] = w[3];
        s.gpr[5].w[3] = w[4]; s.gpr[5].w[2] = w[5]; s.gpr[5].w[1] = w[6]; s.gpr[5].w[0] = w[7];
        s.gpr[3].pi = _mm_setr_epi32(0, 0, 0, (int)ctx);
        s.gpr[1].pi = _mm_add_epi32(r1.pi, _mm_set1_epi32(-448));   // (every word, as ai r1,r1,-448)
        s.set_pref(0, 0x5C88);
        const u32 handler = s.ls32(0x12180 + 4 * op) & 0x3FFFC;
        const u64 prof_t0 = g_geom_op_prof ? __rdtsc() : 0, prof_i0 = s.instructions;
        if (t_cmp) reg_capture(s, handler);
        if (!nj_handler(s, handler)) {
            s.pc = handler;
            static const bool hist = getenv("F1CE_GEOM_PC_HIST") != nullptr;   // debug: op0's instructions per word
            if (hist && op == 0) t_spu_pc_hist = g_pc_hist;
            s.run_until(0x5C88);
            t_spu_pc_hist = nullptr;
            if (s.halted || s.pc != 0x5C88) fatal("geom nj: handler %05x of command %u did not return (pc %05x)", handler, op, s.pc);
        {
            static std::atomic<int> dbg2{0};
            if (op >= 41 || (op != 0 && op != 4 && op != 6 && op != 8 && op != 9 && op != 12 && op != 14 && op != 15 && op != 30 && op != 35 && op != 37))
                if (dbg2++ < 6) LOG_I("geom nj list: unexpected command %u at %05x (p %05x, [560] %05x)", op, cmd, p, s.ls32(ctx + 560));
        }
        }
        if (prof_t0) geom_op_prof(handler, __rdtsc() - prof_t0);
        if (prof_t0 && op == 0) geom_op_prof(29, s.instructions - prof_i0);   // (slot 29: SPU instructions of op0, as "ticks")
        s.ls32(ctx + 352, s.ls32(ctx + 352) + 2 * s.ls[0x12230 + op]);
        if (op == 0) break;
    }
    if (g_geom_op_prof) t_list_end = __rdtsc();
    s.gpr[1] = r1;
    s.gpr[0] = r0;   // (the SPU loop returns through bi r0 with the caller's link: generated callers compare it)
    s.pc = ret;
    return true;
}

// op35 (0xAFD0 -> 0xADD8 -> 0xD400): transform and cull of the triangle list at ctx+400 (n = (ctx[384] - ctx[388]) / 3
// triangles of 8 bytes, a b c a), compacted in place to the kept ones; then ctx[384] = kept * 3, ctx[388] = 0,
// ctx[1152] = 1; the transformed vertices go to scratch above the bump pointer ctx[272], which ends where it started.
// Here the kept list computed from the job's input (geom_native_job.cpp). Declined unless the list is the one computed.
bool nj_handler(SPUThread& s, u32 handler) {
    static std::atomic<int> regs_logged{0};   // debug (F1CE_GEOM_NJ_REGS): the handlers' inputs, first 40 calls
    static const bool log_regs = getenv("F1CE_GEOM_NJ_REGS") != nullptr;
    if (log_regs && regs_logged++ < 40) {
        const u32 ctx = s.pref(3) & 0x3FFF0, sp = s.ls32(ctx + 352);
        LOG_I("geom nj regs: handler %05x r1 %05x r3 %08x %08x %08x %08x r4 %08x %08x %08x %08x r5 %08x %08x %08x %08x [352] %05x: %08x %08x "
              "64(r1) %08x %08x %08x %08x 80(r1) %08x %08x %08x %08x", handler, s.pref(1), s.gpr[3].w[3], s.gpr[3].w[2], s.gpr[3].w[1], s.gpr[3].w[0],
              s.gpr[4].w[3], s.gpr[4].w[2], s.gpr[4].w[1], s.gpr[4].w[0], s.gpr[5].w[3], s.gpr[5].w[2], s.gpr[5].w[1], s.gpr[5].w[0], sp,
              s.ls32(sp & ~3u), s.ls32((sp & ~3u) + 4), s.ls32(s.pref(1) + 64), s.ls32(s.pref(1) + 68), s.ls32(s.pref(1) + 72), s.ls32(s.pref(1) + 76),
              s.ls32(s.pref(1) + 80), s.ls32(s.pref(1) + 84), s.ls32(s.pref(1) + 88), s.ls32(s.pref(1) + 92));
    }
    const std::vector<u16>* kept = t_nj_kept;
    if (t_cap) reg_capture(s, handler);
    if (t_sk) return geom_sk_handler(s, handler);
    if (!kept) return false;
    // op4 / op12 native as for the skinned pieces (geom_skin_job.cpp; F1CE_GEOM_NJ_OPS=0: the SPU handlers)
    static const bool nj_ops = !(getenv("F1CE_GEOM_NJ_OPS") && atoi(getenv("F1CE_GEOM_NJ_OPS")) == 0);
    if (handler == 0x07790 && nj_ops) return geom_sk_handler(s, handler);
    if (handler == 0x07C50 && nj_ops) {
        t_geom_list_dead = true;
        t_geom_list_kept = (u32)kept->size() / 3;
        const bool r = geom_sk_handler(s, handler);
        t_geom_list_dead = false;
        return r;
    }
    static const bool skip9 = !(getenv("F1CE_GEOM_NJ9") && atoi(getenv("F1CE_GEOM_NJ9")) == 0);
    if (handler == 0xB300 && skip9) {   // op9: the positions it decodes only feed op35's transform
        g_nj_op9.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (handler != 0xAFD0) return false;
    const u32 ctx = s.pref(3) & 0x3FFF0;
    const u32 list = s.ls32(ctx + 400) & 0x3FFFF, n3 = s.ls32(ctx + 384) - s.ls32(ctx + 388), kn = (u32)kept->size() / 3;
    if (s.ls32(ctx + 688) || n3 % 3 || n3 / 3 < kn || list + n3 / 3 * 8 > 0x40000) return false;
    u8* p = s.ls + list;
    for (u32 i = 0; i < kn; i++, p += 8) {
        const u16 a = (*kept)[3 * i], b = (*kept)[3 * i + 1], c = (*kept)[3 * i + 2];
        p[0] = (u8)(a >> 8); p[1] = (u8)a; p[2] = (u8)(b >> 8); p[3] = (u8)b;
        p[4] = (u8)(c >> 8); p[5] = (u8)c; p[6] = (u8)(a >> 8); p[7] = (u8)a;
    }
    if (t_nj_pad != ~0u && kn < n3 / 3) {
        p[0] = (u8)(t_nj_pad_abc[0] >> 8); p[1] = (u8)t_nj_pad_abc[0]; p[2] = (u8)(t_nj_pad_abc[1] >> 8); p[3] = (u8)t_nj_pad_abc[1];
        p[4] = (u8)(t_nj_pad_abc[2] >> 8); p[5] = (u8)t_nj_pad_abc[2]; p[6] = (u8)(t_nj_pad_abc[0] >> 8); p[7] = (u8)t_nj_pad_abc[0];
    }
    s.ls32(ctx + 384, kn * 3);
    s.ls32(ctx + 388, 0);
    s.ls32(ctx + 1152, 1);
    s.gpr[3].pi = _mm_set1_epi32(1);
    g_nj_op35.fetch_add(1, std::memory_order_relaxed);
    return true;
}
std::atomic<u64> g_llar{0}, g_long{0}, g_long_us{0};   // F1CE_GEOM_HLE_STATS: GETLLARs, jobs over 50 us and their time
std::atomic<u64> g_jobs{0}, g_usec{0}, g_insns{0}, g_interp{0}, g_slow{0}, g_build{0}, g_grab_us{0};

struct Set { u32 a0 = 0, a1 = 0, a2 = 0; };
struct Elem { bool used = false; u32 ea = 0, size = 0, mode = 0, tag = 0; };

struct Pool {
    SPUThread t;
    u32 k = 0;
    Set sets[16];
    Elem elems[16][32];
    u8 map[16][32] = {};
    u32 words[4] = {}, nwords = 0;
    u16 inuse[64];   // set << 8 | element of the elements taken (each listed once; checked against .used)
    u32 ninuse = 0;
    void free_all() {
        for (u32 n = 0; n < ninuse; n++) {
            const u32 set = inuse[n] >> 8, e = inuse[n] & 255;
            if (elems[set][e].used) { write_back(set, e); elems[set][e].used = false; }
        }
        ninuse = 0;
    }
    u32 cached_ea[16][32] = {};   // mode 0x2000 elements: the EA whose data the element still holds (0: none)
    std::vector<u16> nj_kept_store;

    void get(u32 lsa, u32 ea, u32 size, u32 cmd = 0x40) {
        t.mfc_lsa = lsa; t.mfc_eal = ea; t.mfc_size = size; t.mfc_tag = 1;
        t.dma(cmd);
    }
    void put(u32 lsa, u32 ea, u32 size) {
        while (size) {
            const u32 c = std::min(size, 0x4000u);
            t.mfc_lsa = lsa; t.mfc_eal = ea; t.mfc_size = c; t.mfc_tag = 5;
            t.dma(0x20);
            lsa += c; ea += c; size -= c;
        }
    }
    u32 start(u32 set, u32 e) const { return sets[set].a0 + e * sets[set].a1; }
    void write_back(u32 set, u32 e) {
        Elem& x = elems[set][e];
        if ((x.mode & 0x1000) && x.size) put(start(set, e) << 10, x.ea & ~15u, x.size);
        x.mode &= ~0x1000u;
    }

    // the PM's buffer list walk [0x19f8], for one job alone: every entry succeeds at once
    void buffers(u32 e) {
        for (;; e += 8) {
            const u32 w0 = t.ls32(e), w1 = t.ls32(e + 4);
            if (w0 == 0) continue;
            const u32 type = (w0 >> 9) & 7, set = (w0 >> 5) & 15, buf = w0 & 31;
            if (type == 1) {   // declare a set (it stays usable after a release of its declaration)
                const Set n = {w1 >> 24, (w1 >> 16) & 255, (w1 >> 8) & 127};
                if (n.a0 != sets[set].a0 || n.a1 != sets[set].a1)   // other blocks: nothing cached there
                    for (u32 i = 0; i < 32; i++) cached_ea[set][i] = 0;
                sets[set] = n;
                continue;
            }
            if (type == 2) {
                const Set& s = sets[set];
                const u32 size = (w0 >> 18) << 4, mode = w0 & 0x3000;
                const bool fetch = w0 & 0x8000;
                u32 el = ~0u;
                if (fetch && (mode & 0x2000))   // the element still holding this EA (the job binary)
                    for (u32 i = 0; i < s.a2 && i < 32; i++)
                        if (!elems[set][i].used && cached_ea[set][i] == w1 && elems[set][i].size == size) { el = i; break; }
                const bool cached = el != ~0u;
                if (!cached)
                    for (u32 i = 0; i < s.a2 && i < 32; i++)
                        if (!elems[set][i].used && !cached_ea[set][i]) { el = i; break; }
                if (el == ~0u)
                    for (u32 i = 0; i < s.a2 && i < 32; i++)
                        if (!elems[set][i].used) { el = i; break; }
                if (el == ~0u) {
                    static std::atomic<int> warned{0};
                    if (warned++ < 5) LOG_W("geom hle: set %u has no free element (buffer %u)", set, buf);
                    el = 0;
                }
                Elem& x = elems[set][el];
                if (ninuse < 64) inuse[ninuse++] = (u16)(set << 8 | el);
                else fatal("geom hle: more than 64 buffers in one job");
                x = {true, w1, size, mode, x.tag};
                map[set][buf] = (u8)el;
                if (fetch && !cached) {
                    const u32 lsa = start(set, el) << 10, endb = (start(set, el) + s.a1) << 10;
                    if (w0 & 0x4000) {   // a DMA list: the list goes to the end of the element, then the elements
                        const u32 list = endb - size - ((w1 & 7) << 4);
                        get(list, w1 & ~15u, size);
                        t.mfc_lsa = lsa; t.mfc_eal = list; t.mfc_size = size; t.mfc_tag = 1;
                        t.dma(0x44);
                    } else {
                        for (u32 o = 0; o < size; o += 0x4000) get(lsa + o, (w1 & ~15u) + o, std::min(size - o, 0x4000u));
                    }
                }
                // an element written by this job no longer holds what main memory has at its EA
                cached_ea[set][el] = fetch && (mode & 0x2000) ? w1 : 0;
                continue;
            }
            if (type == 3) continue;   // release of set declarations: blocks only, nothing to keep track of here
            if (type == 4) {
                if (nwords < 4) words[nwords] = w1;
                nwords++;
                continue;
            }
            break;   // 5 (end), 0 / 6 / 7 [0x2170]
        }
    }
    // [0x2d48] r4 = set, r5 = buffer, r6 != 0: give it a DMA tag
    void dma_info(u32 set, u32 buf, bool want_tag, u32 out[4]) {
        const u32 el = map[set & 15][buf & 31];
        Elem& x = elems[set & 15][el];
        if (want_tag && !x.tag) x.tag = 16 + (el & 15);
        const u32 st = start(set & 15, el);
        out[0] = (set & 255) << 24 | (buf & 255) << 16 | x.tag;
        out[1] = (st & 255) << 24 | sets[set & 15].a1 << 8;
        out[2] = x.ea & ~15u;
        out[3] = x.size & 0x3FFF0;
    }
    void release(u32 set, u32 buf) {
        const u32 el = map[set & 15][buf & 31];
        write_back(set & 15, el);
        elems[set & 15][el].used = false;
    }
    void set_r3(u32 w0, u32 w1, u32 w2, u32 w3) { v128& r = t.gpr[3]; r.w[3] = w0; r.w[2] = w1; r.w[1] = w2; r.w[0] = w3; }

    bool service() {
        const u64 svc_t0 = g_geom_op_prof ? __rdtsc() : 0;
        struct SvcTime { u64 t0; ~SvcTime() { if (t0) geom_op_prof(26, __rdtsc() - t0); } } svc_time{svc_t0};
        SPUThread& s = t;
        const u32 ret = s.pref(0) & 0x3FFFC;
        if (s.pc == SVC_RETURN) { s.halted = true; return true; }
        switch (s.pc - SVC_BASE) {
            case 0x30: buffers(s.pref(3)); s.set_pref(3, 1); break;
            case 0x34: {
                u32 o[4];
                dma_info(s.pref(4), s.pref(5), s.pref(6) != 0, o);
                set_r3(o[0], o[1], o[2], o[3]);
                break;
            }
            case 0x38:   // r3 = quadwords (set, buffer, tag wanted), r4 = count: each replaced by its DMA info
                for (u32 a = s.pref(3), n = s.pref(4); n; n--, a += 16) {
                    u32 o[4];
                    dma_info(s.ls32(a), s.ls32(a + 4), s.ls32(a + 8) != 0, o);
                    for (int j = 0; j < 4; j++) s.ls32(a + 4 * j, o[j]);
                }
                break;
            case 0x44: break;   // job end: the PM lets the next job's buffers in; nothing to do here
            case 0x48:
                release(s.pref(4), s.pref(5));
                s.interrupts = false;
                set_r3(~0u, ~0u, ~0u, ~0u);
                break;
            case 0x4C:   // r3 = halfwords (set << 8 | buffer), r4 = count
                for (u32 a = s.pref(3), n = s.pref(4); n; n--, a += 2) {
                    const u32 h = (u32)s.ls[a & 0x3FFFF] << 8 | s.ls[(a + 1) & 0x3FFFF];
                    release(h >> 8, h & 255);
                }
                s.interrupts = false;
                break;
            default:
                fatal("geom hle: job called unknown service %05x (r0=%05x)", s.pc, ret);
        }
        s.pc = ret;
        return true;
    }

    // an atomic update of the 128-byte line at ea through LS 0x80: f(line) returns false to leave it unchanged
    template <class F> void update_line(u32 lsa, u32 ea, F f) {
        for (;;) {
            t.mfc_lsa = lsa; t.mfc_eal = ea & ~127u; t.mfc_size = 128;
            t.dma(0xD0);
            t.atomic_stat = 0;
            if (!f(t.ls + lsa)) return;
            t.mfc_lsa = lsa; t.mfc_eal = ea & ~127u;
            t.dma(0xB4);
            const u32 st = t.atomic_stat;
            t.atomic_stat = 0;
            if (!(st & 1)) return;
        }
    }
    static u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
    static void be16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

    // [0x3480] / [0x33e0]: ready count of a workload (byte wid of the SPURS line); swap, or set only while it is `expect`
    u32 ready(u32 wid, u32 value, int expect = -1) {
        u32 old = 0;
        update_line(LS_READY, g_spurs, [&](u8* l) {
            old = l[wid & 127];
            if (expect >= 0 && old != (u32)expect) return false;
            l[wid & 127] = (u8)value;
            return true;
        });
        return old;
    }
    // [0x2400]: the job's counter halfword at w goes down by one; at 0, ready[byte w+2] = byte w+3
    void notify(u32 w) {
        u32 was = 0, wid = 0, value = 0;
        update_line(LS_COUNTER, w, [&](u8* l) {
            u8* p = l + (w & 127);
            was = be16(p);
            wid = p[2];
            value = p[3];
            be16(p, was - 1);
            return true;
        });
        if (was == 1) ready(wid, value);
    }
    // [0x2288]: the next entry of queue q. 1 with a job (e0, e1, index), 0 when the queue has none for now (no entry, or
    // a wait entry whose counter is not 0 yet); skips and passed wait entries move the head on.
    // holding: this thread still holds the previous job (its halfword not released yet): the update that takes the next
    // job sets the halfword to it outright, the release and the grab in one atomic update (min over the jobs it holds)
    int grab(u32 q, u32 out[3], bool holding = false) {
        const u32 pos = 4 + 2 * k;   // this SPU's halfword in the quadword at +0x10
        for (;;) {
            t.mfc_lsa = LS_LINE; t.mfc_eal = q; t.mfc_size = 128;
            t.dma(0xD0);
            t.atomic_stat = 0;
            u8* l = t.ls + LS_LINE;
            const u32 w0 = (u32)be16(l + 0x10) << 16 | be16(l + 0x12), h = w0 & 0xFFFF, top = w0 >> 16;
            const u32 at = q + 32 + h * 8;
            const u32 e0 = ld32(at), e1 = ld32(at + 4), kind = e0 & 0xFFFF;
            if (kind == 0) return 0;
            if (kind != 1 && kind != 3) {   // wait until the halfword at e1 is 0
                get(LS_COUNTER, e1 & ~15u, 16);
                if (be16(t.ls + LS_COUNTER + (e1 & 15)) != 0) return 0;
            }
            const u32 h1 = (h + 1) & 0xFFFF, top1 = top > h1 ? top : h1;
            be16(l + 0x10, top1);
            be16(l + 0x12, h1);
            if (kind == 1) {
                const u32 mine = be16(l + 0x10 + pos);
                be16(l + 0x10 + pos, holding || mine > h ? h : mine);
            }
            t.mfc_lsa = LS_LINE; t.mfc_eal = q;
            t.dma(0xB4);
            const u32 st = t.atomic_stat;
            t.atomic_stat = 0;
            if (kind != 1 || (st & 1)) continue;
            out[0] = e0; out[1] = e1; out[2] = h;
            return 1;
        }
    }
    // [0x3260] for a thread with no job left: its halfword of the queue line back to 0xFFFF
    void leave(u32 q) {
        update_line(LS_LINE, q, [&](u8* l) {
            if (be16(l + 0x14 + 2 * k) == 0xFFFF) return false;
            be16(l + 0x14 + 2 * k, 0xFFFF);
            return true;
        });
    }

    // debug (F1CE_GEOM_SHAPES=1): job shapes, i.e. the command sequence of the stream in set 1 (opcode in the high byte of
    // each halfword, lengths from the job's table) with the op8 format words and op9's attribute mask; counted, the top
    // ones logged every 10 s
    void shape_count(u32 queue) {
        static const u8 lens[41] = {1, 0, 0, 0, 2, 2, 3, 3, 3, 4, 2, 3, 4, 7, 2, 2, 2, 3, 2, 2, 3, 2, 3, 10, 3, 4, 3, 2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1};
        const u32 s1 = start(1, map[1][0]) << 10;
        std::string key;
        char b[48];
        for (u32 i = 1, guard = 0; i < 0x80 && guard < 40; guard++) {
            const u32 h = (u32)t.ls[s1 + 2 * i] << 8 | t.ls[s1 + 2 * i + 1], op = h >> 8;
            if (op >= 41 || !lens[op]) { key += " ?"; break; }
            auto arg = [&](u32 k) { return (u32)t.ls[s1 + 2 * (i + k)] << 8 | t.ls[s1 + 2 * (i + k) + 1]; };
            if (op == 8) snprintf(b, sizeof(b), " 8[%08x %08x]", t.ls32(s1 + arg(1)), t.ls32(s1 + arg(1) + 4));
            else if (op == 9) snprintf(b, sizeof(b), " 9[%x]", arg(2));
            else snprintf(b, sizeof(b), " %u", op);
            key += b;
            if (op == 0) break;
            i += lens[op];
        }
        static std::mutex m;
        static std::map<std::string, u64> count;
        static u64 total = 0, next = time_usec() + 10000000;
        std::lock_guard lk(m);
        count[(queue == 0x10C15100 ? "w1:" : "w2:") + key]++;
        total++;
        if (time_usec() < next) return;
        next = time_usec() + 10000000;
        std::vector<std::pair<u64, std::string>> v;
        for (auto& [k, c] : count) v.push_back({c, k});
        std::sort(v.rbegin(), v.rend());
        LOG_I("geom shapes: %zu kinds over %llu jobs", v.size(), (unsigned long long)total);
        for (size_t i = 0; i < v.size() && i < 25; i++) LOG_I("geom shape %5.2f%% %s", 100.0 * v[i].first / total, v[i].second.c_str());
    }

    // the native job's command stream: without the commands whose results only fed op35's transform and cull (op6
    // vertex count, op8 format, op9 positions, op14 / op15 matrices); the rest keep their operands (offsets into set 1)
    void nj_stream() {
        static const bool on = !(getenv("F1CE_GEOM_NJ_STREAM") && atoi(getenv("F1CE_GEOM_NJ_STREAM")) == 0);
        if (!on) return;
        static const u8 lens[41] = {1, 0, 0, 0, 2, 2, 3, 3, 3, 4, 2, 3, 4, 7, 2, 2, 2, 3, 2, 2, 3, 2, 3, 10, 3, 4, 3, 2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1};
        u8* s1 = t.ls + (start(1, map[1][0]) << 10);
        u8 out[0x80];
        u32 o = 0;
        for (u32 i = 1, guard = 0; i < 0x40 && guard < 16; guard++) {
            const u32 op = s1[2 * i];
            if (op >= 41 || !lens[op]) return;   // (geom_nj_kept accepted the stream: not reached)
            if (op != 6 && op != 8 && op != 9 && op != 14 && op != 15) {
                memcpy(out + o, s1 + 2 * i, 2 * lens[op]);
                o += 2 * lens[op];
            }
            if (op == 0) break;
            i += lens[op];
        }
        memcpy(s1 + 2, out, o);
    }

    // F1CE_GEOM_NJ=2: the job from its current state twice (see run_job), the second time for real
    struct Put { u32 cmd, ea, size; std::vector<u8> data; };
    void run_twice(std::vector<u16>& kept, u32 idx, u32 desc, bool sk = false) {
        // what a run changes besides the LS and registers: this manager's buffer bookkeeping
        struct Book { Set sets[16]; Elem elems[16][32]; u8 map[16][32]; u32 cached[16][32]; u16 inuse[64]; u32 ninuse, words[4], nwords; };
        auto book = std::make_unique<Book>();
        memcpy(book->sets, sets, sizeof(sets)); memcpy(book->elems, elems, sizeof(elems)); memcpy(book->map, map, sizeof(map));
        memcpy(book->cached, cached_ea, sizeof(cached_ea)); memcpy(book->inuse, inuse, sizeof(inuse));
        book->ninuse = ninuse; memcpy(book->words, words, sizeof(words)); book->nwords = nwords;
        std::vector<u8> ls(t.ls, t.ls + 0x40000);
        v128 gpr[128];
        memcpy(gpr, t.gpr, sizeof(gpr));
        const u32 pc = t.pc;
        std::vector<Put> runs[2];
        for (int r = 0; r < 2; r++) {
            std::vector<Put>* out = &runs[r];
            t.on_dma = [out](SPUThread& s, u32 cmd, u32 lsa, u32 ea, u32 size) {
                const u32 c8 = cmd & 0xFF;
                if ((c8 & 0xF4) == 0x20) out->push_back({c8, ea, size, std::vector<u8>(s.ls + (lsa & 0x3FFFF), s.ls + std::min((lsa & 0x3FFFF) + size, 0x40000u))});
                else if (c8 == 0xB4 || c8 == 0xB8) out->push_back({c8, ea & ~127u, 128, std::vector<u8>(s.ls + (lsa & 0x3FF80), s.ls + (lsa & 0x3FF80) + 128)});
            };
            std::vector<SpuDmaRec> log;
            t.dma_log = r == 0 ? &log : nullptr;
            t.dry_line_ea = ~0u;
            t.has_reservation = false;
            static const bool regdiff = getenv("F1CE_GEOM_NJ_REGDIFF") != nullptr;
            static std::vector<RegCap> cap;
            const bool native = regdiff ? r == 1 : r == 0;
            if (regdiff && r == 0) { cap.clear(); t_cap = &cap; }
            if (regdiff && r == 1) { t_cmp = &cap; t_cmp_at = 0; }
            t_nj_kept = native && !sk ? &kept : nullptr;
            t_sk = native && sk;
            if (native && !sk) nj_stream();
            t.halted = false;
            t.run_until(~0u);
            t_nj_kept = nullptr;
            t_sk = false;
            t_cap = nullptr;
            t_cmp = nullptr;
            t.dma_log = nullptr;
            t.on_dma = nullptr;
            if (r == 0) {   // back to the state before the run
                t.write_ls(0, ls.data(), 0x40000);
                memcpy(t.gpr, gpr, sizeof(gpr));
                t.pc = pc;
                t.interrupts = false;
                memcpy(sets, book->sets, sizeof(sets)); memcpy(elems, book->elems, sizeof(elems)); memcpy(map, book->map, sizeof(map));
                memcpy(cached_ea, book->cached, sizeof(cached_ea)); memcpy(inuse, book->inuse, sizeof(inuse));
                ninuse = book->ninuse; memcpy(words, book->words, sizeof(words)); nwords = book->nwords;
            }
        }
        // compared: the puts, without the atomics; the index ring (0x35xxxxxx) by its data (other workers allocate
        // between the two runs), the RSX command slots without the index offset word, the rest (done records, kick)
        // by address only (their contents follow the other jobs' progress)
        for (auto& v : runs) {
            std::vector<Put> n;
            for (Put& p : v) {
                if (p.cmd == 0xB4 || p.cmd == 0xB8 || p.ea == 0x20001000) continue;
                if ((p.ea >> 24) == 0x35) p.ea = 0;
                else if ((p.ea >> 24) == 0x31) {   // the draws' chunks follow the ring address: their total count
                    std::vector<u8> n2(p.data.begin(), p.data.begin() + std::min<size_t>(20, p.data.size()));
                    for (size_t i = 0; i + 8 <= n2.size(); i += 4) {   // the ring offsets (index array, skinned vertices)
                        const u32 h = (u32)n2[i] << 24 | n2[i + 1] << 16 | n2[i + 2] << 8 | n2[i + 3], m = h & 0xFFFC;
                        if (m == 0x181C || (m >= 0x1680 && m < 0x16C0)) memset(n2.data() + i + 4, 0, 4);
                        if ((h >> 18 & 0x7FF) && !(h & 0xE0000003)) i += 4 * (h >> 18 & 0x7FF);   // (past the data words)
                    }
                    u32 total = 0;
                    for (size_t i = 0; i + 8 <= p.data.size(); i += 4) {
                        const u32 h = (u32)p.data[i] << 24 | p.data[i + 1] << 16 | p.data[i + 2] << 8 | p.data[i + 3];
                        if ((h & 0x1FFFF) != 0x1824) continue;
                        for (u32 k = 0; k < ((h >> 18) & 0x7FF) && i + 8 + 4 * k <= p.data.size(); k++) total += p.data[i + 4 + 4 * k] + 1u;
                        break;
                    }
                    for (int k = 3; k >= 0; k--) n2.push_back((u8)(total >> (8 * k)));
                    p.data.swap(n2);
                }
                else p.data.clear();
                n.push_back(std::move(p));
            }
            v.swap(n);
        }
        bool same = runs[0].size() == runs[1].size();
        size_t at = 0;
        for (; same && at < runs[0].size(); at++) {
            const Put &a = runs[0][at], &b = runs[1][at];
            if (a.cmd != b.cmd || a.ea != b.ea || a.size != b.size || a.data != b.data) same = false;
        }
        (same ? g_nj_twice_same : g_nj_twice_differ).fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            static std::atomic<int> logged{0};
            if (logged++ < 20) {
                std::string d;
                char b[96];
                for (int r = 0; r < 2; r++) {
                    d += r ? " | original:" : " native:";
                    for (const Put& p : runs[r]) {
                        snprintf(b, sizeof(b), " %02x %08x %x", p.cmd, p.ea, p.size);
                        d += b;
                    }
                }
                if (at && at <= runs[0].size() && at <= runs[1].size()) {
                    const Put &a = runs[0][at - 1], &o = runs[1][at - 1];
                    u32 k = 0;
                    while (k < a.data.size() && k < o.data.size() && a.data[k] == o.data[k]) k++;
                    snprintf(b, sizeof(b), " | put %zu differs at byte %u", at - 1, k);
                    d += b;
                    for (int r = 0; r < 2; r++) {
                        const Put& p = runs[r][at - 1];
                        d += r ? " | orig" : " | native";
                        for (size_t i = 0; i + 4 <= p.data.size() && i < 0x40; i += 4) {
                            snprintf(b, sizeof(b), " %02x%02x%02x%02x", p.data[i], p.data[i + 1], p.data[i + 2], p.data[i + 3]);
                            d += b;
                        }
                    }
                }
                LOG_W("geom nj twice: job %u desc %08x (%zu kept):%s", idx, desc, kept.size() / 3, d.c_str());
            }
        }
    }

    void run_job(const Queue& q, const u32 j[3]) {
        const u64 prof_t0 = g_geom_op_prof ? __rdtsc() : 0;
        const u32 e0 = j[0], e1 = j[1], idx = j[2];
        free_all();
        nwords = 0;
        const u32 dsize = (e0 >> 16) & 511;
        memset(t.ls + LS_DESC + dsize, 0, 0x40);   // (the PM's LS holds older bytes there; the params read stays zero)
        if (dsize) get(LS_DESC, e1, dsize);
        buffers(LS_DESC);
        static const bool shapes = getenv("F1CE_GEOM_SHAPES") != nullptr;
        if (shapes) shape_count(q.ea);
        u32 next = LS_DESC;   // after the list's end entry, 16-aligned [0x2134]
        for (;; next += 8) {
            const u32 w = t.ls32(next), type = (w >> 9) & 7;
            if (w && type != 1 && type != 2 && type != 3 && type != 4) break;
        }
        next += 8;
        next += next & 8;
        const u32 tbl[12] = {k, 0, 0, g_spurs, q.wid << 16 | (idx & 0xFFFF), std::min(nwords, 4u), q.ea, 0, next, next, next, next};
        for (u32 i = 0; i < 12; i++) t.ls32(LS_TABLE + 4 * i, tbl[i]);
        const u32 base = start(0, map[0][0]) << 10;
        memset(t.gpr, 0, sizeof(t.gpr));
        t.set_pref(0, SVC_RETURN);
        t.set_pref(1, STACK_TOP);
        t.set_pref(3, LS_TABLE);
        t.set_pref(4, e0 >> 28);
        t.pc = base + t.ls32(base + 0x10);
        t.interrupts = false;
        t.event_mask = t.event_stat = t.stall_stat = 0;
        for (auto& l : t.stalled) l.active = false;
        const u64 i0 = t.instructions, n0 = t.st_interp, s0 = t.st_slow, b0 = t.st_build;
        static const int snaps = getenv("F1CE_GEOM_SNAP") ? atoi(getenv("F1CE_GEOM_SNAP")) : 0;
        static std::atomic<int> snapped{0};
        static const u64 snap_t0 = time_usec();
        t_snap_job = -1;
        if (snaps && time_usec() - snap_t0 > 20000000) {
            static std::atomic<u32> seen{0};
            const u32 every = getenv("F1CE_GEOM_SNAP_EVERY") ? (u32)atoi(getenv("F1CE_GEOM_SNAP_EVERY")) : 1;
            // F1CE_GEOM_SNAP_OP=<opcode>: only jobs whose stream has that command
            static const int only_op = getenv("F1CE_GEOM_SNAP_OP") ? atoi(getenv("F1CE_GEOM_SNAP_OP")) : -1;
            bool has = only_op < 0;
            if (!has) {
                static const u8 lens[41] = {1, 0, 0, 0, 2, 2, 3, 3, 3, 4, 2, 3, 4, 7, 2, 2, 2, 3, 2, 2, 3, 2, 3, 10, 3, 4, 3, 2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1};
                const u8* s1 = t.ls + (start(1, map[1][0]) << 10);
                for (u32 i = 1, g = 0; i < 0x40 && g < 20; g++) {
                    const u32 op = s1[2 * i];
                    if (op >= 41 || !lens[op]) break;
                    if ((int)op == only_op) { has = true; break; }
                    if (op == 0) break;
                    i += lens[op];
                }
            }
            const int n = has && seen++ % every == 0 ? snapped++ : snaps;
            if (n < snaps) {
                LOG_I("geom snap %d: job %u desc %08x", n, idx, e1);
                t_snap_job = n;
                g_geom_snap = snap_write;
                snap_write(t, "start");
                char name[48];
                snprintf(name, sizeof(name), "snap_%d.txt", n);
                if (FILE* f = fopen(name, "w")) { fprintf(f, "%08x %08x %u %08x\n", e0, e1, idx, q.ea); fclose(f); }
            }
        }
        // debug: F1CE_GEOM_HLE_TRACE=<n> logs the MFC commands of n jobs (after 20 s of jobs)
        static const int trace = getenv("F1CE_GEOM_HLE_TRACE") ? atoi(getenv("F1CE_GEOM_HLE_TRACE")) : 0;
        static std::atomic<int> traced{0};
        static const u64 trace_t0 = time_usec();
        std::string tr;
        const bool tracing = t_snap_job >= 0 || (trace && time_usec() - trace_t0 > 20000000 && traced++ < trace);
        if (tracing)
            t.on_dma = [&tr](SPUThread& s, u32 cmd, u32 lsa, u32 ea, u32 size) {
                char b[64];
                snprintf(b, sizeof(b), " |%05x:%02x %05x %08x %x", s.pc, cmd & 0xFF, lsa, ea, size);
                tr += b;
                const u32 c8 = cmd & 0xFF;
                // puts: the data (up to 0x80 bytes); conditional / unconditional line stores: the line
                if ((c8 & 0xF4) == 0x20 || c8 == 0xB4 || c8 == 0xB8) {
                    const u32 n = c8 == 0xB4 || c8 == 0xB8 ? 128 : std::min(size, 0x80u), at = c8 == 0xB4 || c8 == 0xB8 ? lsa & 0x3FF80 : lsa;
                    tr += " =";
                    for (u32 k = 0; k < n; k += 4) { snprintf(b, sizeof(b), " %08x", s.ls32(at + k)); tr += b; }
                }
                if (c8 == 0xD0) {   // the line as read (memory before the atomic)
                    tr += " mem";
                    for (u32 k = 0; k < 128; k += 4) { snprintf(b, sizeof(b), " %08x", ld32((ea & ~127u) + k)); tr += b; }
                }
            };
        // F1CE_GEOM_NJ_CHECK=1: the native job's kept triangles (geom_native_job.cpp) against the job's index put
        static const bool nj_check = getenv("F1CE_GEOM_NJ_CHECK") != nullptr;
        std::vector<u16>& nj_kept = nj_kept_store;   // (reused: a vector per job showed as new / delete in profiles)
        nj_kept.clear();
        std::vector<u8> nj_put;
        bool nj = false, nj_got = false;
        if (nj_check && !tracing) {
            u32 pad_unused; u16 abc_unused[3];
            nj = geom_nj_kept(t.ls + (start(1, map[1][0]) << 10), sets[1].a1 << 10, nj_kept, pad_unused, abc_unused);
            if (nj)
                t.on_dma = [&](SPUThread& s, u32 cmd, u32 lsa, u32, u32 size) {
                    if (!nj_got && (cmd & 0xFF) == 0x20 && (s.pc & 0x3FFFC) == 0x4408) {
                        nj_got = true;
                        nj_put.assign(s.ls + (lsa & 0x3FFFF), s.ls + std::min((lsa & 0x3FFFF) + size, 0x40000u));
                    }
                };
            else
                g_nj_declined.fetch_add(1, std::memory_order_relaxed);
        }
        // F1CE_GEOM_NJ (default 1, 0: off): command handlers replaced by the native job where it covers the job
        // (geom_native_job.cpp);
        // =2: also every 16th such job twice, first with the native handlers and DMA dry (puts logged, atomics on a
        // copy), then the original for real from the same state, and their puts and line stores compared
        static const int nj_mode = getenv("F1CE_GEOM_NJ") ? atoi(getenv("F1CE_GEOM_NJ")) : 1;   // (on by default)
        bool nj_on = false;
        if (nj_mode && !tracing && !nj) {
            nj_on = geom_nj_kept(t.ls + (start(1, map[1][0]) << 10), sets[1].a1 << 10, nj_kept, t_nj_pad, t_nj_pad_abc);
            (nj_on ? g_nj_used : g_nj_declined).fetch_add(1, std::memory_order_relaxed);
        }
        // F1CE_GEOM_SK (default 1, 0: off): skinned pieces with native handlers (geom_skin_job.cpp)
        static const bool sk_on = !(getenv("F1CE_GEOM_SK") && atoi(getenv("F1CE_GEOM_SK")) == 0);
        t_sk = sk_on && nj_mode && !tracing && !nj && !nj_on && geom_sk_shape(t.ls + (start(1, map[1][0]) << 10), sets[1].a1 << 10);
        static std::atomic<u32> nj_checks{0};
        const bool nj_twice = (nj_on || t_sk) && nj_mode == 2 && (nj_checks++ & 15) == 0;
        t.halted = false;   // (not run(): its getenv and flush check on every job showed in profiles)
        const u64 prof_t1 = prof_t0 ? __rdtsc() : 0;
        if (prof_t0) geom_op_prof(5, prof_t1 - prof_t0);   // slot 5: the setup before the job, slot 7: the job's run
        if (nj_twice) {
            run_twice(nj_kept, idx, e1, t_sk);
        } else {
            t_nj_kept = nj_on ? &nj_kept : nullptr;
            if (nj_on) nj_stream();
            t_job_t0 = prof_t1; t_list_end = 0;
            static const bool hist = getenv("F1CE_GEOM_PC_HIST") != nullptr;
            if (hist) t_spu_pc_hist = g_pc_hist;
            t.run_until(~0u);
            t_spu_pc_hist = nullptr;
            if (prof_t1 && t_list_end) geom_op_prof(28, __rdtsc() - t_list_end);
            t_nj_kept = nullptr;
        }
        t_sk = false;
        if (prof_t0) geom_op_prof(7, __rdtsc() - prof_t1);
        if (tracing) {
            t.on_dma = nullptr;
            LOG_I("geom hle trace: job %u queue %08x desc %08x:%s", idx, q.ea, e1, tr.c_str());
            if (t_snap_job >= 0) {
                char name[48];
                snprintf(name, sizeof(name), "snap_%d.txt", t_snap_job);
                if (FILE* f = fopen(name, "a")) { fputs(tr.c_str(), f); fputc('\n', f); fclose(f); }
            }
        }
        if (nj) {
            t.on_dma = nullptr;
            std::vector<u8> mine;
            for (u16 v : nj_kept) { mine.push_back((u8)(v >> 8)); mine.push_back((u8)v); }
            const bool same = nj_kept.empty() ? true : nj_got && nj_put.size() >= mine.size() && !memcmp(nj_put.data(), mine.data(), mine.size());
            (same ? g_nj_same : g_nj_differ).fetch_add(1, std::memory_order_relaxed);
            if (!same) {
                static std::atomic<int> logged{0};
                if (logged++ < 20) {
                    u32 at = 0;
                    while (at < mine.size() && at < nj_put.size() && mine[at] == nj_put[at]) at++;
                    LOG_W("geom nj check: job %u desc %08x: %zu kept triangles native, put of %zu bytes (got %d), first difference "
                          "at byte %u (native %02x%02x put %02x%02x)", idx, e1, nj_kept.size() / 3, nj_put.size(), nj_got, at,
                          at < mine.size() ? mine[at & ~1u] : 0, at + 1 < mine.size() ? mine[(at & ~1u) + 1] : 0,
                          at < nj_put.size() ? nj_put[at & ~1u] : 0, at + 1 < nj_put.size() ? nj_put[(at & ~1u) + 1] : 0);
                }
            }
        }
        g_insns.fetch_add(t.instructions - i0, std::memory_order_relaxed);
        g_interp.fetch_add(t.st_interp - n0, std::memory_order_relaxed);
        g_slow.fetch_add(t.st_slow - s0, std::memory_order_relaxed);
        g_build.fetch_add(t.st_build - b0, std::memory_order_relaxed);
        if (t_snap_job >= 0) { snap_write(t, "end"); t_snap_job = -1; }
        if (t.pc != SVC_RETURN)
            fatal("geom hle: job %u of queue %08x stopped at %05x (binary %08x)", idx, q.ea, t.pc, ld32(e1 + 0x14));
        free_all();
    }

    void init(u32 num) {
        k = num;
        t.id = num;
        t.name = "geom" + std::to_string(num);
        memset(t.ls, 0, 0x4000);
        for (u32 off = 0x30; off <= 0x50; off += 4) t.ls32(LS_TABLE + off, SVC_BASE + off);
        t.flush_blocks();
        t.kernel_limit = KERNEL_LIMIT;
        t.on_kernel_call = [this](SPUThread&) { return service(); };
        t.report_ie = true;   // job code asserts interrupts are on (rdch MachStat)
        t.on_stop = [](SPUThread& s, u32 code) {
            LOG_W("geom hle: %s stop 0x%x at %05x", s.name.c_str(), code, s.pc);
            return false;
        };
    }
};

}  // namespace

// hle_spurs.cpp: a workload added with this argument (its queue line) runs here instead of its policy module
bool geom_hle_claims(u32 queue) { return g_on && (queue == 0x10C15100 || queue == 0x10C29100); }
bool (*g_geom_nj_handler)(SPUThread& s, u32 handler) = nullptr;   // spu_ops.h spu_fast_cmdlist
bool (*g_geom_nj_list)(SPUThread& s) = nullptr;
void geom_hle_add(u32 spurs, u32 wid, u32 queue) {
    g_spurs = spurs;
    if (!(getenv("F1CE_GEOM_NJ") && atoi(getenv("F1CE_GEOM_NJ")) == 0)) {
        g_geom_nj_handler = nj_handler;
        if (!(getenv("F1CE_GEOM_NJ_LIST") && atoi(getenv("F1CE_GEOM_NJ_LIST")) == 0)) g_geom_nj_list = nj_list;
    }
    LOG_I("geom hle: workload %u (queue %08x) runs without its policy module", wid, queue);
}
// Run by SPU worker `num` in place of the workload's PM (hle_spurs.cpp spu_worker): jobs until the queue is empty (its
// ready count then cleared as the PM does) or until another workload waits for an SPU (others_waiting: the PM's kernel
// poll after each job, [0x18e0]). Returns the jobs run.
u32 geom_hle_run(u32 num, u32 wid, u32 queue, bool (*yield)(u32 wid)) {
    thread_local std::unique_ptr<Pool> p;
    if (!p) {
        p = std::make_unique<Pool>();
        p->init(num);
        static const bool stats = getenv("F1CE_GEOM_HLE_STATS") != nullptr;
        if (stats) p->t.on_dma = [](SPUThread&, u32 cmd, u32, u32, u32) { if ((cmd & 0xFF) == 0xD0) g_llar.fetch_add(1, std::memory_order_relaxed); };
    }
    _mm_setcsr(_mm_getcsr() | 0x8040);   // flush denormals to zero, as the SPU does (SPUThread::run)
    const Queue q{queue, wid};
    u32 jobs = 0;
    bool holding = false;   // the last job's halfword is released by the next grab (or by leave when there is none)
    for (;;) {
        u32 j[3];
        const u64 g0 = time_usec();
        if (!p->grab(queue, j, holding)) {
            if (holding) { p->leave(queue); holding = false; }
            const u32 old = p->ready(wid, 0);
            const bool got = p->grab(queue, j);
            if (got) p->ready(wid, old, 0);
            g_grab_us.fetch_add(time_usec() - g0, std::memory_order_relaxed);
            if (!got) break;
        }
        const u64 t0 = time_usec();
        g_grab_us.fetch_add(t0 - g0, std::memory_order_relaxed);
        Current& cur = g_cur[num & 7];
        cur.spu.store(&p->t, std::memory_order_relaxed);
        cur.queue.store(queue, std::memory_order_relaxed);
        cur.idx.store(j[2], std::memory_order_relaxed);
        cur.since.store(t0, std::memory_order_release);
        p->run_job(q, j);
        cur.since.store(0, std::memory_order_release);
        static const bool merge = !(getenv("F1CE_GEOM_MERGE_LEAVE") && atoi(getenv("F1CE_GEOM_MERGE_LEAVE")) == 0);
        if (p->nwords || !merge) {   // counters to decrement: after the release, as the PM does
            p->leave(queue);
            for (u32 i = 0; i < p->nwords && i < 4; i++) p->notify(p->words[i]);
        } else {
            holding = true;
        }
        const u64 dt = time_usec() - t0;
        if (dt > 100000) {
            static std::atomic<int> logged{0};
            if (logged++ < 30) LOG_W("geom hle: job %u of queue %08x on spu%u took %.0f ms", j[2], queue, num, dt / 1e3);
        }
        g_usec.fetch_add(dt, std::memory_order_relaxed);
        if (dt > 50) { g_long.fetch_add(1, std::memory_order_relaxed); g_long_us.fetch_add(dt, std::memory_order_relaxed); }
        g_jobs.fetch_add(1, std::memory_order_relaxed);
        jobs++;
        if (yield(wid)) {
            if (holding) p->leave(queue);
            break;
        }
    }
    return jobs;
}
// F1CE_GEOM_OP_PROF=1: TSC ticks and calls per command handler (spu_ops.h spu_fast_cmdlist), logged with the report
bool g_geom_op_prof = getenv("F1CE_GEOM_OP_PROF") != nullptr;
namespace {
constexpr u32 OP_HANDLERS[41] = {0x08C80, 0x05818, 0x05860, 0x058F0, 0x07790, 0x07858, 0x079B0, 0x07A28, 0x07888, 0x0B300,
                                 0x08600, 0x07C08, 0x07C50, 0x07D78, 0x07E40, 0x07E70, 0x07EA0, 0x07ED0, 0x07FA8, 0x07FD8,
                                 0x08008, 0x08120, 0x08150, 0x0B700, 0x0B280, 0x08B88, 0x056B8, 0x07958, 0x05148, 0x08C10,
                                 0x09FD8, 0x0B020, 0x0B980, 0x0A098, 0x0A530, 0x0AFD0, 0x0AFF8, 0x05D30, 0x073D0, 0x05E28,
                                 0x06A30};
std::atomic<u64> g_op_tsc[42], g_op_n[42];
}  // namespace
void geom_op_prof(u32 handler, u64 tsc) {
    u32 i = 0;   // (handler 1 / 2: the native transform / cull loops, counted under op1 / op2: those opcodes never run)
    while (i < 41 && OP_HANDLERS[i] != handler) i++;
    if ((handler >= 1 && handler <= 7) || (handler >= 16 && handler <= 18) || (handler >= 26 && handler <= 29)) i = handler;   // (opcodes never run)
    g_op_tsc[i].fetch_add(tsc, std::memory_order_relaxed);
    g_op_n[i].fetch_add(1, std::memory_order_relaxed);
}
// watchdog / stall dump (hle_spurs.cpp spurs_watchdog_report): the job each worker is in, and for how long
void geom_hle_dump() {
    if (!g_on) return;
    const u64 now = time_usec();
    std::string out;
    for (u32 k = 0; k < 8; k++) {
        const u64 since = g_cur[k].since.load(std::memory_order_acquire);
        SPUThread* t = g_cur[k].spu.load(std::memory_order_relaxed);
        if (!since || !t) continue;
        char b[128];
        snprintf(b, sizeof(b), " spu%u: queue %08x job %u for %.1f ms pc=%05x n=%llu res=%08x", k, g_cur[k].queue.load(),
                 g_cur[k].idx.load(), (now - since) / 1e3, t->pc, (unsigned long long)t->instructions, t->res_addr);
        out += b;
    }
    LOG_I("  geom hle jobs:%s", out.empty() ? " none" : out.c_str());
}
// SPURS report line (every 5 s)
void geom_hle_report() {
    if (!g_on || !g_spurs) return;
    static u64 last[7] = {};
    const u64 v[7] = {g_jobs.load(), g_usec.load(), g_insns.load(), g_interp.load(), g_slow.load(), g_build.load(), g_grab_us.load()};
    const double jobs = std::max<u64>(1, v[0] - last[0]);
    LOG_I("geom hle: %.0f jobs/s, %.2f cores in jobs, %.2f in grabs; per job: %.0f instructions (%.0f interpreted), %.1f slow "
          "dispatches, %.2f blocks built", (v[0] - last[0]) / 5.0, (v[1] - last[1]) / 5e6, (v[6] - last[6]) / 5e6,
          (v[2] - last[2]) / jobs, (v[3] - last[3]) / jobs, (v[4] - last[4]) / jobs, (v[5] - last[5]) / jobs);
    for (int i = 0; i < 7; i++) last[i] = v[i];
    if (g_nj_used.load())
        LOG_I("geom nj: %llu jobs native (op35 %llu), %llu declined; run twice: %llu same, %llu differ",
              (unsigned long long)g_nj_used.load(), (unsigned long long)g_nj_op35.load(), (unsigned long long)g_nj_declined.load(),
              (unsigned long long)g_nj_twice_same.load(), (unsigned long long)g_nj_twice_differ.load());
    geom_sk_report();
    if (getenv("F1CE_GEOM_PC_HIST"))
        if (FILE* f = fopen("op0_hist.txt", "w")) {
            for (u32 i = 0; i < 0x10000; i++) if (const u32 n = g_pc_hist[i].load()) fprintf(f, "%05x %u\n", i * 4, n);
            fclose(f);
        }
    if (g_nj_same.load() + g_nj_differ.load() + g_nj_declined.load())
        LOG_I("geom nj check: %llu same, %llu differ, %llu declined", (unsigned long long)g_nj_same.load(),
              (unsigned long long)g_nj_differ.load(), (unsigned long long)g_nj_declined.load());
    if (g_geom_op_prof) {   // TSC ticks to microseconds through the host clock
        static u64 tsc0 = __rdtsc(), us0 = time_usec();
        const u64 tsc1 = __rdtsc(), us1 = time_usec();
        const double per_us = (double)(tsc1 - tsc0) / std::max<u64>(1, us1 - us0);
        tsc0 = tsc1; us0 = us1;
        std::string o;
        for (u32 i = 0; i < 42; i++) {
            const u64 n = g_op_n[i].exchange(0), t = g_op_tsc[i].exchange(0);
            if (!n) continue;
            char b[64];
            snprintf(b, sizeof(b), " op%u %.0fms/s %.2fus", i, t / per_us / 5e3, t / per_us / n);
            o += b;
        }
        LOG_I("geom hle ops:%s", o.c_str());
    }
    static const bool stats = getenv("F1CE_GEOM_HLE_STATS") != nullptr;
    if (!stats) return;
    static u64 last_l[3] = {};
    const u64 l[3] = {g_llar.load(), g_long.load(), g_long_us.load()};
    LOG_I("geom hle: per job %.2f GETLLARs; %.0f jobs/s over 50 us, %.2f cores in them", (l[0] - last_l[0]) / jobs,
          (l[1] - last_l[1]) / 5.0, (l[2] - last_l[2]) / 5e6);
    for (int i = 0; i < 3; i++) last_l[i] = l[i];
}
