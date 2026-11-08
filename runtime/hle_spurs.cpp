// HLE: cellSpurs.
//
// Workloads run their policy module (PM) on SPU interpreters, driven by a host-side kernel.
// The kernel/PM interface is being derived from the game's own PM by observation (LS kernel-area
// accesses, kernel-area branches and DMA traffic are logged), so this file starts minimal.
// Tasksets run their tasks on host threads; event flags and queues live in guest memory in the layout the
// task-side library of the game's task ELFs uses, so PPU and SPU sides share them.
#include "kernel.h"
#include "spu.h"

#include <unordered_map>
#include <set>
#include <condition_variable>
#include <deque>
#include <map>
#include <vector>
void set_thread_name(const char* name);   // core.cpp

double watch_after();   // core.cpp (debug)
u64 dma_count(int k);   // spu.cpp (profiling)
void line_written(u32 ea);         // core.cpp: a write under the lock (fails SPU PUTLLCs on the line)
void place_thread(int kind);   // core.cpp
void joblist_kick(u32 queue, u32 value);   // hle_joblist.cpp (F1CE_JOBLIST_LOG): ready stores, workers entering
void profile_register_thread();   // core.cpp (host profiler)
extern std::atomic<u64> g_pm_skips;           // spu_pm.cpp: job queue skip entries passed (diagnostics)
extern std::atomic<u32> g_pm_skip_run_max;
std::atomic<int> g_worker_wid[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // profiling (F1CE_PROFILE): workload each SPU worker runs

namespace {
// diagnostics: the first 0x80 bytes of a workload's argument block (job manager control: counters, SPU slots, queue)
void dump_wkl_arg(const char* why, u32 wid, u64 arg) {
    u32 a = (u32)arg & ~15u;
    if (a < 0x10000) return;
    for (u32 k = 0; k < 0x80; k += 32) {
        std::string h;
        for (u32 j = 0; j < 32; j += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32(a + k + j)); h += b; }
        LOG_I("  wkl %u arg (%s) %08x:%s", wid, why, a + k, h.c_str());
    }
}
struct Task { u32 taskset, elf, context, ls_pattern; u64 arg[2]; };
std::mutex g_spurs_mutex;
std::vector<Task> g_tasks;

constexpr u32 PM_BASE = 0xA00;      // all PM-internal branches resolve inside [0xA00, 0xA00 + size)
constexpr u32 KCTX = 0x100;         // context block handed to the PM in r3

// Kernel-area interface used by the game's PM (derived from its code):
//   ls[0x1C0] u64  SPURS instance EA (target of its atomic updates)
//   ls[0x1DC] u32  id of the workload being run
//   ls[0x1E0] u32  address of the "exit to kernel" service (does not return)
//   ls[0x1E4] u32  address of the poll service: returns in r3 the id the kernel wants to run;
//                  the PM yields when it differs from ls[0x1DC]
//   ls[0x1C8] u32  number of the SPU running the PM (read together with the SPURS EA)
constexpr u32 LS_SPURS_EA = 0x1C0, LS_SPU_NUM = 0x1C8, LS_WKL_ID = 0x1DC, LS_EXIT_PTR = 0x1E0, LS_POLL_PTR = 0x1E4;
constexpr u32 SVC_EXIT = 0x200, SVC_POLL = 0x210;     // our service entry points (handled on the host)

struct Workload {
    u32 pm = 0, size = 0;
    u64 arg = 0;
    bool used = false;
    u32 running = 0;          // SPUs currently executing this workload
    u32 max_contention = 1;   // how many SPUs may run it at once (from cellSpursAddWorkload)
    u64 busy_since = 0;       // time the current set of runs started (running went 0 -> 1)
    u64 last_run = 0;       // time_usec() of the last time it got an SPU
    u64 runs = 0;
    u64 insns = 0;
    bool pm_native = false;   // its PM is the job manager whose functions spu_pm.cpp has natively
};

struct SpursState {
    u32 ea = 0, nspus = 0;
    Workload wkl[16];
    std::mutex m;
    bool started = false;
    u32 rr = 0;
};
SpursState g_sp;
// each SPU reports its own number in the kernel context (the job manager keeps per-SPU progress slots);
// F1CE_SPU_NUM=0 forces 0 for every SPU (debugging)
const bool g_spu_num_mode = !(getenv("F1CE_SPU_NUM") && atoi(getenv("F1CE_SPU_NUM")) == 0);
SPUThread* g_spu_by_wkl[16] = {};
std::atomic<u32> g_ready_calls[16];
SPUThread* g_worker_spu[8] = {};   // diagnostics
std::atomic<u64> g_task_interp{0};  // instructions the SPURS tasks ran in the interpreter (summed when each task ends)
std::atomic<u64> g_task_runs{0}, g_task_usec{0};   // SPURS task runs and their time (SPURS report)
std::atomic<u64> g_worker_busy[8];   // profiling: microseconds each SPU worker spent running workloads   // cellSpursReadyCountStore calls per workload (diagnostics)   // SPU currently running each workload (diagnostics)

// true if another workload has signalled work (ready count > 0) and has no SPU. Workloads that are
// merely polled for work do not count, so a parallel job is not forced to give up its SPU.
// Read without g_sp.m: the PM asks this on every job (the mutex was ~2.7% of the workers' time), and the answer is only
// a hint, so a value one update old is fine (the PM asks again at its next job).
bool others_waiting(u32 self) {
    for (u32 i = 0; i < 16; i++) {
        Workload& w = g_sp.wkl[i];
        if (i != self && std::atomic_ref<bool>(w.used).load(std::memory_order_relaxed) &&
            std::atomic_ref<u32>(w.running).load(std::memory_order_relaxed) == 0 && ld8(g_sp.ea + i) > 0)
            return true;
    }
    return false;
}

u32 spurs_send_event(SPUThread& s, u32 intr, u32 data1);   // below (task support)

u64 run_workload(SPUThread& spu, u32 wid, const Workload& w) {
    memset(spu.ls, 0, PM_BASE);   // kernel area (no code)
    spu.write_ls(PM_BASE, gmem(w.pm), w.size);   // keeps the decoded PM when it is the same module
    u64 ea = __builtin_bswap64((u64)g_sp.ea);
    memcpy(spu.ls + LS_SPURS_EA, &ea, 8);
    spu.ls32(LS_SPU_NUM, g_spu_num_mode ? spu.id : 0);
    spu.ls32(LS_WKL_ID, wid);
    spu.ls32(LS_EXIT_PTR, SVC_EXIT);
    spu.ls32(LS_POLL_PTR, SVC_POLL);
    memset(spu.gpr, 0, sizeof(spu.gpr));
    spu.set_pref(1, 0x3FFF0);
    spu.set_pref(3, KCTX);
    spu.gpr[4].d[1] = w.arg;
    spu.pc = PM_BASE;
    spu.kernel_limit = PM_BASE;
    spu.pm_native = w.pm_native;
    spu.instructions = 0;
    spu.interrupts = false;
    spu.event_mask = spu.event_stat = spu.stall_stat = 0;
    for (auto& l : spu.stalled) l.active = false;
    spu.on_kernel_call = [wid](SPUThread& s) {
        if (s.pc == SVC_EXIT) { s.halted = true; return true; }
        if (s.pc == SVC_POLL) {
            u32 want = others_waiting(wid) ? (wid + 1) % 16 : wid;
            s.set_pref(3, want);
            s.pc = s.pref(0) & 0x3FFFC;   // return to the caller (link in r0)
            return true;
        }
        std::string tr;   // with F1CE_TRACE_WKL=<wid>: the last blocks run
        if (s.pc_trace)
            for (size_t k = s.pc_trace->size() > 40 ? s.pc_trace->size() - 40 : 0; k < s.pc_trace->size(); k++) {
                char b[8]; snprintf(b, sizeof(b), " %05x", (*s.pc_trace)[k]); tr += b;
            }
        fatal("SPURS wkl %u: PM branched to unknown kernel address %05x (r0=%05x srr0=%05x ints=%d ev=%x/%x)%s", wid, s.pc, s.pref(0),
              s.srr0, s.interrupts, s.event_stat, s.event_mask, tr.c_str());
    };
    // the audio workload prints through the same send_event routine as the tasks (port 15, then two InMbox reads): it
    // waited for ever on the second word, music stopped and main later blocked on audio state (Time Trial load freeze)
    spu.on_send_event = spurs_send_event;
    spu.event_needs_completion = true;
    spu.event_completion_pending = false;
    // the pit-entry job (job manager wkl 5) asserts that interrupts are enabled and stopped with stopd: the job never
    // finished and the PPU waited forever for its completion (freeze entering the pits)
    spu.report_ie = true;
    spu.on_stop = [wid](SPUThread& s, u32 code) {
        LOG_W("SPURS wkl %u: stop 0x%x at %05x", wid, code, s.pc);
        return false;
    };
    // diagnostics: atomic traffic of jobs that run for very long (spin detection)
    // F1CE_SPU_DMA_STATS=1: per worker thread, every 10 s, the DMA sites (workload, pc, command) by count and bytes
    static const bool dma_stats = getenv("F1CE_SPU_DMA_STATS") != nullptr;
    if (dma_stats) spu.on_dma = [wid](SPUThread& s, u32 cmd, u32, u32, u32 size) {
        struct St { u64 n = 0, bytes = 0; };
        thread_local std::unordered_map<u64, St> st;
        thread_local u64 next = time_usec() + 10000000;
        u32 c8 = cmd & 0xFF;
        St& e = st[(u64)wid << 32 | (s.pc & 0x3FFFC) << 8 | c8];
        e.n++;
        e.bytes += (c8 & 0xF4) == 0x44 || (c8 & 0xF4) == 0x24 ? 0 : size;
        if (time_usec() < next) return;
        next = time_usec() + 10000000;
        std::vector<std::pair<u64, u64>> v;
        u64 total = 0;
        for (auto& [k, x] : st) { v.push_back({x.n, k}); total += x.n; }
        std::sort(v.rbegin(), v.rend());
        std::string out;
        for (size_t i = 0; i < v.size() && i < 24; i++) {
            char b[64];
            const St& x = st[v[i].second];
            snprintf(b, sizeof(b), " w%u:%05x:%02x %.0f/s %.0fB", (u32)(v[i].second >> 32), (u32)(v[i].second >> 8) & 0x3FFFF,
                     (u32)v[i].second & 0xFF, x.n / 10.0, x.n ? (double)x.bytes / x.n : 0.0);
            out += b;
        }
        LOG_I("dma stats %s (%.0f/s):%s", s.name.c_str(), total / 10.0, out.c_str());
        st.clear();
    };
    static const bool dma_diag = getenv("F1CE_SPU_DMA_DIAG") != nullptr;   // (a shared counter on every DMA: costly)
    if (dma_diag) spu.on_dma = [wid](SPUThread& s, u32 cmd, u32 lsa, u32 ea, u32 size) {
        static std::atomic<int> budget{60};
        u32 c8 = cmd & 0xFF;
        // diagnostics: the PM's ready-count swap (LS 0x3480: index r3 -> r7 = r3 - 3, value r4 -> r9) on another
        // workload's count; caller 0x2484 = job-triggered signal, 0x2e90 = clear of ls[0x1400]
        if (c8 == 0xB4 && (ea & ~127u) == g_sp.ea && s.pc == 0x3504) {
            u32 idx = s.pref(7) + 3, val = s.pref(9) & 0xFF;
            static std::atomic<int> n{0};
            if (idx != wid && n++ < 60)
                LOG_W("SPURS ready swap: wkl %u sets ready[%u] = %u (caller %05x, ls[0x1400]=%u)", wid, idx, val, s.pref(0), s.ls32(0x1400));
        }
        if (s.instructions < 1000000000ull || budget-- <= 0) return;
        u32 line = ea & ~127u;
        LOG_I("spin wkl %u pc=%05x cmd=%02x lsa=%05x size=%x tag=%u ea=%08x mem: %08x %08x %08x %08x | %08x %08x %08x %08x  ls: %08x %08x %08x %08x",
              wid, s.pc, c8, lsa, size, s.mfc_tag, ea, ld32(line), ld32(line + 4), ld32(line + 8), ld32(line + 12), ld32(line + 16),
              ld32(line + 20), ld32(line + 24), ld32(line + 28), s.ls32((lsa & 0x3FF80)), s.ls32((lsa & 0x3FF80) + 4),
              s.ls32((lsa & 0x3FF80) + 8), s.ls32((lsa & 0x3FF80) + 12));
    };
    // debug: F1CE_TRACE_WKL=<wid> logs the block start addresses of that workload's runs (consecutive repeats merged)
    static const int trace_wkl = getenv("F1CE_TRACE_WKL") ? atoi(getenv("F1CE_TRACE_WKL")) : -1;
    std::vector<u32> trace;
    if ((int)wid == trace_wkl) spu.pc_trace = &trace;
    g_spu_by_wkl[wid] = &spu;
    spu.run();
    g_spu_by_wkl[wid] = nullptr;
    spu.pc_trace = nullptr;
    if ((int)wid == trace_wkl) {
        std::string t;
        u32 last = ~0u, rep = 0;
        auto flush = [&] { if (last == ~0u) return; char b[24]; snprintf(b, sizeof(b), rep > 1 ? " %05x*%u" : " %05x", last, rep); t += b; };
        for (u32 p : trace) { if (p == last) { rep++; continue; } flush(); last = p; rep = 1; }
        flush();
        for (size_t k = 0; k < t.size(); k += 1800) LOG_I("wkl %u trace (%zu blocks): %s", wid, trace.size(), t.substr(k, 1800).c_str());
    }
    return spu.instructions;
}

// Profiling aid (F1CE_SPU_PROFILE=<seconds before start>): samples the workload and SPU pc of every worker each
// millisecond for 10 s. Unlike pc_samples it sees cluster code (pc is the cluster's entry block while it runs).
void spu_profiler(u32 n) {
    sleep_usec((u64)(atof(getenv("F1CE_SPU_PROFILE")) * 1e6));
    std::map<u32, u32> by_pc;   // wid << 20 | pc
    std::map<int, u32> by_wid;
    u32 total = 0;
    // F1CE_SPU_PROFILE_DUMP=<wid>: also saves the LS of a worker running that workload (ls_w<wid>_<n>.bin, 4 times)
    const char* dump_env = getenv("F1CE_SPU_PROFILE_DUMP");
    int dump_wid = dump_env ? atoi(dump_env) : -1, dumps = 0;
    u64 next_dump = time_usec() + 2000000;
    for (u64 end = time_usec() + 10000000; time_usec() < end;) {
        sleep_usec(1000);
        for (u32 k = 0; k < n && k < 8; k++) {
            int wid = g_worker_wid[k].load(std::memory_order_relaxed);
            SPUThread* t = g_worker_spu[k];
            total++;
            by_wid[wid]++;
            if (wid >= 0 && t) by_pc[(u32)wid << 20 | (t->pc & 0x3FFFC)]++;
            if (dump_wid >= 0 && wid == dump_wid && t && dumps < 4 && time_usec() >= next_dump) {
                std::vector<u8> copy(t->ls, t->ls + 0x40000);
                char name[64];
                snprintf(name, sizeof(name), "ls_w%d_%d.bin", wid, dumps++);
                if (FILE* f = fopen(name, "wb")) { fwrite(copy.data(), 1, copy.size(), f); fclose(f); }
                LOG_I("spu profile: LS of worker %u (wkl %d, pc %05x) saved to %s", k, wid, t->pc, name);
                next_dump = time_usec() + 2000000;
            }
        }
    }
    std::string s;
    for (auto& [w, c] : by_wid) { char b[32]; snprintf(b, sizeof(b), " %s%d %.1f%%", w < 0 ? "idle" : "w", w < 0 ? 0 : w, 100.0 * c / total); s += b; }
    LOG_I("spu profile: %u samples:%s", total, s.c_str());
    std::vector<std::pair<u32, u32>> v;
    for (auto& [k, c] : by_pc) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 40; i++)
        LOG_I("spu profile %5.1f%% w%u %05x", 100.0 * v[i].first / total, v[i].second >> 20, v[i].second & 0xFFFFF);
    // policy module pcs over all workloads (they share one PM)
    std::map<u32, u32> pm_pc;
    for (auto& [k, c] : by_pc) if ((k & 0xFFFFF) < 0x3580) pm_pc[k & 0xFFFFF] += c;
    v.clear();
    for (auto& [k, c] : pm_pc) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    s.clear();
    for (size_t i = 0; i < v.size() && i < 60; i++) { char b[24]; snprintf(b, sizeof(b), " %05x:%.1f", v[i].second, 100.0 * v[i].first / total); s += b; }
    LOG_I("spu profile pm pcs:%s", s.c_str());
    // per workload: policy module (< 0x3580) against job / task code
    std::map<u32, std::pair<u32, u32>> pm;
    for (auto& [k, c] : by_pc) ((k & 0xFFFFF) < 0x3580 ? pm[k >> 20].first : pm[k >> 20].second) += c;
    s.clear();
    for (auto& [w, p] : pm) { char b[48]; snprintf(b, sizeof(b), " w%u pm %.1f%% code %.1f%%", w, 100.0 * p.first / total, 100.0 * p.second / total); s += b; }
    LOG_I("spu profile by part:%s", s.c_str());
}

void spu_worker(u32 num) {
    set_thread_name(("spu w" + std::to_string(num)).c_str());
    place_thread(2);
    if (!getenv("F1CE_HOST_PROFILE_RSX")) profile_register_thread();
    auto spu = std::make_unique<SPUThread>();
    spu->id = num;
    spu->name = "spurs" + std::to_string(num);
    g_worker_spu[num & 7] = spu.get();
    u32 idle_streak = 0;
    u64 idle_since = 0;
    LineWait idle_lw;
    static const bool old_idle = getenv("F1CE_SPIN_WAIT") != nullptr;
    for (;;) {
        int wid = -1, rule = 0;
        u32 ready_at_pick = 0;
        Workload w;
        {
            std::lock_guard lk(g_sp.m);
            u64 now = time_usec();
            // 1) signalled work first: among workloads with ready count > 0 that may take another SPU
            //    (running < min(ready, max contention)), the one with the fewest SPUs gets it, so that
            //    concurrently signalled workloads share the SPUs instead of one taking them all
            u32 best_running = ~0u;
            for (u32 k = 0; k < 16; k++) {
                u32 i = (g_sp.rr + k) % 16;
                Workload& c = g_sp.wkl[i];
                u32 ready = ld8(g_sp.ea + i);
                if (!c.used || !ready || c.running >= std::min(ready, c.max_contention)) continue;
                if (c.running < best_running) { best_running = c.running; wid = (int)i; rule = 1; }
            }
            // 2) join a workload that has been busy for a while (likely a parallel job) up to its contention, while it
            //    still signals work: a PM clears its ready count when its queue runs dry, and one let into a finished
            //    frame list (ready 0, its last PM still inside) took the next frame's reserved skip entries while main
            //    was building it, before the SPU writers filled them: the frame's jobs were lost and its in-order jobs
            //    waited for ever (the race freeze, 2026-11-07). F1CE_SPURS_JOIN_IDLE=1: the old rule (A/B)
            static const bool join_idle = getenv("F1CE_SPURS_JOIN_IDLE") != nullptr;
            for (u32 i = 0; i < 16 && wid < 0; i++) {
                Workload& c = g_sp.wkl[i];
                if (c.used && c.running > 0 && c.running < c.max_contention && now - c.busy_since > 1000 &&
                    (join_idle || ld8(g_sp.ea + i) > 0)) {
                    wid = (int)i;
                    rule = 2;
                }
            }
            // 3) otherwise round-robin over the workloads that can take another SPU
            //    Like the SPURS kernel, only signalled workloads run (plus one initial run each);
            //    F1CE_SPURS_POLL=1 restores continuous polling of every workload (debugging)
            static const bool ready_only = getenv("F1CE_SPURS_POLL") == nullptr;
            for (u32 k = 0; k < 16 && wid < 0; k++) {
                u32 i = (g_sp.rr + k) % 16;
                Workload& c = g_sp.wkl[i];
                if (c.used && c.running < c.max_contention && (!ready_only || c.runs == 0)) { wid = (int)i; rule = 3; }
            }
            if (wid >= 0 && getenv("F1CE_WATCH_LINE") && now / 1e6 >= watch_after()) {
                static std::atomic<int> n{0};
                if (n++ < 60)
                    LOG_I("sched: spu%u picks wkl %d (ready=%u running=%u max=%u)", num, wid, ld8(g_sp.ea + wid),
                          g_sp.wkl[wid].running, g_sp.wkl[wid].max_contention);
            }
            if (wid >= 0) {
                ready_at_pick = ld8(g_sp.ea + wid);
                g_sp.rr = wid + 1;
                if (g_sp.wkl[wid].running == 0) g_sp.wkl[wid].busy_since = now;
                g_sp.wkl[wid].running++;
                g_sp.wkl[wid].last_run = time_usec();
                w = g_sp.wkl[wid];
            }
        }
        if (wid < 0) {
            // nothing ready: block until the ready counts' line is written (SPU atomics, PPU atomics and HLE updates
            // notify; 1 ms at most, which also covers rule 2), after a short spin for the gaps between jobs
            if (old_idle) { idle_wait(idle_since, 100); continue; }
            const u64 now = time_usec();
            if (!idle_since) idle_since = now;
            if (now - idle_since < 30) { for (int i = 0; i < 16; i++) _mm_pause(); continue; }
            if (!idle_lw.armed) { line_wait_arm(idle_lw, g_sp.ea, g_sp.ea); continue; }   // (checks once more first)
            line_wait_block(idle_lw, 1);
            continue;
        }
        idle_since = 0;
        u64 run_t0 = time_usec();
        g_worker_wid[num & 7].store(wid, std::memory_order_relaxed);
        joblist_kick((u32)w.arg, num << 12 | rule << 8 | ready_at_pick);
        u64 n = run_workload(*spu, (u32)wid, w);
        g_worker_wid[num & 7].store(-1, std::memory_order_relaxed);
        if (w.runs < 8) {   // diagnostics: rarely run workloads (the pit-entry job manager)
            LOG_I("SPURS wkl %d run %llu on spu%u ended after %llu instructions", wid, (unsigned long long)w.runs + 1, num,
                  (unsigned long long)n);
            dump_wkl_arg("after run", (u32)wid, w.arg);
        }
        g_worker_busy[num & 7] += time_usec() - run_t0;
        {
            std::lock_guard lk(g_sp.m);
            g_sp.wkl[wid].running--;
            g_sp.wkl[wid].runs++;
            g_sp.wkl[wid].insns += n;
            g_sp.wkl[wid].last_run = time_usec();
            static u64 last_report = 0;
            u64 now = time_usec();
            if (now - last_report > 5000000) {
                last_report = now;
                std::string r;
                for (u32 i = 0; i < 16; i++)
                    if (g_sp.wkl[i].used) {
                        char b[64];
                        snprintf(b, sizeof(b), " w%u:%llu/%llu[%u]", i, (unsigned long long)g_sp.wkl[i].runs,
                                 (unsigned long long)g_sp.wkl[i].insns, g_sp.wkl[i].running);
                        r += b;
                    }
                for (u32 i = 0; i < 16; i++)
                    if (SPUThread* t = g_spu_by_wkl[i]) {
                        static bool dumped[16] = {};
                        if (t->instructions > 1000000000ull && !dumped[i]) {
                            dumped[i] = true;
                            char name[32];
                            snprintf(name, sizeof(name), "spu_wkl%u.ls", i);
                            if (FILE* f = fopen(name, "wb")) { fwrite(t->ls, 1, 0x40000, f); fclose(f); }
                            std::map<u32, u32> h;
                            for (u32 k = 0; k < 256; k++) h[t->pc_samples[k] & ~0x3Fu]++;
                            std::string hs;
                            for (auto& [pcv, cnt] : h) { char b[24]; snprintf(b, sizeof(b), " %05x:%u", pcv, cnt); hs += b; }
                            LOG_I("  wkl %u pc histogram (64-byte buckets):%s", i, hs.c_str());
                        }
                        LOG_I("  wkl %u running on %s: pc=%05x instructions=%llu event_mask=%x", i, t->name.c_str(),
                              t->pc, (unsigned long long)t->instructions, t->event_mask);
                    }
                std::string rc;
                for (u32 i = 0; i < 16; i++)
                    if (u32 n = g_ready_calls[i].load()) { char b[24]; snprintf(b, sizeof(b), " w%u:%u", i, n); rc += b; }
                LOG_I("SPURS ready-count stores:%s", rc.c_str());
                auto dump = [](const char* what, u32 ea, u32 n) {
                    std::string h;
                    for (u32 k = 0; k < n; k += 4) { char b[12]; snprintf(b, sizeof(b), "%s%08x", k % 32 ? " " : " | ", ld32(ea + k)); h += b; }
                    LOG_I("  %s @%08x:%s", what, ea, h.c_str());
                };
                dump("spurs line0", g_sp.ea, 128);
                for (u32 i = 1; i <= 2; i++)
                    if (g_sp.wkl[i].used) dump(i == 1 ? "wkl1 ctrl" : "wkl2 ctrl", (u32)g_sp.wkl[i].arg, 128);
                {
                    static u64 last_busy[8] = {}, last_t = time_usec();
                    u64 dt = std::max<u64>(1, now - last_t);
                    std::string bs;
                    for (u32 k = 0; k < g_sp.nspus && k < 8; k++) {
                        u64 b = g_worker_busy[k].load();
                        char t[16]; snprintf(t, sizeof(t), " %.0f%%", 100.0 * (b - last_busy[k]) / dt); bs += t;
                        last_busy[k] = b;
                    }
                    last_t = now;
                    LOG_I("SPURS worker busy:%s", bs.c_str());
                    {
                        static u64 last[5] = {};
                        u64 cur[5] = {dma_count(0), dma_count(1), dma_count(2), dma_count(3), dma_count(4)};
                        LOG_I("SPU DMA per second: get %.0f put %.0f getllar %.0f putllc %.0f (failed %.0f)", (cur[0] - last[0]) * 1e6 / dt,
                              (cur[1] - last[1]) * 1e6 / dt, (cur[2] - last[2]) * 1e6 / dt, (cur[3] - last[3]) * 1e6 / dt, (cur[4] - last[4]) * 1e6 / dt);
                        for (int k = 0; k < 5; k++) last[k] = cur[k];
                    }
                    // where the workloads spend their instructions: PM code (< 0x3580) vs job code, per workload
                    for (u32 i = 0; i < 16; i++)
                        if (SPUThread* t = g_spu_by_wkl[i]) {
                            u32 pm = 0, job = 0;
                            std::map<u32, u32> h;
                            for (u32 k = 0; k < 256; k++) {
                                u32 pcv = t->pc_samples[k];
                                (pcv < 0x3580 ? pm : job)++;
                                h[pcv & ~0xFFu]++;
                            }
                            std::vector<std::pair<u32, u32>> v;
                            for (auto& [k, c] : h) v.push_back({c, k});
                            std::sort(v.rbegin(), v.rend());
                            std::string hs;
                            for (size_t k = 0; k < v.size() && k < 5; k++) { char b2[24]; snprintf(b2, sizeof(b2), " %05x:%u", v[k].second, v[k].first); hs += b2; }
                            LOG_I("  wkl %u samples: pm %u job %u | top 256-byte ranges:%s", i, pm, job, hs.c_str());
                            static bool dumped_ls[16] = {};
                            if (getenv("F1CE_DUMP_WKL_LS") && !dumped_ls[i]) {   // debug: LS snapshot of a busy workload
                                dumped_ls[i] = true;
                                char fn[32];
                                snprintf(fn, sizeof(fn), "wkl%u_ls.bin", i);
                                if (FILE* f = fopen(fn, "wb")) { fwrite(t->ls, 1, 0x40000, f); fclose(f); }
                                std::string ps;
                                for (u32 k = 0; k < 64; k++) { char b3[8]; snprintf(b3, sizeof(b3), " %05x", t->pc_samples[k]); ps += b3; }
                                LOG_I("  wkl %u pc samples:%s", i, ps.c_str());
                            }
                        }
                }
                {   // code invalidations per second per SPU (code_writes)
                    static u64 last_cw[8] = {};
                    std::string cw;
                    for (u32 k = 0; k < 8; k++)
                        if (SPUThread* t = g_worker_spu[k]) {
                            char b[24]; snprintf(b, sizeof(b), " %llu", (unsigned long long)((t->code_writes - last_cw[k]) / 5));
                            cw += b; last_cw[k] = t->code_writes;
                        }
                    LOG_I("SPU code invalidations per second:%s", cw.c_str());
                    static u64 last_st[6] = {};
                    u64 st[6] = {};
                    for (u32 k = 0; k < 8; k++)
                        if (SPUThread* t = g_worker_spu[k]) {
                            st[0] += t->st_fast; st[1] += t->st_slow; st[2] += t->st_build; st[3] += t->st_interp;
                            st[4] += t->st_slow_tsc; st[5] += t->st_build_tsc;
                        }
                    st[3] += g_task_interp.load(std::memory_order_relaxed);
                    {   // SPURS tasks (a host thread each)
                        static u64 last_runs = 0, last_us = 0;
                        const u64 runs = g_task_runs.load(), us = g_task_usec.load();
                        LOG_I("SPURS tasks: %.0f runs/s, %.2f cores", (runs - last_runs) / 5.0, (us - last_us) / 5e6);
                        static u64 last_skips = 0;
                        const u64 skips = g_pm_skips.load();
                        LOG_I("job queue skips: %.0f/s, longest run %u", (skips - last_skips) / 5.0, g_pm_skip_run_max.exchange(0));
                        last_skips = skips;
                        last_runs = runs;
                        last_us = us;
                    }
                    LOG_I("SPU dispatches per second: fast %llu slow %llu rebuilt %llu", (unsigned long long)((st[0] - last_st[0]) / 5),
                          (unsigned long long)((st[1] - last_st[1]) / 5), (unsigned long long)((st[2] - last_st[2]) / 5));
                    {   // time in the slow path, in worker cores (TSC rate measured against the host clock)
                        static u64 tsc0 = __rdtsc(), us0 = time_usec();
                        const u64 tsc1 = __rdtsc(), us1 = time_usec();
                        const double hz = (double)(tsc1 - tsc0) * 1e6 / (double)(us1 - us0), sec = (us1 - us0) / 1e6;
                        LOG_I("SPU slow dispatch time: %.3f cores (rebuilds %.3f)", (st[4] - last_st[4]) / hz / sec,
                              (st[5] - last_st[5]) / hz / sec);
                        tsc0 = tsc1;
                        us0 = us1;
                    }
                    // untranslated code is ~10x slower: a game situation never recorded (rain was) shows up here
                    const u64 interp = (st[3] - last_st[3]) / 5;
                    if (interp > 2000000)
                        LOG_W("SPU: %.1f M interpreted instructions/s - record them with F1CE_SPU_DUMP_BLOCKS and run tools/spu_aot.py",
                              interp / 1e6);
                    for (int k = 0; k < 6; k++) last_st[k] = st[k];
                }
                LOG_I("SPURS runs/instructions:%s  ready=%02x%02x%02x%02x%02x%02x%02x%02x%02x", r.c_str(),
                      ld8(g_sp.ea), ld8(g_sp.ea + 1), ld8(g_sp.ea + 2), ld8(g_sp.ea + 3), ld8(g_sp.ea + 4),
                      ld8(g_sp.ea + 5), ld8(g_sp.ea + 6), ld8(g_sp.ea + 7), ld8(g_sp.ea + 8));
            }
        }
        // a run that found no work is short; back off once every workload had an idle turn
        if (n >= 2000) LOG_T("SPURS wkl %d: busy run, %llu instructions", wid, (unsigned long long)n);
        if (n < 2000) {
            if (++idle_streak >= 16) { idle_streak = 0; sleep_usec(200); }
        } else {
            idle_streak = 0;
        }
    }
}
}  // namespace

// Watchdog (core.cpp): what each SPU worker runs and the scheduler's view of every workload (racy, diagnostics only).
// The SPURS report above is printed only when a run ends, so a freeze with every worker idle or stuck shows nothing.
SPUThread* spurs_worker_spu(u32 k) { return k < 8 ? g_worker_spu[k] : nullptr; }   // diagnostics (hle_joblist.cpp)
// SPU workers inside the workload whose argument is the job queue `q` (spu_pm.cpp: the job waits)
u32 spurs_workers_on_queue(u32 q) {
    for (u32 i = 0; i < 16; i++) {
        Workload& w = g_sp.wkl[i];
        if (!std::atomic_ref<bool>(w.used).load(std::memory_order_relaxed) || (u32)w.arg != q) continue;
        u32 n = 0;
        for (u32 k = 0; k < 8; k++) n += g_worker_wid[k].load(std::memory_order_relaxed) == (int)i;
        return n;
    }
    return 0;
}

void spurs_watchdog_report() {
    if (!g_sp.ea) return;
    std::string ws;
    for (u32 k = 0; k < g_sp.nspus && k < 8; k++) {
        SPUThread* t = g_worker_spu[k];
        char b[96];
        snprintf(b, sizeof(b), " spu%u:w%d pc=%05x n=%llu ev=%x/%x", k, g_worker_wid[k].load(), t ? t->pc : 0,
                 t ? (unsigned long long)t->instructions : 0ull, t ? t->event_stat : 0, t ? t->event_mask : 0);
        ws += b;
    }
    LOG_I("  spurs workers:%s", ws.c_str());
    const u64 now = time_usec();
    std::string ls;
    for (u32 i = 0; i < 16; i++) {
        Workload& w = g_sp.wkl[i];
        if (!w.used) continue;
        char b[80];
        snprintf(b, sizeof(b), " w%u:ready%u/run%u/max%u/idle%.1fs", i, ld8(g_sp.ea + i), w.running, w.max_contention,
                 w.last_run ? (now - w.last_run) / 1e6 : -1.0);
        ls += b;
    }
    LOG_I("  spurs workloads:%s", ls.c_str());
    for (u32 i = 0; i < 16; i++) {
        Workload& w = g_sp.wkl[i];
        if (!w.used || w.pm != g_sp.wkl[1].pm || !mem::readable((u32)w.arg, 64)) continue;   // job managers: queue head
        std::string h;
        for (u32 k = 0; k < 64; k += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32((u32)w.arg + k)); h += b; }
        LOG_I("  wkl %u ctrl %08x:%s", i, (u32)w.arg, h.c_str());
    }
}

HLE_FUNC(cellSpursInitialize) {
    LOG_I("cellSpursInitialize(spurs=%08llx, nSpus=%lld, prio=%lld)", (unsigned long long)ARG(0), (long long)ARG(1),
          (long long)ARG(2));
    std::lock_guard lk(g_sp.m);
    g_sp.ea = (u32)ARG(0);
    g_sp.nspus = (u32)ARG(1);
    RET(CELL_OK);
}
HLE_FUNC(cellSpursFinalize) { RET(CELL_OK); }
// CellSpursInfo (280 bytes): nSpus, spuThreadGroupPriority, ppuThreadPriority, exitIfNoWork, spurs2, ...,
// spuThreadGroup @0x24, spuThreads[8] @0x28, handler threads @0x48/0x4c, namePrefix[16] @0x50, prefix len @0x60
HLE_FUNC(cellSpursGetInfo) {
    u32 p = (u32)ARG(1);
    memset(gmem(p), 0, 280);
    st32(p + 0x00, 5);
    st32(p + 0x04, 250);
    st32(p + 0x08, 128);
    st32(p + 0x24, 0x04000100);
    for (u32 i = 0; i < 5; i++) st32(p + 0x28 + i * 4, 0x02000100 + i);
    gstr_write(p + 0x50, "spurs", 16);
    st32(p + 0x60, 5);
    RET(CELL_OK);
}
HLE_FUNC(cellSpursSetPriorities) { RET(CELL_OK); }
// cellSpursReadyCountStore(spurs, wid, value)
HLE_FUNC(cellSpursReadyCountStore) {
    u32 wid = (u32)ARG(1);
    if (wid >= 16 || (u32)ARG(0) != g_sp.ea) {
        LOG_W("cellSpursReadyCountStore(spurs=%08llx, wid=%llu, %llu): unexpected arguments", (unsigned long long)ARG(0),
              (unsigned long long)ARG(1), (unsigned long long)ARG(2));
        RET(CELL_EINVAL);
    }
    {
        // per-workload ready counters live in the first line of the SPURS instance (the PM
        // updates them with atomic DMA), so publish the value there under the reservation lock
        std::lock_guard lk(atomic_lock(g_sp.ea));
        st8(g_sp.ea + wid, (u8)std::min<u32>((u32)ARG(2), 255));
        line_written(g_sp.ea);   // even an unchanged value: a PM deciding "no work" on the old line must retry
    }
    joblist_kick((u32)g_sp.wkl[wid].arg, (u32)ARG(2));
    if (g_ready_calls[wid]++ < 20) {
        LOG_I("cellSpursReadyCountStore(spurs=%08llx, wid=%u, %u) t=%.2fs", (unsigned long long)ARG(0), wid, (u32)ARG(2),
              time_usec() / 1e6);
        dump_wkl_arg("ready store", wid, g_sp.wkl[wid].arg);
        // the first queued job's descriptor (entry at +0x20: u32 header, u32 EA)
        u32 job = ld32((u32)g_sp.wkl[wid].arg + 0x24);
        if (job >= 0x10000 && (job & 0xF) == 0) dump_wkl_arg("first job", wid, job);
    }
    RET(CELL_OK);
}
HLE_FUNC(cellSpursRequestIdleSpu) { RET(CELL_OK); }
HLE_FUNC(cellSpursAttachLv2EventQueue) { RET(CELL_OK); }
HLE_FUNC(cellSpursDetachLv2EventQueue) { RET(CELL_OK); }
HLE_FUNC(cellSpursSetExceptionEventHandler) { RET(CELL_OK); }
// cellSpursAddWorkload(spurs, u32* wid, pm, size, arg, priority*, minContention, maxContention)
HLE_FUNC(cellSpursAddWorkload) {
    std::lock_guard lk(g_sp.m);
    u32 wid = 0;
    while (wid < 16 && g_sp.wkl[wid].used) wid++;
    if (wid == 16) RET((s32)0x80410709);
    Workload& w = g_sp.wkl[wid];
    w = {};
    w.pm = (u32)ARG(2);
    w.size = (u32)ARG(3);
    w.max_contention = std::clamp<u32>((u32)ARG(7), 1, std::max(1u, g_sp.nspus));
    w.arg = ARG(4);
    w.pm_native = g_pm_native && spu_pm_image_ok(gmem(w.pm), w.size);
    st32((u32)ARG(1), wid);
    LOG_I("SPURS workload %u: pm=%08x size=%x arg=%016llx contention %llu..%llu%s", wid, w.pm, w.size,
          (unsigned long long)w.arg, (unsigned long long)ARG(6), (unsigned long long)ARG(7), w.pm_native ? " (native job manager)" : "");
    if (!g_sp.started) {
        g_sp.started = true;
        // F1CE_SPURS_SPUS overrides the number of SPU workers (debugging)
        u32 n = std::max(1u, g_sp.nspus);
        if (const char* e = getenv("F1CE_SPURS_SPUS")) n = std::max(1, atoi(e));
        for (u32 i = 0; i < n; i++) std::thread(spu_worker, i).detach();
        if (getenv("F1CE_SPU_PROFILE")) std::thread(spu_profiler, n).detach();
    }
    w.used = true;
    RET(CELL_OK);
}
HLE_FUNC(cellSpursRemoveWorkload) { RET(CELL_OK); }
HLE_FUNC(cellSpursShutdownWorkload) { RET(CELL_OK); }
HLE_FUNC(cellSpursWaitForWorkloadShutdown) { RET(CELL_OK); }
// Taskset structure (in guest memory, as the task-side library in the game's task ELFs reads it):
//   +0x00 running, +0x10 ready, +0x20 pending ready, +0x30 enabled, +0x40 signalled, +0x50 waiting
//   (128-bit task masks), +0x60 u64 SPURS EA, +0x68 u64 taskset argument, +0x74 u32 workload id
//   (a task signalling another one also raises bit 0x8000 >> wid of the u16 at SPURS +0x70 when wid < 16)
HLE_FUNC(cellSpursCreateTaskset) {
    u32 ts = (u32)ARG(1);
    LOG_I("cellSpursCreateTaskset(spurs=%08x, taskset=%08x, arg=%016llx)", (u32)ARG(0), ts, (unsigned long long)ARG(2));
    memset(gmem(ts), 0, 0x80);
    st64(ts + 0x60, (u32)ARG(0));
    st64(ts + 0x68, ARG(2));
    st32(ts + 0x74, 0x10);   // no SPURS workload behind it: tasks run on host threads
    RET(CELL_OK);
}
HLE_FUNC(cellSpursShutdownTaskset) { RET(CELL_OK); }
HLE_FUNC(cellSpursJoinTaskset) { RET(CELL_OK); }

namespace {
// Task kernel interface (derived from the task ELFs' runtime): the task reads the SPURS EA from
// ls[0x1C0] (and a DMA tag from ls[0x1CC]); its exit path calls the u32 at [ls[0x2FB8] + 0xC4].
constexpr u32 TASK_BASE = 0x3000, LS_TASK_TABLE = 0x2FB8, TASK_TABLE = 0x2E00, SVC_TASK_EXIT = 0x220;

// Finished task contexts are kept per ELF and reused: the game starts ~200 tasks per second from a few ELFs, and a
// reused context keeps its decoded SPU blocks (the reload below rewrites only the 128-byte lines that differ).
std::mutex g_task_pool_m;
std::unordered_multimap<u32, std::unique_ptr<SPUThread>> g_task_pool;   // task ELF EA -> idle context

std::unique_ptr<SPUThread> take_task_context(u32 elf) {
    {
        std::lock_guard lk(g_task_pool_m);
        if (auto it = g_task_pool.find(elf); it != g_task_pool.end()) {
            auto spu = std::move(it->second);
            g_task_pool.erase(it);
            SPUThread& s = *spu;
            memset(s.gpr, 0, sizeof(s.gpr));
            s.mfc_lsa = s.mfc_eah = s.mfc_eal = s.mfc_size = s.mfc_tag = 0;
            s.tag_mask = s.atomic_stat = s.event_mask = s.event_stat = s.dec_start = s.srr0 = s.stall_stat = 0;
            s.dec_time = 0;
            s.interrupts = s.has_reservation = s.in_mbox_valid = s.out_mbox_valid = false;
            s.event_completion_pending = false;
            for (auto& l : s.stalled) l = {};
            s.snr[0] = s.snr[1] = 0;
            s.instructions = 0;
            return spu;
        }
    }
    auto spu = std::make_unique<SPUThread>();
    memset(spu->ls, 0, 0x40000);
    // debug: F1CE_SPU_NOAOT_TASK=<elf hex> runs the tasks of that ELF in the interpreter (AOT against interpreter)
    static const char* noaot = getenv("F1CE_SPU_NOAOT_TASK");
    if (noaot && (u32)strtoul(noaot, nullptr, 16) == elf) spu->no_aot = true;
    return spu;
}

// sys_spu_thread_send_event from SPURS tasks / workloads (OutIntrMbox = port << 24 | data0, OutMbox = data1): not
// connected to an lv2 event queue here, so the request is acknowledged (CELL_OK) and logged. Without a reply the SPU
// waited forever on its second mailbox write, its task never finished and the PPU waited on an event flag that
// task would have set (freeze in the race).
// Prints (data1 -> saved registers, r4 first = format string) are logged with their text.
u32 spurs_send_event(SPUThread& s, u32 intr, u32 data1) {
    static std::atomic<int> n{0};
    if (n++ < 50) {
        u32 p = __builtin_bswap32(*(u32*)(s.ls + (data1 & 0x3FFF0))) & 0x3FFFF;
        LOG_I("SPU %s: send_event port %u data0=%06x data1=%08x at %05x (acknowledged) print: %.80s", s.name.c_str(), intr >> 24,
              intr & 0xFFFFFF, data1, s.pc, (const char*)s.ls + p);
    }
    return 0;
}

// Freeze diagnostics: tasks run on host threads, invisible to the PPU watchdog. When a PPU thread stays in an event
// flag wait for 3 s, dump_flag_stall logs the live tasks, the last finished ones and the flags and queues created last,
// and saves the live tasks' local stores (freeze_task<tid>.bin in the working directory).
struct LiveTask { SPUThread* spu; Task t; u64 start; };
struct DoneTask { u32 tid, elf, r3; u64 arg0, arg1, instr, t; };
std::mutex g_live_m;
std::map<u32, LiveTask> g_live;
DoneTask g_done[16];
u32 g_done_pos = 0;
std::mutex g_sync_m;
std::deque<std::pair<u32, char>> g_sync_objs;   // recent flags ('F') and queues ('Q')
void note_sync_obj(u32 ea, char kind) {
    std::lock_guard lk(g_sync_m);
    g_sync_objs.push_back({ea, kind});
    while (g_sync_objs.size() > 16) g_sync_objs.pop_front();
}
void dump_flag_stall(u32 f, u16 mask, u32 mode) {
    LOG_W("freeze: PPU waits on event flag %08x for 3 s (mask %04x mode %u, bits %04x)", f, mask, mode, ld16(f));
    u64 now = time_usec();
    {
        std::lock_guard lk(g_live_m);
        LOG_W("freeze: %zu live tasks, %zu tasks created", g_live.size(), g_tasks.size());
        for (auto& [tid, l] : g_live) {
            SPUThread& s = *l.spu;
            u32 pc0 = s.pc; u64 i0 = s.instructions;
            sleep_usec(20000);
            LOG_W("freeze: task %u elf=%08x arg=%016llx%016llx age %llu ms pc=%05x -> %05x instr %llu -> %llu halted=%d tag_mask=%x stall=%x res=%d/%08x mbox in=%d out=%d",
                  tid, l.t.elf, (unsigned long long)l.t.arg[0], (unsigned long long)l.t.arg[1], (unsigned long long)((now - l.start) / 1000),
                  pc0, s.pc, (unsigned long long)i0, (unsigned long long)s.instructions, (int)s.halted, s.tag_mask, s.stall_stat,
                  (int)s.has_reservation, s.res_addr, (int)s.in_mbox_valid, (int)s.out_mbox_valid);
            char fn[64];
            snprintf(fn, sizeof(fn), "freeze_task%u.bin", tid);
            if (FILE* o = fopen(fn, "wb")) { fwrite(s.ls, 1, 0x40000, o); fclose(o); }
        }
        for (u32 i = 0; i < 16; i++) {
            const DoneTask& e = g_done[(g_done_pos + i) % 16];
            if (e.elf) LOG_W("freeze: finished task %u elf=%08x arg=%016llx%016llx r3=%x instr=%llu, %llu ms ago", e.tid, e.elf,
                             (unsigned long long)e.arg0, (unsigned long long)e.arg1, e.r3, (unsigned long long)e.instr, (unsigned long long)((now - e.t) / 1000));
        }
    }
    std::lock_guard lk(g_sync_m);
    for (auto [ea, kind] : g_sync_objs) {
        std::string l;
        for (u32 o = 0; o < 0x40; o += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32(ea + o)); l += b; }
        LOG_W("freeze: %c %08x:%s", kind, ea, l.c_str());
    }
}

void run_task(u32 tid, Task t) {
    set_thread_name("spu task");
    auto spu = take_task_context(t.elf);
    {
        std::lock_guard lk(g_live_m);
        g_live[tid] = {spu.get(), t, time_usec()};
    }
    // TerrainCollider prints through send_event (data1 -> saved registers, r4 first = format string) when a queue pop
    // keeps failing, for instance while another task is preempted between PopBegin and PopEnd, more likely in heavy
    // scenes (Monaco grid). The routine waits for a second mailbox word from the PPU-side handler, which never came:
    // the task hung, main waited on its flag for ever (freeze with SPURS workloads 0-2 and 4 idle).
    spu->on_send_event = spurs_send_event;
    spu->event_needs_completion = true;
    spu->report_ie = true;
    spu->id = 8 + tid;
    spu->name = "task" + std::to_string(tid);
    // fresh image: zeroed LS + PT_LOAD segments (SPU ELF: entry at 0x18, program headers at 0x1C, count at 0x2C)
    thread_local std::vector<u8> img(0x40000);
    std::fill(img.begin(), img.end(), 0);
    u32 entry = ld32(t.elf + 0x18), phoff = ld32(t.elf + 0x1C), phnum = ld16(t.elf + 0x2C);
    for (u32 i = 0; i < phnum; i++) {
        u32 ph = t.elf + phoff + i * 32;
        if (ld32(ph) != 1) continue;
        u32 off = ld32(ph + 4), vaddr = ld32(ph + 8), filesz = ld32(ph + 16);
        memcpy(img.data() + vaddr, gmem(t.elf + off), filesz);
    }
    // F1CE_STUCK_TEST=1 (debug): TerrainCollider gives up after 2 pops instead of 100000, so it reaches its STUCK message
    static const bool stuck_test = getenv("F1CE_STUCK_TEST") != nullptr;
    if (stuck_test && t.elf == 0x10082500) { u32 w = __builtin_bswap32(0x42000151); memcpy(img.data() + 0xE288, &w, 4); }
    for (u32 a = 0; a < 0x40000; a += 128)
        if (memcmp(spu->ls + a, img.data() + a, 128)) spu->write_ls(a, img.data() + a, 128);
    u64 spurs = __builtin_bswap64(ld64(t.taskset + 0x60));
    memcpy(spu->ls + 0x1C0, &spurs, 8);
    spu->ls32(0x1CC, 0);
    spu->ls32(LS_TASK_TABLE, TASK_TABLE);
    spu->ls32(TASK_TABLE + 0xC4, SVC_TASK_EXIT);
    // entry registers: r3 = task argument (16 bytes), r4 = taskset argument
    for (int k = 0; k < 8; k++) {
        spu->gpr[3].b[15 - k] = (u8)(t.arg[0] >> (56 - 8 * k));
        spu->gpr[3].b[7 - k] = (u8)(t.arg[1] >> (56 - 8 * k));
    }
    spu->gpr[4].d[1] = ld64(t.taskset + 0x68);
    spu->set_pref(1, 0x3FFF0);
    spu->pc = entry;
    spu->kernel_limit = TASK_BASE;
    spu->on_kernel_call = [tid](SPUThread& s) {
        if (s.pc == SVC_TASK_EXIT) {
            static std::atomic<int> logged{0};
            if (logged++ < 30) LOG_I("SPURS task %u: exit (r3=%x) after %llu instructions", tid, s.pref(3), (unsigned long long)s.instructions);
            s.halted = true;
            return true;
        }
        fatal("SPURS task %u: branch to unknown kernel address %05x (r0=%05x)", tid, s.pc, s.pref(0));
    };
    spu->on_stop = [tid](SPUThread& s, u32 code) {
        LOG_W("SPURS task %u: stop 0x%x at %05x", tid, code, s.pc);
        return false;
    };
    // diagnostics (F1CE_TASK_DMA_LOG): each distinct (pc, command, 128-byte line) DMA of the tasks once
    // (F1CE_TASK_DMA_LOG=<elf hex> limits it to the tasks of that ELF)
    static const char* dma_env = getenv("F1CE_TASK_DMA_LOG");
    static const u32 dma_elf = dma_env ? (u32)strtoul(dma_env, nullptr, 16) : 0;
    if (dma_env && (dma_elf <= 1 || dma_elf == t.elf)) spu->on_dma = [tid](SPUThread& s, u32 cmd, u32 lsa, u32 ea, u32 size) {
        static std::mutex m;
        static std::set<u64> seen;
        u64 key = ((u64)s.pc << 40) ^ ((u64)(cmd & 0xFF) << 32) ^ (ea & ~127u);
        std::lock_guard lk(m);
        if (seen.size() < 2000 && seen.insert(key).second) {
            std::string words;   // small transfers (parameter blocks): their data too
            for (u32 o = 0; size <= 0x80 && o < size; o += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32(ea + o)); words += b; }
            LOG_I("task %u dma pc=%05x cmd=%02x lsa=%05x ea=%08x size=%x%s", tid, s.pc, cmd & 0xFF, lsa, ea, size, words.c_str());
        }
    };
    static std::atomic<int> logged{0};
    if (logged++ < 30)
        LOG_I("SPURS task %u: start elf=%08x entry=%05x arg=%016llx%016llx", tid, t.elf, entry, (unsigned long long)t.arg[0],
              (unsigned long long)t.arg[1]);
    const u64 task_t0 = time_usec();
    spu->run();
    g_task_runs.fetch_add(1, std::memory_order_relaxed);
    g_task_usec.fetch_add(time_usec() - task_t0, std::memory_order_relaxed);
    g_task_interp.fetch_add(spu->st_interp, std::memory_order_relaxed);   // (SPURS report: untranslated code)
    spu->st_interp = 0;
    {
        std::lock_guard lk(g_live_m);
        g_live.erase(tid);
        g_done[g_done_pos++ % 16] = {tid, t.elf, spu->pref(3), t.arg[0], t.arg[1], spu->instructions, time_usec()};
    }
    std::lock_guard lk(g_task_pool_m);
    g_task_pool.emplace(t.elf, std::move(spu));
}

// Task threads are kept and reused: a new task wakes an idle one (creating a thread per task cost tens of microseconds
// each, ~500 times per second, on the path of main's waits for the tasks' flags). A thread is created only when none is
// idle, so a task that waits for another (queues between tasks) never holds the next one back.
std::mutex g_task_q_m;
std::condition_variable g_task_q_cv;
std::deque<std::pair<u32, Task>> g_task_q;
u32 g_task_idle = 0;
void task_thread() {
    raise_thread_priority();
    for (;;) {
        std::pair<u32, Task> job;
        {
            std::unique_lock lk(g_task_q_m);
            g_task_idle++;
            g_task_q_cv.wait(lk, [] { return !g_task_q.empty(); });
            g_task_idle--;
            job = g_task_q.front();
            g_task_q.pop_front();
        }
        run_task(job.first, job.second);
    }
}
void start_task(u32 tid, const Task& t) {
    static const bool per_task = getenv("F1CE_TASK_THREADS") != nullptr;   // debug: a new thread per task, as before
    if (per_task) { std::thread(run_task, tid, t).detach(); return; }
    bool spawn;
    {
        std::lock_guard lk(g_task_q_m);
        spawn = g_task_idle <= g_task_q.size();   // every idle thread already has a task waiting for it
        g_task_q.push_back({tid, t});
    }
    if (spawn) std::thread(task_thread).detach();
    else g_task_q_cv.notify_one();
}
}  // namespace

// cellSpursCreateTask(taskset, u32* tid, elf, context, size, lsPattern*, argument*)
HLE_FUNC(cellSpursCreateTask) {
    std::lock_guard lk(g_spurs_mutex);
    Task t{(u32)ARG(0), (u32)ARG(2), (u32)ARG(3), (u32)ARG(5), {0, 0}};
    if (ARG(6)) { t.arg[0] = ld64((u32)ARG(6)); t.arg[1] = ld64((u32)ARG(6) + 8); }
    u32 tid = (u32)g_tasks.size();
    g_tasks.push_back(t);
    st32((u32)ARG(1), tid);
    start_task(tid, t);
    RET(CELL_OK);
}

// event flags
// CellSpursEventFlag (128 bytes, layout as used by the task-side library in the game's task ELFs):
//   +0x00 u16 event bits, +0x04 u16 bits a PPU waiter wants, +0x06 its wait mode, +0x07 notification pending,
//   +0x0C SPU port for the PPU notification, +0x0E direction (0 SPU2SPU, 1 SPU2PPU, 2 PPU2SPU), +0x0F clear mode
//   (0 = auto-clear the bits a wait consumed), +0x30 bits pending for the waiter.
// The PPU side never registers itself as a waiter (+0x04 stays 0): an SPU then just ORs its bits into +0x00,
// which the PPU wait below polls under the reservation mutex.

// _cellSpursEventFlagInitialize(spurs, taskset, eventFlag*, clearMode, direction) (one of spurs/taskset is null)
HLE_FUNC(_cellSpursEventFlagInitialize) {
    u32 f = (u32)ARG(2);
    LOG_I("_cellSpursEventFlagInitialize(spurs=%08x, taskset=%08x, flag=%08x, clear=%u, direction=%u)", (u32)ARG(0),
          (u32)ARG(1), f, (u32)ARG(3), (u32)ARG(4));
    note_sync_obj(f, 'F');
    std::lock_guard lk(atomic_lock(f));
    memset(gmem(f), 0, 128);
    st8(f + 0x0E, (u8)ARG(4));
    st8(f + 0x0F, (u8)ARG(3));
    line_written(f);
    RET(CELL_OK);
}
HLE_FUNC(cellSpursEventFlagAttachLv2EventQueue) { RET(CELL_OK); }
HLE_FUNC(cellSpursEventFlagDetachLv2EventQueue) { RET(CELL_OK); }
HLE_FUNC(cellSpursEventFlagSet) {
    u32 f = (u32)ARG(0);
    std::lock_guard lk(atomic_lock(f));
    st16(f, ld16(f) | (u16)ARG(1));
    line_written(f);
    RET(CELL_OK);
}
HLE_FUNC(cellSpursEventFlagClear) {
    u32 f = (u32)ARG(0);
    std::lock_guard lk(atomic_lock(f));
    st16(f, ld16(f) & ~(u16)ARG(1));
    line_written(f);
    RET(CELL_OK);
}
// cellSpursEventFlagWait(eventFlag, u16* mask, mode)  mode: 0 = AND, 1 = OR; *mask receives the event bits
// waits of the PPU-side SPURS calls: the first call arms on the object's line (the loop checks once more), later ones block
// until the line is written (SPU or PPU atomics, HLE updates) or 1 ms passed; F1CE_SPIN_WAIT=1: the old 50 us spin
static void line_block(LineWait& lw, u32 ea) {
    static const bool old_spin = getenv("F1CE_SPIN_WAIT") != nullptr;
    if (old_spin) { sleep_usec(50); return; }
    if (!lw.armed) { line_wait_arm(lw, ea, ea); return; }
    line_wait_block(lw, 1);
}

static s32 flag_wait(u32 f, u32 mask_ptr, u32 mode, bool block) {
    u16 mask = ld16(mask_ptr);
    u64 wait_start = 0;
    LineWait lw;
    struct Rel { LineWait& w; ~Rel() { line_wait_release(w); } } rel{lw};
    bool dumped = false;
    for (u64 spins = 0;; spins++) {
        if (block && !dumped && spins >= 64) {
            if (!wait_start) wait_start = time_usec();
            else if (time_usec() - wait_start > 3000000) { dumped = true; dump_flag_stall(f, mask, mode); }
        }
        {
            std::lock_guard lk(atomic_lock(f));
            u16 ev = ld16(f);
            if (mode == 0 ? (ev & mask) == mask : (ev & mask) != 0) {
                st16(mask_ptr, ev);
                if (ld8(f + 0x0F) == 0) { st16(f, ev & ~mask); line_written(f); }   // CELL_SPURS_EVENT_FLAG_CLEAR_AUTO
                return CELL_OK;
            }
        }
        if (!block) return (s32)0x80410901;   // CELL_SPURS_TASK_ERROR_AGAIN
        if (spins < 64) std::this_thread::yield();
        else line_block(lw, f);
    }
}
HLE_FUNC(cellSpursEventFlagWait) { RET(flag_wait((u32)ARG(0), (u32)ARG(1), (u32)ARG(2), true)); }
HLE_FUNC(cellSpursEventFlagTryWait) { RET(flag_wait((u32)ARG(0), (u32)ARG(1), (u32)ARG(2), false)); }
HLE_FUNC(cellSpursEventFlagGetClearMode) { st32((u32)ARG(1), ld8((u32)ARG(0) + 0x0F)); RET(CELL_OK); }
HLE_FUNC(cellSpursEventFlagGetDirection) { st32((u32)ARG(1), ld8((u32)ARG(0) + 0x0E)); RET(CELL_OK); }

// queues
// CellSpursQueue (128 bytes, layout as used by the task-side library in the game's task ELFs):
//   +0x00 pop counter, +0x04 push counter (both modulo 2 * depth; stored as ~value while an operation is in
//   progress), +0x08 element size, +0x0C depth, +0x10 u64 buffer EA, +0x1C direction (0 SPU2SPU, 1 SPU2PPU,
//   2 PPU2SPU), +0x20 flag byte. Items = (push - pop) mod 2*depth; an element lives at buffer + (counter % depth) * size.
// PPU-side operations update it under the reservation mutex, so SPU getllar/putllc loops see the changes.
namespace {
u32 qcount(u32 pop, u32 push, u32 depth) { return push >= pop ? push - pop : push + 2 * depth - pop; }

// push (pop_side = false) or pop (true) one element between the queue buffer and guest memory at `data`
s32 queue_op(u32 q, u32 data, bool blocking, bool pop_side) {
    {   // diagnostics: PPU-side operations per queue
        static std::mutex m;
        static std::map<u32, std::pair<u64, u64>> ops;
        static u64 total = 0;
        std::lock_guard lk(m);
        auto& o = ops[q];
        (pop_side ? o.second : o.first)++;
        if (++total % 500 == 1) {
            std::string r;
            for (auto& [k, v] : ops) { char b[64]; snprintf(b, sizeof(b), " %08x:push%llu/pop%llu", k, (unsigned long long)v.first, (unsigned long long)v.second); r += b; }
            LOG_I("SPURS queues (PPU side):%s", r.c_str());
        }
    }
    LineWait lw;
    struct Rel { LineWait& w; ~Rel() { line_wait_release(w); } } rel{lw};
    for (u64 spins = 0;; spins++) {
        {
            std::lock_guard lk(atomic_lock(q));
            u32 pop = ld32(q), push = ld32(q + 4), size = ld32(q + 8), depth = ld32(q + 12);
            u64 buf = ld64(q + 16);
            if (!depth) return (s32)0x80410902;   // not initialised
            bool busy = (s32)pop < 0 || (s32)push < 0;
            u32 n = qcount(pop, push, depth);
            if (!busy && (pop_side ? n > 0 : n < depth)) {
                u32 c = pop_side ? pop : push;
                u32 elem = (u32)buf + (c % depth) * size;
                if (pop_side) memcpy(gmem(data), gmem(elem), size);
                else memcpy(gmem(elem), gmem(data), size);
                st32(q + (pop_side ? 0 : 4), (c + 1) % (2 * depth));
                line_written(q);
                return CELL_OK;
            }
        }
        if (!blocking) return (s32)0x80410901;   // CELL_SPURS_TASK_ERROR_AGAIN
        if (spins < 64) std::this_thread::yield();
        else line_block(lw, q);
    }
}
}  // namespace

// _cellSpursQueueInitialize(spurs, taskset, queue*, buffer, size, depth, direction) (one of spurs/taskset is null)
HLE_FUNC(_cellSpursQueueInitialize) {
    u32 q = (u32)ARG(2);
    LOG_I("_cellSpursQueueInitialize(spurs=%08x, taskset=%08x, queue=%08x, buffer=%08x, size=%u, depth=%u, direction=%u)",
          (u32)ARG(0), (u32)ARG(1), q, (u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(6));
    note_sync_obj(q, 'Q');
    std::lock_guard lk(atomic_lock(q));
    memset(gmem(q), 0, 128);
    st32(q + 8, (u32)ARG(4));
    st32(q + 12, (u32)ARG(5));
    st64(q + 16, (u32)ARG(3));
    st32(q + 0x1C, (u32)ARG(6));
    line_written(q);
    RET(CELL_OK);
}
HLE_FUNC(cellSpursQueueAttachLv2EventQueue) { RET(CELL_OK); }
HLE_FUNC(cellSpursQueueDetachLv2EventQueue) { RET(CELL_OK); }
HLE_FUNC(cellSpursQueueClear) {
    u32 q = (u32)ARG(0);
    std::lock_guard lk(atomic_lock(q));
    st32(q, 0);
    st32(q + 4, 0);
    line_written(q);
    RET(CELL_OK);
}
// cellSpursQueuePushBody(queue, buffer, isBlocking)
HLE_FUNC(cellSpursQueuePushBody) { RET(queue_op((u32)ARG(0), (u32)ARG(1), ARG(2) != 0, false)); }
// cellSpursQueuePopBody(queue, buffer, isBlocking)
HLE_FUNC(cellSpursQueuePopBody) { RET(queue_op((u32)ARG(0), (u32)ARG(1), ARG(2) != 0, true)); }
