// Debug: trace of how the frame job lists (w1/w2) and the w0 queue are built and consumed (F1CE_JOBLIST_LOG=1), made
// for the in-order job freeze of 2026-11-07 (README progress log). Probes (tools/recomp.py PROBES) at the entry of the
// PPU's list builders: 0x55165c / 0x5516f0 (job, r4 = list, r5 = descriptor), 0x551780 / 0x5517f8 (raw entry), 0x551878
// (wait entry), 0x5515d0 (reserve of r4 skip entries, r3 = list), 0x550f7c (one alignment skip), 0x5514a4 (atomic push);
// at the frame end's w0 drain (0x489cb8), previous-list wait (0x48aca8) and compaction (0x48acc8). SPU writes to the
// queues come from SPUThread::dma, ready-count stores and workers entering a workload from hle_spurs.cpp, grabs and job
// ends from spu_pm.cpp. Per frame list a timeline of the current and previous cycle is kept and printed when the
// watchdog sees the same job manager wait twice (or for the first cycles after F1CE_JOBLIST_DUMP_AT seconds); jobs
// written behind a queue's head are logged at once (the job manager has passed them: they never run).
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "kernel.h"
#include "spu.h"

extern std::atomic<int> g_worker_wid[8];   // hle_spurs.cpp: workload each SPU worker runs

namespace {
struct Ev {
    u32 tid, idx, count, lr;
    char kind;   // J job, O ordered job, R reserve, P alignment skip, E raw entry, W wait entry
    u32 bt[6];
};
struct ListTrace {
    u32 queue = 0;
    u32 last_idx = 0;
    u32 skip_run = 0;   // skip entries appended since the last non-skip entry
    Ev ring[48];
    u32 pos = 0;
};
std::mutex g_m;
ListTrace g_lists[8];
int g_logged = 0;

// Timeline of one cycle of each frame list (from a PPU write below the previous one to the next), printed for the
// first 3 cycles after F1CE_JOBLIST_DUMP_AT seconds (default 60): PPU events and SPU entry writes, merged into ranges.
struct Tl { char src, kind; u32 a, b, who; u32 head; u64 t; u32 tid, caller; };   // SPU: tid/caller = the writer's
// current PM slot info words 0 and 2 (its job's queue and index)
struct Timeline {
    std::vector<Tl> ev, prev;   // this cycle, the one before
    u32 last_ppu = 0, printed = 0;
    u64 t0 = 0, prev_t0 = 0;
};
std::mutex g_tl_m;
Timeline g_tl[3];   // w1, w2, w0

void print_events(u32 q, const std::vector<Tl>& ev, u64 t0, const char* which) {
    LOG_I("joblist: %s cycle of queue %08x (%zu ranges):", which, q, ev.size());
    struct { u64 t0; } t{t0};
    std::string line;
    for (const Tl& e : ev) {
        char b[96];
        if (e.src == 'P')
            snprintf(b, sizeof(b), " P%c%u-%u@%u[t%x %06x<%06x h%u]", e.kind, e.a, e.b, (u32)((e.t - t.t0) / 100),
                     e.tid & 0xFF, e.who, e.caller, e.head);
        else
            snprintf(b, sizeof(b), " S%c%u-%u@%u[%cw%x %05x job %x:%u h%u]", e.kind, e.a, e.b, (u32)((e.t - t.t0) / 100),
                     (char)('0' + (e.who >> 24)), (e.who >> 20) & 15, e.who & 0xFFFFF, e.tid, e.caller & 0xFFFF, e.head);
        line += b;
        if (line.size() > 900) { LOG_I("joblist: %s", line.c_str()); line.clear(); }
    }
    if (!line.empty()) LOG_I("joblist: %s", line.c_str());
}
void print_cycle(u32 q, const Timeline& t) {
    if (!t.prev.empty()) print_events(q, t.prev, t.prev_t0, "previous");
    print_events(q, t.ev, t.t0, "current");
}

int tl_index(u32 q) { return q == 0x10C15100 ? 0 : q == 0x10C29100 ? 1 : q == 0x10410D80 ? 2 : -1; }

void tl_add(u32 q, char src, char kind, u32 idx, u32 count, u32 who, u32 tid = 0, u32 caller = 0) {
    const int i = tl_index(q);
    if (i < 0) return;
    static const double at = getenv("F1CE_JOBLIST_DUMP_AT") ? atof(getenv("F1CE_JOBLIST_DUMP_AT")) : 60.0;
    static const u64 start = time_usec();
    const u64 now = time_usec();
    std::lock_guard lk(g_tl_m);
    Timeline& t = g_tl[i];
    {
        if (idx < 4 && t.last_ppu > 100) {   // a new cycle: print the one that ended
            if (now - start >= (u64)(at * 1e6) && t.printed < 3 && !t.ev.empty()) {
                t.printed++;
                print_cycle(q, t);
            }
            t.prev.swap(t.ev);
            t.ev.clear();
            t.prev_t0 = t.t0;
            t.t0 = now;
        }
        t.last_ppu = idx;
    }
    const u32 end = idx + (count ? count - 1 : 0);
    if (!t.ev.empty()) {
        Tl& l = t.ev.back();
        if (l.src == src && l.kind == kind && l.who == who && l.caller == caller && idx == l.b + 1) { l.b = end; return; }
    }
    if (t.ev.size() < 4000) t.ev.push_back({src, kind, idx, end, who, ld16(q + 0x12), now, tid, caller});
}

bool enabled() {
    static const bool on = getenv("F1CE_JOBLIST_LOG") != nullptr;
    return on;
}

ListTrace& list_of(u32 q) {
    for (auto& l : g_lists)
        if (l.queue == q) return l;
    for (auto& l : g_lists)
        if (!l.queue) { l.queue = q; return l; }
    return g_lists[7];
}

void record(PPUContext& c, u32 obj, char kind, u32 count) {
    if (!mem::readable(obj, 0x28)) return;
    const u32 q = ld32(obj + 4), idx = ld32(obj + 0x24);
    Ev e{};
    e.tid = t_self ? t_self->id : 0;
    e.idx = idx;
    e.count = count;
    e.lr = (u32)c.lr;
    e.kind = kind;
    u32 sp = (u32)c.r[1];
    for (int d = 0; d < 6 && sp >= mem::STACK_BASE && sp < mem::STACK_BASE + mem::STACK_SIZE; d++) {
        const u32 next = (u32)ld64(sp);
        if (next <= sp || next >= mem::STACK_BASE + mem::STACK_SIZE) break;
        e.bt[d] = (u32)ld64(next + 16);
        sp = next;
    }
    tl_add(q, 'P', kind, idx, count, e.bt[0], e.tid, e.bt[1]);
    std::lock_guard lk(g_m);
    static u64 window = 0;
    static u32 calls[128], max_count = 0;
    calls[(u8)kind & 127]++;
    if (count > max_count) max_count = count;
    if (const u64 now = time_usec(); now - window >= 5000000) {
        if (window)
            LOG_I("joblist: 5 s: J %u O %u E %u W %u R %u (max %u) P %u", calls['J'], calls['O'], calls['E'], calls['W'],
                  calls['R'], max_count, calls['P']);
        window = now;
        memset(calls, 0, sizeof(calls));
        max_count = 0;
    }
    ListTrace& l = list_of(q);
    if (idx < l.last_idx) l.skip_run = 0;   // the list was reset
    l.last_idx = idx;
    l.ring[l.pos++ % 48] = e;
    if (kind == 'R' || kind == 'P') {
        l.skip_run += count;
        return;
    }
    const u32 run = l.skip_run;
    l.skip_run = 0;
    if (run < 96 || (kind != 'J' && kind != 'O')) return;
    static int plain_logged = 0;
    if (kind == 'J' ? plain_logged++ >= 3 : g_logged++ >= 40) return;
    LOG_W("joblist: queue %08x %s job at %u after %u skip entries (thread %08x); last events:", q,
          kind == 'O' ? "in-order" : "plain", idx, run, e.tid);
    for (u32 k = l.pos > 48 ? l.pos - 48 : 0; k < l.pos; k++) {
        const Ev& x = l.ring[k % 48];
        LOG_W("joblist:   %c idx %u count %u thread %08x lr %08x bt %08x %08x %08x %08x %08x %08x", x.kind, x.idx,
              x.count, x.tid, x.lr, x.bt[0], x.bt[1], x.bt[2], x.bt[3], x.bt[4], x.bt[5]);
    }
}

char job_kind(u32 desc) { return mem::readable(desc + 0x30, 4) && ld32(desc + 0x30) == 0x01000200 ? 'O' : 'J'; }
}  // namespace

void joblist_job(PPUContext& c) { if (enabled()) record(c, (u32)c.r[4], job_kind((u32)c.r[5]), 1); }
void joblist_entry(PPUContext& c) {   // r5 = the entry (kind in the low halfword of its first word)
    if (!enabled()) return;
    const u64 e = c.r[5];
    const u32 kind = (u32)(e >> 32) & 0xFFFF;
    record(c, (u32)c.r[4], kind == 1 ? job_kind((u32)e) : kind == 3 ? 'P' : 'E', 1);
}
void joblist_wait(PPUContext& c) { if (enabled()) record(c, (u32)c.r[4], 'W', 1); }
void joblist_reserve(PPUContext& c) { if (enabled()) record(c, (u32)c.r[3], 'R', (u32)c.r[4]); }
void joblist_pad(PPUContext& c) { if (enabled()) record(c, (u32)c.r[4], 'P', 1); }

// SPU writes (data = the bytes about to land at ea) into the entries of the frame job lists w1/w2: counted, and an
// entry written at or below the consumers' head is logged (the job manager has passed it: it never runs).
void joblist_spu_write(const char* who, u32 pc, u32 ea, const u8* data, u32 size, const u8* ls) {
    if (!enabled()) return;
    if (ea <= 0x10410D80 && ea + size >= 0x10410DA0) {   // the w0 queue line: head and SPU halfwords after the write
        const u8* p = data + (0x10410D90 - ea);
        auto w = [&](int o) { return (u32)p[o] << 24 | p[o + 1] << 16 | p[o + 2] << 8 | p[o + 3]; };
        void joblist_w0_event(char, u32, u32, u32, u32);
        if (memcmp(p, gmem(0x10410D90), 16)) joblist_w0_event('H', w(12), w(8), w(0), w(4));
    }
    static const u32 queues[3] = {0x10C15100, 0x10C29100, 0x10410D80};
    for (u32 q : queues) {
        const u32 lo = q + 0x20, hi = q + 0x20 + 8 * (q == 0x10410D80 ? 0x7FC : 0x27FC);
        if (ea + size <= lo || ea >= hi) continue;
        const u32 head = ld16(q + 0x12), top = ld16(q + 0x10);
        static std::atomic<u64> writes{0}, behind{0}, window{0};
        for (u32 a = std::max(ea, lo) & ~7u; a < std::min(ea + size, hi); a += 8) {
            if (a < ea) continue;
            const u8* p = data + (a - ea);
            if (!memcmp(p, gmem(a), 8)) continue;   // unchanged (the job manager's PUTLLC of the queue's first line)
            const u32 w0 = (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
            const u32 w1 = (u32)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
            const u32 idx = (a - lo) / 8, kind = w0 & 0xFFFF;
            const u32 spu = who[0] == 's' && strlen(who) > 5 ? (u32)atoi(who + 5) : 9;   // "spursN"
            const u32 wid = spu < 8 ? (u32)g_worker_wid[spu].load(std::memory_order_relaxed) & 15 : 15;
            auto lsw = [&](u32 a) { u32 v; memcpy(&v, ls + (a & 0x3FFFC), 4); return __builtin_bswap32(v); };
            const u32 info = 0x1430 + (lsw(0x12E0) & 3) * 32;   // the PM's running slot
            tl_add(q, 'S', kind == 1 ? job_kind(w1) : (char)('0' + kind), idx, 1, spu << 24 | wid << 20 | (pc & 0xFFFFF),
                   lsw(info), lsw(info + 8));
            if (kind != 1) continue;
            writes++;
            if (idx >= head) continue;
            behind++;
            static std::atomic<int> n{0};
            if (n++ < 60)
                LOG_W("joblist: %s pc %05x wrote job %u (%08x %08x%s) to queue %08x behind its head %u (top %u)", who, pc,
                      idx, w0, w1, job_kind(w1) == 'O' ? ", in-order" : "", q, head, top);
        }
        const u64 now = time_usec();
        u64 w = window.load();
        if (now - w >= 5000000 && window.compare_exchange_strong(w, now) && w)
            LOG_I("joblist: SPU job entries written %llu, behind the head %llu (5 s)", (unsigned long long)writes.exchange(0),
                  (unsigned long long)behind.exchange(0));
    }
}

// watchdog (main thread stuck in a job list wait): the cycles under way of both frame lists, once
void joblist_dump_current() {
    if (!enabled()) return;
    static bool done = false;
    std::lock_guard lk(g_tl_m);
    if (done) return;
    done = true;
    print_cycle(0x10C15100, g_tl[0]);
    print_cycle(0x10C29100, g_tl[1]);
    print_cycle(0x10410D80, g_tl[2]);
    extern void print_drains_fwd();
    print_drains_fwd();
    extern void joblist_w0_print();
    joblist_w0_print();
}

// probe before the compaction in fn 0x48ac34 (after the previous list's reset): the current list is compacted next
void joblist_compact(PPUContext& c) {
    if (!enabled()) return;
    const u32 obj = (u32)c.r[28] + ld32((u32)c.r[29]) * 0x28;   // the list array and the current index's address
    if (!mem::readable(obj, 0x28)) return;
    tl_add(ld32(obj + 4), 'P', 'C', ld32(obj + 0x24), 1, 0x48ACC8, t_self ? t_self->id : 0, 0);
    extern void joblist_w0_event(char, u32, u32, u32, u32);
    joblist_w0_event('C', ld32(obj + 0x24), 9, ld32(obj + 4), 0);
}

// probes: 0x489cb8 (fn 0x489c90: the wait for the w0 queue to drain returned), 0x48aca8 (fn 0x48ac34: the wait for the
// previous frame list returned); marked in both frame lists' timelines
static void mark_both(char kind, u32 lr) {
    for (u32 q : {0x10C15100u, 0x10C29100u}) tl_add(q, 'P', kind, 9999, 1, lr, t_self ? t_self->id : 0, ld16(q + 0x10));
}
// snapshots at the last drain returns: the w0 queue's header and each worker's workload and PM slot infos
struct DrainSnap { u64 t; u32 hdr[8]; int wid[6]; u32 slot[6][3][4]; u32 cur[6]; };
static DrainSnap g_drain[4];
static std::atomic<u32> g_drain_pos{0};
SPUThread* spurs_worker_spu(u32 k);   // hle_spurs.cpp
void joblist_w0_drained(PPUContext&) {
    if (!enabled()) return;
    mark_both('D', 0x489CB8);
    extern void joblist_w0_event(char, u32, u32, u32, u32);
    joblist_w0_event('D', ld16(0x10410D80 + 0x12), 9, ld32(0x10410D80 + 0x10), ld32(0x10410D80 + 0x1C));
    DrainSnap& d = g_drain[g_drain_pos++ % 4];
    d.t = time_usec();
    for (int k = 0; k < 8; k++) d.hdr[k] = ld32(0x10410D80 + 0x10 + 4 * k);
    for (u32 k = 0; k < 6; k++) {
        d.wid[k] = g_worker_wid[k].load();
        SPUThread* t = spurs_worker_spu(k);
        d.cur[k] = t ? t->ls32(0x12E0) : 0;
        for (u32 s = 0; s < 3; s++)
            for (u32 w = 0; w < 4; w++) d.slot[k][s][w] = t ? t->ls32(0x1430 + s * 32 + 4 * w) : 0;
    }
}
static void print_drains() {
    const u32 n = g_drain_pos.load();
    for (u32 i = n > 4 ? n - 4 : 0; i < n; i++) {
        const DrainSnap& d = g_drain[i % 4];
        LOG_I("joblist: w0 drained at %.3f s: queue %08x %08x %08x %08x", d.t / 1e6, d.hdr[0], d.hdr[1], d.hdr[2], d.hdr[3]);
        for (u32 k = 0; k < 5; k++)
            LOG_I("joblist:   spu%u w%d slot %u | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", k,
                  d.wid[k], d.cur[k], d.slot[k][0][0], d.slot[k][0][1], d.slot[k][0][2], d.slot[k][0][3], d.slot[k][1][0],
                  d.slot[k][1][1], d.slot[k][1][2], d.slot[k][1][3], d.slot[k][2][0], d.slot[k][2][1], d.slot[k][2][2],
                  d.slot[k][2][3]);
    }
}
void joblist_prev_done(PPUContext&) { if (enabled()) mark_both('V', 0x48ACA8); }
void print_drains_fwd() { print_drains(); }
// probe at fn 0x5514a4 (the PPU's atomic push of a job, r4 = list, r5 = entry): pushes into w0 in its timeline
void joblist_push(PPUContext& c) {
    if (!enabled()) return;
    const u32 obj = (u32)c.r[4];
    if (!mem::readable(obj, 8)) return;
    const u32 q = ld32(obj + 4);
    if (q == 0x10410D80) tl_add(q, 'P', 'J', 9000 + ld16(q + 0x10), 1, (u32)c.lr, t_self ? t_self->id : 0, 0);
}

// ring of w0 events (F1CE_JOBLIST_LOG): native grabs (G), job service ends (E), drain returns (D), compactions (C),
// halfword-line writes by SPUs (H: the queue line's head and SPU halfwords after a PUTLLC), printed by the watchdog
struct W0Ev { u64 t; char kind; u32 idx, spu, a, b; };
static W0Ev g_w0[512];
static std::atomic<u32> g_w0_pos{0};
void joblist_w0_event(char kind, u32 idx, u32 spu, u32 a, u32 b) {
    if (!enabled()) return;
    g_w0[g_w0_pos++ % 512] = {time_usec(), kind, idx, spu, a, b};
}
void joblist_w0_print() {
    const u32 n = g_w0_pos.load();
    std::string line;
    for (u32 i = n > 512 ? n - 512 : 0; i < n; i++) {
        const W0Ev& e = g_w0[i % 512];
        char b[64];
        snprintf(b, sizeof(b), " %c%u/%u@%llu[%x %x]", e.kind, e.idx, e.spu, (unsigned long long)(e.t % 100000000) / 10, e.a, e.b);
        line += b;
        if (line.size() > 900) { LOG_I("joblist w0:%s", line.c_str()); line.clear(); }
    }
    if (!line.empty()) LOG_I("joblist w0:%s", line.c_str());
}
// cellSpursReadyCountStore on a frame list's or w0's workload: K events in its timeline (value in the head field)
void joblist_kick(u32 q, u32 value) {
    if (!enabled() || tl_index(q) < 0) return;
    tl_add(q, 'P', 'K', 9999, 1, value, t_self ? t_self->id : 0, ld16(q + 0x12));
}
