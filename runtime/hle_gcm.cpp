// HLE: cellGcmSys + a FIFO-processing RSX front end.
//
// The RSX thread walks the command buffer exactly like the real GPU front end (jumps, calls,
// returns, method headers) so that the game's synchronisation (references, semaphores, labels,
// reports and flips) behaves correctly. Draw methods are forwarded to the renderer hook.
#include "host.h"
#include "kernel.h"
#include "rsx.h"

#include <array>
#include <bit>
#include <cmath>
#include <vector>
#include <deque>
#include <memory>
void set_thread_name(const char* name);   // core.cpp

namespace rsx { u64 g_last_frame_draws = 0; }
std::string profile_main_between(u64 t0, u64 t1);   // core.cpp
bool shake_filter_on();   // hle_physics.cpp
void log_thread_cpu();    // core.cpp
u64 g_rsx_idle_us = 0;   // profiling: time the FIFO reader waited for the PPU (read by rsx_gl.cpp)

void place_thread(int kind);      // core.cpp
void profile_register_thread();   // core.cpp (host profiler)

extern u32 g_ticks_per_frame_ea;

namespace {
constexpr u32 LOCAL_SIZE = 0x0F900000;
constexpr u32 CMD_SEGMENT = 32 * 1024;
constexpr u32 GCM_FLIP_COMMAND = 0xFEAC;

struct DisplayBuffer { u32 offset = 0, pitch = 0, width = 0, height = 0; };

struct GcmState {
    bool init = false;
    u32 context_addr = 0;       // CellGcmContextData* returned to the game
    u32 control_addr = 0;       // CellGcmControl {put, get, ref}
    u32 label_addr = 0;         // 256 labels * 16 bytes
    u32 io_addr = 0, io_size = 0, cmd_size = 0;
    u32 config_addr = 0;
    std::vector<u32> io_map = std::vector<u32>(4096, 0xFFFFFFFF);   // 1 MiB pages: io offset -> ea
    u32 io_next = 0;
    DisplayBuffer buffers[8];
    u32 flip_handler = 0, vblank_handler = 0, user_handler = 0, queue_handler = 0, graphics_handler = 0;
    u32 flip_mode = 0;
    std::atomic<u32> flip_status{0};
    std::atomic<u64> flips_queued{0}, flips_done{0};   // cellGcmSetFlip calls / flips processed by the RSX
    std::atomic<u64> vblank_count{0};
    PPUThread* intr_thread = nullptr;
    std::mutex intr_mutex;
    std::condition_variable intr_cv;
    std::vector<std::pair<u32, u32>> intr_queue;   // (handler, arg)
};
GcmState g;

// recent FIFO history for diagnostics: (get, header)
std::pair<u32, u32> g_hist[32];
u32 g_hist_pos = 0;

// recent semaphore operations: (kind 'A'cquire / 'R'elease, address, value, wait us, get)
struct SemRec { char kind; u32 addr, val, wait_us, get, before; u64 t = 0; };
SemRec g_sem_hist[16];
u32 g_sem_pos = 0;
// recent flow commands: (time ms, get, command, put, reference)
struct FlowRec { u32 ms, get, cmd, put, ref; };
FlowRec g_flow_hist[48];
u32 g_flow_pos = 0;
u32 g_cur_get = 0;

u32 io_to_ea(u32 io);
void dump_history() {
    for (u32 i = 0; i < 48; i++) {
        const FlowRec& r = g_flow_hist[(g_flow_pos + i) % 48];
        if (r.cmd) LOG_E("  flow t=%u ms @%08x %08x put=%08x ref=%08x", r.ms, r.get, r.cmd, r.put, r.ref);
    }
    for (u32 i = 0; i < 16; i++) {
        const SemRec& r = g_sem_hist[(g_sem_pos + i) % 16];
        if (r.kind) LOG_E("  sem %c @%08x val=%08x (was %08x) waited %u us at get=%08x", r.kind, r.addr, r.val, r.before, r.wait_us, r.get);
    }
    std::string s;
    for (u32 o = 0x10000; o < 0x10080; o += 4) {
        char b[12];
        snprintf(b, sizeof(b), " %08x", ld32(g.io_map[0] + o));
        s += b;
    }
    LOG_E("  io[0x10000..0x10080]:%s", s.c_str());
    for (u32 base : {0x11000u, 0x44100u}) {
        s.clear();
        for (u32 o = base; o < base + 0x60; o += 4) {
            char b[12];
            snprintf(b, sizeof(b), " %08x", ld32(g.io_map[0] + o));
            s += b;
        }
        LOG_E("  io[%x..]:%s", base, s.c_str());
    }
    u32 ctx = g.context_addr;
    LOG_E("  libgcm context: begin=%08x end=%08x current=%08x callback=%08x", ld32(ctx), ld32(ctx + 4),
          ld32(ctx + 8), ld32(ctx + 12));
    for (u32 i = 0; i < 32; i++) {
        auto [get, cmd] = g_hist[(g_hist_pos + i) % 32];
        if (cmd || get) LOG_E("  fifo get=%08x cmd=%08x", get, cmd);
    }
}

u32 io_to_ea(u32 io) {
    u32 page = g.io_map[(io >> 20) & 4095];
    if (page == 0xFFFFFFFF) {
        dump_history();
        fatal("RSX: unmapped IO offset %08x (put=%08x get=%08x)", io, ld32(g.control_addr), ld32(g.control_addr + 4));
    }
    return page + (io & 0xFFFFF);
}

u32 ea_to_io(u32 ea) {
    for (u32 i = 0; i < 4096; i++)
        if (g.io_map[i] != 0xFFFFFFFF && ea >= g.io_map[i] && ea < g.io_map[i] + 0x100000) return (i << 20) | (ea - g.io_map[i]);
    return 0xFFFFFFFF;
}

void io_map(u32 ea, u32 io, u32 size) {
    for (u32 o = 0; o < size; o += 0x100000) g.io_map[(io + o) >> 20] = ea + o;
}

// queue a guest handler to run on the GCM interrupt thread
void raise_interrupt(u32 handler, u32 arg) {
    if (!handler) return;
    {
        std::lock_guard lk(g.intr_mutex);
        g.intr_queue.push_back({handler, arg});
    }
    g.intr_cv.notify_one();
}

// ------------------------------------------------------------------ RSX front end
struct Rsx {
    u32 sem_offset_406e = 0;
    u32 sem_offset_4097 = 0;
    u32 call_stack[8] = {};
    u32 call_depth = 0;
} rsx_state;

u32 label_ptr(u32 offset) { return g.label_addr + offset; }
// the label a SEMAPHORE_ACQUIRE waits on (0: none). Plain PPU stores do not wake line waits, and the RSX's 1 ms timeout
// sometimes took 10-30 ms on its pinned processor: the vblank handler's release of the flip label then came a vblank
// late every 0.25-0.5 s (1% low ~16 fps at 30). So the interrupt thread wakes this line after each handler.
std::atomic<u32> g_rsx_acquire_ea{0};

// the player's front wheel in the world, in a 3D scene: the swing of pace_ticks shows here, and only a stopped car has it. Frames of
// two ticks hide the swing (the wheel reads the same each time), so once on, this holds until the car really moves or the scene ends.
bool car_standing() {
    const u32 wheel = 0x10110670;
    static float px, pz;
    static int small = 0, since_swing = 1000;
    static bool hold = false;
    const float x = std::bit_cast<float>(ld32(wheel)), z = std::bit_cast<float>(ld32(wheel + 8));
    const float moved = std::fabs(x - px) + std::fabs(z - pz);
    px = x;
    pz = z;
    static int changes = 0;
    static bool in_3d = false;
    if ((rsx::g_last_frame_draws >= 800) != in_3d) {
        in_3d = !in_3d;
        if (changes++ < 200) LOG_I("60 fps: %s 3D scene at %.2f s", in_3d ? "entering" : "leaving", time_usec() / 1e6);
    }
    if (moved >= 0.03f || rsx::g_last_frame_draws < 800) {
        if (hold && changes++ < 200)
            LOG_I("60 fps: hold off at %.2f s (moved %.4f, %u draws)", time_usec() / 1e6, moved, (u32)rsx::g_last_frame_draws);
        small = 0;
        hold = false;
        return false;
    }
    small++;
    since_swing = moved > 0 ? 0 : since_swing + 1;
    // (8 frames: with 30 the car shook for ~0.6 s each time the grid appeared)
    if (!hold && small >= 8 && since_swing < 8) {
        hold = true;
        if (changes++ < 200) LOG_I("60 fps: hold on at %.2f s", time_usec() / 1e6);
    }
    return hold;
}

// F1CE_60FPS: the game logic runs one tick per 59.94 Hz vblank, so the next frame gets one tick for each vblank period
// this one took. The range is the game's own at the top (2 ticks: 30 fps); below 30 fps it slows down as on the PS3.
void pace_ticks() {
    const s64 tick_us = 16683;
    static u64 last = time_usec(), window = last, flips = 0, ticks = 0;
    static s64 owed = 0;
    u64 now = time_usec();
    const u64 previous = last;
    owed += (s64)(now - last);
    last = now;
    s64 n = std::clamp<s64>((owed + tick_us / 2) / tick_us, 1, 2);
    // A stopped car does not settle: its tyre forces change sign every tick and the heading swings ~0.08 degrees (the wheels
    // ~3 mm). The game shows every second tick, so that never appears; one tick per frame shows both phases and the car shakes
    // on the grid. So while it stands, a frame takes two ticks and 33 ms, as in the original game.
    // (off with the shake filter of hle_physics.cpp, which removes the swing from what is drawn)
    static const bool no_hold = getenv("F1CE_60FPS_NOHOLD") != nullptr || shake_filter_on();
    if (!no_hold && car_standing()) {
        n = 2;
        const u64 end = previous + 2 * tick_us;
        if (now < end) { sleep_until_precise(end); last = end; }
        owed = 0;
    }
    // F1CE_FPS_CAP=<n>: frames of at least 1/n s (30: two ticks each, as the original, but with this mode's pacing: the
    // next frame's work is not held back to a vblank, which on a slow machine kept the original mode under 30)
    static const int cap = getenv("F1CE_FPS_CAP") ? atoi(getenv("F1CE_FPS_CAP")) : 0;
    if (cap > 0) {
        const u64 end = previous + 1000000 / (u64)cap;
        if (now < end) {
            sleep_until_precise(end);
            owed += (s64)(end - now);
            last = end;
            n = std::clamp<s64>((owed + tick_us / 2) / tick_us, 1, 2);
        }
    }
    static const bool fixed1 = getenv("F1CE_TICKS_FIXED") != nullptr;   // debug: one tick per frame whatever the time
    if (fixed1) n = 1;
    owed = std::clamp<s64>(owed - n * tick_us, -tick_us, tick_us);
    // debug: F1CE_TICK_DUMP=<s>,<ea hex>,<size hex>[,<ea hex>,<size hex>...]: <s> seconds into the first 3D scene, that
    // memory on 32 (F1CE_TICK_DUMP_FRAMES) consecutive frames (tick_<k>.bin for the first range, tick_<k>_<ea>.bin for the others), to find values
    // that swing from tick to tick
    static const char* tick_dump = getenv("F1CE_TICK_DUMP");
    if (tick_dump) {
        static u64 since3d = 0;
        static int k = -1;
        static std::vector<std::vector<u8>> snaps;
        static std::vector<std::pair<u32, u32>> ranges;
        if (k < 0 && rsx::g_last_frame_draws >= 800) {
            if (!since3d) since3d = now;
            if (now - since3d >= (u64)(atof(tick_dump) * 1e6)) {
                for (const char* p = strchr(tick_dump, ','); p && strchr(p + 1, ',');) {
                    const char* q = strchr(p + 1, ',');
                    ranges.emplace_back((u32)strtoul(p + 1, nullptr, 16), (u32)strtoul(q + 1, nullptr, 16));
                    p = strchr(q + 1, ',');
                }
                if (ranges.empty()) ranges.emplace_back(0x10100000, 0x40000);
                k = 0;
            }
        }
        static const int frames = getenv("F1CE_TICK_DUMP_FRAMES") ? atoi(getenv("F1CE_TICK_DUMP_FRAMES")) : 32;
        if (k >= 0 && k < frames) {
            for (const auto& [ea, size] : ranges) snaps.emplace_back(gmem(ea), gmem(ea) + size);
            if (++k == frames) {
                for (int i = 0; i < frames; i++)
                    for (size_t r = 0; r < ranges.size(); r++) {
                        char fn[48];
                        if (r == 0) snprintf(fn, sizeof(fn), "tick_%d.bin", i);
                        else snprintf(fn, sizeof(fn), "tick_%d_%08x.bin", i, ranges[r].first);
                        const auto& s = snaps[i * ranges.size() + r];
                        if (FILE* f = fopen(fn, "wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
                    }
                LOG_I("tick dump: %d frames of %zu ranges saved (ticks per frame %lld)", frames, ranges.size(), (long long)n);
            }
        }
    }
    st32(g_ticks_per_frame_ea, (u32)n);
    flips++;
    ticks += (u64)n;
    if (now - window >= 5000000) {
        LOG_I("60 fps: %.1f flips/s, %.1f ticks/s", flips * 1e6 / (now - window), ticks * 1e6 / (now - window));
        window = now; flips = ticks = 0;
    }
}

// cores in use by the whole process, averaged over 5 s: what the game costs in power, whatever the frame rate
void log_cpu_use() {
    static u64 window = time_usec(), last_cpu = process_cpu_usec(), flips = 0;
    flips++;
    u64 now = time_usec();
    if (now - window < 5000000) return;
    u64 cpu = process_cpu_usec();
    LOG_I("cpu: %.2f cores at %.1f fps", (double)(cpu - last_cpu) / (now - window), flips * 1e6 / (now - window));
    log_thread_cpu();
    window = now; last_cpu = cpu; flips = 0;
}

// frame pacing over 5 s: median / 99th percentile / worst flip interval ("1% low" = 1000 / p99), and for each frame over
// 45 ms what the renderer thread spent it on (semaphore waits = waiting for the game, flip = present + GPU)
void log_frame_pacing(u64 flip_us) {
    static u64 last = 0, window = 0, sem_last = 0, flip_last = 0;
    static std::vector<u32> iv;
    static int slow_logged = 0;
    static const bool prof = getenv("F1CE_PROFILE") != nullptr;   // + what the main thread did (core.cpp)
    const u64 now = time_usec();
    const u64 sem = rsx::g_fe.t_sem, fl = rsx::g_fe.t_flip;
    const u64 dsem = sem >= sem_last ? sem - sem_last : sem, dfl = fl >= flip_last ? fl - flip_last : fl;
    sem_last = sem; flip_last = fl;
    if (last) {
        const u32 d = (u32)(now - last);
        iv.push_back(d);
        if (d > 45000 && slow_logged < 400) {
            slow_logged++;
            LOG_I("pacing: slow frame %.1f ms at %.2f s | renderer: semaphore waits %.1f ms, flip %.1f ms, this flip %.1f ms, %u draws",
                  d / 1e3, now / 1e6, dsem / 1e3, dfl / 1e3, flip_us / 1e3, (u32)rsx::g_last_frame_draws);
            if (prof) {
                LOG_I("pacing:   main thread %s", profile_main_between(last, now).c_str());
                std::string sl;
                const u64 end = g_main_sleep_pos.load();
                for (u64 i = end > 256 ? end - 256 : 0; i < end; i++) {
                    const MainSleep& s = g_main_sleeps[i % 256];
                    if (s.t < last || s.t > now) continue;
                    char b[64];
                    snprintf(b, sizeof(b), " +%.1f lr=%08x %u->%u", (s.t - last) / 1e3, s.lr, s.req, s.took);
                    sl += b;
                }
                LOG_I("pacing:   main sleeps (ms into frame, lr, us asked->taken):%s", sl.c_str());
                std::string sm;
                for (u32 i = 0; i < 16; i++) {
                    const SemRec& r = g_sem_hist[(g_sem_pos + i) % 16];
                    if (r.kind != 'A' || r.wait_us < 2000) continue;
                    char b[64];
                    snprintf(b, sizeof(b), " @%08x=%08x %.3f+%.1f ms", r.addr, r.val, r.t / 1e6, r.wait_us / 1e3);
                    sm += b;
                }
                static u64 vb_last = 0;
                LOG_I("pacing:   frame %.3f..%.3f s, vblanks %llu, long acquires:%s", last / 1e6, now / 1e6,
                      (unsigned long long)(g.vblank_count - vb_last), sm.c_str());
                vb_last = g.vblank_count;
            }
        } else if (prof && slow_logged < 400 && d < 36000 && (iv.size() % 50) == 0) {
            LOG_I("pacing:   normal frame %.1f ms, main thread %s", d / 1e3, profile_main_between(last, now).c_str());
        }
    } else window = now;
    last = now;
    if (now - window >= 5000000 && iv.size() > 10) {
        std::vector<u32> s = iv;
        std::sort(s.begin(), s.end());
        auto pct = [&](double p) { return s[std::min(s.size() - 1, (size_t)(p * s.size()))] / 1e3; };
        u32 over40 = 0, over60 = 0;
        for (u32 d : s) { over40 += d > 40000; over60 += d > 60000; }
        LOG_I("pacing: %zu frames, ms p50 %.1f p90 %.1f p99 %.1f max %.1f | 1%% low %.1f fps | >40 ms %u, >60 ms %u",
              s.size(), pct(0.5), pct(0.9), pct(0.99), s.back() / 1e3, 1000.0 / pct(0.99), over40, over60);
        iv.clear();
        window = now;
    }
}

void do_flip(u32 id) {
    const DisplayBuffer& b = g.buffers[id & 7];
    const u64 t0 = time_usec();
    rsx::flip(mem::RSX_LOCAL + b.offset, b.pitch, b.width, b.height);
    log_frame_pacing(time_usec() - t0);
    if (g_ticks_per_frame_ea) pace_ticks();
    log_cpu_use();
    g.flip_status = 0;
    g.flips_done++;
    raise_interrupt(g.flip_handler, 1);
}

// Synchronisation writes (reference, semaphore releases) become visible only after the next command has been
// fetched, as with the RSX's command prefetch: the game reuses (clears) a command list as soon as the RSX releases
// it, and reading the list's closing RET/JUMP after the release found zeros, ran on through them into stale data and
// hit a FIFO fault (entering DRIVER in the Quick Race menu, and before the race start).
struct PendingWrite { u32 addr, val; };
PendingWrite g_pending[16];
u32 g_npending = 0;
void flush_pending() {
    for (u32 i = 0; i < g_npending; i++) { st32(g_pending[i].addr, g_pending[i].val); guest_written(g_pending[i].addr, 4); }
    g_npending = 0;
}
void defer_st32(u32 addr, u32 val) {
    if (g_npending == 16) flush_pending();
    g_pending[g_npending++] = {addr, val};
}

u32 g_front_ref = 0;   // the last reference value the front end executed (list snapshots age by it)

u32 g_my_get = ~0u;   // the GET value the front end last wrote
void set_get(u32 v) { st32(g.control_addr + 4, v); g_my_get = v; guest_written(g.control_addr + 4, 4); }
// GET at the command being run, before one that may wait for the PPU (runs of commands write it only at their end)
void sync_get() { if (g_my_get != g_cur_get) set_get(g_cur_get); }

void exec_method(u32 method, u32 arg) {
    switch (method) {
        case 0x0050: defer_st32(g.control_addr + 8, arg); g_front_ref = arg; break;   // NV406E_SET_REFERENCE
        case 0x0064: rsx_state.sem_offset_406e = arg; break;           // NV406E_SEMAPHORE_OFFSET
        case 0x0068: {                                                  // NV406E_SEMAPHORE_ACQUIRE
            sync_get();
            flush_pending();   // the game may wait for our earlier releases before releasing this one
            u32 a = label_ptr(rsx_state.sem_offset_406e);
            u64 start = time_usec();
            u32 before = ld32(a);
            // after a short spin, block until the label's line is written (SPU puts and atomics, our own releases) or
            // 1 ms passed (plain PPU stores do not wake it); F1CE_SPIN_WAIT=1: yield in a loop as before
            static const bool old_spin = getenv("F1CE_SPIN_WAIT") != nullptr;
            static u64 woken = 0, timeouts = 0, window = 0;
            LineWait lw;
            g_rsx_acquire_ea.store(a, std::memory_order_seq_cst);
            while (ld32(a) != arg) {
                const u64 now = time_usec();
                if (now - start > 5000000) {
                    LOG_W("RSX: semaphore acquire timeout @%08x want %08x have %08x", a, arg, ld32(a));
                    break;
                }
                if (old_spin) { std::this_thread::yield(); continue; }
                if (now - start < 30) { for (int i = 0; i < 16; i++) _mm_pause(); continue; }
                if (!lw.armed) { line_wait_arm(lw, a, a); continue; }
                (line_wait_block(lw, 1) ? woken : timeouts)++;
            }
            line_wait_release(lw);
            g_rsx_acquire_ea.store(0, std::memory_order_relaxed);
            if (!window) window = start;
            if (start - window > 5000000) {
                LOG_I("RSX: semaphore waits woken by a write %.0f/s, by the 1 ms timeout %.0f/s (last %08x)", woken / 5.0, timeouts / 5.0, a);
                woken = timeouts = 0;
                window = start;
            }
            g_sem_hist[g_sem_pos++ % 16] = {'A', a, arg, (u32)(time_usec() - start), g_cur_get, before, start};
            rsx::g_fe.t_sem += time_usec() - start;
            break;
        }
        case 0x006C:   // NV406E_SEMAPHORE_RELEASE
            g_sem_hist[g_sem_pos++ % 16] = {'R', label_ptr(rsx_state.sem_offset_406e), arg, 0, g_cur_get, ld32(label_ptr(rsx_state.sem_offset_406e))};
            defer_st32(label_ptr(rsx_state.sem_offset_406e), arg);
            break;
        case 0x1D6C: rsx_state.sem_offset_4097 = arg; break;                    // NV4097_SET_SEMAPHORE_OFFSET
        case 0x1D70:                                                            // BACK_END_WRITE_SEMAPHORE_RELEASE
            defer_st32(label_ptr(rsx_state.sem_offset_4097), (arg & 0xFF00FF00) | ((arg & 0xFF) << 16) | ((arg >> 16) & 0xFF));
            break;
        case 0x1D74: defer_st32(label_ptr(rsx_state.sem_offset_4097), arg); break;   // TEXTURE_READ_SEMAPHORE_RELEASE
        case 0x1800: {                                                          // NV4097_GET_REPORT
            u32 a = mem::RSX_LOCAL + 0x0E000000 + (arg & 0xFFFFFF);
            st64(a, ppu_timebase() * 1000 / 80);
            if (!rsx::report(arg >> 24, a)) st32(a + 8, 0);
            st32(a + 12, 0);
            break;
        }
        case GCM_FLIP_COMMAND: { sync_get(); u64 t = prof_usec(); do_flip(arg); rsx::g_fe.t_flip += prof_usec() - t; break; }
        default: rsx::method(method, arg); break;
    }
}

// the arguments of one command. Most are register writes: with a renderer that says so, they skip the calls above
FORCE_INLINE void exec_command(u32 method, bool non_inc, const u32* args, u32 count) {
    static const auto front = [] {   // the methods exec_method handles itself
        std::array<bool, 0x800> t{};
        for (u32 x : {0x0050u, 0x0064u, 0x0068u, 0x006Cu, 0x1D6Cu, 0x1D70u, 0x1D74u, 0x1800u}) t[x >> 2] = true;
        return t;
    }();
    const bool plain = rsx::g_plain_on;
    const auto upload = rsx::g_upload_words;
    for (u32 i = 0; i < count; i++) {
        const u32 m = non_inc ? method : method + i * 4;
        if (plain && m < 0x2000 && rsx::g_plain[m >> 2] && !front[m >> 2]) rsx::R[m >> 2] = args[i];
        else if (upload && (m - 0x0B80u < 0x80 || m - 0x1F00u < 0x80)) {   // vertex program words: the rest of the run at once
            const u32 n = non_inc ? count - i : std::min(count - i, ((m & ~0x7Fu) + 0x80 - m) / 4);
            upload(m, args + i, n, non_inc);
            i += n - 1;
        } else exec_method(m, args[i]);
    }
}

// How far the PPU is ahead of the RSX, in reference steps: the game's ring slots start with SET_REFERENCE, so the
// reference in the slot PUT points into is the latest the PPU published, and control->ref the latest the RSX ran.
// The game builds its frames into two alternating command lists and does not wait for the RSX before rewriting
// one (on hardware the RSX keeps up): when the renderer stalls (texture decoding at the race start, shader linking)
// the PPU got two slots ahead, rewrote a list the RSX was still running and the FIFO ran into garbage (crashes
// entering DRIVER in the Quick Race menu and before the race start). 0 when the layout is not recognised.
// reference value of the ring slot PUT was last seen at (the PPU's frame); 0: none yet
std::atomic<u32> g_last_slot_ref{0};
std::atomic<bool> g_have_slot_ref{false};

// the reference of the slot PUT stands at (ring layout: PUT stops at a slot's jump, slot + 0x60), recorded
void note_slot(u32 put) {
    if ((put & 0xFFF) != 0x060) return;
    u32 slot = put & ~0xFFFu;
    u32 page = g.io_map[(slot >> 20) & 4095];
    if (page == 0xFFFFFFFF) return;
    u32 ea = page + (slot & 0xFFFFF);
    if (ld32(ea) != 0x00040050) return;   // NV406E_SET_REFERENCE, one word
    g_last_slot_ref = ld32(ea + 4);
    g_have_slot_ref = true;
}

// frames (reference steps of 2) the PPU is ahead of the RSX front end. While the game streams a frame list (PUT
// inside the list: race frames, loading) the PPU's frame is the slot it entered the list from: this returned 0 then,
// so a lagging renderer (4K, a heavy scene) let the game rewrite lists the front end was still running (FIFO faults
// in races). A stale word is never taken for a slot header: only PUT positions at slot + 0x60 are read.
s32 refs_behind() {
    if (!g.control_addr) return 0;
    note_slot(ld32(g.control_addr));
    if (!g_have_slot_ref) return 0;
    return (s32)(g_last_slot_ref.load() - ld32(g.control_addr + 8));
}

// Snapshots of streamed command lists. While loading (the race grid, the pits) the game streams a frame list with
// PUT inside it, then returns PUT to a ring slot (slot + 0x60) and soon rewrites that list for later frames. The
// renderer can need seconds for such a list (texture decoding), so it was still running it when the game rewrote
// it, and the FIFO hit garbage (crash starting a Quick Race). When PUT leaves a streamed list, a watcher thread
// copies the list (from its start, the target of the ring slot's jump, to just past the closing jump at the old PUT)
// and the front end reads the copy: the commands the game handed over, as hardware would have run them long before.
// Each copy carries the reference value of the ring slot PUT returned to: a copy that stayed in the queue (the
// front end never left it at call depth 0) kept covering its addresses, and two frames later, when the game had
// written a new list there, the front end ran the old commands (stale CALL into rewritten memory: FIFO fault in a
// race). The slot's reference is the list's own frame + 2 (one step per frame); once the front end is two frames
// past the list (front reference >= copy reference + 2) the copy is ignored and dropped.
struct ListSnap { u32 lo, hi; std::vector<u32> w; bool entered = false; u32 ref = 0; bool has_ref = false; };
bool snap_stale(const ListSnap& sn) { return sn.has_ref && (s32)(g_front_ref - sn.ref) >= 2; }
std::mutex g_snap_m;
std::deque<std::shared_ptr<ListSnap>> g_snaps;
std::atomic<u32> g_snap_gen{0}, g_snap_count{0};
std::atomic<u32> g_list_starts[4];   // recent targets of jumps taken from ring slots (frame list starts)
std::atomic<u32> g_list_start_pos{0};
std::atomic<u32> g_front_get{0};     // where the front end is (for the watcher)
std::atomic<u32> g_last_list{0};     // the frame list the front end last entered from the ring

void note_list_start(u32 target) {
    g_last_list = target;
    for (auto& t : g_list_starts) if (t.load() == target) return;
    g_list_starts[g_list_start_pos++ % 4] = target;
}

void snap_watcher() {
    set_thread_name("rsx snap");
    static const bool off = getenv("F1CE_NO_LIST_SNAP") != nullptr;   // debug
    if (off) return;
    u32 prev = 0;
    u64 last_poll = time_usec();
    for (;;) {
        sleep_usec(1000);   // (sleeps below 200 us spin: this thread must not take a core)
        {   // F1CE_SNAP_LAG=<ms> (debug): a late wake-up every ~2 s
            static const int lag = getenv("F1CE_SNAP_LAG") ? atoi(getenv("F1CE_SNAP_LAG")) : 0;
            static u32 polls = 0;
            if (lag && ++polls % 400 == 0) sleep_usec(lag * 1000);
        }
        if (!g.control_addr) continue;
        u32 put = ld32(g.control_addr);
        note_slot(put);
        u64 poll_t = time_usec(), poll_gap = poll_t - last_poll;
        last_poll = poll_t;
        // The copy needs this list's real end and the slot it closes into. A late wake-up (Windows, all cores busy)
        // sees PUT at a later slot or an old PUT inside the list: the copy then ended short of the closing jump and
        // lived one frame too long, and a later frame's list ran into its old words at a misaligned address (FIFO
        // fault late in a race: vertex program microcode decoded as a CALL). Closing jump not found: no copy.
        u32 closing = 0;
        if (put != prev && prev && (prev & 0xFFF) != 0x060 && (put & 0xFFF) == 0x060 && poll_gap < 30000) {
            const u32 want = 0x20000000 | (put & ~0xFFFu);
            for (u32 a = prev & ~3u, end = a + 0x200000; a < end; a += 4) {
                u32 page = g.io_map[(a >> 20) & 4095];
                if (page == 0xFFFFFFFF) break;
                if (ld32(page + (a & 0xFFFFF)) == want) { closing = a; break; }
            }
            if (!closing) {
                static u32 n = 0;
                if (n++ < 20) LOG_I("RSX: streamed list at %08x not copied: no closing jump to slot of PUT %08x (poll gap %llu us)", prev, put, (unsigned long long)poll_gap);
            }
        }
        if (closing) {
            u32 lo = 0;
            for (auto& t : g_list_starts) { u32 v = t.load(); if (v && v <= prev && v > lo && prev - v < 0x1000000) lo = v; }
            u32 hi = closing + 4;
            u32 get = g_front_get.load();
            // only while the front end has not finished the list: it is in it, or has not entered it yet (the last
            // list it entered from the ring is the other one). A finished list must not be copied: the copy would
            // replay that frame's commands when the front end comes back to the same address for the next frame.
            bool pending = lo && ((get >= lo && get < hi) || g_last_list.load() != lo);
            // only what the front end has not run yet: it never goes back within the list
            if (pending && get > lo && get < hi) lo = get & ~3u;
            if (pending && g.io_map[(lo >> 20) & 4095] != 0xFFFFFFFF && g.io_map[((hi - 4) >> 20) & 4095] != 0xFFFFFFFF) {
                auto sn = std::make_shared<ListSnap>();
                sn->lo = lo; sn->hi = hi;
                {   // the slot's SET_REFERENCE (first word pair of the slot)
                    u32 slot = put & ~0xFFFu, page = g.io_map[(slot >> 20) & 4095];
                    if (page != 0xFFFFFFFF && ld32(page + (slot & 0xFFFFF)) == 0x00040050) {
                        sn->ref = ld32(page + (slot & 0xFFFFF) + 4);
                        sn->has_ref = true;
                    }
                }
                if (!sn->has_ref) { prev = put; continue; }   // a copy without a reference would never age
                sn->w.resize((hi - lo) / 4);
                for (u32 a = lo; a < hi;) {   // raw (big-endian) words, page by page
                    u32 n = std::min(hi - a, 0x100000 - (a & 0xFFFFF));
                    memcpy(&sn->w[(a - lo) / 4], gmem(g.io_map[(a >> 20) & 4095] + (a & 0xFFFFF)), n);
                    a += n;
                }
                {
                    std::lock_guard lk(g_snap_m);
                    g_snaps.push_back(sn);
                    while (g_snaps.size() > 4) g_snaps.pop_front();
                    g_snap_count = (u32)g_snaps.size();
                }
                g_snap_gen++;
                static u32 n = 0;
                if (n++ < 20) LOG_I("RSX: streamed list %08x..%08x copied (front end at %08x)", lo, hi, get);
            }
        }
        prev = put;
    }
}

// Command words: from a list snapshot when one covers the address, else guest memory. The front end keeps a local
// copy of the snapshot list (refreshed when the watcher publishes a new one: no lock per word, which took ~10% of
// the render thread) and reads a command with its arguments through one pointer.
std::vector<std::shared_ptr<ListSnap>> g_local_snaps;   // front end's view
u32 g_local_gen = ~0u;

void snaps_refresh() {
    u32 gen = g_snap_gen.load();
    if (gen == g_local_gen) return;
    std::lock_guard lk(g_snap_m);
    g_local_gen = g_snap_gen.load();
    g_local_snaps.assign(g_snaps.begin(), g_snaps.end());
}

// the copy the front end has left: dropped (with older ones) from the shared list
void snap_done(const std::shared_ptr<ListSnap>& sn) {
    std::lock_guard lk(g_snap_m);
    while (!g_snaps.empty()) {
        bool last = g_snaps.front() == sn;
        g_snaps.pop_front();
        if (last) break;
    }
    g_snap_count = (u32)g_snaps.size();
    g_snap_gen++;
}

// big-endian words io .. io + 4n from one source, or nullptr (straddles a page or a snapshot end: read per word)
std::shared_ptr<ListSnap> g_cur_snap;
const u32* fifo_ptr(u32 io, u32 n) {
    auto& cur = g_cur_snap;
    if (g_snap_count.load(std::memory_order_relaxed) != 0 || cur) {
        snaps_refresh();
        if (cur && (!(io >= cur->lo && io < cur->hi) || snap_stale(*cur))) {
            if (snap_stale(*cur)) {
                static u32 n = 0;
                if (n++ < 20) LOG_I("RSX: list copy %08x..%08x (ref %x) dropped as stale at %08x (ref %x)", cur->lo, cur->hi, cur->ref, io, g_front_ref);
                snap_done(cur);
            } else if (cur->entered && rsx_state.call_depth == 0) snap_done(cur);   // (a CALL out of the list comes back)
            cur = nullptr;
        }
        if (!cur)
            for (auto& sn : g_local_snaps)
                if (io >= sn->lo && io < sn->hi && !snap_stale(*sn)) { cur = sn; break; }
        if (cur) {
            if (io + 4 * n > cur->hi) return nullptr;
            cur->entered = true;
            return &cur->w[(io - cur->lo) / 4];
        }
    }
    u32 page = g.io_map[(io >> 20) & 4095];
    if (page == 0xFFFFFFFF || (io & 0xFFFFF) + 4 * n > 0x100000) return nullptr;
    return (const u32*)gmem(page + (io & 0xFFFFF));
}

// words left at io in the source fifo_ptr just used
u32 fifo_avail(u32 io) {
    if (g_cur_snap) return io < g_cur_snap->hi ? (g_cur_snap->hi - io) / 4 : 0;
    return (0x100000 - (io & 0xFFFFF)) / 4;
}

u32 fifo_word(u32 io) {
    if (const u32* p = fifo_ptr(io, 1)) return __builtin_bswap32(*p);
    return ld32(io_to_ea(io));   // (unmapped: faults with the FIFO history)
}

// the word at io as the front end would read it, without moving it between copies (fifo_ptr may drop the current one)
u32 fifo_peek(u32 io) {
    snaps_refresh();
    for (auto& sn : g_local_snaps)
        if (io >= sn->lo && io < sn->hi && !snap_stale(*sn)) return __builtin_bswap32(sn->w[(io - sn->lo) / 4]);
    return ld32(io_to_ea(io));
}

// a copy turned out to disagree with guest memory on a flow word: forget all copies (front end thread)
void snaps_drop_all() {
    std::lock_guard lk(g_snap_m);
    g_snaps.clear();
    g_snap_count = 0;
    g_snap_gen++;
    g_cur_snap = nullptr;
}

// F1CE_FIFO_VERIFY=1 (debug): the front end records each command header it runs; a thread compares it with guest
// memory a few ms later. A difference means the front end read the word before the game (or an SPU job) wrote it,
// although PUT had passed it: the cause of FIFO faults on stale list contents.
struct VerifyRec { u32 get, cmd, ref, put; u64 t; };
const u32 VERIFY_N = 1u << 18;
VerifyRec* g_verify = nullptr;
std::atomic<u32> g_verify_pos{0};
void verify_thread() {
    u32 pos = 0, reported = 0;
    u64 checked = 0, diffs = 0, last_report = time_usec();
    for (;;) {
        sleep_usec(1000);
        u32 end = g_verify_pos.load();
        if (end - pos > VERIFY_N / 2) pos = end - VERIFY_N / 2;
        u64 now = time_usec();
        for (; pos != end; pos++) {
            const VerifyRec& r = g_verify[pos & (VERIFY_N - 1)];
            if (now - r.t < 4000) break;        // (give writers time)
            if (now - r.t > 12000) continue;    // too old: the game may have rewritten the list for a later frame
            u32 page = g.io_map[(r.get >> 20) & 4095];
            if (page == 0xFFFFFFFF) continue;
            if (ld32(g.control_addr + 8) != r.ref) continue;   // later frame: rewriting the list is normal then
            checked++;
            u32 w = ld32(page + (r.get & 0xFFFFF));
            if (w != r.cmd) {
                diffs++;
                if (reported++ < 200)
                    LOG_W("FIFO verify: @%08x ran %08x, memory now %08x (age %llu us, ref %x, put then %08x, now %08x, get %08x)",
                          r.get, r.cmd, w, (unsigned long long)(now - r.t), r.ref, r.put, ld32(g.control_addr), ld32(g.control_addr + 4));
            }
        }
        if (now - last_report > 5000000) {
            last_report = now;
            LOG_I("FIFO verify: %llu headers checked, %llu changed after they ran", (unsigned long long)checked, (unsigned long long)diffs);
        }
    }
}


u32 g_nop_run = 0;   // NOPs in a row (a long run is a list the front end reached before it was written)
FORCE_INLINE void count_nop(u32 get) {
    if (++g_nop_run == 256) {
        auto [lg, lc] = g_hist[(g_hist_pos + 31) % 32];
        LOG_W("RSX: long NOP run from %08x (put=%08x); last command %08x @%08x", get - 255 * 4, ld32(g.control_addr), lc, lg);
    }
}

void rsx_thread() {
    set_thread_name("rsx");
    place_thread(0);
    if (getenv("F1CE_HOST_PROFILE_RSX")) profile_register_thread();
    const bool verify = getenv("F1CE_FIFO_VERIFY") != nullptr;
    if (verify) {
        g_verify = new VerifyRec[VERIFY_N]();
        std::thread(verify_thread).detach();
    }
    for (;;) {
        u32 put = ld32(g.control_addr), get = ld32(g.control_addr + 4);
        {   // diagnostics: GET changed by someone else (the game resetting the FIFO) since our last write
            static u32 last_put = ~0u;
            static u32 n = 0, nb = 0;
            if (g_my_get != ~0u && get != g_my_get && n < 50) {
                n++;
                LOG_W("RSX: GET written externally: %08x -> %08x (put %08x -> %08x)", g_my_get, get, last_put, put);
            }
            g_my_get = get;
            // (the ring wrap 0x13060 -> 0x10060 is normal)
            if (put != last_put && last_put != ~0u && ((put - last_put) & 0x80000000u) && !(last_put == 0x13060 && put == 0x10060) &&
                nb < 50) {
                nb++;
                LOG_W("RSX: PUT moved back: %08x -> %08x (get %08x)", last_put, put, get);
            }
            last_put = put;
        }
        if (put == get) {
            flush_pending();
            static u64 idle_since = 0;
            u64 t = time_usec();
            // after 200 us without commands, block until the game writes PUT (probe after its store, put_written) or 1 ms
            static const bool old_spin = getenv("F1CE_SPIN_WAIT") != nullptr;
            static LineWait lw;
            if (old_spin) idle_wait(idle_since, 50);
            else {
                if (!idle_since) idle_since = t;
                if (t - idle_since < 200) std::this_thread::yield();
                else if (!lw.armed) line_wait_arm(lw, g.control_addr, g.control_addr);   // (checks PUT once more first)
                else line_wait_block(lw, 1);
            }
            g_rsx_idle_us += time_usec() - t;
            if (ld32(g.control_addr) != ld32(g.control_addr + 4)) idle_since = 0;
            continue;
        }
        u32 cmd = fifo_word(get);
        g_cur_get = get;
        g_front_get.store(get, std::memory_order_release);   // (a plain store: seq_cst was an xchg per command)
        rsx::g_fe.entries++;
        if (cmd) g_hist[g_hist_pos++ % 32] = {get, cmd};
        if (verify && cmd && !g_cur_snap) {
            u32 vp = g_verify_pos.load(std::memory_order_relaxed);
            g_verify[vp & (VERIFY_N - 1)] = {get, cmd, g_front_ref, put, time_usec()};
            g_verify_pos.store(vp + 1);
        }
        static u32 logged = 0;
        if (((cmd & 0xE0000003) == 0x20000000 || (cmd & 3) || cmd == 0x00020000) && logged < 64) {
            logged++;
            u32 tgt = (cmd & 0xE0000003) == 0x20000000 ? (cmd & 0x1FFFFFFC) : (cmd & 0xFFFFFFFC);
            LOG_I("RSX flow @%08x: %08x (put=%08x) target words: %08x %08x %08x %08x", get, cmd, ld32(g.control_addr),
                  ld32(g.io_map[0] + tgt), ld32(g.io_map[0] + tgt + 4), ld32(g.io_map[0] + tgt + 8),
                  ld32(g.io_map[0] + tgt + 12));
        }
        // a jump/call to an unmapped offset is a stale word of a command list the game is still writing (lists
        // reached by CALL are not bounded by PUT; seen after long render-thread stalls): wait for it to be written
        if ((cmd & 3) || (cmd & 0xE0000003) == 0x20000000) {
            u32 tgt = (cmd & 0xE0000003) == 0x20000000 ? (cmd & 0x1FFFFFFC) : (cmd & 0xFFFFFFFC);
            if (g.io_map[(tgt >> 20) & 4095] == 0xFFFFFFFF) {
                u64 start = time_usec();
                while (ld32(io_to_ea(get)) == cmd && time_usec() - start < 500000) std::this_thread::yield();
                if (ld32(io_to_ea(get)) != cmd) {
                    LOG_W("RSX: stale command %08x @%08x replaced after %llu us", cmd, get,
                          (unsigned long long)(time_usec() - start));
                    continue;
                }
            }
        }
        // A jump/call into empty memory is a stale word as well: the front end reached a part of a list that is not
        // written yet, and the old contents there (vertex program microcode from an earlier frame) decoded as a CALL
        // into the unused end of the IO area (FIFO fault late in a race). No list starts with a NOP. Wait for the word
        // or the target to be written. When it comes from a list copy that disagrees with memory, the copy is wrong.
        if ((cmd & 3) == 1 || (cmd & 3) == 2 || (cmd & 0xE0000003) == 0x20000000) {
            u32 tgt = (cmd & 0xE0000003) == 0x20000000 ? (cmd & 0x1FFFFFFC) : (cmd & 0xFFFFFFFC);
            if (tgt != put && !fifo_peek(tgt)) {
                flush_pending();
                u32 guest = ld32(io_to_ea(get));
                if (guest != cmd) {
                    LOG_W("RSX: flow word %08x @%08x from a list copy %08x..%08x (ref %x, front ref %x), memory has %08x: copies dropped",
                          cmd, get, g_cur_snap ? g_cur_snap->lo : 0, g_cur_snap ? g_cur_snap->hi : 0, g_cur_snap ? g_cur_snap->ref : 0, g_front_ref, guest);
                    snaps_drop_all();
                    continue;
                }
                u64 start = time_usec();
                while (ld32(io_to_ea(get)) == cmd && !ld32(io_to_ea(tgt)) && ld32(g.control_addr) != tgt && time_usec() - start < 500000)
                    std::this_thread::yield();
                u64 waited = time_usec() - start;
                u32 now_word = ld32(io_to_ea(get));
                if (now_word != cmd) {
                    LOG_W("RSX: stale flow word %08x @%08x replaced by %08x after %llu us (put %08x)", cmd, get, now_word,
                          (unsigned long long)waited, ld32(g.control_addr));
                    continue;
                }
                if (ld32(io_to_ea(tgt)) == 0 && ld32(g.control_addr) != tgt)
                    LOG_W("RSX: flow word %08x @%08x: target still empty after %llu us (put %08x, list copies %u)", cmd, get,
                          (unsigned long long)waited, ld32(g.control_addr), g_snap_count.load());
            }
        }
        if ((cmd & 3) || (cmd & 0xE0000003) == 0x20000000 || cmd == 0x00020000) {
            rsx::g_fe.flows++;
            g_flow_hist[g_flow_pos++ % 48] = {(u32)(time_usec() / 1000), get, cmd, put, ld32(g.control_addr + 8)};
            flush_pending();
        }
        if (((cmd & 0xE0000003) == 0x20000000 || (cmd & 3) == 1) && (get & 0xFFF) == 0x060)   // ring slot -> frame list
            note_list_start((cmd & 0xE0000003) == 0x20000000 ? (cmd & 0x1FFFFFFC) : (cmd & 0xFFFFFFFC));
        if ((cmd & 0xE0000003) == 0x20000000) {        // old jump
            set_get(cmd & 0x1FFFFFFC);
            continue;
        }
        if ((cmd & 3) == 1) {                          // new jump
            set_get(cmd & 0xFFFFFFFC);
            continue;
        }
        if ((cmd & 3) == 2) {                          // call
            if (rsx_state.call_depth < 8) rsx_state.call_stack[rsx_state.call_depth++] = get + 4;
            set_get(cmd & 0xFFFFFFFC);
            continue;
        }
        if (cmd == 0x00020000) {                        // return
            u32 ret = rsx_state.call_depth ? rsx_state.call_stack[--rsx_state.call_depth] : get + 4;
            set_get(ret);
            continue;
        }
        if (cmd == 0) {   // nop
            rsx::g_fe.nops++;
            flush_pending();
            count_nop(get);
            set_get(get + 4);
            continue;
        }
        g_nop_run = 0;
        u32 method = cmd & 0xFFFC, count = (cmd >> 18) & 0x7FF;
        bool non_inc = cmd & 0x40000000;
        static u32 args[2048];   // fetched before the pending writes of earlier commands are made visible
        const u32* ap = fifo_ptr(get + 4, count);
        if (ap)
            for (u32 i = 0; i < count; i++) args[i] = __builtin_bswap32(ap[i]);
        else
            for (u32 i = 0; i < count; i++) args[i] = fifo_word(get + 4 + i * 4);
        flush_pending();
        exec_command(method, non_inc, args, count);
        rsx::g_fe.cmds++;
        rsx::g_fe.words += count;
        get += 4 + count * 4;
        set_get(get);
        if (!ap || verify) continue;
        // Register writes in a row: the checks above are about flow commands and list copies, so a run of plain commands
        // and NOPs (~37k per race frame, each one used to end the run) from one source only needs to stop where those
        // could matter: at PUT, at any flow command, at the end of the source, when the reference moved (a copy may go
        // stale) or when a copy appeared. GET is written at the end of the run (and before the commands that can wait
        // for the PPU, see sync_get): it shares a cache line with PUT, and a store per command bounced that line between
        // the PPU and render threads.
        const u32 ref = g_front_ref, snap_gen = g_snap_gen.load(std::memory_order_relaxed);
        u32 avail = fifo_avail(get);
        ap += count;
        for (u32 burst = 256; --burst && get != put && avail; ) {
            cmd = __builtin_bswap32(*ap);
            if (!cmd) {
                rsx::g_fe.nops++;
                flush_pending();
                count_nop(get);
                get += 4;
                ap++;
                avail--;
                continue;
            }
            if ((cmd & 3) || (cmd & 0xE0000003) == 0x20000000 || cmd == 0x00020000) break;
            count = (cmd >> 18) & 0x7FF;
            if (count + 1 > avail || g_front_ref != ref || g_snap_gen.load(std::memory_order_relaxed) != snap_gen) break;
            g_nop_run = 0;
            method = cmd & 0xFFFC;
            non_inc = cmd & 0x40000000;
            g_cur_get = get;
            g_front_get.store(get, std::memory_order_release);
            g_hist[g_hist_pos++ % 32] = {get, cmd};
            for (u32 i = 0; i < count; i++) args[i] = __builtin_bswap32(ap[i + 1]);
            flush_pending();
            exec_command(method, non_inc, args, count);
            rsx::g_fe.cmds++;
            rsx::g_fe.words += count;
            get += 4 + count * 4;
            ap += count + 1;
            avail -= count + 1;
        }
        set_get(get);
    }
}

void vblank_thread() {
    set_thread_name("vblank");
    u64 next = time_usec();
    // F1CE_60FPS: 119.88 Hz, as the game's limiter waits for two vblanks per frame
    const u64 period = getenv("F1CE_60FPS") ? 8342 : 16683;   // 59.94 Hz
    for (;;) {
        next += period;
        u64 now = time_usec();
        if (next > now) sleep_usec(next - now);
        g.vblank_count++;
        raise_interrupt(g.vblank_handler, 1);
    }
}

// command buffer segmentation used by the callback (32 KiB segments, first one starts at +4 KiB)
std::pair<u32, u32> next_segment(u32 current) {
    u32 base = g.io_addr;
    u32 seg = (current - base) / CMD_SEGMENT + 1;
    if ((seg + 1) * CMD_SEGMENT > g.cmd_size) seg = 0;
    u32 begin = base + seg * CMD_SEGMENT + (seg == 0 ? 4096 : 0);
    return {begin, base + (seg + 1) * CMD_SEGMENT - 4};
}

// called by the game's inline libgcm code when the current segment is full
void gcm_callback(PPUContext& c) {
    u32 ctx = (u32)c.r[3];
    u32 current = ld32(ctx + 8);
    st32(g.control_addr, ea_to_io(current));                     // flush
    auto [begin, end] = next_segment(current);
    st32(current, 0x20000000 | ea_to_io(begin));                 // old-style jump to the next segment
    st32(ctx + 0, begin);
    st32(ctx + 4, end);
    st32(ctx + 8, begin);
    u32 b_io = ea_to_io(begin), e_io = ea_to_io(end);
    // wait until the RSX has left the new segment: neither the current get nor a return address of a CALL in
    // progress may lie inside it (a called command list elsewhere returns into the segment later)
    auto inside = [&](u32 io) { return io >= b_io && io <= e_io; };
    for (;;) {
        bool busy = inside(ld32(g.control_addr + 4));
        for (u32 i = 0, n = std::min<u32>(rsx_state.call_depth, 8); i < n && !busy; i++) busy = inside(rsx_state.call_stack[i]);
        if (!busy) break;
        sleep_usec(20);
    }
    c.r[3] = CELL_OK;
}
}  // namespace

// _cellGcmInitBody(CellGcmContextData** ctx, cmdSize, ioSize, ioAddress)
HLE_FUNC(_cellGcmInitBody) {
    u32 cmd_size = (u32)ARG(1), io_size = (u32)ARG(2), io_addr = (u32)ARG(3);
    LOG_I("_cellGcmInitBody(cmd=%x, io=%x @ %08x)", cmd_size, io_size, io_addr);
    g.io_addr = io_addr;
    g.io_size = io_size;
    g.cmd_size = cmd_size;
    io_map(io_addr, 0, io_size);
    g.io_next = (io_size + 0xFFFFF) & ~0xFFFFFu;

    g.control_addr = mem::alloc(0x1000, 0x1000);
    g.label_addr = mem::alloc(0x1000, 0x1000);
    LOG_I("gcm: control registers at %08x, labels at %08x", g.control_addr, g.label_addr);
    for (u32 i = 0; i < 256; i++) st32(g.label_addr + i * 16, 0x1337C0D3);
    g.config_addr = mem::alloc(0x40, 16);
    g.context_addr = mem::alloc(0x100, 0x100);

    u32 ctx = g.context_addr;
    st32(ctx + 0, io_addr + 4096);                          // begin
    st32(ctx + 4, io_addr + CMD_SEGMENT - 4);               // end
    st32(ctx + 8, io_addr + 4096);                          // current
    st32(ctx + 12, host_func_opd(gcm_callback));            // callback
    st32(g.control_addr + 0, 4096);                         // put
    set_get(4096);                         // get
    st32(g.control_addr + 8, 0xFFFFFFFF);                   // ref
    st32((u32)ARG(0), ctx);

    rsx::init();
    std::thread(rsx_thread).detach();
    std::thread(snap_watcher).detach();
    std::thread(vblank_thread).detach();
    g.intr_thread = thread_create_host("gcm_interrupt", [](PPUContext& c) {
        for (;;) {
            std::pair<u32, u32> job;
            {
                std::unique_lock lk(g.intr_mutex);
                g.intr_cv.wait(lk, [] { return !g.intr_queue.empty(); });
                job = g.intr_queue.front();
                g.intr_queue.erase(g.intr_queue.begin());
            }
            guest_call(c, job.first, {job.second});
            std::atomic_thread_fence(std::memory_order_seq_cst);   // (the handler's plain stores before the check)
            if (const u32 ea = g_rsx_acquire_ea.load(std::memory_order_seq_cst)) guest_written(ea, 4);
        }
    });
    g.init = true;
    RET(CELL_OK);
}

HLE_FUNC(cellGcmGetControlRegister) { RET(g.control_addr); }
HLE_FUNC(cellGcmGetLabelAddress) { RET(g.label_addr + 0x10 * ((u32)ARG(0) & 255)); }
// CellGcmConfig: localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency
HLE_FUNC(cellGcmGetConfiguration) {
    u32 p = (u32)ARG(0);
    st32(p + 0, mem::RSX_LOCAL);
    st32(p + 4, g.io_addr);
    st32(p + 8, LOCAL_SIZE);
    st32(p + 12, g.io_size);
    st32(p + 16, 650000000);
    st32(p + 20, 500000000);
    RET(CELL_OK);
}
HLE_FUNC(cellGcmAddressToOffset) {
    u32 ea = (u32)ARG(0);
    u32 off;
    if (ea >= mem::RSX_LOCAL && ea < mem::RSX_LOCAL + mem::RSX_LOCAL_SIZE) off = ea - mem::RSX_LOCAL;
    else if ((off = ea_to_io(ea)) == 0xFFFFFFFF) RET((s32)0x802100FF);   // CELL_GCM_ERROR_FAILURE
    st32((u32)ARG(1), off);
    RET(CELL_OK);
}
// cellGcmMapMainMemory(ea, size, u32* offset)
HLE_FUNC(cellGcmMapMainMemory) {
    u32 ea = (u32)ARG(0), size = ((u32)ARG(1) + 0xFFFFF) & ~0xFFFFFu;
    u32 io = g.io_next;
    io_map(ea, io, size);
    g.io_next += size;
    st32((u32)ARG(2), io);
    LOG_I("cellGcmMapMainMemory(%08x, %x) -> io %08x", ea, size, io);
    RET(CELL_OK);
}
// cellGcmSetDisplayBuffer(id, offset, pitch, width, height)
HLE_FUNC(cellGcmSetDisplayBuffer) {
    u32 id = (u32)ARG(0);
    if (id > 7) RET((s32)0x802100FF);
    g.buffers[id] = {(u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4)};
    RET(CELL_OK);
}
// cellGcmSetFlip(ctx, id): append a flip command to the game's command buffer
HLE_FUNC(cellGcmSetFlip) {
    u32 ctx = (u32)ARG(0), id = (u32)ARG(1);
    if (id > 7) RET((s32)0x802100FF);
    // frames in flight: on hardware the RSX keeps up, and the game rewrites per-frame data in video memory (e.g.
    // fragment programs) assuming it is no longer in use. When the renderer lags, keep the PPU at most
    // F1CE_MAX_FRAMES_AHEAD (default 1) flips ahead of the RSX.
    static const u64 max_ahead = getenv("F1CE_MAX_FRAMES_AHEAD") ? atoi(getenv("F1CE_MAX_FRAMES_AHEAD")) : 1;
    while (g.flips_queued - g.flips_done > max_ahead) sleep_usec(100);
    g.flips_queued++;
    static bool logged = false;
    if (!logged) { logged = true; LOG_I("cellGcmSetFlip: ctx=%08x (default %08x)", ctx, g.context_addr); }
    u32 cur = ld32(ctx + 8);
    if (cur + 8 >= ld32(ctx + 4)) {
        guest_call(c, ld32(ctx + 12), {ctx, 8});
        cur = ld32(ctx + 8);
    }
    st32(cur, (1u << 18) | GCM_FLIP_COMMAND);
    st32(cur + 4, id);
    st32(ctx + 8, cur + 8);
    g.flip_status = 1;
    if (ctx == g.context_addr) st32(g.control_addr, ea_to_io(cur + 8));
    RET(CELL_OK);
}
HLE_FUNC(cellGcmSetFlipImmediate) { do_flip((u32)ARG(0)); RET(CELL_OK); }
HLE_FUNC(cellGcmSetWaitFlip) { RET(CELL_OK); }
HLE_FUNC(cellGcmSetFlipMode) { g.flip_mode = (u32)ARG(0); RET(CELL_OK); }
HLE_FUNC(cellGcmSetFlipHandler) { g.flip_handler = (u32)ARG(0); RET(CELL_OK); }
HLE_FUNC(cellGcmSetVBlankHandler) { g.vblank_handler = (u32)ARG(0); RET(CELL_OK); }
HLE_FUNC(cellGcmSetUserHandler) { g.user_handler = (u32)ARG(0); RET(CELL_OK); }
HLE_FUNC(cellGcmSetQueueHandler) { g.queue_handler = (u32)ARG(0); RET(CELL_OK); }
HLE_FUNC(cellGcmSetGraphicsHandler) { g.graphics_handler = (u32)ARG(0); RET(CELL_OK); }
HLE_FUNC(cellGcmSetDebugOutputLevel) { RET(CELL_OK); }
HLE_FUNC(cellGcmSetTile) { RET(CELL_OK); }
HLE_FUNC(cellGcmSetZcull) { RET(CELL_OK); }
HLE_FUNC(_cellGcmFunc15) { RET(CELL_OK); }

namespace rsx {
// IO offset -> guest EA, for renderers reading main-memory resources
u32 io_offset_to_ea(u32 io) { return io_to_ea(io); }
}  // namespace rsx

// PPU-side throttle (lv2.cpp timer_usleep)
// A PPU wait for the RSX's reference register (the PPU throttle, libgcm's wait for command buffer space): blocks until
// the RSX writes the control registers' line, at most 1 ms. Returns at once when the reference moved since this
// thread's previous call, so the caller checks again first (its check was before this call).
// the game's flip handler (0x489a2c) just stored to a label (r9) that the RSX may be waiting for (semaphore acquire):
// plain PPU stores do not wake line waits, so this probe after the store does (tools/recomp.py PROBES)
void flip_label_written(PPUContext& c) { guest_written((u32)c.r[9], 4); }

// the game (libgcm flush, 0x48b050) just wrote PUT (r3 = the control registers): wakes an idle RSX front end
void put_written(PPUContext& c) { guest_written((u32)c.r[3], 4); }

void rsx_ref_wait() {
    static thread_local LineWait w;
    static thread_local u32 prev = ~0u;
    const u32 a = g.control_addr + 8;
    line_wait_arm(w, a, a);
    const u32 r = ld32(a);
    if (r != prev) { prev = r; return; }
    line_wait_block(w, 1);
    prev = ld32(a);
}

s32 rsx_refs_behind() { return refs_behind(); }
