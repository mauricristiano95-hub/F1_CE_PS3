// Guest memory, dispatch tables, threads, logging and other host services.
#include "kernel.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <synchapi.h>
#include <tlhelp32.h>
#else
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

u8* g_base = nullptr;
u32 g_watch_line = getenv("F1CE_WATCH_LINE") ? (u32)strtoul(getenv("F1CE_WATCH_LINE"), nullptr, 16) & ~127u : 1;

double watch_after() {
    static const double t = getenv("F1CE_WATCH_AFTER") ? atof(getenv("F1CE_WATCH_AFTER")) : 0.0;
    return t;
}
void watch_store(u32 ea, u64 value, u32 size) {
    if (time_usec() / 1e6 < watch_after()) return;
    static std::atomic<int> n{0};
    static const int max_n = getenv("F1CE_WATCH_MAX") ? atoi(getenv("F1CE_WATCH_MAX")) : 400;
    if (n++ > max_n) return;
    if (t_self)
        LOG_I("watch PPU store%u [%08x] = %llx  pc=%08x fn=%08x lr=%08x r3-r8=%08x %08x %08x %08x %08x %08x", size * 8, ea,
              (unsigned long long)value, t_self->ctx.pc, t_self->ctx.cur_fn, (u32)t_self->ctx.lr, (u32)t_self->ctx.r[3],
              (u32)t_self->ctx.r[4], (u32)t_self->ctx.r[5], (u32)t_self->ctx.r[6], (u32)t_self->ctx.r[7], (u32)t_self->ctx.r[8]);
    else
        LOG_I("watch host store%u [%08x] = %llx", size * 8, ea, (unsigned long long)value);
}
bool g_log_trace = false;
thread_local PPUThread* t_self = nullptr;

// ------------------------------------------------------------------ logging
static std::mutex g_log_mutex;
static FILE* g_log_file = nullptr;

void log_msg(LogLevel lv, const char* fmt, ...) {
    if (lv == LogLevel::trace && !g_log_trace) return;
    // F1CE_NO_LOG: no f1ce_recomp.log; info lines are dropped before formatting, warnings and errors still go to the console
    static const bool no_log = getenv("F1CE_NO_LOG") != nullptr;
    if (no_log && lv < LogLevel::warn) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static const char* tags[] = {"T", "I", "W", "E"};
    std::lock_guard lk(g_log_mutex);
    if (!g_log_file && !no_log) {
        g_log_file = fopen("f1ce_recomp.log", "w");
        if (g_log_file) setvbuf(g_log_file, nullptr, _IOFBF, 1 << 20);
    }
    u32 tid = t_self ? t_self->id : 0;
    // The console gets warnings and errors only (F1CE_LOG_CONSOLE=1: everything). Printing every line to a terminal
    // (Git Bash / Windows console) took long enough, under this lock, to stall the renderer and the PPU: slow
    // loading and timing-dependent FIFO faults that did not happen with the output redirected.
    static const bool console_all = getenv("F1CE_LOG_CONSOLE") != nullptr;
    if (console_all || lv >= LogLevel::warn) fprintf(stderr, "[%s][%08x] %s\n", tags[(int)lv], tid, buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%s][%08x] %s\n", tags[(int)lv], tid, buf);
        // flushed at once for warnings and errors, else at most once a second (a flush per line is slow too)
        static u64 last_flush = 0;
        u64 now = time_usec();
        if (lv >= LogLevel::warn || now - last_flush > 1000000) { fflush(g_log_file); last_flush = now; }
    }
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_msg(LogLevel::error, "FATAL: %s", buf);
    if (t_self) {
        auto& c = t_self->ctx;
        log_msg(LogLevel::error, "  fn=%08x lr=%08x ctr=%08x pc=%08x r1=%08x r2=%08x", c.cur_fn, (u32)c.lr,
                (u32)c.ctr, c.pc, (u32)c.r[1], (u32)c.r[2]);
        for (int i = 0; i < 32; i += 4)
            log_msg(LogLevel::error, "  r%-2d=%016llx r%-2d=%016llx r%-2d=%016llx r%-2d=%016llx", i,
                    (unsigned long long)c.r[i], i + 1, (unsigned long long)c.r[i + 1], i + 2,
                    (unsigned long long)c.r[i + 2], i + 3, (unsigned long long)c.r[i + 3]);
        // PPC64 ABI stack walk: back chain at 0(sp), saved LR at 16(frame)
        std::string bt;
        u32 sp = (u32)c.r[1];
        for (int depth = 0; depth < 24 && sp >= mem::STACK_BASE && sp < mem::STACK_BASE + mem::STACK_SIZE; depth++) {
            u32 next = (u32)ld64(sp);
            if (next <= sp || next >= mem::STACK_BASE + mem::STACK_SIZE) break;
            char buf[16];
            snprintf(buf, sizeof(buf), " %08x", (u32)ld64(next + 16));
            bt += buf;
            sp = next;
        }
        log_msg(LogLevel::error, "  backtrace (return addresses):%s", bt.c_str());
    }
    fflush(stderr);
    std::_Exit(1);
}

// ------------------------------------------------------------------ time
static const auto g_start = std::chrono::steady_clock::now();
const u64 g_tsc_scale = [] {
    const auto t0 = std::chrono::steady_clock::now();
    const u64 c0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2)) {}
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return (u64)(us * 4294967296.0 / (double)(__rdtsc() - c0));
}();
u64 time_usec() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}
// Short waits (polling loops of the emulated hardware) must not round up to the OS timer tick (>= 1 ms on
// Windows): they yield until the deadline instead. Longer ones sleep (timer resolution raised to 1 ms).
void sleep_usec(u64 us) {
    if (us <= 200) {
        u64 end = time_usec() + us;
        // (a yield per poll is a system call: the pauses leave the core to its other hardware thread)
        do {
            for (int i = 0; i < 32; i++) _mm_pause();
            std::this_thread::yield();
        } while (time_usec() < end);
        return;
    }
#ifdef _WIN32
    static const bool period = [] { timeBeginPeriod(1); return true; }();
    (void)period;
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}
// ---- blocking waits on guest memory lines (ppu_runtime.h)
namespace {
struct alignas(64) LineSlot {
    std::atomic<u32> line0{~0u}, line1{~0u};
    std::atomic<u32> seq{0};
    std::atomic<u32> sleeping{0};
};
LineSlot g_line_slots[16];
std::atomic<u32> g_line_slots_used{0};   // high-water mark: guest_written_slow scans slots below it
std::atomic<u32> g_line_slots_taken{0};  // bit per slot owned by a live thread
void futex_wait(std::atomic<u32>* a, u32 expected, u32 timeout_ms) {
#ifdef _WIN32
    WaitOnAddress((volatile void*)a, &expected, sizeof(u32), timeout_ms);
#else
    struct timespec ts = {(time_t)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000000L};
    syscall(SYS_futex, (u32*)a, FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
#endif
}
void futex_wake_all(std::atomic<u32>* a) {
#ifdef _WIN32
    WakeByAddressAll((void*)a);
#else
    syscall(SYS_futex, (u32*)a, FUTEX_WAKE_PRIVATE, 0x7fffffff, nullptr, nullptr, 0);
#endif
}
}  // namespace
std::atomic<u32> g_line_waiters{0};

void guest_written_slow(u32 ea, u32 size) {
    const u32 first = ea & ~127u, last = (ea + std::max(size, 1u) - 1) & ~127u;
    const u32 n = std::min<u32>(g_line_slots_used.load(std::memory_order_acquire), 16);
    for (u32 i = 0; i < n; i++) {
        LineSlot& s = g_line_slots[i];
        const u32 l0 = s.line0.load(std::memory_order_relaxed), l1 = s.line1.load(std::memory_order_relaxed);
        if ((l0 >= first && l0 <= last) || (l1 >= first && l1 <= last)) {
            s.seq.fetch_add(1, std::memory_order_seq_cst);
            if (s.sleeping.load(std::memory_order_seq_cst)) futex_wake_all(&s.seq);
        }
    }
}

thread_local int t_line_slot = -1;
thread_local bool t_line_registered = false;
// Slots go back to the pool when their PPU thread exits (line_wait_thread_exit): the game starts a new 'AsyncRender'
// thread for every loading screen, and slots that were never returned ran out after ~10 loads (fatal on a GP load).
LineSlot& my_line_slot() {
    if (t_line_slot < 0) {
        u32 taken = g_line_slots_taken.load(std::memory_order_relaxed);
        do {
            if (taken == 0xFFFF) fatal("line_wait: more than 16 waiting threads");
            t_line_slot = __builtin_ctz(~taken);
        } while (!g_line_slots_taken.compare_exchange_weak(taken, taken | 1u << t_line_slot));
        u32 used = g_line_slots_used.load(std::memory_order_relaxed);
        while ((u32)t_line_slot >= used && !g_line_slots_used.compare_exchange_weak(used, t_line_slot + 1)) {}
    }
    return g_line_slots[t_line_slot];
}

void line_wait_arm(LineWait& w, u32 line0, u32 line1) {
    LineSlot& s = my_line_slot();
    w.line0 = line0 & ~127u;
    w.line1 = line1 & ~127u;
    s.line0.store(w.line0, std::memory_order_relaxed);
    s.line1.store(w.line1, std::memory_order_relaxed);
    if (!t_line_registered) { g_line_waiters.fetch_add(1, std::memory_order_seq_cst); t_line_registered = true; }
    w.armed = true;
    w.seq = s.seq.load(std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);   // (the registration before the caller's check)
}

bool line_wait_block(LineWait& w, u32 timeout_ms) {
    LineSlot& s = my_line_slot();
    if (!t_line_registered || s.line0.load(std::memory_order_relaxed) != w.line0 ||
        s.line1.load(std::memory_order_relaxed) != w.line1) {
        line_wait_arm(w, w.line0, w.line1);   // another wait of this thread took the slot: register again, check first
        return false;
    }
    const u32 before = w.seq;
    s.sleeping.store(1, std::memory_order_seq_cst);
    if (s.seq.load(std::memory_order_seq_cst) == w.seq) futex_wait(&s.seq, w.seq, timeout_ms);
    s.sleeping.store(0, std::memory_order_relaxed);
    w.seq = s.seq.load(std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return w.seq != before;
}

void line_wait_release(LineWait& w) {
    if (!w.armed) return;
    w.armed = false;
    LineSlot& s = my_line_slot();
    if (!t_line_registered || s.line0.load(std::memory_order_relaxed) != w.line0) return;   // (another wait's now)
    s.line0.store(~0u, std::memory_order_relaxed);
    s.line1.store(~0u, std::memory_order_relaxed);
    g_line_waiters.fetch_sub(1, std::memory_order_seq_cst);
    t_line_registered = false;
}

void line_wait_thread_exit() {
    if (t_line_slot < 0) return;
    LineSlot& s = g_line_slots[t_line_slot];
    s.line0.store(~0u, std::memory_order_relaxed);
    s.line1.store(~0u, std::memory_order_relaxed);
    if (t_line_registered) { g_line_waiters.fetch_sub(1, std::memory_order_seq_cst); t_line_registered = false; }
    g_line_slots_taken.fetch_and(~(1u << t_line_slot), std::memory_order_release);
    t_line_slot = -1;
}

// The job manager's waits (0x551228, 0x5513BC): r3 + 4 points to a block whose 16-bit job counters at +0x12..+0x1F the
// SPUs advance. Spin ~20 us (short waits keep their latency), then arm on the counters' lines and block; a pause of more
// than 50 us between calls means a new wait. F1CE_SPIN_WAIT=1: the old spin with yields.
void ppu_spin(PPUContext& c) {
    static const bool old_spin = getenv("F1CE_SPIN_WAIT") != nullptr;
    static thread_local u32 n = 0;
    if (old_spin) {
        for (int i = 0; i < 8; i++) _mm_pause();
        if ((++n & 63) == 0) std::this_thread::yield();
        return;
    }
    static thread_local LineWait w;
    static thread_local u64 start = 0, last = 0;
    const u64 now = time_usec();
    if (now - last > 50) start = now;
    last = now;
    if (now - start < 20) {
        for (int i = 0; i < 8; i++) _mm_pause();
        return;
    }
    const u32 p = ld32((u32)c.r[3] + 4);
    const u32 l0 = (p + 0x12) & ~127u, l1 = (p + 0x1F) & ~127u;
    if (!w.armed || l0 != w.line0) {
        line_wait_arm(w, l0, l1);   // (the loop reads the counters once more before the next call blocks)
        return;
    }
    line_wait_block(w, 1);
    last = time_usec();
}
// Sleeps until time_usec() reaches t: a high-resolution timer for all but the last ~0.6 ms (its granularity on Windows is
// ~0.5 ms; sleep_for is ~2 ms), then yields.
void sleep_until_precise(u64 t) {
    u64 now = time_usec();
    if (now >= t) return;
#ifdef _WIN32
    if (t - now > 700) {
        static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(t - now - 600) * 10;
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
        } else {
            sleep_usec(t - now - 2000 > 0 && t - now > 2500 ? t - now - 2000 : 0);
        }
    }
#else
    if (t - now > 300) {
        struct timespec ts = {(time_t)((t - now - 200) / 1000000), (long)((t - now - 200) % 1000000) * 1000};
        nanosleep(&ts, nullptr);
    }
#endif
    while (time_usec() < t) std::this_thread::yield();
}

// names host threads (debuggers, F1CE_THREAD_CPU)
void set_thread_name(const char* name) {
#ifdef _WIN32
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (!fn) return;
    wchar_t w[64];
    int i = 0;
    for (; name[i] && i < 63; i++) w[i] = (wchar_t)(unsigned char)name[i];
    w[i] = 0;
    fn(GetCurrentThread(), w);
#else
    char n[16];   // (at most 15 characters)
    snprintf(n, sizeof(n), "%s", name);
    pthread_setname_np(pthread_self(), n);
#endif
}

// F1CE_THREAD_CPU=1: with the "cpu:" line, the cores used by each thread over the last window (threads above 2%)
void log_thread_cpu() {
#ifdef _WIN32
    static const bool on = getenv("F1CE_THREAD_CPU") != nullptr;
    if (!on) return;
    using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const Fn get_desc = (Fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    static std::map<DWORD, u64> last;
    static u64 last_wall = 0;
    const u64 wall = time_usec();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    std::vector<std::pair<double, std::string>> rows;
    std::map<DWORD, u64> now;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            auto us = [](const FILETIME& f) { return (((u64)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10; };
            const u64 t = us(k) + us(u);
            now[te.th32ThreadID] = t;
            auto it = last.find(te.th32ThreadID);
            if (last_wall && it != last.end()) {
                const double cores = (double)(t - it->second) / (double)(wall - last_wall);
                if (cores >= 0.02) {
                    std::string name = "tid " + std::to_string(te.th32ThreadID);
                    PWSTR d = nullptr;
                    if (get_desc && SUCCEEDED(get_desc(h, &d)) && d && d[0]) {
                        name.clear();
                        for (PWSTR p = d; *p; p++) name += (char)*p;
                    }
                    if (d) LocalFree(d);
                    rows.push_back({cores, name});
                }
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    last.swap(now);
    if (last_wall) {
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string s;
        char b[96];
        for (auto& [cores, name] : rows) { snprintf(b, sizeof(b), " %s %.2f", name.c_str(), cores); s += b; }
        LOG_I("thread cpu:%s", s.c_str());
    }
    last_wall = wall;
#endif
}

u64 process_cpu_usec() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto us = [](const FILETIME& f) { return (((u64)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10; };
    return us(kernel) + us(user);
#else
    return (u64)(clock() / (double)CLOCKS_PER_SEC * 1e6);
#endif
}
u64 ppu_timebase() {  // 79.8 MHz
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count();
    return (u64)((unsigned __int128)ns * 798 / 10000);
}

// ------------------------------------------------------------------ memory
namespace mem {
static std::mutex g_mem_mutex;
static u32 g_heap_top = MAIN_HEAP;
static std::map<u32, u32> g_user_blocks;   // addr -> size
static std::map<u32, u32> g_stack_blocks;

static void commit(u32 addr, u32 size) {
#ifdef _WIN32
    if (!VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE)) fatal("commit %08x+%x failed", addr, size);
#else
    if (mprotect(g_base + addr, size, PROT_READ | PROT_WRITE)) fatal("mprotect %08x+%x failed", addr, size);
#endif
}

static void install_fault_handler();

void init() {
    const u64 span = 0x100000000ull + 0x10000;   // 4 GiB + guard
#ifdef _WIN32
    g_base = (u8*)VirtualAlloc(nullptr, span, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_base = p == MAP_FAILED ? nullptr : (u8*)p;
#endif
    if (!g_base) fatal("cannot reserve guest address space");
    commit(0x00010000, 0x10000000 - 0x10000);   // ELF image + main memory
    commit(0x10000000, 0x01000000);             // ELF data segment
    commit(MAIN_HEAP, MAIN_HEAP_SIZE);
    commit(USER_BASE, USER_SIZE);
    commit(RSX_LOCAL, RSX_LOCAL_SIZE);
    commit(STACK_BASE, STACK_SIZE);
    install_fault_handler();
    LOG_I("guest memory reserved at %p", (void*)g_base);
}

u32 alloc(u32 size, u32 align) {
    std::lock_guard lk(g_mem_mutex);
    u32 a = (g_heap_top + align - 1) & ~(align - 1);
    if (a + size > MAIN_HEAP + MAIN_HEAP_SIZE) fatal("HLE heap exhausted");
    g_heap_top = a + size;
    memset(gmem(a), 0, size);
    return a;
}

static u32 range_alloc(std::map<u32, u32>& blocks, u32 base, u32 limit, u32 size, u32 align) {
    u32 cur = base;
    for (auto& [a, s] : blocks) {
        u32 cand = (cur + align - 1) & ~(align - 1);
        if (cand + size <= a) break;
        cur = a + s;
    }
    u32 cand = (cur + align - 1) & ~(align - 1);
    if ((u64)cand + size > (u64)base + limit) return 0;
    blocks[cand] = size;
    memset(gmem(cand), 0, size);
    return cand;
}

u32 user_alloc(u32 size, u32 align) {
    std::lock_guard lk(g_mem_mutex);
    return range_alloc(g_user_blocks, USER_BASE, USER_SIZE, size, align);
}
void user_free(u32 addr) {
    std::lock_guard lk(g_mem_mutex);
    g_user_blocks.erase(addr);
}
u32 user_available() {
    std::lock_guard lk(g_mem_mutex);
    u32 used = 0;
    for (auto& [a, s] : g_user_blocks) used += s;
    return USER_SIZE - used;
}
u32 stack_alloc(u32 size) {
    std::lock_guard lk(g_mem_mutex);
    size = (size + 0xFFFF) & ~0xFFFFu;
    // leave a 64 KiB gap between stacks to catch overflows in logs
    u32 a = range_alloc(g_stack_blocks, STACK_BASE + 0x10000, STACK_SIZE - 0x10000, size + 0x10000, 0x10000);
    if (!a) fatal("out of stack space");
    return a + 0x10000;
}
void stack_free(u32 addr) {
    std::lock_guard lk(g_mem_mutex);
    g_stack_blocks.erase(addr - 0x10000);
}

#ifdef _WIN32
static LONG CALLBACK fault_handler(PEXCEPTION_POINTERS ep) {
    auto* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    u8* addr = (u8*)rec->ExceptionInformation[1];
    // host code offset relative to the image base, to look up in f1ce.map
    unsigned long long rip_off = ep->ContextRecord->Rip - (unsigned long long)GetModuleHandleW(nullptr);
    if (addr >= g_base && addr < g_base + 0x100010000ull)
        fatal("guest memory fault: %s at guest address %08llx (host rip = image+0x%llx)",
              rec->ExceptionInformation[0] ? "write" : "read", (unsigned long long)(addr - g_base), rip_off);
    fatal("host access violation at %p (rip=%p)", (void*)addr, (void*)ep->ContextRecord->Rip);
}
static void install_fault_handler() { AddVectoredExceptionHandler(1, fault_handler); }
#else
static void fault_handler(int, siginfo_t* si, void*) {
    u8* addr = (u8*)si->si_addr;
    if (addr >= g_base && addr < g_base + 0x100010000ull)
        fatal("guest memory fault at guest address %08llx", (unsigned long long)(addr - g_base));
    fatal("host segmentation fault at %p", (void*)addr);
}
static void install_fault_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
#endif
}  // namespace mem

std::string gstr(u32 ea, size_t max) {
    if (!ea) return {};
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)ld8(ea + (u32)i);
        if (!ch) break;
        s += ch;
    }
    return s;
}
void gstr_write(u32 ea, const std::string& s, size_t max) {
    size_t n = std::min(s.size(), max - 1);
    memcpy(gmem(ea), s.data(), n);
    st8(ea + (u32)n, 0);
}

// ------------------------------------------------------------------ function dispatch
static constexpr u32 CODE_LO = 0x10000, CODE_HI = 0x720000;
static std::vector<PPUFunc> g_dispatch;   // (addr - CODE_LO) / 4

static void build_dispatch() {
    g_dispatch.assign((CODE_HI - CODE_LO) / 4, nullptr);
    for (u32 i = 0; i < g_func_count; i++) {
        u32 a = g_func_table[i].addr;
        if (a >= CODE_LO && a < CODE_HI) g_dispatch[(a - CODE_LO) / 4] = g_func_table[i].fn;
    }
}

// Host functions callable from guest code live at fake code addresses in the (unmapped) low page.
static constexpr u32 HOST_FN_BASE = 0x1000, HOST_FN_MAX = 1024;
static HleFnPtr g_host_fns[HOST_FN_MAX];
static std::atomic<u32> g_host_fn_count{0};

u32 host_func_opd(HleFnPtr fn) {
    u32 idx = g_host_fn_count++;
    if (idx >= HOST_FN_MAX) fatal("too many host functions");
    g_host_fns[idx] = fn;
    u32 opd = mem::alloc(8, 8);
    st32(opd, HOST_FN_BASE + idx * 4);
    st32(opd + 4, 0);
    return opd;
}

void call_indirect(PPUContext& c, u32 addr) {
    if (addr >= CODE_LO && addr < CODE_HI && !(addr & 3)) {
        if (PPUFunc f = g_dispatch[(addr - CODE_LO) / 4]) return f(c);
    }
    if (addr >= HOST_FN_BASE && addr < HOST_FN_BASE + HOST_FN_MAX * 4) {
        u32 idx = (addr - HOST_FN_BASE) / 4;
        if (idx < g_host_fn_count) return g_host_fns[idx](c);
    }
    fatal("indirect call to unknown address %08x (lr=%08x)", addr, (u32)c.lr);
}

PPUThread* thread_create_host(const std::string& name, std::function<void(PPUContext&)> body) {
    static u32 dummy_opd = 0;
    if (!dummy_opd) {
        dummy_opd = mem::alloc(8, 8);
        st32(dummy_opd + 4, 0);
    }
    u32 id;
    PPUThread* t = thread_create(dummy_opd, 0, 1, 0x10000, false, name, &id);
    t->host = std::thread([t, body = std::move(body)] {
        t_self = t;
        set_thread_name(t->name.c_str());
        try {
            body(t->ctx);
        } catch (const ThreadExit&) {
        }
    });
    t->host.detach();
    return t;
}

u64 guest_call(PPUContext& c, u32 opd, std::initializer_list<u64> args) {
    u64 saved_r2 = c.r[2], saved_lr = c.lr;
    int i = 0;
    for (u64 a : args) c.r[3 + i++] = a;
    c.r[2] = ld32(opd + 4);
    call_indirect(c, ld32(opd));
    c.r[2] = saved_r2;
    c.lr = saved_lr;
    return c.r[3];
}

// ------------------------------------------------------------------ traps
void trap_invalid(PPUContext& c, u32 addr, u32 word) {
    c.pc = addr;
    fatal("invalid instruction %08x at %08x", word, addr);
}
void trap_unsupported(PPUContext& c, u32 addr, const char* what) {
    c.pc = addr;
    fatal("unsupported instruction '%s' at %08x", what, addr);
}
void trap_hit(PPUContext& c, u32 addr) {
    c.pc = addr;
    fatal("trap instruction hit at %08x", addr);
}

// ------------------------------------------------------------------ reservations (lwarx/stwcx.)
// A global lock makes the reserve/conditional-store pair atomic with respect to other
// reservation users; plain stores from other threads are detected by value comparison.
// shared by PPU lwarx/stwcx. and SPU getllar/putllc so both kinds of atomic update exclude each other; striped by
// 128-byte line (every user updates a single line under it)
struct alignas(64) PaddedLock { SpinLock m; };
// (1024 stripes, hashed: with 64 the SPURS lines shared locks and the SPU workers spun on them ~3% of their time)
static PaddedLock g_atomic_locks[1024];
SpinLock& atomic_lock(u32 ea) { return g_atomic_locks[((ea >> 7) ^ (ea >> 17)) & 1023].m; }

// Write generation per 128-byte line (hashed; a collision only fails a conditional store, which is retried). Every
// write made under the line's reservation lock (PPU stwcx./stdcx., SPU PUTLLC/PUTLLUC, the HLE SPURS writers) bumps it,
// and an SPU PUTLLC fails when it changed since its GETLLAR, as a lost reservation does on the Cell: comparing the
// data alone missed stores of an unchanged value. The SPURS job manager read ready count 1 and an empty queue, the
// game queued jobs and stored ready count 1 again (no change), and the PM's PUTLLC of ready 0 succeeded: work queued
// with nobody scheduled to run it (freeze at the race start: main thread in cellSpursEventFlagWait for ever).
// (Tried 2026-11-03: the generation in the lock stripe's cache line was ~2% slower on the grid.)
static std::atomic<u32> g_line_gen[4096];
u32 line_generation(u32 ea) { return g_line_gen[(ea >> 7) & 4095].load(std::memory_order_relaxed); }
void line_written(u32 ea) {
    g_line_gen[(ea >> 7) & 4095].fetch_add(1, std::memory_order_relaxed);
    guest_written(ea, 1);
}

u32 ppu_lwarx(PPUContext& c, u32 ea) {
    std::lock_guard lk(atomic_lock(ea));
    c.res_addr = ea;
    c.res_value = ld32(ea);
    return (u32)c.res_value;
}
u64 ppu_ldarx(PPUContext& c, u32 ea) {
    std::lock_guard lk(atomic_lock(ea));
    c.res_addr = ea | 1;   // low bit marks a doubleword reservation
    c.res_value = ld64(ea);
    return c.res_value;
}
void ppu_stwcx(PPUContext& c, u32 ea, u32 v) {
    std::lock_guard lk(atomic_lock(ea));
    bool ok = c.res_addr == ea && ld32(ea) == (u32)c.res_value;
    if (ok) { st32(ea, v); line_written(ea); }
    c.res_addr = 0;
    c.cr[0] = 0; c.cr[1] = 0; c.cr[2] = ok; c.cr[3] = c.xer_so;
}
void ppu_stdcx(PPUContext& c, u32 ea, u64 v) {
    std::lock_guard lk(atomic_lock(ea));
    bool ok = c.res_addr == (ea | 1) && ld64(ea) == c.res_value;
    if (ok) { st64(ea, v); line_written(ea); }
    c.res_addr = 0;
    c.cr[0] = 0; c.cr[1] = 0; c.cr[2] = ok; c.cr[3] = c.xer_so;
}

// ------------------------------------------------------------------ threads
static IdMap<PPUThread> g_threads(0x01000000);
extern u32 g_tls_addr, g_tls_filesz, g_tls_memsz;   // from loader
u32 tls_alloc();                                      // hle_sys.cpp

PPUThread* thread_find(u32 id) { return g_threads.get(id).get(); }

static std::vector<std::shared_ptr<PPUThread>> g_thread_refs;
static std::mutex g_thread_refs_mutex;

PPUThread* thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stack_size, bool joinable, const std::string& name,
                         u32* out_id) {
    auto t = std::make_shared<PPUThread>();
    t->priority = prio;
    t->name = name;
    t->joinable = joinable;
    t->stack_size = std::max<u32>(stack_size, 0x4000);
    t->stack_addr = mem::stack_alloc(t->stack_size);
    t->id = g_threads.add(t);
    {
        std::lock_guard lk(g_thread_refs_mutex);
        g_thread_refs.push_back(t);
    }
    auto& c = t->ctx;
    c.thread = t.get();
    c.r[1] = (t->stack_addr + t->stack_size - 0x70) & ~0xFu;
    c.r[2] = ld32(entry_opd + 4);
    c.r[3] = arg;
    c.r[13] = tls_alloc();
    c.lr = 0;
    c.pc = ld32(entry_opd);
    c.vscr = 0x10000;   // non-Java mode
    if (out_id) *out_id = t->id;
    LOG_I("thread created: id=%08x '%s' entry=%08x prio=%d stack=%08x+%x", t->id, name.c_str(), c.pc, prio,
          t->stack_addr, t->stack_size);
    return t.get();
}

void place_thread(int kind);
static void thread_body(PPUThread* t) {
    t_self = t;
    set_thread_name(("ppu " + t->name).c_str());
    if (t->id == 0x01000000) { place_thread(1); raise_thread_priority(); }
    try {
        call_indirect(t->ctx, t->ctx.pc);
        t->exit_status = t->ctx.r[3];
    } catch (const ThreadExit& e) {
        t->exit_status = e.status;
    }
    LOG_I("thread %08x '%s' exited (status %llx)", t->id, t->name.c_str(), (unsigned long long)t->exit_status);
    line_wait_thread_exit();
    {
        std::lock_guard lk(t->m);
        t->finished = true;
    }
    t->cv.notify_all();
}

void thread_start(PPUThread* t) { t->host = std::thread(thread_body, t); t->host.detach(); }

static std::string backtrace_of(const PPUContext& c) {
    std::string bt;
    u32 sp = (u32)c.r[1];
    for (int depth = 0; depth < 16 && sp >= mem::STACK_BASE && sp < mem::STACK_BASE + mem::STACK_SIZE; depth++) {
        u32 next = (u32)ld64(sp);
        if (next <= sp || next >= mem::STACK_BASE + mem::STACK_SIZE) break;
        char buf[16];
        snprintf(buf, sizeof(buf), " %08x", (u32)ld64(next + 16));
        bt += buf;
        sp = next;
    }
    return bt;
}

// the main thread's samples of the last ~8 s, for profile_main_between (slow frames)
struct MainSample { u64 t; u32 fn; const char* hle; u8 busy; };
static MainSample g_main_ring[4096];
static std::atomic<u32> g_main_pos{0};

// what the main thread did between two times (F1CE_PROFILE only): its most frequent states, with the number of busy SPU
// workers in brackets
std::string profile_main_between(u64 t0, u64 t1) {
    std::map<std::string, int> h;
    int total = 0;
    const u32 end = g_main_pos.load();
    for (u32 i = end, n = 0; i-- > 0 && n < 4096; n++) {
        const MainSample& s = g_main_ring[i % 4096];
        if (s.t < t0) break;
        if (s.t > t1) continue;
        char b[64];
        if (s.hle) snprintf(b, sizeof(b), "hle %s[%u]", s.hle, s.busy);
        else snprintf(b, sizeof(b), "fn %08x[%u]", s.fn, s.busy);
        h[b]++;
        total++;
    }
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : h) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    for (size_t i = 0; i < v.size() && i < 6; i++) line += " " + v[i].second + " " + std::to_string(v[i].first);
    return std::to_string(total) + " samples:" + line;
}

// Profiling aid (F1CE_PROFILE=1): samples every 2 ms what each guest thread is doing (the HLE call it blocks
// in, or the guest function it executes) and logs the most frequent states per thread every 10 s.
void start_profiler() {
    if (!getenv("F1CE_PROFILE")) return;
    std::thread([] {
        std::map<std::string, std::map<std::string, u64>> hist;
        u64 last = time_usec();
        for (;;) {
            sleep_usec(2000);
            {
                std::lock_guard lk(g_thread_refs_mutex);
                for (auto& t : g_thread_refs) {
                    if (t->finished) continue;
                    const char* hle = t->in_hle.load();
                    char b[48];
                    if (hle) snprintf(b, sizeof(b), "hle %s", hle);
                    else snprintf(b, sizeof(b), "fn %08x", t->ctx.cur_fn);
                    hist[t->name][b]++;
                    if (t->name == "main") {
                        extern std::atomic<int> g_worker_wid[8];
                        u8 busy = 0;
                        for (int k = 0; k < 5; k++) busy += g_worker_wid[k].load() >= 0;
                        g_main_ring[g_main_pos % 4096] = {time_usec(), t->ctx.cur_fn, hle, busy};
                        g_main_pos++;
                    }
                    // what the SPU workers run while main waits for them (job counters / task event flag)
                    if (t->name == "main" && (strcmp(b, "fn 00551228") == 0 || strcmp(b, "hle cellSpursEventFlagWait") == 0)) {
                        extern std::atomic<int> g_worker_wid[8];
                        std::string s = b[0] == 'f' ? "jobwait" : "flagwait";
                        int idle = 0;
                        std::map<int, int> per;
                        for (int k = 0; k < 5; k++) { int w = g_worker_wid[k].load(); if (w < 0) idle++; else per[w]++; }
                        char c[32];
                        snprintf(c, sizeof(c), " idle%d", idle);
                        s += c;
                        for (auto& [w, n] : per) { snprintf(c, sizeof(c), " w%d:%d", w, n); s += c; }
                        hist["main-wait"][s]++;
                    }
                }
            }
            if (time_usec() - last < 10000000) continue;
            last = time_usec();
            for (auto& [name, h] : hist) {
                u64 total = 0;
                for (auto& [k, v] : h) total += v;
                std::vector<std::pair<u64, std::string>> v;
                for (auto& [k, n] : h) v.push_back({n, k});
                std::sort(v.rbegin(), v.rend());
                std::string line;
                for (size_t i = 0; i < v.size() && i < 8; i++) {
                    char b[80];
                    snprintf(b, sizeof(b), " %s %.0f%%", v[i].second.c_str(), 100.0 * v[i].first / total);
                    line += b;
                }
                LOG_I("profile %-24s:%s", name.c_str(), line.c_str());
            }
            hist.clear();
        }
    }).detach();
}

// With at least 12 logical processors (assumed two per core): the renderer gets a core, with its second logical processor
// left to the rest of the process, and the SPU workers stay off it, so their polling does not slow the renderer down.
// The renderer only prefers its processor (ideal processor), it is not bound to it: hard-pinned, it sometimes did not run
// for 10-30 ms while something above its priority held that processor (not one of our threads: HIGHEST priority did not
// help), and the flip label the vblank handler released was taken a vblank late every 0.25-0.5 s (30 fps average, 1% low
// ~16 at Silverstone). F1CE_PIN=1 also gives the main PPU thread a core, F1CE_PIN=0 leaves all threads to the system.
void place_thread(int kind) {
#ifdef _WIN32
    static const char* pin = getenv("F1CE_PIN");
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask);
    const u32 n = proc_mask == sys_mask ? GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) : 0;   // (not with F1CE_CPUS)
    if ((pin && pin[0] == '0') || n < 12 || n > 64 || (n & 1)) return;
    const u64 all = n == 64 ? ~0ull : (1ull << n) - 1;
    const bool only_rsx = !pin || pin[0] != '1';
    if (only_rsx && kind == 1) return;
    if (kind == 0) {
        PROCESSOR_NUMBER p = {0, (BYTE)(n - 2), 0};
        if (!SetThreadIdealProcessorEx(GetCurrentThread(), &p, nullptr)) LOG_W("place_thread: ideal processor not set");
        return;
    }
    const u64 mask = kind == 1 ? 1ull << (n - 4) : all >> (only_rsx ? 2 : 4);
    if (!SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)mask)) LOG_W("place_thread: affinity not set");
#endif
}

// Threads on the frame's critical path (the main PPU thread, SPURS task threads: main waits for their flags) run above
// the SPU workers' priority, so on a machine with fewer cores than busy threads they are not queued behind a worker.
// F1CE_PRIO=0: all at normal priority.
void raise_thread_priority() {
#ifdef _WIN32
    static const bool off = getenv("F1CE_PRIO") && getenv("F1CE_PRIO")[0] == '0';
    if (!off) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    // a lower nice value for this thread only; refused without CAP_SYS_NICE or an RLIMIT_NICE allowance (the usual
    // desktop and SteamOS case): then every thread stays at the same priority
    static const bool off = getenv("F1CE_PRIO") && getenv("F1CE_PRIO")[0] == '0';
    if (off) return;
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -5) != 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) LOG_I("thread priority: not raised (no permission), all threads at normal priority");
    }
#endif
}

// Test aid: F1CE_CPUS=<n> runs the process on its first n logical processors (e.g. 8 for a 4-core / 8-thread machine such
// as the Steam Deck; this one has two logical processors per core, so the first n are n / 2 cores).
void limit_cpus() {
#ifdef _WIN32
    const char* e = getenv("F1CE_CPUS");
    if (!e) return;
    const int n = atoi(e);
    if (n <= 0 || n >= 64) return;
    if (SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)((1ull << n) - 1))) LOG_I("F1CE_CPUS: %d logical processors", n);
    else LOG_W("F1CE_CPUS: affinity not set");
#else
    const char* e = getenv("F1CE_CPUS");
    if (!e) return;
    const int n = atoi(e);
    if (n <= 0 || n >= CPU_SETSIZE) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; i++) CPU_SET(i, &set);
    // (called before any thread starts: new threads inherit the mask)
    if (!sched_setaffinity(0, sizeof(set), &set)) LOG_I("F1CE_CPUS: %d logical processors", n);
    else LOG_W("F1CE_CPUS: affinity not set");
#endif
}

std::mutex g_prof_threads_mutex;
std::set<u32> g_prof_threads;   // host thread ids the host profiler samples (SPU workers register themselves)
void profile_register_thread() {
#ifdef _WIN32
    std::lock_guard lk(g_prof_threads_mutex);
    g_prof_threads.insert(GetCurrentThreadId());
#endif
}
#ifdef _WIN32
// Profiling aid (F1CE_HOST_PROFILE=<seconds before start>): samples the host instruction pointer of every thread
// of the process for 10 s and logs the hottest functions (resolved through build-win/f1ce.map).
void start_host_profiler() {
    const char* e = getenv("F1CE_HOST_PROFILE");
    if (!e) return;
    double delay = atof(e);
    std::thread([delay] {
        sleep_usec((u64)(delay * 1e6));
        LOG_I("host profile: sampling for 10 s");
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        std::map<u64, u64> hits;
        std::map<u64, u64> dll_callers;   // samples outside the executable, by the first executable frame on the stack
        u64 total = 0, end = time_usec() + 10000000;
        u64 base = (u64)GetModuleHandleW(nullptr);
        std::vector<HANDLE> sampled;
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                std::lock_guard lk(g_prof_threads_mutex);
                if (!g_prof_threads.empty() && !g_prof_threads.count(te.th32ThreadID)) continue;
                if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)) sampled.push_back(h);
            }
            CloseHandle(snap);
        }
        while (time_usec() < end) {
            for (HANDLE h : sampled) {
                if (SuspendThread(h) != (DWORD)-1) {
                    // no allocation while the thread is suspended: it may hold the CRT heap lock (the profiler
                    // then deadlocked and never printed its results)
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_FULL;
                    bool got = GetThreadContext(h, &c);
                    u64 caller = 0;
                    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
                    u64 image_size = nt->OptionalHeader.SizeOfImage;
                    if (got) {
                        CONTEXT u = c;
                        for (int depth = 0; depth < 32 && u.Rip - base >= image_size; depth++) {
                            DWORD64 img = 0;
                            RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(u.Rip, &img, nullptr);
                            if (!fe) { u.Rip = *(DWORD64*)u.Rsp; u.Rsp += 8; continue; }   // leaf function
                            void* hd = nullptr;
                            DWORD64 est = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, u.Rip, fe, &u, &hd, &est, nullptr);
                            if (!u.Rip) break;
                        }
                        if (c.Rip - base >= image_size && u.Rip - base < image_size) caller = u.Rip - base;
                    }
                    ResumeThread(h);
                    if (got) {
                        hits[c.Rip - base]++;
                        total++;
                        if (caller) dll_callers[caller]++;
                    }
                }
            }
            Sleep(1);
        }
        for (HANDLE h : sampled) CloseHandle(h);
        LOG_I("host profile: %llu samples", (unsigned long long)total);
        // resolve: map file lines "address size align out in symbol" (symbols are the lines with 4+ fields)
        std::vector<std::pair<u64, std::string>> syms;
        if (FILE* f = fopen("../../build-win/f1ce.map", "r")) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                unsigned long long a, sz;
                unsigned al;
                int n = 0;
                if (sscanf(line, "%llx %llx %u%n", &a, &sz, &al, &n) == 3) {
                    const char* p = line + n;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p && *p != '.' && *p != '<' && strncmp(p, "C:", 2) != 0) {
                        std::string nm(p);
                        while (!nm.empty() && (nm.back() == '\n' || nm.back() == '\r')) nm.pop_back();
                        syms.push_back({a - 0x1000 + 0x1000, nm});
                    }
                }
            }
            fclose(f);
        }
        std::sort(syms.begin(), syms.end());
        std::map<std::string, u64> by_fn;
        u64 image_end = syms.empty() ? 0 : syms.back().first + 0x10000;
        for (auto& [rva, n] : hits) {
            std::string nm;
            if (rva >= image_end) {   // outside the executable: name the module (system DLLs: waits, yields, copies)
                HMODULE mod = nullptr;
                char path[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(rva + base), &mod))
                    GetModuleFileNameA(mod, path, sizeof(path));
                const char* slash = strrchr(path, '\\');
                nm = std::string("[dll] ") + (slash ? slash + 1 : path);
            } else {
                auto it = std::upper_bound(syms.begin(), syms.end(), std::make_pair(rva, std::string("\xff")));
                nm = it == syms.begin() ? "?" : std::prev(it)->second;
            }
            by_fn[nm.substr(0, 90)] += n;
        }
        std::vector<std::pair<u64, std::string>> v;
        for (auto& [k, n] : by_fn) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 40; i++)
            LOG_I("host profile %5.1f%% %s", 100.0 * v[i].first / std::max<u64>(total, 1), v[i].second.c_str());
        {   // where the time outside the executable comes from
            std::map<std::string, u64> by_caller;
            for (auto& [rva, n] : dll_callers) {
                auto it = std::upper_bound(syms.begin(), syms.end(), std::make_pair(rva, std::string("ÿ")));
                by_caller[(it == syms.begin() ? std::string("?") : std::prev(it)->second).substr(0, 90)] += n;
            }
            std::vector<std::pair<u64, std::string>> w;
            for (auto& [k, n] : by_caller) w.push_back({n, k});
            std::sort(w.rbegin(), w.rend());
            for (size_t i = 0; i < w.size() && i < 15; i++)
                LOG_I("host profile dll time from %5.1f%% %s", 100.0 * w[i].first / std::max<u64>(total, 1), w[i].second.c_str());
            std::vector<std::pair<u64, u64>> top;
            for (auto& [rva, n] : dll_callers) top.push_back({n, rva});
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 10; i++)
                LOG_I("host profile dll caller rva %llx %5.2f%%", (unsigned long long)top[i].second, 100.0 * top[i].first / std::max<u64>(total, 1));
        }
        // hottest single addresses (for disassembly when a symbol hides static helpers)
        std::vector<std::pair<u64, u64>> top;
        for (auto& [rva, n] : hits) if (rva < image_end) top.push_back({n, rva});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 25; i++)
            LOG_I("host profile rva %llx %5.2f%%", (unsigned long long)top[i].second, 100.0 * top[i].first / std::max<u64>(total, 1));
    }).detach();
}
#else
void start_host_profiler() {}
#endif

// Debug aid: periodically dumps where every guest thread is (racy snapshot, diagnostics only).
void start_watchdog(u32 period_s) {
    std::thread([period_s] {
        for (;;) {
            sleep_usec(period_s * 1000000ull);
            std::lock_guard lk(g_thread_refs_mutex);
            LOG_I("---- watchdog: %zu threads ----", g_thread_refs.size());
            if (const char* e = getenv("F1CE_WATCH_MEM")) {   // debug: dump 0x200 bytes at a guest address
                u32 a = (u32)strtoul(e, nullptr, 16) & ~15u;
                u32 n = getenv("F1CE_WATCH_MEM_SIZE") ? (u32)strtoul(getenv("F1CE_WATCH_MEM_SIZE"), nullptr, 16) : 0x200;
                for (u32 k = 0; k < n; k += 32) {
                    std::string h;
                    for (u32 j = 0; j < 32; j += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32(a + k + j)); h += b; }
                    LOG_I("  mem %08x:%s", a + k, h.c_str());
                }
            }
            for (auto& t : g_thread_refs) {
                if (t->finished) continue;
                const auto& c = t->ctx;
                const char* hle = t->in_hle.load();
                LOG_I("  %08x %-28s fn=%08x pc=%08x lr=%08x r3=%08x r4=%08x r5=%08x r31=%08x%s%s bt:%s", t->id,
                      t->name.c_str(), c.cur_fn, c.pc, (u32)c.lr, (u32)c.r[3], (u32)c.r[4], (u32)c.r[5], (u32)c.r[31],
                      hle ? " in " : "", hle ? hle : "", backtrace_of(c).c_str());
                u32 p = (u32)c.r[3];
                if (mem::readable(p & ~15u, 64)) {
                    std::string mem;
                    for (u32 k = 0; k < 64; k += 4) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32((p & ~15u) + k)); mem += b; }
                    LOG_I("      [r3 & ~15] =%s", mem.c_str());
                    u32 tbl = c.cur_fn == 0x5513b8 && mem::readable(p, 40) ? ld32(p + 4) : 0;
                    if (mem::readable(tbl, 0x20 + 8 * 0x10000 + 8)) {   // job manager wait: control block layout from fn 0x55221c
                        // (racy diagnostics: the pointer is only followed when it points into guest memory)
                        std::string cs;
                        for (u32 k = 0; k < 6; k++) cs += " " + std::to_string(ld16(tbl + 0x14 + 2 * k));
                        u32 base = ld16(tbl + 0x12);
                        std::string jobs;
                        for (u32 j = (base > 3 ? base - 3 : 0); j < base + 5; j++) {
                            char b[40];
                            snprintf(b, sizeof(b), " [%u]%08x%08x", j, ld32(tbl + 0x20 + 8 * j), ld32(tbl + 0x24 + 8 * j));
                            jobs += b;
                        }
                        LOG_I("      job wait: target=%u ctrl=%08x submitted=%u base=%u spu_slots:%s", ld16(p + 38), tbl,
                              ld16(tbl + 0x10), base, cs.c_str());
                        LOG_I("      jobs:%s", jobs.c_str());
                    }
                }
                if (c.cur_fn == 0x5513b8 || c.cur_fn == 0x55127c || c.cur_fn == 0x551228) {
                    // the same job manager wait in two reports in a row: a freeze (hle_joblist.cpp, F1CE_JOBLIST_LOG)
                    static std::string last_wait;
                    const u32 q = mem::readable(p, 8) ? ld32(p + 4) : 0;
                    std::string w = std::to_string(t->id) + " " + std::to_string(c.cur_fn) + " " + std::to_string(p);
                    if (mem::readable(q, 0x20))
                        for (u32 k = 0; k < 8; k++) w += " " + std::to_string(ld16(q + 0x10 + 2 * k));
                    if (w == last_wait) {
                        extern void joblist_dump_current();
                        joblist_dump_current();
                    }
                    last_wait = w;
                }
                if (c.cur_fn == 0x4d31cc) {   // audio thread waiting for its three job lists (fn 0x551358 each, from r29)
                    const u32 slots = (u32)c.r[29];
                    for (u32 k = 0; k < 3 && mem::readable(slots + 4 * k, 4); k++) {
                        const u32 jl = ld32(slots + 4 * k);
                        const u32 q = mem::readable(jl, 0x28) ? ld32(jl + 4) : 0;
                        if (!mem::readable(q, 0x20)) continue;
                        std::string cs;
                        for (u32 j = 0; j < 7; j++) cs += " " + std::to_string(ld16(q + 0x12 + 2 * j));
                        LOG_I("      audio wait %u: list %08x queue %08x target=%u top=%u head+spus:%s", k, jl, q,
                              ld16(jl + 0x26), ld16(q + 0x10), cs.c_str());
                    }
                }
            }
            extern void spurs_watchdog_report();
            spurs_watchdog_report();
        }
    }).detach();
}

void thread_exit(PPUContext& c, u64 status) { throw ThreadExit{status}; }

void run_main_thread(u32 entry_opd, u32 stack_size, s32 prio) {
    build_dispatch();
    u32 id;
    PPUThread* t = thread_create(entry_opd, 0, prio, stack_size, true, "main", &id);
    auto& c = t->ctx;
    // crt0 register contract (see crt0 analysis): argc/argv/envp, then TLS info for sys_initialize_tls
    // argv/envp are arrays of 64-bit big-endian pointers (crt0 copies the low words)
    u32 argv = mem::alloc(32, 16);
    u32 arg0 = mem::alloc(64);
    gstr_write(arg0, "/app_home/EBOOT.BIN", 64);
    st64(argv, arg0);
    st64(argv + 8, 0);
    st64(argv + 16, 0);   // envp terminator
    c.r[3] = 1;
    c.r[4] = argv;
    c.r[5] = argv + 16;
    c.r[6] = 0;
    c.r[7] = id;
    c.r[8] = g_tls_addr;
    c.r[9] = g_tls_filesz;
    c.r[10] = g_tls_memsz;
    c.r[11] = ld32(entry_opd);
    extern u32 g_malloc_pagesize;
    c.r[12] = g_malloc_pagesize;
    c.r[13] = 0;   // set by sys_initialize_tls
    thread_body(t);
}

// ------------------------------------------------------------------ HLE registry
static std::unordered_map<std::string, HleFn>& hle_registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}
HleRegistrar::HleRegistrar(const char* name, HleFn fn) { hle_registry()[name] = fn; }

void hle_call(PPUContext& c, u32 idx) {
    static std::vector<HleFn> resolved;
    static std::vector<std::atomic<bool>> warned(g_import_count);
    static std::once_flag once;
    std::call_once(once, [] {
        resolved.resize(g_import_count);
        u32 missing = 0;
        for (u32 i = 0; i < g_import_count; i++) {
            auto it = hle_registry().find(g_imports[i].name);
            resolved[i] = it == hle_registry().end() ? nullptr : it->second;
            missing += !resolved[i];
        }
        LOG_I("HLE: %u/%u imports implemented", g_import_count - missing, g_import_count);
    });
    if (HleFn f = resolved[idx]) {
        LOG_T("HLE %s(%llx, %llx, %llx, %llx)", g_imports[idx].name, (unsigned long long)c.r[3],
              (unsigned long long)c.r[4], (unsigned long long)c.r[5], (unsigned long long)c.r[6]);
        if (t_self) t_self->in_hle = g_imports[idx].name;
        f(c);
        if (t_self) t_self->in_hle = nullptr;
        return;
    }
    if (!warned[idx].exchange(true))
        LOG_W("unimplemented HLE function %s::%s (r3=%llx r4=%llx) -> CELL_OK", g_imports[idx].library,
              g_imports[idx].name, (unsigned long long)c.r[3], (unsigned long long)c.r[4]);
    c.r[3] = 0;
}
