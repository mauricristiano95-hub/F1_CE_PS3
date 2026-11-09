// Cell SPU interpreter: registers, local store, channels and MFC (DMA + atomics).
#pragma once
#include <memory>
#include <vector>
#include <algorithm>
#include "kernel.h"

#include <functional>

// ahead-of-time translated SPU blocks: key = FNV-1a 64 of the start pc and the instruction words
struct SpuAotBlock { u64 key; void (*fn)(struct SPUThread&); s32 cluster; };
extern const SpuAotBlock g_spu_aot[];
extern const u32 g_spu_aot_count;
// Linked blocks translated into one function that can be entered at any member (it dispatches on pc) and jumps
// between members directly. Usable only while every member's words are in the LS: hash = FNV-1a 64 over the
// members' words in table order; members = (pc, word count) pairs; [lo, hi) spans them.
struct SpuAotCluster {
    u64 hash; u32 lo, hi; const u32* members; u32 count; void (*fn)(struct SPUThread&);
    const u16* lines; u32 nlines;   // the 128-byte LS lines holding member words (guarded instead of the whole span)
};
extern const SpuAotCluster g_spu_aot_clusters[];
extern const u32 g_spu_aot_cluster_count;

struct SpuDmaRec { u32 cmd, lsa, ea, size, tag; };

// the C library's memcmp compares bytes; these are at most 128 bytes
FORCE_INLINE bool same_bytes(const u8* a, const u8* b, u32 n) {
    for (; n >= 32; a += 32, b += 32, n -= 32)
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)a), _mm256_loadu_si256((const __m256i*)b))) != -1)
            return false;
    for (; n; a++, b++, n--) if (*a != *b) return false;
    return true;
}

// at most 128 bytes, mostly whole lines: the C library call per line showed up in profiles
FORCE_INLINE void copy_bytes(u8* d, const u8* s, u32 n) {
    for (; n >= 32; d += 32, s += 32, n -= 32) _mm256_storeu_si256((__m256i*)d, _mm256_loadu_si256((const __m256i*)s));
    if (n) memcpy(d, s, n);
}

struct SPUThread {
    // Registers use the same byte-reversed layout as VMX registers: BE word i lives in w[3-i],
    // so the preferred slot (BE word 0) is w[3].
    v128 gpr[128];
    u8* ls;                     // 256 KiB local store (big-endian bytes)
    u32 pc = 0;
    u32 id = 0;
    std::string name;

    // channels / MFC
    u32 mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
    u32 tag_mask = 0, atomic_stat = 0;
    u32 event_mask = 0, event_stat = 0;
    u32 dec_start = 0;
    u64 dec_time = 0;
    u32 srr0 = 0;
    bool interrupts = false;    // enabled/disabled by the E/D bits of indirect branches
    // list DMA suspended at a stall-and-notify element, per tag
    struct StalledList { bool active = false, get = false; u32 list = 0, index = 0, count = 0, lsa = 0; };
    StalledList stalled[32];
    u32 stall_stat = 0;
    void run_list(u32 tag);

    bool has_reservation = false;
    u32 res_addr = 0, res_gen = 0;   // reservation line, its write generation at GETLLAR
    alignas(16) u8 res_data[128];
    u32 llar_polls = 0;   // GETLLARs in a row that found the reserved line unchanged (polling, see spu.cpp)
    // mailboxes
    std::mutex mbox_m;
    std::condition_variable mbox_cv;
    bool in_mbox_valid = false, out_mbox_valid = false;
    // SPURS tasks: spu_printf-style send_event reads a second word, the completion from the PPU-side handler
    bool event_needs_completion = false, event_completion_pending = false;
    u32 in_mbox = 0, out_mbox = 0;
    u32 snr[2] = {0, 0};

    // Called on 'stop' (code). Return true to continue at pc+4 (or wherever pc was set), false to halt.
    // SPU_RdMachStat bit 0 reports interrupts enabled even while this emulator keeps them off: the SPURS kernel runs
    // workloads and tasks with interrupts on, and job code asserts it (rdch MachStat; brz -> stopd)
    bool report_ie = false;
    std::vector<u32>* pc_trace = nullptr;   // debug: block start addresses of this run (F1CE_TRACE_WKL)
    std::function<bool(SPUThread&, u32 code)> on_stop;
    // Called when the SPU writes SPU_WrOutIntrMbox (sys_spu_thread_send_event / lv2 service request): the value
    // written, and the outbound mailbox word (data1) if one is pending. The result is put in the inbound mailbox.
    std::function<u32(SPUThread&, u32 intr, u32 data1)> on_send_event;
    // Called for branches that target the kernel area (pc < kernel_limit); return true if handled.
    u32 kernel_limit = 0;
    std::function<bool(SPUThread&)> on_kernel_call;

    // diagnostics: log LS accesses below trace_ls_limit (budgeted) and every MFC command
    u32 trace_ls_limit = 0, trace_budget = 0;
    // debug (F1CE_LS_WRITERS=<ls addr>): last writer of each quadword in [addr, addr + 0x800): pc (store) or
    // 0xD0000000 | ea >> 4 (DMA get), and the value of the last store
    u32 lsw_base = 0;
    u32 lsw_pc[128] = {};
    u32 lsw_val[128] = {};

    // block cache: basic blocks decoded once (handler + instruction word per op), invalidated per 128-byte line
    // when LS memory holding decoded code is written (stores, DMA)
    struct BlockOp { void (*h)(SPUThread&, u32); u32 w; };
    struct Block {
        u32 start = 0, n = 0, line0 = 0, line1 = 0;
        u64 ver_sum = 0;
        u64 checked = 0;   // region_writes[home] when the block was last validated
        u32 home = 0;      // the region whose epoch covers this block (see region_writes)
        void (*native)(SPUThread&) = nullptr;   // ahead-of-time translated version (generated/spu_aot*.cpp)
        s32 aot = -1;                            // its g_spu_aot index
        u32 count_n = 0;                         // instructions counted per dispatch (cluster code counts itself)
        const u16* lines = nullptr;              // cluster code: guarded lines (else line0..line1)
        u32 nlines = 0;
        s32 cluster = -1;                        // its g_spu_aot_clusters index
        std::vector<BlockOp> ops;
        std::unique_ptr<Block> alt;   // older variants decoded at the same pc (code swapped in and out by DMA)
    };
    // block slots per LS word in a two-level table (256 pages of 256 words, allocated where code runs), so the hot
    // part stays small in cache
    struct BlockPage { std::unique_ptr<Block> b[256]; };
    std::unique_ptr<BlockPage> block_pages[256];
    bool blocks_on = false;   // block cache in use (host LS writes must invalidate)
    bool no_aot = false;      // debug: this thread interprets everything (F1CE_SPU_NOAOT_TASK)
    Block* find_block(u32 pc) const {
        const BlockPage* p = block_pages[(pc >> 10) & 255].get();
        return p ? p->b[(pc >> 2) & 255].get() : nullptr;
    }
    std::unique_ptr<Block>& block_slot(u32 pc) {
        auto& p = block_pages[(pc >> 10) & 255];
        if (!p) p = std::make_unique<BlockPage>();
        return p->b[(pc >> 2) & 255];
    }
    // A line's version is a hash of its bytes, so job code swapped out and back (a few binaries alternate in one LS
    // buffer, ~2000 times per second per SPU) is valid again at once: with a counter, every return meant new lookups and
    // hashing whole clusters. line_code[l]: line_ver[l] is current. Writes only clear it, the hash is taken when a block
    // on the line is checked; an unmarked line is always hashed first, so no write can go unseen.
    u64 line_ver[2048] = {};
    u64 code_writes = 1;   // bumped whenever a line holding decoded code is written
    bool line_code[2048] = {};
    // Per-region epochs (4 KB regions): a job binary swapped in, or the code the job manager's interrupt handler
    // writes on its stack, leaves the policy module's blocks valid. region_links[r]: other regions with a block or
    // cluster that guards a line in r (their epoch is `home`, one region per block), so a write in r bumps them too.
    static constexpr u32 LINES_PER_REGION_SHIFT = 5;
    u64 region_writes[64];
    u64 region_links[64];
    __attribute__((noinline)) void code_hit(u32 l) {   // (rare: keeps the store path of translated code short)
        code_writes++;
        line_code[l] = false;
        u32 r = l >> LINES_PER_REGION_SHIFT;
        region_writes[r]++;
        for (u64 m = region_links[r]; m; m &= m - 1) region_writes[__builtin_ctzll(m)]++;
    }
    void link_region(u32 line, u32 home) {
        u32 r = line >> LINES_PER_REGION_SHIFT;
        if (r != home) region_links[r] |= 1ull << home;
    }
    void ls_written(u32 a, u32 n) {
        if (!blocks_on) return;
        for (u32 l = (a & 0x3FFFF) >> 7, e = ((a + std::max(n, 1u) - 1) & 0x3FFFF) >> 7; ; l = (l + 1) & 2047) {
            if (line_code[l]) code_hit(l);
            if (l == e) break;
        }
    }
    u64 line_version(u32 l) {
        if (!line_code[l]) {
            u64 w[16];
            memcpy(w, ls + l * 128, sizeof(w));
            u64 h = (l + 1) * 0x9E3779B97F4A7C15ull;
            for (u64 x : w) { h = (h ^ x) * 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
            line_ver[l] = h;
            line_code[l] = true;
        }
        return line_ver[l];
    }
    u64 st_fast = 0, st_slow = 0, st_build = 0;   // dispatch statistics (logged with the SPURS report)
    u64 st_slow_tsc = 0, st_build_tsc = 0;        // TSC ticks spent in slow dispatches / block rebuilds
    u64 st_interp = 0;   // instructions run by the interpreter (blocks without an ahead-of-time translation)
    std::vector<u64> cluster_ver;   // per cluster: version sum of its lines when its words last matched (0 = never)
    std::vector<u64> cluster_ok;    // per cluster: epoch of its home region when its lines last matched that sum
    u64 lines_ver(const u16* l, u32 n) {
        u64 v = 0;
        for (u32 i = 0; i < n; i++) v += line_version(l[i]);
        return v;
    }
    u64 block_ver(const Block& b) {
        if (b.lines) return lines_ver(b.lines, b.nlines);
        u64 v = 0;
        for (u32 l = b.line0; ; l = (l + 1) & 2047) { v += line_version(l); if (l == b.line1) break; }
        return v;
    }
    // copies into LS and invalidates decoded code only if the bytes change (re-loaded overlays keep their blocks)
    void write_ls(u32 lsa, const void* src, u32 n) {
        lsa &= 0x3FFFF;
        const u8* p = (const u8*)src;
        if (!blocks_on || lsa + n > 0x40000) {   // (wrapping writes are not expected: the slow path covers them)
            memmove(ls + lsa, p, n);
            if (blocks_on) ls_written(lsa, n);
            return;
        }
        // per line: a binary that shares lines with the previous one keeps the blocks on those lines
        while (n) {
            u32 k = std::min(n, 128 - (lsa & 127)), l = lsa >> 7;
            if (!line_code[l]) copy_bytes(ls + lsa, p, k);
            else if (!same_bytes(ls + lsa, p, k)) { copy_bytes(ls + lsa, p, k); code_hit(l); }
            lsa += k; p += k; n -= k;
        }
    }
    // a direct jump inside translated cluster code may skip the dispatcher only when the dispatcher would not act:
    // no code was written since entry, the target is not a kernel call and no interrupt is pending
    bool can_chain(u64 cw, u32 target) const {
        return code_writes == cw && target >= kernel_limit && !(interrupts && (event_stat & event_mask));
    }
    // dispatch cache: native code of recently run blocks in one compact direct-mapped table, valid while no code was
    // written since the block was verified (the block objects are scattered on the heap, and the cache misses of
    // the full lookup were the largest single cost of the SPU threads). Cluster code: cluster >= 0 (it counts itself),
    // and the entry stays good once any block of its cluster was verified again.
    // (Tried 2026-11-03: 16-byte entries, the table at 64 KB, were ~2% slower on the grid.)
    struct FastEnt { u64 checked; void (*fn)(SPUThread&); u32 pc, home; s32 cluster; u32 count_n; };
    FastEnt fast[4096] = {};
    Block* build_block(u32 pc);
    void flush_blocks();
    std::function<void(SPUThread&, u32 cmd, u32 lsa, u32 ea, u32 size)> on_dma;
    // checks (F1CE_PM_CHECK): when set, MFC commands are logged here and only gets reach memory (puts and atomics do not)
    std::vector<SpuDmaRec>* dma_log = nullptr;
    u32 dry_line_ea = ~0u;   // with dma_log: the line its conditional stores wrote, kept here instead of in memory
    u8 dry_line[128];
    // this thread runs the game's job manager policy module: its native functions may replace the SPU code (spu_pm.cpp)
    bool pm_native = false;

    bool halted = false;
    u64 instructions = 0;
    u32 pc_samples[256] = {};   // one PC every 4096 instructions (diagnostics)

    SPUThread();
    ~SPUThread();
    SPUThread(const SPUThread&) = delete;

    u32 ls32(u32 a) const { u32 v; memcpy(&v, ls + (a & 0x3FFFC), 4); return __builtin_bswap32(v); }
    void ls32(u32 a, u32 v) { v = __builtin_bswap32(v); memcpy(ls + (a & 0x3FFFC), &v, 4); }
    u32 pref(int r) const { return gpr[r].w[3]; }   // preferred word slot
    void set_pref(int r, u32 v) { gpr[r].pi = _mm_setzero_si128(); gpr[r].w[3] = v; }

    void run();            // execute until halted
    void dma(u32 cmd);     // perform an MFC command synchronously
};

// Loads an SPU ELF image (from guest memory) into local store; returns the entry point.
u32 spu_load_elf(SPUThread& spu, u32 elf_ea);

// Interprets from spu.pc until pc == to or the SPU halts; returns the instructions run, or ~0 after `limit` (checks).
u64 spu_interpret_to(SPUThread& spu, u32 to, u64 limit);

// native job manager (spu_pm.cpp): whether a policy module image is the one its native functions were written from
bool spu_pm_image_ok(const u8* image, u32 size);
extern bool g_pm_native;   // F1CE_PM_NATIVE=0: the original code only
