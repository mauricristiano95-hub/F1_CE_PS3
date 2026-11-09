// Native job manager: functions of the game's SPURS policy module (PM, guest 0x100be000, LS 0xA00..0x3580) rewritten in
// C++. Each one replaces the PM code at the head of its first block (tools/spu_aot.py SKIPS) and leaves the local store,
// the MFC state and the registers its callers read exactly as the SPU code would. The PM's own code was read from its
// disassembly; LS addresses below are the PM's data.
//
// PM data used here (LS):
//   0x0DF0 + slot*64 + set*4   buffer set descriptor D: A0 (first 1 KB block) << 24 | A1 (blocks per element) << 16 |
//                               0x8000 (declared) | A2 (elements, 7 bits) << 8 | A3 (first element entry)
//   0x0EB0 + 8*i               element entries E: w0 = flags (0x80000000 free, 0x40000000 / 0x20000000 output modes,
//                               0x10000000 handed over, 0x08000000 in use) | DMA tag << 22 | size/16 << 8 | age byte;
//                               w1 = EA | set index
//   0x11B0 + A3 + n            element chosen for a job's buffer n (byte)
//   0x1230/0x1240, 0x1250/0x1260, 0x1270/0x1280   256-bit maps of the 1 KB LS blocks: in use / output / declared
//   0x1290 DMA tags in use, 0x13B0/0x13C0 next free element entry / entries left, 0x13E0 job counter,
//   0x1430 + slot*32 slot info (w1: last element << 16 | 0xFFFF when the slot has outputs, w3: count), 0x1440 + slot*32
//   the slot's list of up to four words
//   0x1540 jump table of the buffer list entry types
// Three job slots (0..2) let the PM prefetch the next job's buffers while one runs.
//
// F1CE_PM_NATIVE=0 runs the original code. F1CE_PM_CHECK=1 runs every 64th call on two copies of the SPU as well, by the
// interpreter and natively, with DMA puts only logged (=2: every call), and compares local store, MFC state, DMA commands
// and results.
#include "spu.h"

#include <array>
#include <map>
#include <mutex>

bool g_pm_native = !(getenv("F1CE_PM_NATIVE") && atoi(getenv("F1CE_PM_NATIVE")) == 0);
static const int g_pm_check = getenv("F1CE_PM_CHECK") ? atoi(getenv("F1CE_PM_CHECK")) : 0;
// diagnostics (SPURS report): job queue entries the grab passed over as skips (kind 3), and the longest run of them
std::atomic<u64> g_pm_skips{0};
std::atomic<u32> g_pm_skip_run_max{0};
// F1CE_PM_SKIP_LOG: skips passed per queue since its last job (over several grabs)
static std::mutex g_skip_m;
static std::map<u32, u32> g_skip_run;

// Jobs a job manager has taken from a queue and not finished yet. The PPU's waits for a queue (fn 0x551228, 0x55127c,
// 0x5513b8) read the per-SPU halfwords of the queue line, which the PM rewrites from its job slots (0x3260); a job the
// grab took that is not in a slot yet (its buffers still loading) is missing from that minimum for a while, so a wait
// can return with the job still to run. The game compacts a frame list right after such a wait (fn 0x48ac34) while a
// w0 job is about to append in-order jobs to it: those land behind thousands of skip entries and the in-order chain
// never reaches them (the race freeze at Interlagos, reproduced by toggling pause, 2026-11-07). jobwait_* (PROBES at the
// waits' returns) also wait for the jobs listed here. A job is listed by the native grab, marked ended at the job
// service 0x1670 (its slot is [0x12e0] there) and finished once no PM slot holds it any more (the PM frees a slot when
// its outputs are back in main memory: they go out at the end of the next job on that SPU).
namespace {
struct Taken {
    std::atomic<u64> key{0};   // queue EA << 32 | index; 0 free, 1 being filled
    std::atomic<u64> since{0};
    std::atomic<bool> ended{false};
};
Taken g_taken[48];
std::atomic<u32> g_taken_lost{0}, g_taken_stale{0};
constexpr u64 TAKEN_MAX_US = 100000;   // listed longer: its end was not seen (a path left to the original code)
}  // namespace
SPUThread* spurs_worker_spu(u32 k);   // hle_spurs.cpp

static bool in_a_slot(u64 key) {
    for (u32 k = 0; k < 8; k++)
        if (const SPUThread* t = spurs_worker_spu(k))
            for (u32 s = 0; s < 3; s++)
                if (((u64)t->ls32(0x1430 + s * 32) << 32 | (t->ls32(0x1438 + s * 32) & 0xFFFF)) == key) return true;
    return false;
}
// a listed job that is finished (or stale) leaves the list; true when it is still open
static bool taken_open(Taken& e, u64 key, u64 now) {
    if (now - e.since.load(std::memory_order_relaxed) > TAKEN_MAX_US) {
        const bool unseen = !e.ended.load(std::memory_order_acquire);   // (an ended job still in a slot runs on)
        if (e.key.compare_exchange_strong(key, 0) && unseen) g_taken_stale++;
        return false;
    }
    if (!e.ended.load(std::memory_order_acquire) || in_a_slot(key)) return true;
    e.key.compare_exchange_strong(key, 0);
    return false;
}
void joblist_w0_event(char kind, u32 idx, u32 spu, u32 a, u32 b);   // hle_joblist.cpp (F1CE_JOBLIST_LOG)
static void inflight_add(u32 q, u32 idx) {
    if (q == 0x10410D80) joblist_w0_event('G', idx, 0, 0, 0);
    const u64 v = (u64)q << 32 | idx, now = time_usec();
    for (int pass = 0; pass < 2; pass++)
        for (auto& e : g_taken) {
            u64 k = e.key.load(std::memory_order_relaxed);
            if (pass && k > 1 && !taken_open(e, k, now)) k = e.key.load(std::memory_order_relaxed);   // reclaim
            u64 z = 0;
            if (k == 0 && e.key.compare_exchange_strong(z, 1)) {
                e.since.store(now, std::memory_order_relaxed);
                e.ended.store(false, std::memory_order_relaxed);
                e.key.store(v, std::memory_order_release);
                return;
            }
        }
    if (g_taken_lost++ == 0) LOG_W("pm: taken-jobs list full (queue %08x job %u not listed)", q, idx);
}
// 0x1670 (the job service at the end of a job), before it runs: the ending job's slot is [0x12e0]
void spu_pm_job_end(const SPUThread& t) {
    const u32 info = 0x1430 + (t.ls32(0x12E0) & 3) * 32;
    const u64 v = (u64)t.ls32(info) << 32 | (t.ls32(info + 8) & 0xFFFF);
    if ((u32)(v >> 32) == 0x10410D80) joblist_w0_event('E', (u32)v, t.id, 0, 0);
    for (auto& e : g_taken)
        if (e.key.load(std::memory_order_acquire) == v) { e.ended.store(true, std::memory_order_release); return; }
}
// a job of queue q below index `below` taken and not finished
bool job_inflight(u32 q, u32 below) {
    const u64 now = time_usec();
    bool open = false;
    for (auto& e : g_taken) {
        const u64 k = e.key.load(std::memory_order_acquire);
        if (k > 1 && (u32)(k >> 32) == q && (u32)k < below && taken_open(e, k, now)) open = true;
    }
    return open;
}

// FNV-1a 64 of the PM image the native functions were written from (checked when a workload is added)
bool spu_pm_image_ok(const u8* image, u32 size) {
    if (size != 0x2B80) return false;
    u64 h = 1469598103934665603ull;
    for (u32 a = 0; a < size; a++) { h ^= image[a]; h *= 1099511628211ull; }
    return h == 0xa3c735e64565f5c4ull;
}

namespace {
using u128 = unsigned __int128;
constexpr u128 ONES = ~(u128)0;

// quadword shifts with the SPU's count rules (shlqbybi / rotqmbybi / shlqbi / rotqmbi / shlqby)
FORCE_INLINE u128 shlqbybi(u128 v, u32 rb) { u32 n = (rb >> 3) & 31; return n > 15 ? 0 : v << (8 * n); }
FORCE_INLINE u128 rotqmbybi(u128 v, u32 rb) { u32 n = (0u - ((rb >> 3) & 31)) & 31; return n > 15 ? 0 : v >> (8 * n); }
FORCE_INLINE u128 shlqbi(u128 v, u32 rb) { return v << (rb & 7); }
FORCE_INLINE u128 rotqmbi(u128 v, u32 rb) { return v >> ((0u - rb) & 7); }
FORCE_INLINE u128 shlqby(u128 v, u32 rb) { u32 n = rb & 31; return n > 15 ? 0 : v << (8 * n); }
FORCE_INLINE u32 rotl32(u32 v, u32 n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
FORCE_INLINE s32 mpy16(u32 a, u32 b) { return (s32)(s16)(a & 0xFFFF) * (s32)(s16)(b & 0xFFFF); }

struct Masks { u128 m0, m1; };
// 0x2750: the bits [start, start + count) of a 256-bit block map, as its two quadwords
Masks block_mask(u32 start, u32 count) {
    const u32 end = start + count;
    const s32 t74 = (s32)(start - 128), t75 = (s32)(128 - end);
    const u32 a = t74 > -1 ? (u32)t74 : 0, b = t75 > -1 ? (u32)t75 : 0, e = 256 - end;
    const u128 q4 = shlqbi(shlqbybi(ONES, e), e), q75 = shlqbi(shlqbybi(ONES, b), b);
    const u128 q74 = rotqmbi(rotqmbybi(ONES, 7 - a), 0u - a), q3 = rotqmbi(rotqmbybi(ONES, 7 - start), 0u - start);
    return {q3 & q75, q74 & q4};
}

struct Pm {
    SPUThread& t;
    u8* const ls;
    u64 n = 0;   // SPU instructions the original would have run (an estimate: the worker loop uses the count)

    explicit Pm(SPUThread& th) : t(th), ls(th.ls) {}

    u8 rd8(u32 a) const { return ls[a & 0x3FFFF]; }
    u32 rd32(u32 a) const { u32 v; memcpy(&v, ls + (a & 0x3FFFC), 4); return __builtin_bswap32(v); }
    u128 ldq(u32 a) const {
        const u8* p = ls + (a & 0x3FFF0);
        u64 hi, lo;
        memcpy(&hi, p, 8);
        memcpy(&lo, p + 8, 8);
        return (u128)__builtin_bswap64(hi) << 64 | __builtin_bswap64(lo);
    }
    // stores as the SPU's: a store into decoded code invalidates it when the bytes change
    void put(u32 a, const void* src, u32 size) {
        u8* p = ls + a;
        const u32 l = (a >> 7) & 2047;
        if (__builtin_expect(t.line_code[l], 0)) {
            if (!memcmp(p, src, size)) return;
            memcpy(p, src, size);
            t.code_hit(l);
            return;
        }
        memcpy(p, src, size);
    }
    void stq(u32 a, u128 v) {
        u64 b[2] = {__builtin_bswap64((u64)(v >> 64)), __builtin_bswap64((u64)v)};
        put(a & 0x3FFF0, b, 16);
    }
    void st32(u32 a, u32 v) { v = __builtin_bswap32(v); put(a & 0x3FFFC, &v, 4); }
    void st8(u32 a, u8 v) { put(a & 0x3FFFF, &v, 1); }
    // a doubleword at an 8-aligned address (shufb with a cdd control), and its other half of the quadword
    void st64(u32 a, u32 w0, u32 w1) { st32(a & ~7u, w0); st32((a & ~7u) + 4, w1); }
    static u32 word(u128 q, int k) { return (u32)(q >> (96 - 32 * k)); }
    static u128 set_word(u128 q, int k, u32 v) {
        const int sh = 96 - 32 * k;
        return (q & ~((u128)0xFFFFFFFFu << sh)) | (u128)v << sh;
    }

    // MFC: the channel writes of the original (fields not written keep their values) and the command
    bool cmd(u32 pc, u32 c) {
        t.pc = pc;
        t.dma(c);
        return !t.halted;
    }
    void tag_wait(u32 mask) { t.tag_mask = mask; n += 11; }   // 0x2190: all DMA completes at once here
    // an element entry as the SPU code holds it: the doubleword at a (8-aligned), then the other half of its quadword
    void rd_entry(u32 a, u32 e[4]) const { e[0] = rd32(a); e[1] = rd32(a + 4); e[2] = rd32(a ^ 8); e[3] = rd32((a ^ 8) + 4); }
    // 0x1560 on the quadword of an entry: word k of the tags-in-use quadword loses the bit of tag (e[k] >> 22) & 31
    void free_tags(const u32 e[4]) {
        u128 q = ldq(0x1290);
        for (int k = 0; k < 4; k++) q = set_word(q, k, word(q, k) & ~(1u << ((e[k] >> 22) & 31)));
        stq(0x1290, q);
        n += 8;
    }
    void set_r3(u32 w0, u32 w1, u32 w2, u32 w3) { v128& r = t.gpr[3]; r.w[3] = w0; r.w[2] = w1; r.w[1] = w2; r.w[0] = w3; }
    static u128 quad(const v128& r) { return (u128)r.w[3] << 96 | (u128)r.w[2] << 64 | (u128)r.w[1] << 32 | r.w[0]; }
    static u128 splat(u32 v) { return (u128)v << 96 | (u128)v << 64 | (u128)v << 32 | v; }

    bool buffers();     // 0x19f8
    bool dma_info();    // 0x2d48
    bool release();     // 0x2ce8 (+ 0x2c80)
    bool job_done();    // 0x1670 up to 0x18e0 (+ 0x2940)
    bool grab();        // 0x2288
    bool irq_enter();   // 0x0a2c up to the event handler 0x21c0
    bool irq_leave();   // 0x0a98 to the iret
    void stack_code(u32 r1_0, u32 tmpl);
    bool evict(u32 d, u32 ea_ls, const u32 e[4], u32 start, u32 tag);   // 0x2848
    void fill(u32 start, u32 size, u64 pattern);   // 0x27b8
    void flush_outputs();   // 0x2940
};

// 0x27b8: fills [start, start + size) with a doubleword (8-aligned bounds), first and last quadword merged
void Pm::fill(u32 start, u32 size, u64 pattern) {
    const u128 pat = (u128)pattern << 64 | pattern;
    const u32 end = start + size, s8 = start & 8, e8 = end & 8;
    const u128 first_old = ldq(start), last_old = ldq(end);
    const u128 keep_first = shlqby(ONES, s8), take_last = shlqby(ONES, 16 - e8);
    const u128 first = (keep_first & first_old) | (~keep_first & pat), last = (take_last & pat) | (~take_last & last_old);
    u32 a73 = start + s8, body = size - s8 - e8;
    const u32 odd = body & 16;
    n += 20;
    if (size == 0) return;
    body -= odd;
    stq(start, first);
    u32 a74 = odd + a73;
    stq(end, last);
    if (odd) {
        stq(a73, pat);
        a73 += odd;
        if (body == 0) return;
    } else if (body == 0) {
        return;
    }
    do {
        body -= 32;
        stq(a74 + 16, pat);
        a74 = a73 + 32;
        stq(a73, pat);
        a73 += 32;
        n += 6;
    } while (body != 0);
}

// 0x2848: frees element e (at ea_ls, first block start, its set's descriptor d) for a buffer that needs its blocks. A
// written element is put back to main memory first (returns false: not free yet); else its DMA tag is waited for and
// released, the entry cleared and its blocks leave the output map (returns true). e: the entry's quadword rotated to it.
bool Pm::evict(u32 d, u32 ea_ls, const u32 e[4], u32 start, u32 tag) {
    n += 12;
    if (e[0] & 0x20000000) {
        u32 lsa = start << 10, ea = e[1] & ~15u, size = (e[0] >> 4) & 0x3FFF0;
        t.mfc_tag = tag;
        do {
            const u32 c = size > 0x4000 ? 0x4000 : size;
            t.mfc_lsa = lsa; t.mfc_eal = ea; t.mfc_size = c;
            if (!cmd(0x28AC, 0x20)) return false;
            size -= c; lsa += c; ea += c;
            n += 11;
        } while (size);
        st64(ea_ls, e[0] & ~0x20000000u, e[1] & ~0x20000000u);
        return false;
    }
    const u32 tg = (e[0] >> 22) & 31;
    if (tg) {
        tag_wait(1u << tg);
        free_tags(e);
    }
    st64(ea_ls, 0, e[1]);
    const Masks m = block_mask(start, (d >> 16) & 255);
    stq(0x1250, ldq(0x1250) & ~m.m0);
    stq(0x1260, ldq(0x1260) & ~m.m1);
    n += 40;
    return true;
}

// 0x19f8 (r3 = the other slot, r4 = this slot, r5 = the buffer list; returns r3 = 1 when every entry was done). Walks a
// job's buffer list: 8-byte entries w0 = size/16 << 18 | flags | type << 9 | set << 5 | buffer, w1 = EA or parameters.
// Done entries are zeroed; entries that must wait (blocks still in use, write-backs under way) stay for the next call.
bool Pm::buffers() {
    const u32 other = t.pref(3), slot = t.pref(4), ret = t.pref(0) & 0x3FFFC;
    const v128 slotq = t.gpr[4];
    const bool this_is_next = rd32(0x12D0) == slot;   // r13: the slot the next job uses (DMA tags 1/3, else 2/4)
    u32 e = t.pref(5);   // r2
    bool ok = true;      // r10
    bool wrote = false;  // r9: a write-back was started for the last entry that needed blocks of the other slot
    n += 16;
    for (;;) {
        // 0x1a1c
        const u32 w0 = rd32(e), w1 = rd32(e + 4), ea = e;   // ea: r14
        e += 8;
        n += 7;
        if (w0 == 0) continue;
        const u32 type = (w0 >> 9) & 7;
        if (type == 7) break;
        const u32 handler = rd32(0x1540 + type * 4);
        // 0x2c00: the set's descriptor and the element of buffer (w0 & 31)
        const u32 set = (w0 >> 5) & 15, buf = w0 & 31;
        const u32 daddr = 0xDF0 + slot * 64 + set * 4, d = rd32(daddr);
        const u32 a0 = d >> 24, a1 = (d >> 16) & 255, a2 = (d >> 8) & 127, a3 = d & 255;
        const u32 map = 0x11B0 + a3 + buf, b = rd8(map);
        const u32 elem0 = 0xEB0 + a3 * 8;   // r26
        u32 eaddr = 0xEB0 + (a3 + b) * 8;   // r28
        u32 r27 = b, r31 = (u32)(mpy16(a1, b) + (s32)a0);
        u32 r29w0 = rd32(eaddr), r29w1 = rd32(eaddr + 4);
        n += 30;
        // the entry's quadword with the entry zeroed (done) or with w0 | 0x10000 (to retry with its element)
        auto clear_entry = [&] { st64(ea, 0, 0); };
        if (handler == 0x2170) break;
        if (handler == 0x1A60) {   // declare a buffer set: A2 elements of A1 blocks from block A0
            const u32 b4 = w1 >> 24, b5 = (w1 >> 16) & 255, b6 = (w1 >> 8) & 255;
            const u128 q13b0 = ldq(0x13B0);
            const u32 ptr = word(q13b0, 0);
            const u32 dn = ((w1 & 0xFFFFFF00u) | (ptr & 0xFF)) | 0x8000;
            const Masks m = block_mask(b4, (u32)mpy16(b5, b6));
            const u128 c0 = ldq(0x1270), c1 = ldq(0x1280);
            n += 60;
            if ((c0 & m.m0) | (c1 & m.m1)) { ok = false; continue; }
            // r17 = [dn, the next three words of D's quadword]; byte 2 of each word & 127 is that word's count
            u32 cnt[4];
            cnt[0] = (dn >> 8) & 127;
            for (int k = 1; k < 4; k++) cnt[k] = (rd32((daddr & ~15u) | ((daddr + 4 * k) & 15)) >> 8) & 127;
            const u128 q13c0 = ldq(0x13C0);
            u128 p = q13b0, r = q13c0;
            for (int k = 0; k < 4; k++) {
                p = set_word(p, k, word(p, k) + cnt[k]);
                r = set_word(r, k, word(r, k) - cnt[k]);
            }
            stq(0x1270, c0 | m.m0);
            stq(0x1280, c1 | m.m1);
            st32(daddr, dn);
            clear_entry();
            stq(0x13B0, p);
            stq(0x13C0, r);
            fill(0xEB0 + rotl32(ptr, 3), cnt[0] << 3, (u64)0x80000000u << 32 | ((w0 >> 5) & 15));
            continue;
        }
        if (handler == 0x1B38) {   // a job buffer: pick an element of set `set`, get its data
            const bool direct = w0 & 0x10000;   // retried entry: element already chosen (map)
            const u32 set4 = (w0 >> 3) & 60;
            n += 10;
            bool handed = false;   // element taken over from the other slot (data already in place)
            if (!direct) {
                if (d == 0) { ok = false; continue; }
                const u32 dother = rd32(0xDF0 + other * 64 + set4);
                const u32 a3o8 = (dother & 255) << 3, a2o = (dother >> 8) & 127;
                const bool same = (d >> 16) == (dother >> 16);   // r17: same blocks in both slots
                n += 20;
                if ((w0 & 0x8000) && same) {
                    // 0x1ba0: element k of the other slot holds this EA and is complete: take it over
                    u32 k = 0, cur = elem0, oth = a3o8;
                    while ((s32)a2 > (s32)k) {
                        const u32 ew0 = rd32(cur);
                        const u32 ow0 = rd32(0xEB0 + oth), ow1 = rd32(0xEB0 + oth + 4);
                        const u32 r18 = oth;
                        k++; cur += 8; oth += 8;
                        n += 30;
                        r29w0 = ew0; r29w1 = rd32(cur - 8 + 4);
                        if ((ow1 ^ w1) > 15u || !(ow0 & 0x40000000) || !(ew0 & 0x80000000)) continue;
                        k--;
                        r31 = (u32)(mpy16(k, a1) + (s32)a0);
                        st64(0xEB0 + r18, (ow0 & ~0x6FC00000u) | 0x10000000, ow1);
                        eaddr = cur - 8;
                        r27 = k;
                        r29w0 = ow0 | 0x08000000; r29w1 = ow1;
                        handed = true;
                        break;
                    }
                }
                if (!handed) {
                    // 0x1c3c: the best free element whose blocks are not in use
                    const u32 counter = rd32(0x13E0);
                    const u128 ia0 = ldq(0x1230), ia1 = ldq(0x1240), ob0 = ldq(0x1250), ob1 = ldq(0x1260);
                    u32 k = 0, cur = elem0, oth = a3o8, best = 0, best_k = 0;
                    while ((s32)a2 > (s32)k) {
                        const u32 ew0 = rd32(cur), ew1 = rd32(cur + 4);
                        const u32 ow0 = rd32(0xEB0 + oth);
                        const u32 i = k;
                        k++;
                        const u32 start = (u32)(mpy16(i, a1) + (s32)a0);
                        cur += 8; oth += 8;
                        r29w0 = ew0; r29w1 = ew1;
                        n += 18;
                        if (!(ew0 & 0x80000000)) continue;
                        const Masks m = block_mask(start, a1);
                        const bool any_a = ((ia0 & m.m0) | (ia1 & m.m1)) != 0, any_b = ((ob0 & m.m0) | (ob1 & m.m1)) != 0;
                        const bool has_o = (s32)a2o > (s32)i;
                        u32 score = 0x10000000 | (has_o ? (counter - ew0) & 255 : 255);
                        if ((ow0 & 0x40000000) && same) score ^= 0x40000000;
                        if (!any_b) score |= 0x20000000;
                        n += 70;
                        if ((s32)score > (s32)best && !(any_a || (any_b && wrote))) { best = score; best_k = i; }
                    }
                    if (best == 0) { ok = false; continue; }
                    r27 = best_k;
                    r31 = (u32)(mpy16(a1, best_k) + (s32)a0);
                    eaddr = elem0 + best_k * 8;
                }
            }
            if (!handed) {
                // 0x1d48: blocks of the other slot's outputs that overlap ours (or hold our EA) must be free first
                r29w0 = 0x08000000;
                const u32 end = r31 + a1;
                const u32 iw1 = rd32(0x1430 + other * 32 + 4);
                n += 20;
                if (iw1 & 0xFFFF) {
                    bool waiting = false;   // r22
                    u32 idx = other * 32, off = other * 256;
                    const u32 lim = iw1 >> 16;
                    bool busy = false;
                    while ((s32)lim > (s32)idx) {
                        const u32 at = 0xEB0 + off;
                        idx++; off += 8;
                        n += 13;
                        u32 eo[4];
                        eo[0] = rd32(at); eo[1] = rd32(at + 4);
                        if (!(eo[0] & 0x40000000)) continue;
                        const u32 k = idx - 1;
                        const u32 dd = rd32(0xDF0 + other * 64 + (eo[1] & 15) * 4);
                        const bool ea_differs = (w1 ^ eo[1]) > 15u;
                        const u32 so = (u32)(mpy16((dd >> 16) & 255, k - (dd & 255)) + (s32)(dd >> 24));
                        n += 22;
                        if ((eo[0] & 0x08000000) && !ea_differs) { busy = true; break; }
                        const bool overlap = (s32)end > (s32)so && (s32)(((dd >> 16) & 255) + so) > (s32)r31;
                        if (!overlap && ea_differs) continue;
                        // the entry's quadword rotated to it: its other doubleword follows
                        const u32 pair = at ^ 8;
                        eo[2] = rd32(pair); eo[3] = rd32(pair + 4);
                        const bool freed = evict(dd, at, eo, so, this_is_next ? 3 : 4);
                        if (t.halted) { t.instructions += n; return true; }
                        waiting |= !freed;
                        wrote = waiting;
                    }
                    if (busy) { ok = false; continue; }
                    if (waiting) {
                        // 0x2004: keep the element for the retry
                        st32(ea, w0 | 0x10000);
                        st64(eaddr, r29w0, r29w1);
                        st8(map, (u8)r27);
                        ok = false;
                        n += 18;
                        continue;
                    }
                }
                // 0x1e3c: the transfer
                r29w1 = (r29w1 & 15) | (w1 & ~15u);
                r29w0 = (r29w0 & ~0x003FFF00u) | ((w0 >> 10) & 0x003FFF00u);
                const u32 endb = a1 + r31;
                n += 20;
                if (w0 & 0x8000) {
                    tag_wait(0x20);
                    const u32 tag = this_is_next ? 1 : 2;
                    const u32 size = (r29w0 & 0x003FFF00u) >> 4, src = r29w1 & ~15u;
                    u32 lsa = r31 << 10;
                    if (w0 & 0x4000) {   // a DMA list: elements fetched to the end of the buffer, then the list
                        const u32 list = (endb << 10) - size - ((w1 & 7) << 4);
                        t.mfc_lsa = list; t.mfc_eal = src; t.mfc_size = size; t.mfc_tag = tag;
                        if (!cmd(0x1EDC, 0x40)) { t.instructions += n; return true; }
                        t.mfc_lsa = lsa; t.mfc_eal = list;
                        if (!cmd(0x1EEC, 0x45)) { t.instructions += n; return true; }
                        n += 18;
                    } else {
                        u32 rem = size, from = src;
                        bool last;
                        do {
                            last = 0x4000 > (s32)rem;
                            const u32 c = last ? rem : 0x4000;
                            t.mfc_lsa = lsa; t.mfc_eal = from; t.mfc_tag = tag; t.mfc_size = c;
                            rem -= 0x4000;
                            lsa += c; from += c;
                            if (!cmd(0x1F24, 0x40)) { t.instructions += n; return true; }
                            n += 12;
                        } while (!last);
                    }
                }
            }
            // 0x1f30: the element belongs to this job now
            const u32 mode = w0 & 0x3000;
            const u32 info = 0x1430 + slot * 32;
            u128 iq = ldq(info);
            if (mode) iq = set_word(iq, 1, word(iq, 1) | 0xFFFF);
            r29w0 = (r29w0 & ~0x60000000u) | (mode << 17);
            r29w0 = (r29w0 & ~0xFFu) | (rd32(0x13E0) & 0xFF);
            stq(info, iq);
            const Masks m = block_mask(r31, a1);
            const u128 ia0 = ldq(0x1230), ia1 = ldq(0x1240), ob0 = ldq(0x1250), ob1 = ldq(0x1260);
            const u128 out = mode ? ONES : 0;
            st64(eaddr, r29w0, r29w1);
            st8(map, (u8)r27);
            clear_entry();
            stq(0x1230, ia0 | m.m0);
            stq(0x1240, ia1 | m.m1);
            stq(0x1250, (m.m0 & out) | (~m.m0 & ob0));
            stq(0x1260, (m.m1 & out) | (~m.m1 & ob1));
            n += 75;
            continue;
        }
        if (handler == 0x2048) {   // release buffer sets (bits of w1)
            n += 2;
            if (!ok) continue;
            clear_entry();
            u32 bits = w1;
            const u32 base = 0xDF0 + slot * 64;
            do {
                const s32 i = 31 - (s32)(bits ? __builtin_clz(bits) : 32);
                const u32 cnt = (u32)i & 63;
                bits &= ~(cnt < 32 ? 1u << cnt : 0u);
                const u32 at = base + (u32)i * 4;
                const u32 dd = rd32((at & ~15u) | (((u32)i * 4) & 15));
                const u32 dc = dd & ~0x8000u;
                const Masks m = block_mask(dc >> 24, (u32)mpy16((dd >> 8) & 127, (dd >> 16) & 255));
                st32((at & ~15u) | (((u32)i * 4) & 15), dc);
                stq(0x1270, ldq(0x1270) & ~m.m0);
                stq(0x1280, ldq(0x1280) & ~m.m1);
                n += 66;
            } while (bits);
            continue;
        }
        if (handler == 0x20E8) {   // append w1 to the slot's word list
            const u32 info = 0x1430 + slot * 32;
            const u32 cnt = rd32(info + 12);
            st32(info + 0x10 + (cnt & 3) * 4, w1);
            clear_entry();
            st32(info + 12, cnt + 1);
            n += 19;
            continue;
        }
        if (handler == 0x2134) {   // end of the list: what the job start needs, unless an entry must wait
            if (wrote) {   // 0x26e4: a stall-and-notify list on tag 3 raises an interrupt once the write-backs are out
                t.mfc_tag = 3; t.mfc_lsa = 0; t.mfc_eal = 0x14F0; t.mfc_size = 0x10;
                if (!cmd(0x2708, 0x45)) { t.instructions += n; return true; }
                n += 12;
            }
            n += 6;
            if (!ok) break;
            // the registers of 0x2c00 for this entry, whole quadwords: r15 = the entry and the other half of its quadword
            u32 r15[4] = {w0, w1, rd32(ea ^ 8), rd32((ea ^ 8) + 4)};
            u128 r20 = 0, r28 = 0;
            for (int k = 0; k < 4; k++) {
                r20 = set_word(r20, k, (slotq.w[3 - k] << 6) + (((r15[k] >> 5) & 15) << 2) + 0xDF0);
                const u32 b_k = rd8((map & ~15u) | ((map + 4 * k) & 15));
                r28 = set_word(r28, k, k ? 0xEB0 + rotl32(b_k, 3) : eaddr);
            }
            const u32 next = e + (e & 8);
            const u128 q1420 = ldq(0x1420);
            u128 top = 0;
            for (int k = 0; k < 4; k++) top = set_word(top, k, word(q1420, k) >> 28);
            stq(0x1310, r20);
            stq(0x1320, r28);
            stq(0x1330, (u128)rotl32(r31, 10) << 96);
            stq(0x1360, ((u128)next << 96) | ((u128)next << 64) | ((u128)next << 32) | next);
            stq(0x13D0, top);
            n += 8;
            break;
        }
        fatal("SPU %s: native job manager: unknown buffer list handler %05x (type %u)", t.name.c_str(), handler, type);
    }
    t.set_pref(3, ok ? 1 : 0);
    t.gpr[3].w[0] = t.gpr[3].w[1] = t.gpr[3].w[2] = t.gpr[3].w[3];   // il / ila: every word
    t.pc = ret;
    t.instructions += n;
    return true;
}

// 0x2d48 (job service: r4 = buffer set, r5 = buffer, r6 != 0: give it a DMA tag). Returns in r3 what a job needs to DMA
// the buffer: [set << 24 | buffer << 16 | tag, first block (low byte) << 24 | blocks << 8, EA, size]. An element handed
// over from the other slot keeps its tag in the next slot's entry. With every tag in use the original waits for a job to
// end (0x3130): left to it (nothing is written before that point).
bool Pm::dma_info() {
    const v128 in4 = t.gpr[4], in5 = t.gpr[5];
    const u32 want_tag = t.pref(6), ret = t.pref(0) & 0x3FFFC;
    const auto byte = [](const v128& v, int j) { return (u32)v.b[15 - j]; };
    // 0x2c00 (r3 = the running slot)
    const u32 slot = rd32(0x12E0), set = in4.w[3], buf = in5.w[3];
    const u32 d = rd32(0xDF0 + (slot << 6) + (set << 2));
    const u32 a0 = d >> 24, a1 = (d >> 16) & 255, a3 = d & 255;
    const u32 b = rd8(0x11B0 + a3 + buf);
    const u32 eaddr = 0xEB0 + ((a3 + b) << 3);
    const u32 start = (u32)(mpy16(a1, b) + (s32)a0);
    u32 e[4];
    rd_entry(eaddr, e);
    n += 32 + 15;
    u32 f[4] = {e[0], e[1], e[2], e[3]}, fa = eaddr;
    if (e[0] & 0x10000000) {
        const u32 next = rd32(0x12D0);
        const u32 r10 = byte(in4, 3) << 24 | byte(in4, 4) << 16 | byte(in4, 5) << 8 | byte(in4, 6);
        const u32 dn = rd32(0xDF0 + (next << 6) + (r10 << 2));
        fa = 0xEB0 + (((dn & 255) + b) << 3);
        rd_entry(fa, f);
        n += 15;
    }
    u32 tag = (f[0] & 0x07C00000) >> 22;
    n += 22;
    if (want_tag != 0 && tag == 0) {
        // 0x2710: the highest free tag (each word of the quadword likewise)
        u128 q = ldq(0x1290);
        if (word(q, 0) == 0xFFFFFFFFu) return false;
        u32 idx0 = 0;
        for (int k = 0; k < 4; k++) {
            const u32 w = word(q, k);
            const u32 idx = 31u - (~w ? (u32)__builtin_clz(~w) : 32u), c = idx & 63;
            q = set_word(q, k, w | (c < 32 ? 1u << c : 0u));
            if (k == 0) idx0 = idx;
        }
        stq(0x1290, q);
        tag = idx0;
        f[0] = (f[0] & ~0x07C00000u) | (rotl32(idx0, 22) & 0x07C00000u);
        st64(fa, f[0], f[1]);
        n += 20;
    }
    set_r3(byte(in4, 3) << 24 | byte(in5, 3) << 16 | (tag & 0xFFFF), (start & 0xFF) << 24 | a1 << 8, e[1] & ~15u,
           ((e[0] >> 8) & 0x3FFF) << 4);
    t.pc = ret;
    t.instructions += n;
    return true;
}

// 0x2ce8 (job service, via 0x1968 / 0x1988: r4 = buffer set, r5 = buffer): the job is done with a buffer. Its DMA is
// waited for, the entry cleared, (0x2c80:) the tag released unless the element is an output, and its blocks leave the
// in-use map. Returns r3 = all ones. Its interrupt window (0x2d14) would take an interrupt only with an event pending:
// left to the original then. Interrupts are disabled afterwards.
bool Pm::release() {
    if (t.event_stat & t.event_mask) return false;
    const u32 set = t.pref(4), buf = t.pref(5), ret = t.pref(0) & 0x3FFFC;
    const u32 slot = rd32(0x12E0);
    const u32 d = rd32(0xDF0 + (slot << 6) + (set << 2));
    const u32 a0 = d >> 24, a1 = (d >> 16) & 255, a3 = d & 255;
    const u32 b = rd8(0x11B0 + a3 + buf);
    const u32 eaddr = 0xEB0 + ((a3 + b) << 3);
    u32 e[4];
    rd_entry(eaddr, e);
    const u32 tag = (e[0] >> 22) & 31;
    n += 32 + 12;
    if (tag) tag_wait(1u << tag);
    st64(eaddr, 0, 0);
    // 0x2c80
    if (tag) {
        tag_wait(1u << tag);
        if (!(e[0] & 0x40000000)) free_tags(e);
    }
    const Masks m = block_mask((u32)(mpy16(a1, b) + (s32)a0), a1);
    stq(0x1230, ldq(0x1230) & ~m.m0);
    stq(0x1240, ldq(0x1240) & ~m.m1);
    n += 50;
    t.interrupts = false;
    set_r3(~0u, ~0u, ~0u, ~0u);
    t.pc = ret;
    t.instructions += n;
    return true;
}

// 0x2940: the outputs of the slot in [0x1300] (the job that ended) go back to main memory (0x2848, DMA tag 5). When it
// found any, 0x26e4 asks for an interrupt once tag 5 is done (a stall-and-notify list). Returns to 0x18e0 (its caller).
void Pm::flush_outputs() {
    const u32 s = rd32(0x1300);
    const u32 iw1 = rd32(0x1430 + (s << 5) + 4);
    const u32 lim = iw1 >> 16;
    const u32 dbase = 0xDF0 + (s << 6);
    u32 k = s << 5, at = 0xEB0 + (s << 8);
    u32 any = 0;   // r69
    n += 16;
    if (iw1 & 0xFFFF) {
        for (;;) {
            u32 e[4];
            rd_entry(at, e);
            n += 5;
            if (e[0] & 0x40000000) {
                const u32 d = rd32(dbase + ((e[1] & 15) << 2));
                const u32 start = (u32)(mpy16((d >> 16) & 255, k - (d & 255)) + (s32)(d >> 24));
                n += 16;
                const bool freed = evict(d, at, e, start, 5);
                if (t.halted) return;
                any |= freed ? ~1u : ~0u;
            }
            k++;
            at += 8;
            n += 5;
            if ((k & 0xFFFF) == (lim & 0xFFFF)) break;
        }
    }
    t.set_pref(4, 0x18E0);
    if (any) {
        t.mfc_tag = 5; t.mfc_lsa = 0; t.mfc_eal = 0x14F0; t.mfc_size = 0x10;
        cmd(0x2708, 0x45);
        set_r3(0x45, 0x45, 0x45, 0x45);
        t.gpr[5].w[0] = t.gpr[5].w[1] = t.gpr[5].w[2] = t.gpr[5].w[3] = 0x10;
        n += 18;
    } else {
        set_r3(~0u, ~0u, ~0u, ~0u);
        n += 3;
    }
}

// 0x1670 (job service, at the end of a job) up to 0x18e0. For the running slot ([0x12e0]): buffer sets that the slot of
// [0x1300] has declared and this one has not are declared here too where their blocks are free of this slot's sets (that
// check reads only the first descriptor of each quadword, as the original does), elements whose data the other slot holds
// are handed over, then the outputs of the [0x1300] slot are flushed (0x2940). The SPU code continues at 0x18e0 (kernel
// poll, next job). Needs a 16-aligned stack pointer (insertion controls are made from it).
bool Pm::job_done() {
    const u32 sp = t.pref(1);
    if (sp & 15) return false;
    stq(sp - 128, quad(t.gpr[1]));   // back chain
    stq(sp + 16, quad(t.gpr[0]));    // link register, in the caller's frame
    t.gpr[1].pi = _mm_add_epi32(t.gpr[1].pi, _mm_set1_epi32(-128));
    stq(0x12B0, splat(2));
    const u32 other = rd32(0x1300), slot = rd32(0x12E0);
    n += 20;
    if (rd32(0x1430 + (other << 5) + 4) & 0xFFFF) {
        // 0x16bc: blocks of this slot's sets (the first descriptor of each quadword, four times each)
        u128 used0 = 0, used1 = 0;
        for (u32 i = 0; i < 16; i++) {
            const u32 q0 = rd32((0xDF0 + (slot << 6) + 4 * i) & ~15u);
            const Masks m = block_mask(q0 >> 24, (u32)mpy16((q0 >> 8) & 127, (q0 >> 16) & 255));
            used0 |= m.m0;
            used1 |= m.m1;
            n += 40;
        }
        // 0x1718: declare the other slot's sets here. The element pointer advances for sets skipped for their blocks too.
        u128 ptrq = ldq(0x13B0);
        for (u32 i = 0; i < 16; i++) {
            const u32 da = 0xDF0 + (slot << 6) + 4 * i, oa = 0xDF0 + (other << 6) + 4 * i;
            const u32 di = rd32(da);
            u32 o[4];   // the other slot's descriptor quadword rotated to set i
            for (int k = 0; k < 4; k++) o[k] = rd32((oa & ~15u) | ((oa + 4 * k) & 15));
            const u128 rem = ldq(0x13C0);
            const u32 a2o = (o[0] >> 8) & 127;
            n += 20;
            if (di != 0 || o[0] == 0 || (s32)a2o > (s32)word(rem, 0)) continue;
            const Masks m = block_mask(o[0] >> 24, (u32)mpy16((o[0] >> 16) & 255, a2o));
            const u32 ptr = word(ptrq, 0);
            u128 rem2 = rem;
            for (int k = 0; k < 4; k++) {
                const u32 c = (o[k] >> 8) & 127;
                rem2 = set_word(rem2, k, word(rem, k) - c);
                ptrq = set_word(ptrq, k, word(ptrq, k) + c);
            }
            n += 60;
            if ((used0 & m.m0) | (used1 & m.m1)) continue;
            const u32 info = 0x1430 + (slot << 5);
            const u128 iq = ldq(info);
            stq(0x13C0, rem2);
            stq(0x13B0, ptrq);
            stq(info, set_word(iq, 1, word(iq, 1) | 0xFFFF));
            st32(da, (o[0] & 0xFFFFFF00u) | (ptr & 0xFF));
            fill(0xEB0 + (ptr << 3), a2o << 3, (u64)0x80000000u << 32 | i);
        }
        // 0x1800: free elements of this slot take over the data the other slot's matching element holds
        const u32 ptr = rd32(0x13B0);
        const u32 dslot = 0xDF0 + (slot << 6), doth = 0xDF0 + (other << 6);
        for (u32 k = slot << 5, at = 0xEB0 + (slot << 8); k != ptr; k++, at += 8) {
            const u128 qk = ldq(at);
            const u32 ew0 = rd32(at), ew1 = rd32(at + 4);
            n += 9;
            if (!(ew0 & 0x80000000)) continue;
            const u32 si = (ew1 & 15) << 2;
            const u32 dd = rd32(dslot + si), dot = rd32(doth + si);
            const u32 oa = 0xEB0 + (((dot & 255) + (k - (dd & 255))) << 3);
            const u128 qo = ldq(oa);   // loaded before the first store (the same quadword when both slots are one)
            const u32 ow0 = rd32(oa), ow1 = rd32(oa + 4);
            n += 18;
            if ((dd >> 16) != (dot >> 16) || !(ow0 & 0x40000000)) continue;
            const auto ins = [](u128 q, u32 a, u32 w0, u32 w1) {
                const int k0 = (a & 8) ? 2 : 0;
                return set_word(set_word(q, k0, w0), k0 + 1, w1);
            };
            stq(at, ins(qk, at, ow0 & ~0x08000000u, ow1));
            stq(oa, ins(qo, oa, (ow0 & 0x08000000) ? ow0 & ~0x67C00000u : 0, ow1));
            n += 12;
        }
    }
    // 0x18c8: the slot's last element
    const u32 info = 0x1430 + (slot << 5);
    st32(info + 4, (rd32(0x13B0) & 0xFFFF) << 16 | (rd32(info + 4) & 0xFFFF));
    n += 6;
    flush_outputs();
    t.pc = 0x18E0;
    t.instructions += n;
    return true;
}

// 0x2288 (r3 = the job queue's line, r4 = this SPU's number; returns through r5): takes the next entry of the job queue.
// Queue line: +0x10 word = high << 16 | head, then a halfword per SPU (the lowest index it took); entries of 8 bytes
// from +0x20 (low halfword of the first word: 0 none, 1 job, 2 wait until the halfword at the EA in the second word is 0,
// 3 skip). The head moves by an atomic update of the line (GETLLAR / PUTLLC through the MFC, as the original: only the
// code between them is native, so the reservation rules are the same). The line of the entry is cached at LS 0xd00
// ([0x1490]: its EA), a waited-for record at 0x14a0 ([0x14b0]). Returns r3 = [entry, 0, head] for a job or no entry,
// 0 when a wait entry is not satisfied yet.
bool Pm::grab() {
    if (t.interrupts && (t.event_stat & t.event_mask)) return false;
    const v128 q3 = t.gpr[3];
    const u32 ea = q3.w[3], spu = t.pref(4), ret = t.pref(5) & 0x3FFFC;
    const u32 pos = (2 * spu + 4) & 14;   // this SPU's halfword in the quadword at LS 0x90
    n += 4;
    u32 skips = 0;
    struct SkipCount {
        u32& s;
        ~SkipCount() {
            if (!s) return;
            g_pm_skips.fetch_add(s, std::memory_order_relaxed);
            u32 m = g_pm_skip_run_max.load(std::memory_order_relaxed);
            while (s > m && !g_pm_skip_run_max.compare_exchange_weak(m, s)) {}
        }
    } skip_count{skips};
    for (;;) {
        t.mfc_lsa = 0x80; t.mfc_eal = ea;
        if (!cmd(0x22AC, 0xD0)) { t.instructions += n; return true; }
        t.atomic_stat = 0;
        const u128 q90 = ldq(0x90);
        const u32 w0 = word(q90, 0), h = w0 & 0xFFFF, top = w0 >> 16;
        const u32 mine = (u32)(q90 >> (8 * (14 - ((2 * spu + 4) & 15)))) & 0xFFFF;   // bytes (2*spu+4)&15, +1
        const u32 at = ea + 32 + h * 8, line = at & ~127u, off = at & 127;
        n += 36;
        if (rd32(0x1490) != line) {
            t.mfc_lsa = 0xD00; t.mfc_eal = line; t.mfc_size = 0x80; t.mfc_tag = 8;
            if (!cmd(0x2328, 0x40)) { t.instructions += n; return true; }
            t.tag_mask = 0x100;
            u128 c = 0;
            for (int k = 0; k < 4; k++) c = set_word(c, k, (q3.w[3 - k] + 32 + (k ? 0 : h * 8)) & ~127u);
            stq(0x1490, c);
            n += 9;
        }
        const u32 ent = 0xD00 + off;
        const u32 e0 = rd32(ent), e1 = rd32(ent + 4), kind = e0 & 0xFFFF;
        static const bool stale_log = getenv("F1CE_JOBLIST_LOG") != nullptr;
        if (stale_log && kind == 3 && (ld32(at) & 0xFFFF) != 3) {   // debug: the cached line is older than the entry
            static std::atomic<int> n{0};
            if (n++ < 60)
                LOG_W("pm: queue %08x entry %u is a skip in the cached line but %08x %08x in memory (head %u top %u)",
                      ea, h, ld32(at), ld32(at + 4), h, top);
        }
        const u32 h1 = (h + 1) & 0xFFFF, top1 = top > h1 ? top : h1;
        u128 nq = set_word(q90, 0, top1 << 16 | h1);
        n += 14;
        if (kind == 0) {   // no entry
            if (skips && getenv("F1CE_PM_SKIP_LOG")) {   // (debug) skips before the end of the queue, for the next job
                std::lock_guard lk(g_skip_m);
                g_skip_run[ea] += skips;
            }
            stq(0x1490, 0);
            stq(0x14B0, 0);
            set_r3(e0, e1, 0, h);
            break;
        }
        if (kind != 3 && kind != 1) {   // 0x23a8: wait until a counter is 0
            n += 4;
            if (rd32(0x14B0) != e1) {
                u128 c = 0;   // r66: the entry's quadword rotated to its second word
                const u32 x0 = rd32(ent ^ 8), x1 = rd32((ent ^ 8) + 4);
                c = set_word(set_word(set_word(set_word(c, 0, e1), 1, x0), 2, x1), 3, e0);
                stq(0x14B0, c);
                t.mfc_lsa = 0x14A0; t.mfc_eal = e1 & ~15u; t.mfc_size = 0x10; t.mfc_tag = 8;
                if (!cmd(0x23D0, 0x40)) { t.instructions += n; return true; }
                t.tag_mask = 0x100;
                const u32 cnt = (u32)rd8(0x14A0 + (e1 & 15)) << 8 | rd8(0x14A0 + ((e1 + 1) & 15));
                n += 14;
                if (cnt != 0) {
                    stq(0x1490, 0);
                    stq(0x14B0, 0);
                    set_r3(0, 0, 0, 0);
                    break;
                }
            }
        }
        if (kind == 1) {   // this SPU's halfword: the lowest index it took
            const u32 low = mine > h ? h : mine;
            const int sh = 8 * (14 - pos);
            nq = (nq & ~((u128)0xFFFF << sh)) | (u128)low << sh;
        }
        stq(0x90, nq);
        t.mfc_lsa = 0x80; t.mfc_eal = ea;
        if (!cmd(0x2388, 0xB4)) { t.instructions += n; return true; }
        const u32 st = t.atomic_stat;
        t.atomic_stat = 0;
        n += 8;
        if (kind == 3 && !(st & 1)) skips++;
        if (kind != 1 || (st & 1)) continue;   // a wait or skip entry passed, or the update failed: again
        stq(0x14B0, 0);
        set_r3(e0, e1, 0, h);
        inflight_add(ea, h);
        static const bool skip_log = getenv("F1CE_PM_SKIP_LOG") != nullptr;
        if (skip_log) {   // debug: a job that follows a long run of skips (also over several grabs)
            static int logged = 0;
            std::lock_guard lk(g_skip_m);
            const u32 total = g_skip_run[ea] + skips;
            g_skip_run[ea] = 0;
            if (total >= 96 && logged++ < 200)
                LOG_I("pm: queue %08x job %u after %u skips: %08x %08x (binary %08x)", ea, h, total, e0, e1,
                      ld32(e1 + 0x14));
        }
        break;
    }
    t.pc = ret;
    t.instructions += n;
    return true;
}

// The interrupt handler (vector at LS 0: bra 0xa2c) saves r0..r79 below the interrupted stack pointer with code it writes
// there: 19 quadwords made from the template of four stores at 0xa40 (each step adds 4 to the register and 64 to the
// offset), then `bi r0`, called; after the event handler (0x21c0) the same with the loads at 0xae0. The code is written
// as the original writes it (the local store stays the same), the saves and loads are done here.
void Pm::stack_code(u32 r1_0, u32 tmpl) {
    u32 w[4];
    for (int j = 0; j < 4; j++) w[j] = rd32(tmpl + 4 * j);
    for (u32 k = 0; k < 19; k++) {
        u128 q = 0;
        for (int j = 0; j < 4; j++) q = set_word(q, j, w[j] + 0x10004 * (k + 1));
        stq(r1_0 - 3312 - 16 * k, q);
    }
    stq(r1_0 - 3296, splat(0x35000000));   // bi r0
    n += 19 * 5 + 6;
}
bool Pm::irq_enter() {
    const v128 r1 = t.gpr[1];
    const u32 sp = r1.w[3];
    stq(0x1500, quad(t.gpr[78]));
    t.gpr[78].pi = _mm_setzero_si128();
    for (int r = 0; r < 4; r++) stq(sp - 3280 + 16 * r, quad(t.gpr[r]));
    stack_code(sp, 0xA40);
    for (int r = 4; r < 80; r++) stq(sp - 3280 + 16 * r, quad(t.gpr[r]));
    t.gpr[1].pi = _mm_add_epi32(r1.pi, _mm_set1_epi32(-3312));
    t.gpr[2].pi = _mm_sub_epi32(r1.pi, _mm_set1_epi32(0xE10));
    t.gpr[3].pi = _mm_set1_epi32(-3312);
    t.set_pref(0, 0xA98);
    n += 76 + 14;
    t.pc = 0x21C0;
    t.instructions += n;
    return true;
}
bool Pm::irq_leave() {
    t.gpr[1].pi = _mm_add_epi32(t.gpr[1].pi, _mm_set1_epi32(0xCF0));
    const u32 sp = t.pref(1);
    stack_code(sp, 0xAE0);
    for (int r = 4; r < 80; r++) {
        const u128 q = ldq(sp - 3280 + 16 * r);
        v128& g = t.gpr[r];
        g.w[3] = word(q, 0); g.w[2] = word(q, 1); g.w[1] = word(q, 2); g.w[0] = word(q, 3);
    }
    for (int r : {3, 2, 1, 0}) {   // r0 from the reloaded r1
        const u128 q = ldq(t.pref(1) - 3280 + 16 * r);
        v128& g = t.gpr[r];
        g.w[3] = word(q, 0); g.w[2] = word(q, 1); g.w[1] = word(q, 2); g.w[0] = word(q, 3);
    }
    n += 76 + 20;
    if (t.pref(78) != 0) {
        t.pc = t.pref(78) & 0x3FFFC;
    } else {
        const u128 q = ldq(0x1500);
        v128& g = t.gpr[78];
        g.w[3] = word(q, 0); g.w[2] = word(q, 1); g.w[1] = word(q, 2); g.w[0] = word(q, 3);
        t.pc = t.srr0 & 0x3FFFC;
        t.interrupts = true;
    }
    t.instructions += n;
    return true;
}

// ------------------------------------------------------------------ check (F1CE_PM_CHECK)
struct PmCheck {
    std::unique_ptr<SPUThread> a = std::make_unique<SPUThread>(), b = std::make_unique<SPUThread>();
    std::vector<SpuDmaRec> la, lb;
};
void copy_spu(SPUThread& d, const SPUThread& s) {
    memcpy(d.ls, s.ls, 0x40000);
    memcpy(d.gpr, s.gpr, sizeof(d.gpr));
    d.pc = s.pc; d.id = s.id; d.name = s.name;
    d.mfc_lsa = s.mfc_lsa; d.mfc_eah = s.mfc_eah; d.mfc_eal = s.mfc_eal; d.mfc_size = s.mfc_size; d.mfc_tag = s.mfc_tag;
    d.tag_mask = s.tag_mask; d.atomic_stat = s.atomic_stat; d.event_mask = s.event_mask; d.event_stat = s.event_stat;
    d.srr0 = s.srr0; d.interrupts = s.interrupts; d.stall_stat = s.stall_stat;
    for (int i = 0; i < 32; i++) d.stalled[i] = s.stalled[i];
    d.halted = false;
    d.instructions = 0;
    d.dry_line_ea = ~0u;
    d.has_reservation = s.has_reservation; d.res_addr = s.res_addr; d.res_gen = s.res_gen;
    memcpy(d.res_data, s.res_data, 128);
}
bool same_spu(const SPUThread& a, const SPUThread& b, const std::vector<int>& regs, std::string& why) {
    char m[200];
    if (memcmp(a.ls, b.ls, 0x40000)) {
        u32 k = 0;
        while (a.ls[k] == b.ls[k]) k++;
        k &= ~15u;
        snprintf(m, sizeof(m), "LS %05x: %08x %08x %08x %08x vs %08x %08x %08x %08x", k, a.ls32(k), a.ls32(k + 4), a.ls32(k + 8),
                 a.ls32(k + 12), b.ls32(k), b.ls32(k + 4), b.ls32(k + 8), b.ls32(k + 12));
        why = m;
        return false;
    }
    for (int r : regs)
        if (memcmp(&a.gpr[r], &b.gpr[r], 16)) {
            const v128 &x = a.gpr[r], &y = b.gpr[r];
            snprintf(m, sizeof(m), "r%d %08x %08x %08x %08x vs %08x %08x %08x %08x", r, x.w[3], x.w[2], x.w[1], x.w[0], y.w[3], y.w[2],
                     y.w[1], y.w[0]);
            why = m;
            return false;
        }
    if (a.pc != b.pc || a.interrupts != b.interrupts) {
        snprintf(m, sizeof(m), "pc %05x ints %d vs pc %05x ints %d", a.pc, a.interrupts, b.pc, b.interrupts);
        why = m;
        return false;
    }
    if (a.mfc_lsa != b.mfc_lsa || a.mfc_eal != b.mfc_eal || a.mfc_size != b.mfc_size || a.mfc_tag != b.mfc_tag ||
        a.tag_mask != b.tag_mask || a.event_stat != b.event_stat || a.stall_stat != b.stall_stat) {
        snprintf(m, sizeof(m), "MFC lsa %x/%x eal %x/%x size %x/%x tag %u/%u mask %x/%x ev %x/%x stall %x/%x", a.mfc_lsa, b.mfc_lsa,
                 a.mfc_eal, b.mfc_eal, a.mfc_size, b.mfc_size, a.mfc_tag, b.mfc_tag, a.tag_mask, b.tag_mask, a.event_stat,
                 b.event_stat, a.stall_stat, b.stall_stat);
        why = m;
        return false;
    }
    for (int i = 0; i < 32; i++) {
        const auto &x = a.stalled[i], &y = b.stalled[i];
        if (x.active != y.active || (x.active && (x.get != y.get || x.list != y.list || x.index != y.index || x.count != y.count || x.lsa != y.lsa))) {
            snprintf(m, sizeof(m), "stalled list on tag %d", i);
            why = m;
            return false;
        }
    }
    return true;
}

struct FnCheck {
    const char* name; u32 entry;
    std::atomic<u32> calls{0}, checked{0}, bad{0}, declined{0}, with_dma{0};
    std::atomic<u64> max_steps{0};
};

// one native function against the interpreter, on copies of the SPU: from `entry` to the pc the native code leaves at;
// regs: the registers its callers read
void check_fn(SPUThread& t, FnCheck& c, const std::vector<int>& regs, bool (Pm::*fn)(), u32 lock_ea) {
    const u32 call = c.calls++;
    if (g_pm_check < 2 && (call & 63)) return;
    // the line both runs update atomically stays still meanwhile (their GETLLARs do not wait for its lock in a check)
    std::unique_lock<SpinLock> held;
    if (lock_ea) held = std::unique_lock<SpinLock>(atomic_lock(lock_ea));
    thread_local PmCheck p;
    SPUThread& a = *p.a;
    SPUThread& b = *p.b;
    copy_spu(a, t);
    copy_spu(b, t);
    p.la.clear(); p.lb.clear();
    b.dma_log = &p.lb;
    Pm pm(b);
    const bool done = (pm.*fn)();
    b.dma_log = nullptr;
    if (!done) { c.declined++; return; }
    a.dma_log = &p.la;
    a.pc = c.entry;
    const u64 steps = spu_interpret_to(a, b.pc, 20000000);
    a.dma_log = nullptr;
    if (steps == ~0ull) { LOG_W("pm check %s: the interpreter did not reach %05x", c.name, b.pc); return; }
    const u32 no = c.checked++;
    if (!p.la.empty()) c.with_dma++;
    if (steps > c.max_steps) c.max_steps = steps;
    std::string why;
    bool same = same_spu(a, b, regs, why);
    if (same && p.la.size() != p.lb.size()) { same = false; why = "DMA command count " + std::to_string(p.la.size()) + " vs " + std::to_string(p.lb.size()); }
    for (size_t i = 0; same && i < p.la.size(); i++) {
        const SpuDmaRec &x = p.la[i], &y = p.lb[i];
        if (memcmp(&x, &y, sizeof(SpuDmaRec))) {
            char m[160];
            snprintf(m, sizeof(m), "DMA %zu: cmd %02x lsa %05x ea %08x size %x tag %u vs cmd %02x lsa %05x ea %08x size %x tag %u", i, x.cmd,
                     x.lsa, x.ea, x.size, x.tag, y.cmd, y.lsa, y.ea, y.size, y.tag);
            why = m;
            same = false;
        }
    }
    if (!same && c.bad++ < 20) {
        LOG_W("pm check %s: DIFFERENT (%s); %llu instructions, %zu DMA commands", c.name, why.c_str(), (unsigned long long)steps, p.la.size());
        if (c.bad <= 3) {
            char fn[48];
            snprintf(fn, sizeof(fn), "pm_check_%s_%u.ls", c.name, c.bad.load());
            if (FILE* f = fopen(fn, "wb")) { fwrite(t.ls, 1, 0x40000, f); fwrite(t.gpr, 1, sizeof(t.gpr), f); fclose(f); }
        }
    }
    if ((no & 1023) == 0)
        LOG_I("pm check %s: %u compared, %u differ, %u declined, %u with DMA, longest %llu instructions (last: %llu, estimate %llu)",
              c.name, no + 1, c.bad.load(), c.declined.load(), c.with_dma.load(), (unsigned long long)c.max_steps.load(),
              (unsigned long long)steps, (unsigned long long)b.instructions);
}

FnCheck g_chk_buffers{"019f8", 0x19F8}, g_chk_dma_info{"02d48", 0x2D48}, g_chk_release{"02ce8", 0x2CE8}, g_chk_job_done{"01670", 0x1670},
    g_chk_grab{"02288", 0x2288}, g_chk_irq_enter{"00a2c", 0xA2C}, g_chk_irq_leave{"00a98", 0xA98};
const std::vector<int> g_first80 = [] { std::vector<int> v; for (int r = 0; r < 80; r++) v.push_back(r); return v; }();

template <class F> bool run_native(SPUThread& t, FnCheck& c, const std::vector<int>& regs, F fn, u32 lock_ea = 0) {
    if (__builtin_expect(g_pm_check != 0, 0)) check_fn(t, c, regs, fn, lock_ea);
    Pm pm(t);
    return (pm.*fn)();
}
}  // namespace

// Entry points, at the head of each function's first block (spu_ops.h). Each returns false to leave the call to the SPU code.

// 0x19f8. Interrupts on (not seen there) are left to the original: its notify list (0x26e4) could raise one inside it.
bool spu_pm_buffers(SPUThread& t) {
    if (t.interrupts || (t.pref(5) & 7)) return false;
    return run_native(t, g_chk_buffers, {3}, &Pm::buffers);
}
bool spu_pm_dma_info(SPUThread& t) { return run_native(t, g_chk_dma_info, {3}, &Pm::dma_info); }
bool spu_pm_release(SPUThread& t) { return run_native(t, g_chk_release, {3}, &Pm::release); }
bool spu_pm_job_done(SPUThread& t) { return run_native(t, g_chk_job_done, {1, 3, 4}, &Pm::job_done); }
bool spu_pm_grab(SPUThread& t) { return run_native(t, g_chk_grab, {3}, &Pm::grab, t.pref(3) & ~127u); }
bool spu_pm_irq_enter(SPUThread& t) { return run_native(t, g_chk_irq_enter, g_first80, &Pm::irq_enter); }
bool spu_pm_irq_leave(SPUThread& t) { return run_native(t, g_chk_irq_leave, g_first80, &Pm::irq_leave); }

// PROBES at the returns of the PPU's job waits (tools/recomp.py): the counters the game read are past its target; wait
// also until no job below it is still taken and unfinished (see g_taken), and after the waits for a whole queue (a frame
// list: fn 0x5513b8 from 0x48ac34, which then resets it for the next frame; w0: fn 0x55127c, after which the frame end
// compacts the current list) until no job manager is still inside its workload. A PM leaves an emptied queue by
// releasing its halfword, grabbing again, clearing its ready count and grabbing once more: in paused frames main reset
// the list and reserved the next frame's entries within that gap (a host thread can lose the CPU there for a time
// slice), the PM took the reserve's skip entries before the writers filled them and the frame's jobs were lost (the same
// freeze). Without the w0 part a w0 job still appended in-order jobs after the compaction (1 freeze in 4 repro runs).
// Bounded: a job whose end was not seen, or a PM that stays, cannot hang the game.
u32 spurs_workers_on_queue(u32 q);   // hle_spurs.cpp
static void wait_jobs(u32 q, u32 below, bool reset_next) {
    auto busy = [&] { return job_inflight(q, below) || (reset_next && spurs_workers_on_queue(q)); };
    if (!busy()) return;
    const bool inflight0 = job_inflight(q, below);
    const u32 inside0 = reset_next ? spurs_workers_on_queue(q) : 0;
    static std::atomic<u64> waits{0}, waited_us{0}, max_us{0}, window{0};
    const u64 t0 = time_usec();
    u64 now = t0;
    for (int spins = 0; busy(); spins++) {
        now = time_usec();
        if (now - t0 > 20000) {
            static std::atomic<int> warned{0};
            if (warned++ < 5)
                LOG_W("pm: queue %08x still busy after 20 ms (taken jobs below %u: %d, job managers inside: %u); not waiting longer",
                      q, below, job_inflight(q, below), spurs_workers_on_queue(q));
            break;
        }
        if (spins < 64) _mm_pause(); else std::this_thread::yield();
    }
    const u64 us = now - t0;
    static std::atomic<int> long_logged{0};
    if (us > 2000 && long_logged++ < 30)
        LOG_I("job wait long: queue %08x below %u reset_next %d: %llu us (taken open %d, managers inside %u at start)", q, below,
              reset_next, (unsigned long long)us, inflight0, inside0);
    waits++;
    waited_us += us;
    for (u64 m = max_us.load(); us > m && !max_us.compare_exchange_weak(m, us);) {}
    u64 w = window.load();
    if (now - w >= 5000000 && window.compare_exchange_strong(w, now) && w)
        LOG_I("job waits: %llu waited for taken jobs or job managers (%llu us, longest %llu us) in 5 s; %u taken jobs whose end "
              "was not seen", (unsigned long long)waits.exchange(0), (unsigned long long)waited_us.exchange(0),
              (unsigned long long)max_us.exchange(0), g_taken_stale.exchange(0));
}
void jobwait_551228(PPUContext& c) { wait_jobs(ld32((u32)c.r[3] + 4), (u32)c.r[4] & 0xFFFF, false); }   // r4 = target
void jobwait_55127c(PPUContext& c) { wait_jobs(ld32((u32)c.r[3] + 4), 0x10000, true); }                 // every job
void jobwait_5513b8(PPUContext& c) { wait_jobs(ld32((u32)c.r[3] + 4), ld16((u32)c.r[3] + 38), true); }
// fn 0x551410 (before its blr): the non-blocking form of these waits, called only by the music stream's MP3 decoder
// (fn 0x536a0c, which polls it with 200 us sleeps after pushing a decode job). r3 = 1 when the per-SPU halfwords are
// past r7 (the first free entry), r8 = the queue line. The same gap as above let it report a just-grabbed decode job as
// done: the stream read the job's sample count before the job wrote it and once got -12, so the copy into the music
// ring (fn 0xb3a7c) ran off the end of user memory at 0x40000000 (crash after a 2-hour session, 2026-11-09). A job
// still taken and unfinished keeps the answer at "not done"; the decoder polls again.
void jobpoll_551410(PPUContext& c) {
    if (!c.r[3] || !job_inflight((u32)c.r[8], (u32)c.r[7] & 0xFFFF)) return;
    c.r[3] = 0;
    static std::atomic<u64> held{0}, window{0};
    held++;
    const u64 now = time_usec();
    u64 w = window.load();
    if (now - w >= 5000000 && window.compare_exchange_strong(w, now))
        LOG_I("job polls: MP3 decode job held back %llu times since the last report (taken, not finished)",
              (unsigned long long)held.exchange(0));
}
