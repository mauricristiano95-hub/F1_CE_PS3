// Shared host-side services for the LV2 kernel emulation and HLE modules.
#pragma once
#include "ppu_runtime.h"

#include <x86intrin.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Held only around 128-byte copies, about 1.5 million times per second: much cheaper than a mutex. It yields after a
// while because the holder may be preempted when threads outnumber cores.
struct SpinLock {
    std::atomic<bool> held{false};
    void lock() {
        while (held.exchange(true, std::memory_order_acquire))
            for (u32 n = 0; held.load(std::memory_order_relaxed); n++) {
                _mm_pause();
                if (n >= 256) { std::this_thread::yield(); n = 0; }
            }
    }
    void unlock() { held.store(false, std::memory_order_release); }
};
SpinLock& atomic_lock(u32 ea);   // per-line reservation lock (core.cpp)

// ------------------------------------------------------------------ error codes
enum : s32 {
    CELL_OK = 0,
    CELL_EAGAIN = (s32)0x80010001,
    CELL_EINVAL = (s32)0x80010002,
    CELL_ENOSYS = (s32)0x80010003,
    CELL_ENOMEM = (s32)0x80010004,
    CELL_ESRCH = (s32)0x80010005,
    CELL_ENOENT = (s32)0x80010006,
    CELL_EDEADLK = (s32)0x80010008,
    CELL_EPERM = (s32)0x80010009,
    CELL_EBUSY = (s32)0x8001000A,
    CELL_ETIMEDOUT = (s32)0x8001000B,
    CELL_EFAULT = (s32)0x8001000D,
    CELL_ESTAT = (s32)0x8001000F,
    CELL_EBADF = (s32)0x8001002A,
    CELL_ECANCELED = (s32)0x8001000C,
    CELL_EEXIST = (s32)0x80010014,
    CELL_EISDIR = (s32)0x80010012,
    CELL_ENOTDIR = (s32)0x80010013,
};

// ------------------------------------------------------------------ logging
enum class LogLevel { trace, info, warn, error };
void log_msg(LogLevel lv, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG_T(...) log_msg(LogLevel::trace, __VA_ARGS__)
#define LOG_I(...) log_msg(LogLevel::info, __VA_ARGS__)
#define LOG_W(...) log_msg(LogLevel::warn, __VA_ARGS__)
#define LOG_E(...) log_msg(LogLevel::error, __VA_ARGS__)
extern bool g_log_trace;
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
s32 rsx_refs_behind();   // hle_gcm.cpp: how far the PPU is ahead of the RSX (reference steps)
void rsx_ref_wait();     // hle_gcm.cpp: wait for the RSX to move its reference (or 1 ms)
extern std::atomic<bool> g_rsx_starting;   // rsx.cpp: the renderer runs its start-up phase on the RSX thread (OpenGL shaders)

// ------------------------------------------------------------------ memory
namespace mem {
constexpr u32 MAIN_HEAP = 0x20000000;   // runtime-owned guest structures (HLE heap)
constexpr u32 MAIN_HEAP_SIZE = 0x10000000;
constexpr u32 USER_BASE = 0x30000000;   // sys_memory_allocate / mmapper
constexpr u32 USER_SIZE = 0x10000000;
constexpr u32 RSX_LOCAL = 0xC0000000;   // RSX local memory (256 MB)
constexpr u32 RSX_LOCAL_SIZE = 0x10000000;
constexpr u32 STACK_BASE = 0xD0000000;  // PPU thread stacks
constexpr u32 STACK_SIZE = 0x10000000;

// whether [addr, addr+size) lies in the committed guest ranges (mem::init); for diagnostics that follow untrusted pointers
inline bool readable(u32 addr, u32 size) {
    u64 a = addr, e = a + size;
    auto in = [&](u64 b, u64 n) { return a >= b && e <= b + n; };
    return in(0x00010000, 0x10000000 - 0x10000) || in(0x10000000, 0x01000000) || in(MAIN_HEAP, MAIN_HEAP_SIZE) ||
           in(USER_BASE, USER_SIZE) || in(RSX_LOCAL, RSX_LOCAL_SIZE) || in(STACK_BASE, STACK_SIZE);
}

void init();
u32 alloc(u32 size, u32 align = 16);           // HLE heap
u32 user_alloc(u32 size, u32 align = 0x10000);  // game memory (sys_memory_allocate)
void user_free(u32 addr);
u32 user_available();
u32 stack_alloc(u32 size);
void stack_free(u32 addr);
}  // namespace mem

// ------------------------------------------------------------------ guest strings
std::string gstr(u32 ea, size_t max = 4096);
void gstr_write(u32 ea, const std::string& s, size_t max);

using HleFnPtr = void (*)(PPUContext&);

// ------------------------------------------------------------------ threads
struct PPUThread {
    PPUContext ctx{};
    u32 id = 0;
    u32 stack_addr = 0, stack_size = 0;
    s32 priority = 1000;
    std::string name;
    std::thread host;
    std::atomic<bool> finished{false};
    u64 exit_status = 0;
    bool joinable = true;
    std::atomic<const char*> in_hle{nullptr};   // name of the HLE function/syscall being executed
    std::mutex m;
    std::condition_variable cv;
};
extern thread_local PPUThread* t_self;
// the main thread's last sys_timer_usleep calls (F1CE_PROFILE only; lv2.cpp)
struct MainSleep { u64 t; u32 lr, req, took; };
extern MainSleep g_main_sleeps[256];
extern std::atomic<u64> g_main_sleep_pos;
PPUThread* thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stack_size, bool joinable, const std::string& name,
                         u32* out_id);
PPUThread* thread_find(u32 id);
void thread_start(PPUThread* t);
[[noreturn]] void thread_exit(PPUContext& c, u64 status);
void run_main_thread(u32 entry_opd, u32 stack_size, s32 prio);

// Thrown to unwind a guest thread's host stack on sys_ppu_thread_exit.
struct ThreadExit { u64 status; };

// Calls a guest function through its OPD descriptor on the current thread's context.
u64 guest_call(PPUContext& c, u32 opd, std::initializer_list<u64> args);

// Exposes a host function to guest code: returns the guest address of an OPD descriptor
// whose entry point dispatches to fn when called indirectly.
u32 host_func_opd(HleFnPtr fn);

// Runs body on a new host thread that owns a guest context (stack + TLS), for
// interrupt-like callbacks (vblank/flip handlers, SPURS event threads...).
PPUThread* thread_create_host(const std::string& name, std::function<void(PPUContext&)> body);

// ------------------------------------------------------------------ IDs
template <typename T>
class IdMap {
public:
    u32 add(std::shared_ptr<T> obj) {
        std::lock_guard lk(m_);
        u32 id = next_++;
        objs_[id] = std::move(obj);
        return id;
    }
    std::shared_ptr<T> get(u32 id) {
        std::lock_guard lk(m_);
        auto it = objs_.find(id);
        return it == objs_.end() ? nullptr : it->second;
    }
    bool remove(u32 id) {
        std::lock_guard lk(m_);
        return objs_.erase(id) != 0;
    }
    explicit IdMap(u32 base) : next_(base) {}

private:
    std::mutex m_;
    u32 next_;
    std::unordered_map<u32, std::shared_ptr<T>> objs_;
};

// ------------------------------------------------------------------ events (used by LV2 and HLE)
struct EventQueue;
std::shared_ptr<EventQueue> event_queue_get(u32 id);
s32 event_queue_push(u32 queue_id, u64 source, u64 d1, u64 d2, u64 d3);
u32 event_queue_create_host(u64 key, u32 size);

// ------------------------------------------------------------------ filesystem
// UTF-8 std::string from a path (C++20 u8string() returns std::u8string).
#define PATH_STR(p) ([](const auto& _p) { auto _s = _p.u8string(); return std::string(_s.begin(), _s.end()); }(p))
std::string fs_host_path(const std::string& guest_path);
extern std::string g_game_dir;   // .../PS3_GAME
extern std::string g_data_dir;   // writable emulated dev_hdd0 etc.

// ------------------------------------------------------------------ HLE registration
using HleFn = void (*)(PPUContext&);
struct HleRegistrar {
    HleRegistrar(const char* name, HleFn fn);
};
#define HLE_FUNC(name)                                         \
    static void hle_##name(PPUContext& c);                     \
    static HleRegistrar hle_reg_##name(#name, hle_##name);     \
    static void hle_##name(PPUContext& c)

// argument / return helpers
#define ARG(n) (c.r[3 + (n)])
#define RET(v) do { c.r[3] = (u64)(s64)(v); return; } while (0)

// time
u64 time_usec();          // monotonic microseconds since start
// Clock of the per-draw timers: TSC ticks in microseconds (time_usec costs ~40 ns, 6% of the render thread at some 15
// reads per draw). Its origin is not time_usec's: only differences of two prof_usec values mean anything.
extern const u64 g_tsc_scale;   // microseconds per tick, 32.32 fixed point
FORCE_INLINE u64 prof_usec() { return (u64)(((unsigned __int128)__rdtsc() * g_tsc_scale) >> 32); }
void sleep_usec(u64 us);
void sleep_until_precise(u64 t);   // core.cpp: until time_usec() == t, without the ~2 ms overshoot of sleep_for
void raise_thread_priority();   // core.cpp: threads on the frame's critical path (F1CE_PRIO=0: off)
u64 process_cpu_usec();   // user + kernel time of all threads
// Wait of a polling thread with nothing to do: short waits (they spin, for latency) until it has been idle for 3 ms, then
// a real sleep. Under load the idle spells are shorter, so the 1 ms of latency only applies in menus and pauses.
inline void idle_wait(u64& idle_since, u64 short_us) {
    u64 now = time_usec();
    if (!idle_since) idle_since = now;
    sleep_usec(now - idle_since > 3000 ? 1000 : short_us);
}
